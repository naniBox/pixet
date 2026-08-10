#pragma once

#include <QString>

// User-configurable settings, backed by the same QSettings("pixet", "pixet") store
// MainWindow already uses for window/layout state - a single place for the actual
// *keys* and defaults, rather than every reader/writer duplicating both. See
// PreferencesDialog for the UI that edits these.
namespace prefs {

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
// false = launch customVideoPlayerPath() instead. Not yet consumed anywhere (video
// double-click still just opens the fullscreen viewer) - stored now so the player
// preference exists ahead of that follow-up.
bool useSystemVideoPlayer();
void setUseSystemVideoPlayer(bool useSystem);

QString customVideoPlayerPath();
void setCustomVideoPlayerPath(const QString &path);

} // namespace prefs
