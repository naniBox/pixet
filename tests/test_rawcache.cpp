// Covers the RAW decode cache (core/cache/RawCache.h) - the on-disk store of full
// demosaic results, keyed by (path, mtime, size, long edge).
//
// Everything here works on synthesized RgbImages rather than real RAW files, deliberately:
// the cache neither knows nor cares that its contents came from a demosaic, and the suite
// has no RAW fixture to build one from (see test_rawcodec.cpp on why). What has to be
// right is the part that has teeth - that a changed file misses, that the budget is
// actually enforced, and that eviction throws away the least recently *used* entry rather
// than the oldest one.
#include "TestHarness.h"
#include "TestPaths.h"

#include <chrono>
#include <filesystem>
#include <thread>

#include "cache/RawCache.h"
#include "decode/RgbImage.h"

using namespace pixet;

namespace {

std::string freshCacheDir(const std::string &name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pixet_tests" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

// A flat-coloured image of a given size. Flat so it encodes small and predictably; the
// colour is what proves a lookup returned *this* entry rather than some other one.
RgbImage solid(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    RgbImage img;
    img.w = w;
    img.h = h;
    img.pixels.resize((size_t)w * h * 3);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        img.pixels[i * 3 + 0] = r;
        img.pixels[i * 3 + 1] = g;
        img.pixels[i * 3 + 2] = b;
    }
    return img;
}

// A memory tier big enough to hold the handful of small images these tests make, so the
// disk behaviour under test is never perturbed by an eviction nobody asked about. The
// tier has its own tests below.
constexpr int64_t kMem = 32 * 1024 * 1024;

int64_t cacheBytes() { return rawcache::stats().bytes; }

} // namespace

PIXET_TEST(RawCacheRoundTripsAnImage) {
    rawcache::configure(freshCacheDir("rawcache_roundtrip"), 64 * 1024 * 1024, 512, kMem);

    RgbImage stored = solid(400, 300, 200, 40, 40);
    rawcache::store("/photos/a.arw", 111, 222, stored);

    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 111, 222, got));
    PIXET_CHECK(got.w == 400);
    PIXET_CHECK(got.h == 300);
    // JPEG is lossy, so the colour is checked with tolerance - the point is that this is
    // the red image and not the green one below, not that it survived bit-exact.
    PIXET_CHECK(got.pixels[0] > 150);
    PIXET_CHECK(got.pixels[1] < 90);
}

PIXET_TEST(RawCacheMissesWhenTheFileChanged) {
    rawcache::configure(freshCacheDir("rawcache_identity"), 64 * 1024 * 1024, 512, kMem);
    rawcache::store("/photos/a.arw", 111, 222, solid(64, 64, 10, 200, 10));

    RgbImage got;
    // The same path, but edited since. Returning the old decode here would show the user
    // the previous version of their photo, which is the one failure mode a cache like this
    // must not have.
    PIXET_CHECK(!rawcache::lookup("/photos/a.arw", 999, 222, got)); // mtime moved
    PIXET_CHECK(!rawcache::lookup("/photos/a.arw", 111, 999, got)); // size moved
    PIXET_CHECK(!rawcache::lookup("/photos/b.arw", 111, 222, got)); // different file
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 111, 222, got));  // and the real one still hits
}

PIXET_TEST(RawCacheStoresAtTheConfiguredLongEdge) {
    rawcache::configure(freshCacheDir("rawcache_size"), 64 * 1024 * 1024, 256, kMem);

    // Bigger than the setting: downscaled on the way in, because an entry being display
    // sized is the whole reason the budget means anything.
    rawcache::store("/photos/big.arw", 1, 1, solid(1024, 768, 90, 90, 200));
    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/big.arw", 1, 1, got));
    PIXET_CHECK(got.w == 256);
    PIXET_CHECK(got.h == 192);

    // Smaller than the setting: stored as-is rather than upscaled, which would invent
    // detail and cost space for it.
    rawcache::store("/photos/small.arw", 2, 2, solid(100, 50, 90, 90, 200));
    PIXET_CHECK(rawcache::lookup("/photos/small.arw", 2, 2, got));
    PIXET_CHECK(got.w == 100);
    PIXET_CHECK(got.h == 50);
}

PIXET_TEST(RawCacheChangingLongEdgeKeepsBothSizesAddressable) {
    std::string dir = freshCacheDir("rawcache_edge_change");
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);
    rawcache::store("/photos/a.arw", 1, 1, solid(1024, 768, 200, 30, 30));

    // The long edge is part of the key, so switching sizes must not return the old size's
    // image at the new size - someone toggling between two settings should get each one's
    // own entry rather than a stale mismatch.
    rawcache::configure(dir, 64 * 1024 * 1024, 512, kMem);
    RgbImage got;
    PIXET_CHECK(!rawcache::lookup("/photos/a.arw", 1, 1, got));

    rawcache::store("/photos/a.arw", 1, 1, solid(1024, 768, 200, 30, 30));
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
    PIXET_CHECK(got.w == 512);

    // And the original entry is still there, so switching back is instant rather than a
    // re-decode of the whole library.
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
    PIXET_CHECK(got.w == 256);
}

