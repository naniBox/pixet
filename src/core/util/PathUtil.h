#pragma once

#include <string>

namespace pixet {

// Resolves to an absolute, no-trailing-separator path in the platform's native
// separator style (backslash-normalized on Windows) - this is exactly the form
// DirWalker/Indexer store in dirs.path (UTF-8). Every caller that builds a path string
// to look up or pass to the indexer (pixet-index's argv, the GUI's QFileSystemModel
// selection) must funnel it through this first, or a separator-style mismatch will
// silently miss rows that are actually there.
std::string normalizePath(const std::string &path);

} // namespace pixet
