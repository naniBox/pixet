#pragma once

#include <QString>

// Handing a path back to the platform's own file manager.
namespace shellops {

// "Open in Explorer" on Windows, "Open in Finder" on macOS. Menu entries use this rather
// than hardcoding one name, so the wording matches whatever the user's file manager is
// actually called.
QString revealActionLabel();

// Opens `path` in the file manager. A directory is opened; a file has its containing
// directory opened with the file itself selected, which is the useful behaviour for
// "where is this photo?" and is not what simply opening the parent would do.
//
// Best-effort and silent on failure. There is no meaningful recovery - the file manager
// either exists or doesn't - and a modal complaining that Explorer wouldn't start would be
// less useful than nothing happening.
void revealInFileManager(const QString &path);

} // namespace shellops
