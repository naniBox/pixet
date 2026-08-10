#pragma once

#include <string>

namespace pixet {

// index.db: dirs/files/claims/journal/bookmarks - small, hot, stays in page cache.
extern const char *kIndexSchemaSql;

// thumbs.db (ATTACHed as `thumbs`): blobs only, kept out of index.db's hot pages.
extern const char *kThumbsSchemaSql;

// File.kind
enum class Kind : int { Image = 0, Video = 1 };

// File.fmt
enum class Format : int {
    Unknown = 0,
    Jpeg = 1,
    Png = 2,
    Heic = 3,
    Raw = 4,
    Tiff = 5,
    Webp = 6,
    Avif = 7,
    Video = 8,
};

// File.state
enum class FileState : int {
    New = 0,        // row exists, no thumbnail attempted yet
    Done = 1,       // thumbnail generated successfully
    Failed = 2,     // decode was attempted and failed (corrupt/unreadable file)
    Unsupported = 3,// format has no decoder yet (P1: everything but JPEG) - retry once support lands
    // RAW only: the current thumbnail was generated from the file's embedded preview
    // (fast, but camera-baked - a different rendering pipeline than the actual sensor
    // data, which can look meaningfully different: white balance, tone curve, crop).
    // Good enough to browse with immediately, same rationale as every other embedded-
    // preview tier, but a `pixet-index --render-raws` pass treats this the same as New
    // - eligible to be picked up and replaced with a real demosaic render. A plain
    // Done (1) RAW file already went through the full render (or --render-raws already
    // caught up to it) and won't be revisited.
    DoneNeedsRender = 4,
};

// Classifies a filename by extension. `filename` is UTF-8. Returns Format::Unknown for
// anything not in the supported image/video set - callers skip inserting those rows
// entirely rather than cluttering the index with sidecar/system files (.xmp,
// Thumbs.db, ...).
Format classifyFormat(const std::string &filename);
Kind kindForFormat(Format fmt);

// Short human-readable label for display (e.g. status bars) - "JPEG", "HEIC", ...
const char *formatName(Format fmt);

} // namespace pixet
