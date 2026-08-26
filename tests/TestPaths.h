#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Returns a fresh (deleted-if-existing) temp path for `name`, so each test gets its
// own index.db/thumbs.db pair without interference between runs. UTF-8, matching
// every path/filename signature in pixet_core. std::filesystem::temp_directory_path()
// and the ASCII-only names actually used in tests (see below) make this portable as
// written - no platform-specific implementation needed here, unlike the WinAPI-backed
// production code in src/core/util/*_win.cpp.
inline std::string testTempPath(const std::string &name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "pixet_tests";
    std::filesystem::create_directories(dir);
    std::filesystem::path path = dir / name;
    std::error_code ec;
    std::filesystem::remove(path, ec); // fresh start; ignore "didn't exist"
    return path.string();
}

// A path guaranteed not to exist, for the "does this fail cleanly on a missing file?"
// tests every codec has. Deliberately absolute and implausible on both platforms rather
// than clever - a Windows-flavoured spelling like "Z:\\does\\not\\exist.<ext>" would work
// just as well on macOS, but reads as though the suite only runs on Windows.
inline std::string nonexistentPath(const std::string &extension) {
    return "/pixet-no-such-directory/nope." + extension;
}

// Writes `data` to `path`, overwriting anything already there - shared by every codec
// test that needs a scratch fixture file. Test paths are always ASCII (see above), so
// a plain narrow-string std::ofstream is safe here even on Windows (unlike production
// code, which must go through *_win.cpp's UTF-16 WinAPI calls for real filenames that
// may contain non-ASCII characters).
inline void writeTestFile(const std::string &path, const std::vector<uint8_t> &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(data.data()), (std::streamsize)data.size());
}
