#include "FileIO.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace pixet {

// Why this is a separate _mac.cpp rather than one portable std::ifstream implementation
// shared with Windows, which is otherwise the obvious simplification:
//
// std::ifstream's narrow-string constructor interprets the path in the platform's *native
// narrow encoding*, which on MSVC is the active ANSI codepage, not UTF-8. pixet_core's
// paths are UTF-8 by contract, so a shared std::ifstream implementation would silently
// fail to open any file with a non-ASCII name on Windows (a folder with an accented
// filename is enough to hit it). FileIO_win.cpp goes through CreateFileW for exactly that
// reason. Here on macOS the native narrow encoding *is* UTF-8, so the same code is right.
//
// (std::filesystem::path's char8_t constructor would sidestep the encoding problem and
// genuinely work on both - but unifying on it would also change Windows' sharing mode and
// read chunking, so it needs testing on both platforms before it's worth doing.)
//
// One behavioral difference from FileIO_win.cpp that is worth knowing rather than
// "fixing": CreateFileW is called there with FILE_SHARE_READ only, so a file another
// process holds open for writing fails to open and becomes ThumbTier::Failed. POSIX has no
// mandatory locking, so on macOS a file that's mid-copy reads successfully and gets
// thumbnailed from whatever partial bytes were present - and stays that way until its mtime
// or size changes. Not worth inventing locking for; noted so that a future "why is this one
// thumbnail corrupt" hunt doesn't start from zero.
bool readWholeFile(const std::string &path, std::vector<uint8_t> &out) {
    std::error_code ec;

    // Reject anything that isn't a regular file. CreateFileW gets this for free (it fails
    // on a directory without FILE_FLAG_BACKUP_SEMANTICS), but POSIX open(O_RDONLY) on a
    // directory *succeeds* and only fails later at read() with EISDIR - so without an
    // explicit check the two platforms would fail in different places for the same input.
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;

    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    out.resize(static_cast<size_t>(size));
    // An empty file is a success with an empty buffer. Matches the Windows version, where
    // the read loop simply never runs and `ok` stays true - callers rely on being able to
    // distinguish "read nothing" from "failed".
    if (size == 0) return true;

    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size));
    if (f.gcount() != static_cast<std::streamsize>(size)) {
        out.clear(); // same as Windows: no partial buffers escape on failure
        return false;
    }
    return true;
}

bool readFilePrefix(const std::string &path, size_t maxBytes, std::vector<uint8_t> &out) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) return false;

    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    size_t toRead = static_cast<size_t>(std::min<uintmax_t>(size, maxBytes));
    out.resize(toRead);
    if (toRead == 0) return true;

    f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(toRead));
    if (static_cast<size_t>(f.gcount()) != toRead) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace pixet
