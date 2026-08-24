#pragma once

namespace pixet {

const char *version();

// Short hash of the commit this binary was built from, or "unknown" when the build tree
// wasn't a git checkout (a source export, or a machine without git). Captured at *build*
// time rather than configure time - see cmake/GitVersion.cmake for why that distinction
// matters.
const char *gitCommit();

// Local wall-clock time this binary was built, as "YYYY-MM-DD HH:MM". Together with
// gitCommit() this is what makes a build identifiable: the hash says which source, this says
// which build of it - the distinction that matters whenever gitDirty() is true, since then the
// hash alone describes several different binaries. Costs a relink per build; see
// cmake/GitVersion.cmake for the measurement behind accepting that.
const char *buildTime();

// Whether tracked files had uncommitted modifications when the binary was built, i.e. the
// source is not exactly `gitCommit()`. Untracked files deliberately don't count.
bool gitDirty();

} // namespace pixet
