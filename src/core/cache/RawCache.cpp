#include "RawCache.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../decode/JpegCodec.h"
#include "../util/FileIO.h"

namespace pixet {
namespace rawcache {

namespace {

// Everything mutable lives behind one mutex. lookup()/store() are called from the
// preview decoder's thread and from both fullscreen decoder threads at once, and the
// size accounting below is the kind of counter that silently drifts if it isn't guarded.
std::mutex g_mutex;
std::string g_dir;
int64_t g_maxBytes = 0;
int g_longEdge = 0;

// Running total, so the common case (a store that leaves the cache under budget) costs
// no directory walk at all. -1 means "not counted yet": the first store after configure()
// pays for one scan, and nothing pays for it again.
int64_t g_bytes = -1;

// The memory tier: decoded images by cache key, with a list holding the keys in
// least-recently-used order so eviction is a pop from the front rather than a scan.
struct MemEntry {
    RgbImage img;
    int64_t bytes = 0;
    std::list<std::string>::iterator lru;
};
std::unordered_map<std::string, MemEntry> g_mem;
std::list<std::string> g_memLru; // front = least recently used
int64_t g_memBytes = 0;
int64_t g_memMaxBytes = 0;

constexpr int kQuality = 92;

// Evicting exactly to the limit means the next insert is immediately over it again, so
// every single store turns into a delete. Going a little under buys back a run of cheap
// inserts before the next sweep.
constexpr double kEvictTo = 0.9;

// FNV-1a, twice with different offset bases, giving 128 bits. Not a cryptographic hash
// and doesn't need to be - the only requirement is that two different files never collide
// in practice, and 128 bits of a decent avalanche is far past the point where that stops
// being a real possibility. Cheap enough to run on every lookup without thinking about it.
void fnv1a128(const std::string &s, uint64_t &hi, uint64_t &lo) {
    hi = 14695981039346656037ULL;
    lo = 1099511628211ULL;
    for (unsigned char c : s) {
        hi = (hi ^ c) * 1099511628211ULL;
        lo = (lo ^ (uint64_t)(c + 0x9E)) * 1099511628211ULL;
    }
    // One extra round each so the last byte is as well mixed as the rest.
    hi *= 1099511628211ULL;
    lo *= 1099511628211ULL;
}

std::string hexKey(const std::string &filePath, int64_t mtime, int64_t size, int longEdge) {
    // The long edge is part of the key rather than a separate directory level, so changing
    // the size setting leaves old entries addressable-but-unasked-for rather than orphaning
    // a whole tree that eviction would then have to be taught about.
    std::string material = filePath;
    material += '\0';
    material += std::to_string(mtime);
    material += '\0';
    material += std::to_string(size);
    material += '\0';
    material += std::to_string(longEdge);

    uint64_t hi = 0, lo = 0;
    fnv1a128(material, hi, lo);

    static const char *digits = "0123456789abcdef";
    std::string out(32, '0');
    for (int i = 0; i < 16; ++i) out[i] = digits[(hi >> (60 - i * 4)) & 0xF];
    for (int i = 0; i < 16; ++i) out[16 + i] = digits[(lo >> (60 - i * 4)) & 0xF];
    return out;
}

std::filesystem::path pathForKey(const std::string &key) {
    return std::filesystem::path(g_dir) / key.substr(0, 2) / key.substr(2, 2) / (key + ".jpg");
}

// Every regular file under the cache root, with its size and last-write time. Used by the
// size scan, by eviction and by stats() - all three want exactly this and nothing else.
struct Entry {
    std::filesystem::path path;
    int64_t bytes = 0;
    std::filesystem::file_time_type used{};
};

std::vector<Entry> scan() {
    std::vector<Entry> entries;
    std::error_code ec;
    if (g_dir.empty() || !std::filesystem::exists(g_dir, ec)) return entries;

    for (std::filesystem::recursive_directory_iterator it(g_dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        Entry e;
        e.path = it->path();
        e.bytes = (int64_t)it->file_size(ec);
        e.used = it->last_write_time(ec);
        if (!ec) entries.push_back(std::move(e));
    }
    return entries;
}

// Defined below with the rest of the memory-tier helpers; declared here because disk
// eviction has to drop the resident copy too, and the two live at opposite ends of this
// anonymous namespace.
void memErase(const std::string &key);

// Caller holds g_mutex.
void evictIfNeeded() {
    if (g_maxBytes <= 0 || g_bytes < 0 || g_bytes <= g_maxBytes) return;

    std::vector<Entry> entries = scan();
    // Oldest *use* first, not oldest creation: lookup() touches an entry's write time on
    // every hit, so this is a genuine LRU rather than a FIFO that would throw away the
    // file someone opens every day just because they imported it first.
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.used < b.used; });

