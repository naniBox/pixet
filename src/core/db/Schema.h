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
};

// Classifies a filename by extension. Returns Format::Unknown for anything not in
// the supported image/video set - callers skip inserting those rows entirely
// rather than cluttering the index with sidecar/system files (.xmp, Thumbs.db, ...).
Format classifyFormat(const std::wstring &filename);
Kind kindForFormat(Format fmt);

} // namespace pixet
