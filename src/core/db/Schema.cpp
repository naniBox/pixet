#include "Schema.h"

#include <algorithm>
#include <cwctype>

namespace pixet {

namespace {
std::wstring lowerExt(const std::wstring &filename) {
    size_t dot = filename.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == filename.size() - 1) return {};
    std::wstring ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return ext;
}
} // namespace

Format classifyFormat(const std::wstring &filename) {
    std::wstring ext = lowerExt(filename);
    if (ext.empty()) return Format::Unknown;

    if (ext == L"jpg" || ext == L"jpeg" || ext == L"jpe") return Format::Jpeg;
    if (ext == L"png") return Format::Png;
    if (ext == L"heic" || ext == L"heif") return Format::Heic;
    if (ext == L"cr2" || ext == L"cr3" || ext == L"nef" || ext == L"arw" || ext == L"dng" || ext == L"orf" ||
        ext == L"rw2" || ext == L"raf" || ext == L"pef" || ext == L"srw")
        return Format::Raw;
    if (ext == L"tif" || ext == L"tiff") return Format::Tiff;
    if (ext == L"webp") return Format::Webp;
    if (ext == L"avif") return Format::Avif;
    if (ext == L"mp4" || ext == L"mov" || ext == L"m4v" || ext == L"avi" || ext == L"mkv" || ext == L"webm")
        return Format::Video;

    return Format::Unknown;
}

Kind kindForFormat(Format fmt) { return fmt == Format::Video ? Kind::Video : Kind::Image; }

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
