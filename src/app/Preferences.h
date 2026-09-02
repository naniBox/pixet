#pragma once

#include <QList>
#include <QSettings>
#include <QString>
#include <QStringList>

#include "decode/DecodeLimits.h"

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

// The on-disk cache of decoded RAW images - see core/cache/RawCache.h for what it stores
// and why it isn't in the database. These three are the whole of its configuration.
//
// Long edge is the size entries are stored at, and is part of every cache key, so changing
// it ages the old size out rather than invalidating anything. Max bytes of 0 turns the
// cache off without deleting what's already there.
//
// kRawCacheSizePresets is what the drop-down offers; the stored value is a plain int, so a
// value from a future build (or a hand-edited ini) still loads rather than being snapped to
// the nearest preset.
constexpr int kRawCacheSizePresets[] = {1440, 1600, 1920, 2560, 3840};
constexpr int kDefaultRawCacheLongEdge = 2560;
constexpr qint64 kDefaultRawCacheMaxBytes = 2LL * 1024 * 1024 * 1024; // 2 GiB

int rawCacheLongEdge();
void setRawCacheLongEdge(int px);
qint64 rawCacheMaxBytes();
void setRawCacheMaxBytes(qint64 bytes);

// How much RAM the decoded tier in front of those files may hold. A decoded 2560px image
// is ~13MB, so the default is roughly 40 of them - a screenful plus room to scroll back
// through. 0 keeps the files but serves every hit from disk.
constexpr qint64 kDefaultRawCacheMemoryBytes = 512LL * 1024 * 1024;
qint64 rawCacheMemoryBytes();
void setRawCacheMemoryBytes(qint64 bytes);

// Absolute path of the cache directory, alongside index.db/thumbs.db in the same per-user
// app data folder - one place to look for everything pixet stores.
QString rawCacheDir();

// Ceilings on what a single image decode may cost - see core/decode/DecodeLimits.h for
// what they bound and the 14 GiB, 5-gigapixel TIFF that made them necessary. 0 means "no
// limit" for either, which is a real choice a user can make and not a broken value.
//
// Exposed as settings rather than hard-coded because the right answer depends on the
// machine and on what is in the folders being browsed: 500 megapixels is generous for a
// laptop browsing camera output and stingy for a workstation that genuinely works with
// gigapixel scans, and neither user should have to rebuild to say so.
constexpr qint64 kDefaultMaxDecodeFileBytes = pixet::decodelimits::kDefaultMaxFileBytes;
constexpr qint64 kDefaultMaxDecodeMegapixels = pixet::decodelimits::kDefaultMaxPixels / 1000000;

qint64 maxDecodeFileBytes();
void setMaxDecodeFileBytes(qint64 bytes);
// Stored in megapixels rather than raw pixels: it is what the UI shows, what anyone
// hand-editing the ini would want to type, and it keeps the stored number small enough to
// read at a glance.
int maxDecodeMegapixels();
void setMaxDecodeMegapixels(int megapixels);

// What the megapixel drop-down offers. Off (0) is deliberately in the list - a user who
// knows they browse gigapixel imagery should be able to say so in the UI rather than by
// editing the ini.
constexpr int kMaxDecodeMegapixelPresets[] = {50, 100, 250, 500, 1000, 2000, 0};

// Hands both limits to pixet_core, which has no prefs of its own - the same arrangement
// applyRawCacheSettings() uses, and called from the same places.
void applyDecodeLimits();

// Hands the four settings above to pixet_core, which has no access to prefs of its own
// (see core/cache/RawCache.h's configure()). Also what trims the cache after the budget
// is lowered, since configure() sweeps on the way in - so this is called on every OK from
// Preferences, not only when something changed.
//
// Lives here rather than in MainWindow.cpp, where it started, because the standalone
// viewer mode never builds a MainWindow and would otherwise open RAW files with the cache
// left unconfigured - i.e. re-demosaicing a file the full app had already cached, which is
// seconds per image and precisely the cost the cache exists to remove.
void applyRawCacheSettings();

// Whether the license has been accepted on this machine. Only ever consulted where no
// installer presented it - see LicenseDialog.h, which owns the whole of that decision and is
// the only thing that should be writing this.
bool licenseAccepted();
void setLicenseAccepted(bool accepted);

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

// The sizes offered in the UI. One list, shared by the status bar drop-down and the
// Preferences dialog so the two can't disagree - a free-form control in Preferences would be
// able to produce a value the drop-down has no entry for.
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
