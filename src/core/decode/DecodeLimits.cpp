#include "DecodeLimits.h"

#include <atomic>

namespace pixet {
namespace decodelimits {

namespace {
// Plain atomics rather than rawcache's mutex: these are read on every decode, on every
// pool thread at once, and written approximately never (once at startup, again if the
// user changes a preference). Relaxed ordering is enough - nothing else is published
// alongside them, and a decode that reads the value a microsecond before a change simply
// runs under the old limit, which is indistinguishable from having started that much
// earlier.
std::atomic<int64_t> g_maxFileBytes{kDefaultMaxFileBytes};
std::atomic<int64_t> g_maxPixels{kDefaultMaxPixels};
} // namespace

void configure(int64_t maxFileBytes, int64_t maxPixels) {
    g_maxFileBytes.store(maxFileBytes, std::memory_order_relaxed);
    g_maxPixels.store(maxPixels, std::memory_order_relaxed);
}

int64_t maxFileBytes() { return g_maxFileBytes.load(std::memory_order_relaxed); }

int64_t maxPixels() { return g_maxPixels.load(std::memory_order_relaxed); }

bool fileSizeAllowed(int64_t sizeBytes) {
    const int64_t limit = maxFileBytes();
    if (limit <= 0) return true;
    return sizeBytes <= limit;
}

bool pixelsAllowed(int64_t width, int64_t height) {
    const int64_t limit = maxPixels();
    if (limit <= 0) return true;
    if (width <= 0 || height <= 0) return true; // not this function's call - see the header
    return width <= limit / height;             // width*height <= limit, without the multiply
}

} // namespace decodelimits
} // namespace pixet
