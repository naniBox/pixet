#include "PathUtil.h"

namespace pixet {

// Kept as one file with an internal #ifdef rather than split into _win/_mac variants,
// following util/ProcessId.cpp's precedent: this is a one-line platform difference, not a
// file's worth of platform logic.
char pathSeparator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

std::string joinPath(const std::string &dir, const std::string &name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    // Treat either separator as "already separated". A caller can reasonably hand us a dir
    // that came from Qt (which uses forward slashes even on Windows), and appending a
    // backslash to that would produce "C:/photos\file.jpg" - a path that works but doesn't
    // string-compare equal to what normalizePath() stores in dirs.path, which is the whole
    // failure mode this function exists to prevent.
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + pathSeparator() + name;
}

} // namespace pixet
