#pragma once

namespace pixet {

const char *version();

// Short hash of the commit this binary was built from, or "unknown" when the build tree
// wasn't a git checkout (a source export, or a machine without git). Captured at *build*
// time rather than configure time - see cmake/GitVersion.cmake for why that distinction
// matters.
const char *gitCommit();

// Whether tracked files had uncommitted modifications when the binary was built, i.e. the
// source is not exactly `gitCommit()`. Untracked files deliberately don't count.
bool gitDirty();

} // namespace pixet
