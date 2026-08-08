#pragma once

#include <string>

namespace pixet {

// Resolves to an absolute, backslash-normalized, no-trailing-slash path - this is
// exactly the form DirWalker/Indexer store in dirs.path. Every caller that builds a
// path string to look up or pass to the indexer (pixet-index's argv, the GUI's
// QFileSystemModel selection) must funnel it through this first, or a separator-style
// mismatch will silently miss rows that are actually there.
std::wstring normalizePath(const std::wstring &path);

} // namespace pixet
