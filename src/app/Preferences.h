#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

// User-configurable settings, backed by the same on-disk store MainWindow also uses
// for window/layout state - a single place for the actual *keys* and defaults,
// rather than every reader/writer duplicating both. See PreferencesDialog for the UI
// that edits these.
namespace prefs {

// The QSettings store every persisted setting is backed by - this file's
// preferences, plus MainWindow's window/splitter geometry, last directory, and
// bookmarks-adjacent local state. One shared definition so every call site agrees on
// where it lives and what format it's in: an .ini file (QSettings::IniFormat) in the
// same per-user app data directory index.db/thumbs.db already live in (see
// AppPaths.h) - human-readable/hand-editable, and consistent across platforms rather
// than switching backends (registry on Windows, plist on macOS) per platform.
QSettings settingsStore();

// Grid thumbnail on-screen size (long edge, pixels) - both what ThumbGridView lays
// cells out at and what ThumbLoader decodes stored blobs down to for display.
// Cached after first read (see Preferences.cpp) since sizeHint()/paint() call this
// every frame for every visible cell - a QSettings-per-call would be needlessly
// expensive there. The cache is thread-safe (std::atomic) because ThumbLoader reads
// it from its own worker thread, not just the UI thread.
int thumbnailIconSize();
void setThumbnailIconSize(int px);
constexpr int kDefaultThumbnailIconSize = 150;
constexpr int kMinThumbnailIconSize = 80;
constexpr int kMaxThumbnailIconSize = 400;

// The long-edge size new thumbnails get generated/stored at (IndexOptions::
// targetLongEdge) - deliberately kept comfortably ahead of thumbnailIconSize() so
// displaying at the chosen on-screen size is always a downscale (sharp), never an
// upscale (blurry) of an old, smaller-stored thumbnail. Existing already-stored
// thumbnails aren't retroactively regenerated at the new size just because this
// preference changed - that's what "Force Re-thumbnail This Folder" is for.
int thumbnailTargetLongEdge();

// true = open videos with whatever the OS has associated with the file type;
// false = launch customVideoPlayerPath() instead - see
// MainWindow::onGridItemActivated().
bool useSystemVideoPlayer();
void setUseSystemVideoPlayer(bool useSystem);

QString customVideoPlayerPath();
void setCustomVideoPlayerPath(const QString &path);

// Recently-visited *folder* history for the path bar's dropdown - directories only,
// even though the bar itself can display a full file path (see
// MainWindow::onGridSelectionChanged()) when a specific selected file, e.g. one
// arrived at by pasting a full path, is shown. The only place entries get added is
// MainWindow::navigateTo(), whose `path` argument is always a resolved directory -
// a pasted/typed file path is resolved to its parent folder before it ever reaches
// there (see navigateToInput()), so this can't accidentally end up with a file path
// in it. Most-recent-first, deduplicated (revisiting an existing entry moves it to
// the front rather than creating a duplicate), capped at kMaxPathHistory entries.
QStringList pathHistory();
void addToPathHistory(const QString &dirPath);
void clearPathHistory();
constexpr int kMaxPathHistory = 20;

} // namespace prefs