    const int64_t target = (int64_t)(g_maxBytes * kEvictTo);
    int64_t total = 0;
    for (const Entry &e : entries) total += e.bytes;

    std::error_code ec;
    for (const Entry &e : entries) {
        if (total <= target) break;
        if (std::filesystem::remove(e.path, ec)) {
            total -= e.bytes;
            // The memory tier is a strict subset of the files, never an independent store.
            // Leaving a resident copy of something just evicted would mean the disk budget
            // stopped deciding what the cache actually holds, and an entry the user watched
            // disappear would keep being served. The filename is the key.
            memErase(e.path.stem().string());
        }
    }
    g_bytes = total;
}

int64_t imageBytes(const RgbImage &img) { return (int64_t)img.pixels.size(); }

// Caller holds g_mutex.
void memTouch(const std::string &key) {
    auto it = g_mem.find(key);
    if (it == g_mem.end()) return;
    g_memLru.splice(g_memLru.end(), g_memLru, it->second.lru); // move to the most-recent end
}

// Caller holds g_mutex.
void memPut(const std::string &key, const RgbImage &img) {
    if (g_memMaxBytes <= 0) return;

    auto existing = g_mem.find(key);
    if (existing != g_mem.end()) {
        memTouch(key);
        return;
    }

    const int64_t bytes = imageBytes(img);
    // An image too big for the whole budget would evict everything and then not fit, so
    // it is simply not admitted - better to re-read one oversized entry than to empty the
    // tier on its account.
    if (bytes > g_memMaxBytes) return;

    while (g_memBytes + bytes > g_memMaxBytes && !g_memLru.empty()) {
        const std::string &victim = g_memLru.front();
        auto vit = g_mem.find(victim);
        if (vit != g_mem.end()) {
            g_memBytes -= vit->second.bytes;
            g_mem.erase(vit);
        }
        g_memLru.pop_front();
    }

    MemEntry e;
    e.img = img;
    e.bytes = bytes;
    g_memLru.push_back(key);
    e.lru = std::prev(g_memLru.end());
    g_mem.emplace(key, std::move(e));
    g_memBytes += bytes;
}

// Caller holds g_mutex.
void memErase(const std::string &key) {
    auto it = g_mem.find(key);
    if (it == g_mem.end()) return;
    g_memBytes -= it->second.bytes;
    g_memLru.erase(it->second.lru);
    g_mem.erase(it);
}

// Caller holds g_mutex.
void memClear() {
    g_mem.clear();
    g_memLru.clear();
    g_memBytes = 0;
}

} // namespace

void configure(const std::string &cacheDirUtf8, int64_t maxBytes, int longEdge, int64_t maxMemoryBytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool identityChanged = (cacheDirUtf8 != g_dir) || (std::max(0, longEdge) != g_longEdge);
    g_dir = cacheDirUtf8;
    g_maxBytes = std::max<int64_t>(0, maxBytes);
    g_longEdge = std::max(0, longEdge);
    g_memMaxBytes = std::max<int64_t>(0, maxMemoryBytes);

    // Keys embed the long edge and the directory, so a change to either makes every
    // resident image answer a question nobody will ask again. Dropping them is both
    // correct and the only way the tier ever shrinks when its budget is lowered.
    if (identityChanged || g_memMaxBytes == 0) memClear();
    while (g_memBytes > g_memMaxBytes && !g_memLru.empty()) {
        auto vit = g_mem.find(g_memLru.front());
        if (vit != g_mem.end()) {
            g_memBytes -= vit->second.bytes;
            g_mem.erase(vit);
        }
        g_memLru.pop_front();
    }
    // Force a recount: the budget may have shrunk, and the sweep below has to work from a
    // real number rather than whatever the previous configuration was tracking.
    g_bytes = -1;

    if (g_maxBytes > 0 && !g_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_dir, ec);
        // Trim now, not on the next store: lowering the limit in Preferences should free
        // the space when the user presses OK, not whenever they next open a RAW.
        int64_t total = 0;
        for (const Entry &e : scan()) total += e.bytes;
        g_bytes = total;
        evictIfNeeded();
    }
}

int cachedLongEdge() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_maxBytes > 0 ? g_longEdge : 0;
}

