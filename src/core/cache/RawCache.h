#pragma once

#include <cstdint>
#include <string>

#include "../decode/RgbImage.h"

namespace pixet {

// An on-disk cache of decoded RAW images, at display size.
//
// It exists because a full demosaic is the one decode in this app that is slow enough to
// be felt every single time: seconds per file on a 24MP RAW, and unavoidable for viewing,
// because a RAW's embedded preview is far too small to fill a modern screen (measured on
// Sony ARW: a 229KB preview against a 6000px sensor image). Browsing a folder of RAWs
// means paying that cost again on every visit, for a result that is identical every time.
//
// Deliberately files on disk rather than rows in thumbs.db. Entries here are ~1-3MB
// against a thumbnail's ~20KB, and a few thousand of them would make the SQLite file the
// largest thing in the app while gaining nothing: nothing ever queries this by anything
// other than its exact key, there are no transactions to be part of, and eviction wants
// to delete bytes rather than leave a file that needs vacuuming. A hashed path is also
// inspectable and individually deletable, which a blob table is not.
//
// Layout is <cacheDir>/ab/cd/abcd...ef.jpg - a 128-bit key rendered as hex, fanned out
// two bytes at a time so no single directory holds more than a few hundred entries.
// Every file is a plain JPEG: the folder can be browsed with any image viewer, which
// matters the day something looks wrong and the question is whether the cache or the
// decoder is at fault.
namespace rawcache {

// Where the cache lives, how big it may get, and what size decodes are stored at. Call
// once at startup and again whenever the user changes any of them.
//
// Changing longEdge does not invalidate anything: it is part of every entry's key, so
// old entries simply stop being asked for and age out through the usual eviction. That is
// deliberate - someone toggling between two sizes shouldn't have to re-decode their
// library twice.
//
// A maxBytes of 0 disables the cache entirely: lookup() always misses and store() does
// nothing. Existing files are left alone rather than deleted, so turning it off and on
// again is free.
// `maxMemoryBytes` sizes a second tier in front of the files: decoded images kept in RAM,
// so an entry that is already resident costs neither a disk read nor a JPEG decode. The
// disk tier alone still leaves ~50ms per image on the table, which is the difference
// between "quick" and the thing feeling instant while scrolling.
//
// Bounded and LRU like the disk tier, because a decoded 2560px image is ~13MB - a
// screenful of them is a quarter of a gigabyte, so this cannot be allowed to simply grow
// with the window. 0 disables the memory tier while leaving the files alone.
void configure(const std::string &cacheDirUtf8, int64_t maxBytes, int longEdge, int64_t maxMemoryBytes);

// The long edge configure() was given, so a caller can tell whether the cache is able to
// satisfy the size it is about to ask for. 0 when disabled.
int cachedLongEdge();

// Fills `out` from the cache if this exact file is in it. `mtimeUnix` and `sizeBytes` are
// part of the key, so an edited file misses rather than returning a stale decode.
//
// A hit also marks the entry as recently used, which is what eviction reads - so a file
// browsed daily survives while one opened once a year does not, regardless of which was
// created first.
bool lookup(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes, RgbImage &out);

// Promotes an entry that is already on disk into the memory tier, so the next lookup()
// for it costs nothing. Returns true if it is resident afterwards.
//
// Deliberately does NOT decode anything that isn't already cached: warming what is visible
// must never turn scrolling past a folder of RAWs into a demosaic of every one of them.
// A file with no disk entry stays a miss until something actually asks for it.
bool prewarm(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes);

// Stores `img` for that file, downscaling to the configured long edge first if it is
// larger. Silently does nothing when the cache is disabled or the write fails - a cache
// that cannot write is a slow app, not a broken one, and there is no caller that could
// sensibly react to the failure anyway.
//
// Triggers eviction when the cache is over budget: oldest-used first, down to slightly
// under the limit rather than exactly to it, so a run of inserts doesn't turn into a
// delete on every single one.
void store(const std::string &filePathUtf8, int64_t mtimeUnix, int64_t sizeBytes, const RgbImage &img);

struct Stats {
    int64_t bytes = 0;
    int64_t entries = 0;
    int64_t memoryBytes = 0;
    int64_t memoryEntries = 0;
};

// What is on disk right now. Walks the cache directory, so this is for the Preferences
// dialog rather than anything on a decode path.
Stats stats();

// Deletes every entry. Returns how many bytes went.
int64_t clear();

} // namespace rawcache
} // namespace pixet
