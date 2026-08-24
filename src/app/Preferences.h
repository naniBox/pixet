#pragma once

#include <QList>
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

// Absolute path of the .ini settingsStore() reads and writes. Exposed so the About box can
// show it and offer to open its folder - the whole point of an explicit-path IniFormat store
// (see this header's own note on that choice) is that the location is knowable, and it is
// worth nothing if nothing ever tells the user where it is.
QString settingsFilePath();

// Grid thumbnail on-screen size (long edge, pixels) - both what ThumbGridView lays
// cells out at and what ThumbLoader decodes stored blobs down to for display.
// Cached after first read (see Preferences.cpp) since sizeHint()/paint() call this
// every frame for every visible cell - a QSettings-per-call would be needlessly
// expensive there. The cache is thread-safe (std::atomic) because ThumbLoader reads
// it from its own worker thread, not just the UI thread.
int thumbnailIconSize();
void setThumbnailIconSize(int px);
constexpr int kDefaultThumbnailIconSize = 180;
constexpr int kMinThumbnailIconSize = 80;
constexpr int kMaxThumbnailIconSize = 480;

// The sizes offered in the UI. One list, used by both the status bar drop-down and the
// Preferences dialog - they used to be a preset list and a free-form spinbox respectively,
// which meant Preferences could produce a value the drop-down had no entry for.
//
// kMin/kMaxThumbnailIconSize deliberately stay wider than this: they're the clamp applied to
// whatever is *read back* from the ini, so a value saved by an older build (the previous
// default was 150) still loads intact rather than being silently snapped to a preset.
constexpr int kThumbnailSizePresets[] = {180, 240, 300, 400, 480};

// The presets, plus `current` inserted in order if it isn't one of them. Keeps a value
// carried over from an older build visible and selectable instead of quietly rewriting the
// user's setting the first time a dialog is opened.
QList<int> thumbnailSizeChoices(int current);

// Grid cells reserve a landscape-shaped image area (height = this x thumbnailIconSize()),
// not a square one.
//
// A square box wastes a lot of room, because almost nothing photographic is square: a 3:2
// landscape shot drawn inside a square box leaves a third of the cell empty - a sixth above
// and a sixth below - and the effect scales with the icon size, so at 240px it's 40px of
// dead space top and bottom on every single tile. 3:4 fits 4:3 exactly and 3:2 nearly, which
// covers essentially every camera and phone. The cost is that *portrait* shots become
// narrower than the cell rather than filling it, which is the unavoidable trade in any grid
// with uniformly-sized tiles.
//
// Shared so ThumbGridView (which lays cells out) and ThumbLoader (which decodes blobs to fit
// them) can't disagree about the tile shape.
constexpr double kThumbnailTileAspect = 0.75; // image-area height / width
int thumbnailImageAreaHeightFor(int iconWidth);

// The long-edge size new thumbnails get generated/stored at (IndexOptions::
// targetLongEdge) - deliberately kept comfortably ahead of thumbnailIconSize() so
// displaying at the chosen on-screen size is always a downscale (sharp), never an
// upscale (blurry) of an old, smaller-stored thumbnail. Existing already-stored
// thumbnails aren't retroactively regenerated at the new size just because this
// preference changed - that's what "Force Re-thumbnail This Folder" is for.
int thumbnailTargetLongEdge();

// When true, browsing into a folder whose stored thumbnails are too small for the current
// display size regenerates them straight away, instead of just lighting the status bar's
// freshness dot red and waiting to be clicked.
//
// Off by default, and deliberately so: re-thumbnailing re-reads and re-decodes every
// original in the folder, so on a large or slow directory it's a visible cost that should be
// something the user opted into rather than something browsing silently triggers.
bool autoRethumbnail();
void setAutoRethumbnail(bool enabled);

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

// Whether ThumbGridView's hover-delay tooltip (cached + EXIF details) shows at all -
// see ThumbGridView::handleHoverMove(). Defaults on, matching the feature's existing
// out-of-the-box behavior; the View menu's "Show Hover Info" checkbox is the only
// writer. Read fresh each hover rather than cached (unlike thumbnailIconSize()) since
// it's not on any hot per-frame path.
bool hoverInfoEnabled();
void setHoverInfoEnabled(bool enabled);

// How many worker threads Indexer spreads Pass B (thumbnail generation) across - see
// IndexOptions::threadCount, which this feeds directly for on-demand navigation
// (FolderIndexer) and defaults pixet-index's own -j flag. 0 = auto-detect
// (std::thread::hardware_concurrency()). Deliberately NOT read by
// BackgroundReconciler/RawRenderer - those stay pinned to threadCount=1, matching
// their own "idle/low priority" design, so a user cranking this up doesn't
// accidentally make the background sweep compete harder for cores too.
int indexerThreadCount();
void setIndexerThreadCount(int count);

// Which field the thumbnail grid is sorted by, and whether reversed - see the View >
// Sort By menu and the sort button bar next to the path bar (MainWindow). Persisted
// as an int; an out-of-range stored value (an older/newer build using a different
// enum) falls back to Name rather than crashing on an invalid cast.
//
// TakenDate has no per-file value for a video, or a photo the EXIF pass hasn't
// reached yet - ThumbGridModel sorts those after every file that does have one,
// regardless of ascending/descending, rather than clumping them at whichever end
// "smallest unix timestamp" would put a 0.
enum class SortKey { Name, FileDate, TakenDate, Size };
SortKey gridSortKey();
void setGridSortKey(SortKey key);
bool gridSortDescending();
void setGridSortDescending(bool descending);

} // namespace prefs