bool lookup(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes, RgbImage &out) {
    std::filesystem::path path;
    std::string key;
    bool residentHit = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_maxBytes <= 0 || g_dir.empty() || g_longEdge <= 0) return false;
        key = hexKey(filePathUtf8, mtimeUnix, sizeBytes, g_longEdge);

        // Memory tier first: no disk read, no JPEG decode, just a copy out.
        auto it = g_mem.find(key);
        if (it != g_mem.end()) {
            out = it->second.img;
            memTouch(key);
            residentHit = true;
        }
        path = pathForKey(key);
    }

    if (residentHit) {
        // Touch the file even though it wasn't read. Disk eviction orders by write time, so
        // without this a file being served from memory looks untouched on disk and gets
        // evicted as cold - taking the resident copy with it, since memory is a subset of
        // the files. The hottest entries in the cache would be the first to go.
        std::error_code ec;
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
        return true;
    }

    std::vector<uint8_t> bytes;
    // Read outside the lock: this is a multi-megabyte read, and holding the mutex across
    // it would serialise the fullscreen viewer's two decoder threads against each other
    // for no reason - they are reading different files.
    if (!readWholeFile(path.string(), bytes) || bytes.empty()) return false;
    if (!decodeJpeg(bytes.data(), bytes.size(), 0, out)) return false;

    // A disk hit is exactly the thing worth keeping resident: it has already proved
    // someone is looking at this file.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        memPut(key, out);
    }

    // Mark as recently used. Best-effort: a cache whose LRU ordering drifts is still a
    // correct cache, so a failure here is not worth failing the hit over.
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    return true;
}

void store(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes, const RgbImage &img) {
    std::filesystem::path path;
    std::string key;
    int longEdge = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_maxBytes <= 0 || g_dir.empty() || g_longEdge <= 0) return;
        longEdge = g_longEdge;
        key = hexKey(filePathUtf8, mtimeUnix, sizeBytes, g_longEdge);
        path = pathForKey(key);
    }
    if (img.empty()) return;

    // Downscale before encoding, not after: the whole point is that an entry is display
    // sized. resizeBoxDownscale never upscales, so a RAW whose decode came out smaller
    // than the setting is stored at its own size and still hits on the way back.
    const RgbImage *toEncode = &img;
    RgbImage resized;
    if (std::max(img.w, img.h) > longEdge) {
        resizeBoxDownscale(img, longEdge, resized);
        toEncode = &resized;
    }

    std::vector<uint8_t> jpeg;
    if (!encodeJpeg(*toEncode, kQuality, jpeg) || jpeg.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    // Written to a temporary name and renamed into place. Two decoder threads can be
    // asked for the same file at once (the fullscreen viewer prefetches neighbours while
    // the current row decodes), and a reader must never see a half-written JPEG - a
    // rename is the only step that is atomic on both platforms.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char *>(jpeg.data()), (std::streamsize)jpeg.size());
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    // Resident immediately, not on the next lookup: the caller just paid for this decode
    // and is about to display it.
    memPut(key, *toEncode);
    if (g_bytes < 0) {
        int64_t total = 0;
        for (const Entry &e : scan()) total += e.bytes;
        g_bytes = total;
    } else {
        g_bytes += (int64_t)jpeg.size();
    }
    evictIfNeeded();
}

bool prewarm(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_memMaxBytes <= 0 || g_maxBytes <= 0 || g_dir.empty() || g_longEdge <= 0) return false;
        const std::string key = hexKey(filePathUtf8, mtimeUnix, sizeBytes, g_longEdge);
        if (g_mem.count(key)) {
            memTouch(key);
            return true; // already resident
        }
    }
    // Reuses lookup() rather than repeating the read/decode/insert: the only difference
    // between warming an entry and using one is what the caller does with the image.
    RgbImage tmp;
    return lookup(filePathUtf8, mtimeUnix, sizeBytes, tmp);
}

Stats stats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    Stats s;
    s.memoryBytes = g_memBytes;
    s.memoryEntries = (int64_t)g_mem.size();
    for (const Entry &e : scan()) {
        s.bytes += e.bytes;
        s.entries++;
    }
    g_bytes = s.bytes; // free recount, since the walk just happened
    return s;
}

int64_t clear() {
    std::lock_guard<std::mutex> lock(g_mutex);
    memClear(); // otherwise a cleared cache would keep serving from RAM
    int64_t freed = 0;
    std::error_code ec;
    for (const Entry &e : scan()) {
        if (std::filesystem::remove(e.path, ec)) freed += e.bytes;
    }
    g_bytes = 0;
    return freed;
}

} // namespace rawcache
} // namespace pixet
