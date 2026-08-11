#include "PathUtil.h"

#include <filesystem>

#include "Unicode.h"

namespace pixet {

// Deliberately *lexical* normalization, not realpath(3).
//
// GetFullPathNameW - the Windows counterpart this has to agree with - makes a path
// absolute and collapses "." / ".." purely as string arithmetic: it never touches the
// filesystem, doesn't require the path to exist, doesn't resolve symlinks, and doesn't
// canonicalize case. realpath() differs on two of those: it resolves symlinks and it fails
// on a path that doesn't exist.
//
// Resolving symlinks would be actively wrong here. dirs.path is the folder's identity
// (TEXT UNIQUE), so a symlinked folder would get stored under its *target's* path, meaning
// the same folder gets a different identity depending on which route the user navigated in
// by. On macOS that's not hypothetical - /tmp, /var and /etc are all symlinks, and users
// symlink photo folders across volumes routinely. Keeping it lexical is what makes both
// platforms agree on what "the same folder" means.
std::string normalizePath(const std::string &path) {
    // GetFullPathNameW returns 0 for an empty input, which the Windows version turns into
    // "return the input unchanged". Match that rather than resolving "" to the CWD.
    if (path.empty()) return path;

    // Trim trailing separators up front, before absolute()/lexically_normal(), so we never
    // have to reason about whether the normal form of "/a/b/" keeps its trailing separator
    // or gains a trailing "." - implementations differ, and it isn't worth depending on.
    std::string trimmed = path;
    while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();

    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(trimmed, ec);
    if (ec) return path; // silent fallback to the unmodified input, same as Windows

    std::string s = abs.lexically_normal().string();
    // Keep a bare "/". The Windows guard is size() > 3 for exactly the same reason, there
    // to preserve "C:\".
    while (s.size() > 1 && s.back() == '/') s.pop_back();

    // PathUtil.h's contract makes this function the single funnel every path passes through
    // before it reaches the DB ("every caller that builds a path string to look up or pass
    // to the indexer must funnel it through this first"), which makes it exactly the right
    // place to pin the Unicode normalization form. See Unicode_mac.cpp.
    return toNfc(s);
}

} // namespace pixet