PIXET_TEST(RawCacheDisabledByAZeroBudget) {
    std::string dir = freshCacheDir("rawcache_disabled");
    rawcache::configure(dir, 0, 512, kMem);
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 10, 10, 10));

    RgbImage got;
    PIXET_CHECK(!rawcache::lookup("/photos/a.arw", 1, 1, got));
    PIXET_CHECK(rawcache::cachedLongEdge() == 0); // callers use this to skip the lookup entirely
    PIXET_CHECK(cacheBytes() == 0);
}

PIXET_TEST(RawCacheEvictsLeastRecentlyUsedWhenOverBudget) {
    std::string dir = freshCacheDir("rawcache_evict");
    // Deliberately generous while filling, so nothing is evicted before the budget drops.
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);

    // Three distinct entries, written in order a, b, c.
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    rawcache::store("/photos/b.arw", 2, 2, solid(256, 256, 20, 200, 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    rawcache::store("/photos/c.arw", 3, 3, solid(256, 256, 20, 20, 200));

    // Touch the oldest one, making it the most recently *used*. A FIFO would still throw
    // this away first; an LRU must not.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));

    const int64_t before = cacheBytes();
    PIXET_CHECK(before > 0);

    // Squeeze to roughly two entries' worth. configure() sweeps on the way in, so this is
    // also the "lowering the budget frees the space now" path.
    rawcache::configure(dir, (before * 2) / 3, 256, kMem);

    PIXET_CHECK(cacheBytes() <= (before * 2) / 3);
    // b was the least recently used, so it is the one that should be gone.
    PIXET_CHECK(!rawcache::lookup("/photos/b.arw", 2, 2, got));
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
}

PIXET_TEST(RawCacheClearEmptiesIt) {
    rawcache::configure(freshCacheDir("rawcache_clear"), 64 * 1024 * 1024, 256, kMem);
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    rawcache::store("/photos/b.arw", 2, 2, solid(256, 256, 20, 200, 20));
    PIXET_CHECK(rawcache::stats().entries == 2);

    PIXET_CHECK(rawcache::clear() > 0);
    PIXET_CHECK(rawcache::stats().entries == 0);
    PIXET_CHECK(cacheBytes() == 0);

    // Still usable afterwards rather than needing a reconfigure - Clear Cache in
    // Preferences doesn't turn the cache off.
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
}

PIXET_TEST(RawCacheMemoryTierServesWithoutTheFile) {
    std::string dir = freshCacheDir("rawcache_mem_hit");
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    PIXET_CHECK(rawcache::stats().memoryEntries == 1); // resident straight after the store

    // Delete the file behind the cache's back. A hit now can only be coming from memory,
    // which is what proves the tier is actually in front of the disk rather than beside it.
    for (const auto &e : std::filesystem::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) { std::error_code ec; std::filesystem::remove(e.path(), ec); break; }
    }
    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
    PIXET_CHECK(got.w == 256);
}

PIXET_TEST(RawCacheMemoryTierIsBoundedAndLru) {
    std::string dir = freshCacheDir("rawcache_mem_bound");
    // Room for about two of these images, so admitting a third has to evict the coldest.
    const RgbImage sample = solid(256, 256, 10, 10, 10);
    const int64_t each = (int64_t)sample.pixels.size();
    rawcache::configure(dir, 64 * 1024 * 1024, 256, each * 2 + each / 2);

    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    rawcache::store("/photos/b.arw", 2, 2, solid(256, 256, 20, 200, 20));
    PIXET_CHECK(rawcache::stats().memoryEntries == 2);

    // Make `a` the most recently used, then admit a third.
    RgbImage got;
    PIXET_CHECK(rawcache::lookup("/photos/a.arw", 1, 1, got));
    rawcache::store("/photos/c.arw", 3, 3, solid(256, 256, 20, 20, 200));

    rawcache::Stats s = rawcache::stats();
    PIXET_CHECK(s.memoryEntries == 2);            // bounded
    PIXET_CHECK(s.memoryBytes <= each * 2 + each / 2);
    PIXET_CHECK(s.entries == 3);                  // all three still on disk, only RAM is tight
}

PIXET_TEST(RawCachePrewarmOnlyPromotesWhatIsAlreadyCached) {
    std::string dir = freshCacheDir("rawcache_prewarm");
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);

    // Never stored, so there is nothing to promote - and prewarm must not manufacture it,
    // or warming a folder of RAWs would demosaic every one of them.
    PIXET_CHECK(!rawcache::prewarm("/photos/never-seen.arw", 1, 1));
    PIXET_CHECK(rawcache::stats().memoryEntries == 0);

    // Write the entry with the memory tier switched off, so it lands on disk and nowhere
    // else - the state prewarm() exists to fix. (Reconfiguring with identical settings
    // deliberately keeps whatever is resident, so it can't be used to empty the tier.)
    rawcache::configure(dir, 64 * 1024 * 1024, 256, 0);
    rawcache::store("/photos/a.arw", 1, 1, solid(256, 256, 200, 20, 20));
    rawcache::configure(dir, 64 * 1024 * 1024, 256, kMem);
    PIXET_CHECK(rawcache::stats().entries == 1);
    PIXET_CHECK(rawcache::stats().memoryEntries == 0);

    PIXET_CHECK(rawcache::prewarm("/photos/a.arw", 1, 1));
    PIXET_CHECK(rawcache::stats().memoryEntries == 1);
}
