#include "Schema.h"

#include <algorithm>
#include <cctype>

namespace pixet {

namespace {
// Extensions are always plain ASCII in practice (jpg, cr2, heic, ...) - casting to
// unsigned char before std::tolower() is the standard-safe way to call it on a UTF-8
// byte sequence (a raw signed char with the high bit set is undefined behavior for
// <cctype> functions otherwise); any actual multi-byte UTF-8 sequence here just won't
// match any of the ASCII comparisons below, correctly falling through to Unknown.
std::string lowerExt(const std::string &filename) {
    size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == filename.size() - 1) return {};
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}
} // namespace

Format classifyFormat(const std::string &filename) {
    std::string ext = lowerExt(filename);
    if (ext.empty()) return Format::Unknown;

    if (ext == "jpg" || ext == "jpeg" || ext == "jpe") return Format::Jpeg;
    if (ext == "png") return Format::Png;
    if (ext == "heic" || ext == "heif") return Format::Heic;
    if (ext == "cr2" || ext == "cr3" || ext == "nef" || ext == "arw" || ext == "dng" || ext == "orf" ||
        ext == "rw2" || ext == "raf" || ext == "pef" || ext == "srw")
        return Format::Raw;
    if (ext == "tif" || ext == "tiff") return Format::Tiff;
    if (ext == "webp") return Format::Webp;
    if (ext == "avif") return Format::Avif;
    if (ext == "mp4" || ext == "mov" || ext == "m4v" || ext == "avi" || ext == "mkv" || ext == "webm")
        return Format::Video;

    return Format::Unknown;
}

Kind kindForFormat(Format fmt) { return fmt == Format::Video ? Kind::Video : Kind::Image; }

const char *formatName(Format fmt) {
    switch (fmt) {
        case Format::Jpeg: return "JPEG";
        case Format::Png: return "PNG";
        case Format::Heic: return "HEIC";
        case Format::Raw: return "RAW";
        case Format::Tiff: return "TIFF";
        case Format::Webp: return "WebP";
        case Format::Avif: return "AVIF";
        case Format::Video: return "Video";
        default: return "Unknown";
    }
}

const char *kIndexSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS dirs(
    id INTEGER PRIMARY KEY,
    parent_id INTEGER,
    path TEXT UNIQUE NOT NULL,
    mtime INTEGER NOT NULL DEFAULT 0,
    scanned_at INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS files(
    id INTEGER PRIMARY KEY,
    dir_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    mtime INTEGER NOT NULL,
    size INTEGER NOT NULL,
    kind INTEGER NOT NULL DEFAULT 0,
    fmt INTEGER NOT NULL DEFAULT 0,
    width INTEGER,
    height INTEGER,
    orientation INTEGER NOT NULL DEFAULT 1,
    taken_at INTEGER,
    duration_ms INTEGER,
    thumb_id INTEGER,
    state INTEGER NOT NULL DEFAULT 0,
    UNIQUE(dir_id, name)
);

CREATE INDEX IF NOT EXISTS idx_files_dir_state ON files(dir_id, state);

CREATE TABLE IF NOT EXISTS claims(
    dir_id INTEGER PRIMARY KEY,
    owner TEXT NOT NULL,
    heartbeat INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS journal(
    seq INTEGER PRIMARY KEY AUTOINCREMENT,
    dir_id INTEGER NOT NULL,
    ts INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS bookmarks(
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    label TEXT,
    sort INTEGER NOT NULL DEFAULT 0
);
)SQL";

const char *kThumbsSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS thumbs.thumbs(
    id INTEGER PRIMARY KEY,
    w INTEGER NOT NULL,
    h INTEGER NOT NULL,
    fmt INTEGER NOT NULL DEFAULT 1,
    bytes BLOB NOT NULL
);
)SQL";

} // namespace pixet
