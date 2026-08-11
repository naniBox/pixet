#pragma once

#include <string>

namespace pixet {

// Resolves to an absolute, no-trailing-separator path in the platform's native
// separator style (backslash-normalized on Windows) - this is exactly the form
// DirWalker/Indexer store in dirs.path (UTF-8). Every caller that builds a path string
// to look up or pass to the indexer (pixet-index's argv, the GUI's QFileSystemModel
// selection) must funnel it through this first, or a separator-style mismatch will
// silently miss rows that are actually there. On macOS this additionally pins the
// Unicode normalization form to NFC - see util/Unicode.h.
std::string normalizePath(const std::string &path);

// Joins a directory path and a single entry name with the platform's separator, matching
// normalizePath()'s output style so that a joined path string-compares equal to what's
// stored in dirs.path. Tolerates `dir` already ending in either separator.
//
// Replaced a hardcoded '\\' join that lived in Indexer.cpp and, despite being in a file
// compiled on every platform, produced paths like "/Users/x/Photos\Sub" anywhere that
// wasn't Windows.
std::string joinPath(const std::string &dir, const std::string &name);

// The platform's path separator: '\\' on Windows, '/' elsewhere.
char pathSeparator();

} // namespace pixet
