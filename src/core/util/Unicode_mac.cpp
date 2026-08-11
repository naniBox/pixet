#include "Unicode.h"

#include <CoreFoundation/CoreFoundation.h>

#include <vector>

namespace pixet {

// Why this exists at all:
//
// File identity in the DB is `dirs.path TEXT UNIQUE` and `UNIQUE(dir_id, name)`, compared
// as raw UTF-8 bytes, and Indexer diffs a directory's contents through an
// std::unordered_map<std::string, ExistingFile> - also raw bytes. macOS is not consistent
// about which Unicode normalization form a filename arrives in: HFS+ stored NFD
// (decomposed - "e" followed by U+0301 COMBINING ACUTE), APFS is normalization-
// *preserving* rather than normalizing, and Qt, Finder and the shell all hand over NFC
// (precomposed U+00E9). So the same file on disk can reach us as two different byte
// strings depending on which API produced it.
//
// Left alone, that means a folder like "mdl.2011-05-15_Noemie" (with the accent) gets a
// second dirs row the first time the scan path and the DB-lookup path disagree, and then
// re-thumbnails forever because neither side can find the other's rows. It is a silent,
// data-level bug rather than a crash, which is why it's worth handling up front instead of
// after someone notices duplicate folders.
//
// CoreFoundation rather than ICU or a hand-rolled table: it costs one
// `-framework CoreFoundation` on the APPLE branch, it's guaranteed present, and this is
// the only Unicode operation the whole codebase needs. NFC rather than NFD because NFC is
// what Qt produces, and Qt is where essentially every lookup originates.
std::string toNfc(const std::string &utf8) {
    if (utf8.empty()) return utf8;

    CFStringRef original =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(utf8.data()),
                                static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    // Not valid UTF-8. Pass it through untouched rather than mangling it - a filename we
    // can't interpret is still a filename we have to be able to open.
    if (!original) return utf8;

    CFMutableStringRef normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, original);
    CFRelease(original);
    if (!normalized) return utf8;

    CFStringNormalize(normalized, kCFStringNormalizationFormC);

    CFIndex length = CFStringGetLength(normalized);
    CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf(static_cast<size_t>(maxBytes));
    CFIndex used = 0;
    Boolean ok = CFStringGetBytes(normalized, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false,
                                  reinterpret_cast<UInt8 *>(buf.data()), maxBytes, &used);
    CFRelease(normalized);
    if (!ok) return utf8;

    return std::string(buf.data(), static_cast<size_t>(used));
}

} // namespace pixet
