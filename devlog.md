# pixet devlog

Running log of decisions and status, kept because development happens across two
machines. Newest entry on top. Append, don't rewrite history.

---

## 2026-08-11 — desktop — Small QOL batch + settings moved out of the registry

**Fullscreen: Enter now closes too** (`Qt::Key_Return`/`Qt::Key_Enter` alongside
`Key_Escape` in `FullscreenViewer::keyPressEvent()`) - it's what opens a thumbnail
(the grid's `activated()` fires on it), so closing on it too makes it a real toggle
instead of an open-only key.

**Side panel toggle** - `T` (also View menu, "Toggle Side Panel") hides/shows
`leftPanel_` (tree, bookmarks, preview) so the grid can take the full window width
while hunting for a specific photo. `QSplitter` already gives a hidden child's space
to whatever's left visible, and `ThumbGridView` already recomputes its column count
on any resize - hiding/showing the one widget is the entire feature, no new layout
logic needed.

**Bookmarks pane got a title** - was just an unlabeled list next to the folder tree,
not obvious what it was on first look. Wrapped in a small container with a
"Bookmarks" label above it.

**Settings moved out of the registry to an .ini file.** New `prefs::settingsStore()`
(`Preferences.h/.cpp`) is now the one place that constructs `QSettings` - backed by
`QSettings::IniFormat` at `appDataDir() + "/pixet.ini"`, the same per-user app data
directory `index.db`/`thumbs.db` already live in, rather than
`QSettings::NativeFormat` (registry on Windows, plist on macOS). Chosen partly with
P5 in mind: one format behaves identically on both platforms rather than switching
backends per-OS, and it'll resolve to `~/Library/Application Support/pixet/pixet.ini`
on macOS once `AppPaths_mac.cpp` exists - not `~/.config` (that's the Linux/XDG
convention, not macOS's). All 6 `QSettings` construction sites (5 in `MainWindow.cpp`,
1 in `Preferences.cpp`) now go through it. Live-verified: `pixet.ini` lands at
`%LOCALAPPDATA%\pixet\pixet.ini` on launch. Not migrated from the old registry
values - existing window layout/last-directory/thumbnail-size settings reset to
defaults once this landed, which is expected and was flagged, not a bug.

Also chased down a build-blocking file lock during this session that turned out to be
`vsdbg.exe` (VS Code's C++ debugger backend) still holding `pixet.exe` open from a
debug session that never cleanly stopped, well after the debugged process itself had
already died (unresponsive, not just exited) - not a code issue, just worth
remembering if a link ever fails with `LNK1168` again after debugging in VS Code.

Build + full test suite (47/47) clean.

---

## 2026-08-10 — desktop — Preferences pane: video player, thumbnail size, re-index, and a database reset

New `Tools > Preferences...` dialog (`src/app/Preferences.h/.cpp` for storage,
`PreferencesDialog.h/.cpp` for the UI), backed by the same `QSettings("pixet",
"pixet")` store window/layout state already uses.

**Default video player** - system default or a custom override. Wired all the way
through: double-clicking a video in the grid now checks `prefs::useSystemVideoPlayer()`
and either `QDesktopServices::openUrl()`s it (also the fallback if "Custom" is
selected with no path ever set - silently doing nothing on activation would look like
the double-click didn't register) or `QProcess::startDetached()`s the configured
player. Non-video activation is unchanged (opens the fullscreen viewer).

**Grid thumbnail size** - a `QSpinBox` (80-400px). Live-applies on OK, not just on
next launch: `ThumbLoader` and `ThumbGridView`'s `kThumbIconSize` constant became
`prefs::thumbnailIconSize()` everywhere it was referenced (7 call sites across 3
files), and a new `ThumbGridView::applyIconSizeChange()` resets `lastFitWidth_`
before re-running `updateGridSize()` - without that reset, its jitter guard (see the
class's long comment on `updateGridSize()`) would see an unchanged viewport width and
wrongly skip recomputing, since it has no way to know the *cell* size is what
changed, not the width. `IndexOptions::targetLongEdge` (what new thumbnails actually
get generated/stored at) now derives from the same preference too
(`prefs::thumbnailTargetLongEdge()` = `max(320, iconSize*2)`, generous HiDPI
headroom) - threaded into `FolderIndexer`, `BackgroundReconciler`, and `RawRenderer`,
which previously all silently left this at the 320 struct default.

**"Re-index Known Folders"** - `BackgroundReconciler::triggerFullSweepNow()` (new
public slot): reloads its directory list and jumps its own timer to 0, same trick as
`RawRenderer::prioritize()`. Reuses the sweep's existing forceRescan-only behavior
(never touches new folders, never force-re-renders) rather than adding a second,
parallel implementation of "walk every known directory."

**"Reset Index"** (Danger Zone, red button, `Yes`/`No` confirmation defaulting to
`No`) - deletes every scanned folder/file/thumbnail row (`dirs`, `files`, `claims`,
`journal`, `thumbs.thumbs`) and `VACUUM`s both schemas to actually reclaim disk space,
deliberately leaving `bookmarks` alone (user-curated, not scan-derived - a rescan
can't regenerate them). Runs synchronously on `MainWindow::db_` under a wait cursor -
a rare, deliberate, already-confirmed action, not worth a background worker for -
then force-rescans whatever folder is on screen so the grid doesn't just go empty
until a manual Refresh.

Also: fullscreen viewer's Z key now toggles fit<->1:1 zoom, same as a click but
centered on the image's middle instead of a click point (`toggleZoomKeyboard()` in
`FullscreenViewer`) - same prefetch/upscale-fallback path either way.

Build + full test suite (47/47) clean. Not live-clicked through this round - the user
is testing the video-player wiring directly.

---

## 2026-08-10 — desktop — P5 prep: audited and fixed every macOS portability blocker reachable from this machine

Nothing to actually build/test for macOS from a Windows box, but everything that
*can* be verified here (still compiling and behaving correctly on Windows) is done -
next machine's P5 session should be `.app` bundling, signing, and writing the small
number of `_mac.cpp` files this now clearly calls out, not an open-ended audit.

**Audit first** (via a research-only subagent sweep of the whole tree): direct WinAPI
usage, `std::wstring`-as-path-type throughout `pixet_core` (an MSVC-only convention -
`std::ifstream`/`Database` accepting a `wstring` path doesn't even compile against
libstdc++/libc++), CMake's `WIN32` executable flag, `GetCurrentProcessId()`, and the
PowerShell-only `scripts/*.ps1`. Full punch list came back; worked through everything
except the PowerShell scripts (a macOS session writes its own `configure.sh`/`build.sh`
- nothing to "port" there, they're a different tool for the same job) and the actual
`_mac.cpp` implementations themselves (can't write *and verify* platform code with no
platform to run it on - a confidently-wrong guess is worse than an honest gap).

**The real work: migrated `pixet_core`'s path/filename type from `std::wstring` to
UTF-8 `std::string` everywhere** (`AppPaths`, `PathUtil`, `FileIO`, `DirWalker`,
`Database`'s ctor, `Schema::classifyFormat`, `Indexer`, `ThumbGenerator`,
`DisplayCodec`, `VideoCodec` - every path-carrying signature in the library). This was
the single biggest blocker and, unlike the WinAPI calls themselves, was fully
verifiable right here: `pixet_core`'s public API is now identical UTF-8 `std::string`
on every platform, with the four functions that must still cross into an actual WinAPI
call (`AppPaths`, `PathUtil`, `FileIO`, `DirWalker`) renamed to `*_win.cpp` and
converting to/from UTF-16 only right at that boundary (`StringUtil`'s `toUtf8`/
`toUtf16`, now scoped to exactly that - previously a general-purpose-looking helper,
misleadingly, since almost nothing actually needed UTF-16 outside these four files).
`CMakeLists.txt` now has an explicit `if(WIN32) ... else() message(FATAL_ERROR
"needs util/FileIO, util/PathUtil, ...") endif()` block instead of silently expecting
Windows - a macOS build fails immediately with a list of exactly what's missing,
rather than a wall of undefined-reference linker errors.

Net effect on the *existing* Windows code, not just macOS-readiness: a lot of
`QString::toStdWString()` → `pixet::toUtf8()` → SQL bind (and the reverse on the way
out) round-trips throughout `src/app/*.cpp` turned out to be pure overhead - `sel.
columnText()` was always UTF-8 (SQLite storage already was), `QString::toStdString()`/
`fromStdString()` already round-trip UTF-8 directly. Removing the wstring hop instead
of just retyping around it made `RawRenderer`, `BackgroundReconciler`, `FolderIndexer`,
`MainWindow`, `ThumbGridModel`, `PreviewDecoder`, and `FullscreenDecoder` all
genuinely simpler, not just "equally complex but portable."

**Smaller pieces**: `GetCurrentProcessId()` (4 call sites, used only to build claim-
owner strings) replaced with a new `util/ProcessId.h`/`.cpp` -
`#ifdef _WIN32` internally (`GetCurrentProcessId()` vs. POSIX `getpid()`) rather than a
separate file, since it's a one-line function, not a file's worth of platform logic.
`add_executable(pixet WIN32 ...)` - harmless on macOS (CMake ignores `WIN32` there) but
produces a bare Unix binary, not a real `.app` - now branches to `MACOSX_BUNDLE` under
`if(APPLE)`. Added `mac-debug`/`mac-release` `CMakePresets.json` entries mirroring the
existing ones, `condition`-gated to Darwin so they don't clutter `cmake --list-presets`
here; `CMAKE_PREFIX_PATH` points at the official-installer Qt default location
(`~/Qt/6.8.3/macos`) per the plan's own stated install method - genuinely untested,
flagged as such, first thing to check if configure fails on the mac.

**Tests**: `TestPaths.h` rewritten on `std::filesystem` (`temp_directory_path()`,
`create_directories`, `remove`) instead of raw `GetTempPathW`/`CreateDirectoryW` -
fully portable as written, since test fixture paths are always plain ASCII (ASCII is a
subset of both UTF-8 and the Windows ANSI codepage, so the usual UTF-8-vs-codepage
`std::ifstream(string)` pitfall that makes production code need the `_win.cpp` WinAPI
route doesn't apply to test scaffolding at all). Consolidated four copy-pasted
`writeFile()` helpers (one per codec test file, each doing its own raw `CreateFileW`)
into one `writeTestFile()` in `TestPaths.h`. Dropped `<Windows.h>` from five test files
entirely.

**Deliberately not attempted**: the `_mac.cpp` implementations themselves
(`AppPaths`/`PathUtil`/`FileIO`/`DirWalker`/`StringUtil`'s Windows-only helpers have no
POSIX equivalents written yet - `FileIO`'s in particular could likely just be a single
portable `std::ifstream`-based implementation shared by both platforms once written,
no `_mac.cpp` needed there at all, but that's a call better made *with* a mac to build
against). `.app` bundling/signing/`macdeployqt`. Actually running anything on macOS.

**Live-verified no Windows regression** from the `wstring`→UTF-8 migration - the
highest-risk part, since a mishandled encoding boundary could have silently mangled
non-ASCII filenames. Full test suite (47/47) clean, then the real library:
navigated to a folder with an accented filename for real
(`Photos\Models\mdl.2011-05-15_Noémie\...Noémie_DMO6831.jpg`) - path bar, tree,
status bar, and thumbnails all render the "é" correctly, nothing mangled.

---

## 2026-08-10 — desktop — Fixed "no preview" for video: DisplayCodec never handled it, and 22 real videos were stuck on stale state from before video support existed

User: "no preview" persists for movies. Two independent bugs stacked on the same files.

**Bug 1 - `decodeForDisplay()` explicitly excluded Video.** It bailed on `fmt ==
Format::Video` before ever reaching a switch case, so the side preview pane (and,
incidentally, the fullscreen viewer, gated by the same `hasFullscreenDecoder()`) never
even tried. Added a Video branch calling `decodeVideoPosterFrame()` directly (same
function `ThumbGenerator::generateVideoThumb()` already uses for the grid thumbnail) -
handled *before* the generic `readWholeFile()` call the other formats share, since video
files are too large to read wholesale just to seek a few seconds in (see
`decodeVideoPosterFrame`'s own doc comment). `targetLongEdge > 0` downscales via
`resizeBoxDownscale()`; `<= 0` (fullscreen zoom) keeps native resolution - there's no
embedded-preview tier to prefer for video, it's already just the one extracted frame.
Also dropped Video from `FullscreenViewer::hasFullscreenDecoder()`'s exclusion list,
since it's now got exactly as much of a decoder as any image format - fullscreen used to
just blow up the small cached grid thumbnail for video, same blurry-thumbnail problem as
today's earlier JPEG fix, for the same reason.

**Bug 2 - real videos found stuck at `FileState::Unsupported`.** Queried the real
`index.db` for the folder the user was looking at
(`InstantUpload\Camera\2026\07`) - all 22 real `.mp4` files (Pixel phone recordings) sat
at `state=3` (Unsupported), `width/height/thumb_id` all NULL. Confirmed via a scratch
copy of one file through `pixet-index` that today's decoder handles it fine (`1 decoded,
0 unsupported, 0 failed`) - so this wasn't a current decode failure, it was the exact
same shape of bug as the RAW `DoneNeedsRender` migration above: these files were scanned
by a build from before video support existed (or worked), and Pass B only ever looks at
`state=New` rows, so `Unsupported` is a dead end forever once hit, regardless of
whether the format gains real support later. Checked whether this could still happen
today for a genuinely-unsupported format: `ThumbGenerator`'s dispatch handles every
`Format` value from 1-8 explicitly now (Video before the switch, the other seven in it),
and `Format::Unknown` files never even get a `files` row (Indexer's Pass A skips them) -
so `FileState::Unsupported` can no longer be produced by anything reachable today,
making it safe to treat as unconditionally stale. Extended the same migration mechanism
(`Database::runMigrations()`, now version 2): `UPDATE files SET state=New WHERE
state=Unsupported`, format-agnostic - covers this class of bug for good, not just video.

Live-verified against the real folder: after the migration, all 22 mp4s got real poster
frames in the grid (previously blank), correct dimensions in the status bar (previously
blank, e.g. 3840×2160), a working side preview (confirmed by alternating with a visually
distinct photo first, since two of the videos happen to look nearly identical to their
JPG neighbors from the same sitting - the preview genuinely tracks the selection), and a
working fullscreen view. Build + full test suite (47/47) clean.

---

## 2026-08-10 — desktop — Fullscreen zoom prefetch: native decode starts before the click, not on it

User: zooming a RAW to 1:1 in fullscreen shows the fit view instantly, but the sharp
native render only appears after a ~1s pause. Not a bug exactly - `requestZoom()` was
always purely reactive (fired only by the actual click/scroll), and a full RAW demosaic
genuinely takes about that long even at the fastest quality setting already in use. But
the current row is known the moment it's shown, well before any click - no reason not to
have that decode already running by the time the user gets there.

Added a debounced prefetch (`FullscreenViewer::prefetchZoom()`, `kZoomPrefetchDelayMs =
400`): `showRow()` restarts a singleshot timer on every navigation, so holding next/prev
to skim a folder never fires it for images only passed through - only the row the user
actually settles on for 400ms+ gets its native decode requested ahead of time, via the
same `requestZoom()` used by the real click (its existing "already cached / already in
flight" guards make this safe to call speculatively with no special-casing). Runs on a
**second** `FullscreenDecoder` instance/thread (`zoomDecoder_`), separate from the one
handling fit-mode prefetch (`decoder_`) - otherwise a slow prefetched zoom decode could
sit in front of the fit-mode decode next/prev actually needs to feel instant, on the same
single-threaded queue. Both funnel into the same `onDecoded()`, which already dispatches
by request id rather than by source.

Live-verified: opened fullscreen on a real ARW, waited 2s (past the debounce plus decode
time), clicked to zoom, screenshotted in the same call immediately after the click - fully
sharp native-resolution detail already on screen, no visible pause. Build + full test
suite (47/47) clean.

---

## 2026-08-10 — desktop — Fixed a regression from the decodeForDisplay() unification: fullscreen JPG showed thumbnail quality until zoomed

User: fullscreening a JPG showed only thumbnail quality, sharpening up only once zoomed.
Introduced by the `decodeForDisplay()` unification two entries down - the old
`FullscreenDecoder`/`PreviewDecoder` JPEG-only code always decoded the full file with
`decodeJpeg()`'s scaled-DCT path directly; `decodeForDisplay()`'s JPEG case, copying the
"prefer the embedded preview" pattern that's correct for RAW/HEIC (their embedded
previews are typically large, near-full-size), added a branch that decodes the file's
*EXIF* thumbnail first instead - a completely different thing, conventionally tiny
(~160×120), meant for OS file-browser previews, not real viewing. Fine for the grid's own
~320px thumbnails (a 160px source is a reasonable match), badly wrong for fullscreen fit
mode where the target is the whole screen (1920px+): the tiny EXIF thumbnail decoded,
couldn't be upscaled by a downscale-only DCT step, and got stretched to fill the window -
exactly "looks like the thumbnail." Fullscreen zoom mode (`targetLongEdge<=0`) was never
affected - it already skips the embedded-preview branch entirely.

Fixed with a size check in `DisplayCodec.cpp` shared across all three embedded-preview
branches (JPEG's EXIF thumb, RAW's embedded preview, HEIC's embedded preview): if the
decoded preview comes up smaller than the caller's `targetLongEdge`, discard it and fall
through to the real decode instead of accepting whatever came back. Cheap when the
preview was already big enough (the common RAW/HEIC case, and JPEG at grid-thumbnail-size
targets), correct when it wasn't (JPEG at fullscreen-size targets). Also applies to the
side preview pane, which had the identical latent bug for plain JPEGs, just not yet hit
in testing.

Live-verified: fullscreened a real camera JPEG immediately after the fix - sharp on
first frame, no zoom needed. Full test suite (47/47) still clean.

---

## 2026-08-10 — desktop — Fixed three real bugs found via the user's own library, plus folder-navigation prioritization

User reported two problems from actual use, against a real folder
(`_EDITING\2026\2026-03\2026-03-01`, 32 ARW files): RAW files there weren't getting
auto-rendered even on repeat visits, and clicking a RAW showed "no preview" in the side
pane. Also asked for the background updater to prioritize whatever folder is currently
on screen, and to update the grid progressively rather than after the whole folder
finishes, so a 600-RAW folder doesn't mean minutes of waiting before anything shows.

**Root cause #1 - stuck-forever migration gap.** Queried the real `index.db` directly
(read-only, via Python - Bash-tool sqlite queries against Windows paths silently fail,
use PowerShell instead) and found all 32 files sitting at plain `Done`, not
`DoneNeedsRender`. That folder had been scanned by a build of the app from *before*
`DoneNeedsRender` existed (see the entry below), so it got the old undifferentiated
`Done` treatment - permanently invisible to `RawRenderer`'s state-based query, forever,
for every RAW file indexed before that code landed. Fixed with a new
`Database::runMigrations()`, gated by `PRAGMA user_version`: reclassifies every
`(fmt=Raw, state=Done)` row back to `DoneNeedsRender` once, idempotent (re-rendering an
already-fully-rendered file just wastes some CPU once, not incorrect). This is the
general mechanism for "the meaning of a stored value changed" fixes going forward, not
just this one bug.

**Root cause #2 - preview pane was JPEG-only.** `PreviewDecoder::doDecode()` only ever
handled `Format::Jpeg` - the same limitation the fullscreen viewer had, flagged as an
unaddressed scope note two entries down ("worth a follow-up if fullscreen full-res
viewing of RAW/HEIC/etc. turns out to matter in practice" - turned out to matter sooner,
for the *side* pane instead). Fixed by extracting a shared `decodeForDisplay()`
(`src/core/decode/DisplayCodec.h/.cpp`) covering every format, used by both
`PreviewDecoder` and `FullscreenDecoder` now. One signal, two behaviors:
`targetLongEdge > 0` (side pane, fullscreen fit-to-screen) prefers the fast embedded
preview; `<= 0` (fullscreen zoom-to-native) skips straight to a full decode, since an
embedded preview is never true native resolution. `FullscreenViewer`'s two
`fmt != Jpeg → bail` gates replaced with a real per-format check.

**Root cause #3 - found live, not reported: sideways RAW previews.** Screenshotting the
fix above showed the preview pane rendering the right file, oriented 90° off from the
grid thumbnail of the same file. `decodeRaw()` (full demosaic) gets orientation applied
automatically by LibRaw's own pipeline; `decodeRawThumb()` (embedded preview) decodes the
embedded JPEG's raw bytes directly and never got any rotation applied at all - same class
of bug as JPEG needing its own explicit `applyOrientation()` call, just missed for RAW's
fast path. Confirmed LibRaw's `sizes.flip` encoding against dcraw's own
`t_flip = "50132467"[exifOrientation & 7]` table in `tiff.cpp` (the inverse of that table
gives `flip → EXIF orientation`), added `exifOrientationForFlip()` and an
`applyOrientation()` call in `decodeRawThumb()`. This bug wasn't limited to the new
preview pane - `ThumbGenerator::generateRawThumb()` calls the same `decodeRawThumb()`,
so it affected the *stored grid thumbnails* for any RAW file still on its embedded-preview
tier. Verified via `pixet-index` against a scratch copy of one real ARW (kept out of the
GUI/RawRenderer entirely to avoid racing the background upgrade): extracted the resulting
thumbnail blob straight from `thumbs.db` and viewed it - upright, correct.

**Prioritization + progressive updates.** `RawRenderer::prioritize(QString path)` (new
public slot) sets a `priorityDir_` checked before the normal "any pending directory"
query, and restarts its timer at 0ms so a folder switch doesn't sit out whatever's left
of the up-to-60s idle wait. `MainWindow::navigateTo()` now emits
`requestRawRenderPriority` alongside the existing `requestIndex`. Separately,
`Indexer.cpp`'s Pass B batch-commit loop (`kBatchSize=64`) now flushes after every single
item when it was a forced RAW render (`batch.size() >= kBatchSize || forceFullRender`),
not just every 64 - otherwise a large folder would sit fully silent until 64 slow
demosaic decodes finished. `RawRenderer`'s `onProgress` callback now emits
`directoryChanged` per-file instead of once at the very end.

**Live-verified against the exact reported folder and file.** Reset all 32 ARW rows back
to pending (`thumb_id=NULL, state=0`, old blobs deleted) to recreate a real backlog, then
relaunched pointed at that folder: grid and correctly-oriented preview populated in under
2s (embedded-preview pass), status bar read "32 RAW: 0 rendered, 32 preview"; by ~30s
later, fully caught up to "32 RAW: 32 rendered, 0 preview" with zero user interaction -
both the auto-render and the no-preview bugs confirmed fixed on the user's own data, not
just a fixture. (Note: build requires `scripts/build.ps1`, not a bare `cmake --build` - it
enters the MSVC dev shell itself; running the built `pixet.exe` directly also needs
`C:\Qt\6.8.3\msvc2022_64\bin` on `PATH`, not baked into the exe.)

Build + full test suite (47/47) clean.

---

## 2026-08-10 — desktop — Automatic background RAW rendering + status bar counts

Follow-up to `pixet-index --render-raws` below: that only reached RAW files through an
explicit CLI invocation. Added the GUI-automatic equivalent, plus a status bar
indicator the user specifically asked for ("how many have proper thumbnails and how
many have embedded").

**Real bug found and fixed first**: Pass B's success path (`Indexer.cpp`) inserted a
new `thumbs.thumbs` row and repointed `files.thumb_id` at it, but never deleted the
*old* blob a file already had - harmless before today (a state=New file's thumb_id was
always already NULL), but `FileState::DoneNeedsRender` made "regenerate a thumbnail for
a file that already has one" a real, common case for the first time, and every
`--render-raws` upgrade would have silently leaked its old embedded-preview blob in
`thumbs.db` forever. Fixed by carrying the pre-existing `thumb_id` through
`PendingThumb` and deleting it once the new one is committed. Live-verified against a
real ARW: old thumb_id present as a real blob before, gone after, new thumb_id present
- both confirmed via direct `thumbs.thumbs` queries in a throwaway temp DB.

**`RawRenderer`** (`src/app/RawRenderer.h/.cpp`): same shape as `BackgroundReconciler`
(own thread, `QThread::LowestPriority`, `Indexer` wrapper) but queries directly for
"any directory with a `DoneNeedsRender` RAW file" rather than rotating through every
known directory - a newly-preview-only RAW file doesn't have to wait for an unrelated,
much slower whole-library drift-detection cycle to reach that directory. Runs
`IndexOptions::renderRaws` one directory at a time (Indexer's own Pass B batching
handles every pending RAW file in that directory together), paced more gently than
BackgroundReconciler (4s vs 1.5s between directories - a real demosaic decode is
genuinely expensive, unlike a cheap mtime check), with a 60s idle retry when there's
nothing pending. Shares `MainWindow::onBackgroundDirectoryChanged` with
BackgroundReconciler's own signal - both mean "a directory's thumbnails changed,
refresh the grid if it's on screen," so one slot handles both.

**Status bar**: `ThumbGridModel` gained `rawRenderedCount()`/`rawPreviewCount()`,
computed in `setDirectory()` like the existing `imageCount()`/`videoCount()`
aggregates, but *also* kept live in `refreshThumbStates()` - unlike those two, a RAW
file's state can flip from `DoneNeedsRender` to `Done` without a full model reset
(exactly what a background render finishing looks like), so the counts need to track
that incremental path too. New `rawStatusLabel_` shows "N RAW: X rendered, Y preview"
next to the existing folder-stats label, blank when the folder has no RAW files.

**Live-verified together, real end-to-end**: pointed the app at a real folder with 7
ARW files, launched fresh. Status bar read "7 RAW: 0 rendered, 7 preview" immediately
(fast embedded-preview pass already done by normal indexing) - 30 seconds later, with
zero user interaction, "7 RAW: 7 rendered, 0 preview" (RawRenderer had found and fully
rendered all 7 in the background, and the status bar picked up the change live).
Screenshots confirm both states.

Build + full test suite (47/47) clean.

---

## 2026-08-10 — desktop — Two-pass RAW rendering (`pixet-index --render-raws`)

Follow-up to P4's RAW support below: the user shoots a lot of RAW (ARW especially) and
wants more than the fast embedded-preview thumbnail long-term - the embedded preview
is camera-baked (its own white balance/tone curve/crop), a genuinely different
rendering than what a real demosaic of the sensor data produces, so "good enough to
browse with" shouldn't be the permanent end state for a RAW-heavy library.

Added `FileState::DoneNeedsRender` (4) - a RAW file whose current thumbnail came from
`ThumbTier::EmbeddedPreview` lands here instead of the ordinary `Done` (1); everything
else (RAW that already got a full render, or any other format's tiers) is unaffected.
No schema migration needed - `state` was already a plain unconstrained INTEGER column.

`generateThumb()` gained a `forceFullRender` parameter (RAW-only; every other format's
codec function ignores it) that skips straight past `decodeRawThumb()` to `decodeRaw()`
- the full LibRaw demosaic pipeline already built for P4's fallback path, just no
longer conditional on the embedded preview failing.

`IndexOptions::renderRaws` threads this through `Indexer`: Pass B's pending-work query
picks up `state=DoneNeedsRender` rows too (in addition to the usual `state=New`) when
set, and every RAW file touched during that run - New or DoneNeedsRender - gets
`forceFullRender=true`. Deliberately a separate opt-in pass rather than something Pass
B does automatically or bundled into `forceRethumbnail`: normal indexing needs to stay
fast (that's the entire point of trying the embedded preview first), so upgrading to
real renders has to be something the user asks for, not a silent cost added to every
scan. Exposed as `pixet-index --render-raws`; safe to re-run since it's a no-op once
everything's already `Done`.

**Live-verified end-to-end against a real Sony ARW file** (copied into a throwaway
temp directory + temp `Database`, not the shared app cache, so the test never touched
the user's real index): pass 1 (normal index) landed the file on `DoneNeedsRender`
with 1 embedded-preview hit; pass 2 (`--render-raws`) upgraded it to `Done` with 1
full-decode hit, zero embedded-preview hits; pass 3 (normal index again) did nothing
at all (0/0), confirming the ladder actually terminates instead of re-rendering every
run. Exact trace is in this session's history, not committed as a permanent test
(depends on an absolute path to a real file on this machine) - only the lighter,
still-portable parameter-threading check made it into `test_rawcodec.cpp`.

**Not done this round**: this only reaches RAW files through `pixet-index
--render-raws`, an explicit CLI invocation - there's no automatic background upgrade
path yet (e.g. extending `BackgroundReconciler` to pick up `DoneNeedsRender` rows at
low priority while the GUI app is just sitting there). Worth adding if periodically
remembering to run the CLI pass turns out to be friction in practice; deferred for now
since the user asked for "commit this work first."

Build + full test suite (47/47) clean.

---

## 2026-08-10 — desktop — P4 format breadth: PNG, RAW, video, TIFF, WebP, AVIF, HEIF

Every format the plan scoped for P4 now has a real decoder, in the user's stated
priority order (PNG > RAW > video > TIFF > WebP > AVIF > HEIF, reflecting that their
phone shoots JPEG rather than HEIC). Before today only JPEG was implemented;
`src/core/decode/` had exactly one codec.

**Refactor first**: pulled `RgbImage`, `resizeBoxDownscale()`, `applyOrientation()`,
and `encodeJpeg()` out of `JpegCodec.h/.cpp` into a new shared `RgbImage.h/.cpp` -
every new codec needs these (every format ends up re-encoded to JPEG for storage
regardless of source), and importing them from something literally named
`JpegCodec.h` stopped making sense once JPEG was one format among many.
`ThumbGenerator.cpp` also got restructured from a single JPEG-only function into a
`switch`-based dispatcher with one `generateXxxThumb()` per format, sharing the same
read-buffer-then-decode-then-resize-then-reencode shape JPEG already established.

**Per-format notes**:
- **PNG** (`PngCodec`, libpng's simplified API) - no scaled decode like JPEG's
  scaled-DCT, always native-resolution then box-downscale. No orientation extraction
  (PNG's eXIf chunk exists but is vanishingly rare in practice - screenshots/graphics/
  exports don't carry it).
- **RAW** (`RawCodec`, LibRaw, `libraw::raw_r` - the thread-safe build, since
  ThumbLoader/FolderIndexer/BackgroundReconciler can all decode concurrently on
  separate threads within one process) - embedded-preview-first ladder just like
  JPEG's EXIF thumbnail (LibRaw's `unpack_thumb()`/`dcraw_make_mem_thumb()`), full
  demosaic via `dcraw_process()` as fallback. LibRaw auto-applies the file's own
  orientation to its output for both tiers, so - unlike JPEG - there's no separate
  `applyOrientation()` step. **Live-verified against a real Pixel-phone DNG**:
  embedded preview extracted correctly, portrait orientation correctly reported
  (3072x4080, matching the phone's actual rotated capture).
- **Video poster frames** (`VideoCodec`, FFmpeg avformat/avcodec/swscale) - the one
  codec that takes a file *path* rather than an in-memory buffer, deliberately
  breaking the pattern every image codec follows: video files can be gigabytes, and
  reading one wholesale just to grab a frame a few seconds in would be wasteful when
  FFmpeg's own demuxer already seeks efficiently from disk. Seeks to
  min(3s, 10% of duration), decodes the nearest keyframe, converts via swscale. Also
  corrects for the video's own display-matrix rotation metadata (common for
  phone-recorded portrait video, stored landscape with a rotate flag) by mapping
  FFmpeg's counter-clockwise rotation value onto `applyOrientation()`'s existing
  EXIF-style orientation constants rather than writing a separate transform. Hit two
  API-vintage issues building against this vcpkg build's FFmpeg (`avformat-63`,
  notably newer than expected): `av_stream_get_side_data()` no longer exists, replaced
  with `av_packet_side_data_get()` over `codecpar->coded_side_data`. **Live-verified
  against a real Pixel-phone portrait MP4** - poster frame extracted, dumped to a file,
  and visually confirmed upright (not sideways or mirrored), which also validated the
  rotation-sign convention empirically rather than trusting the docs' wording alone.
- **TIFF** (`TiffCodec`, libtiff) - `TIFFReadRGBAImageOriented(..., ORIENTATION_TOPLEFT,
  0)` handles the format's wide variety of encodings (bit depth, palette/CMYK/
  grayscale, most compressions) in one call and auto-corrects for the file's own
  `TIFFTAG_ORIENTATION` (same 1-8 encoding EXIF borrowed from TIFF). libtiff has no
  built-in memory-buffer open the way libjpeg/libpng do, so `TIFFClientOpen()` needed
  five small read/write/seek/close/size callbacks wrapping the in-memory buffer.
- **WebP** (`WebpCodec`, libwebp's simple `WebPDecodeRGB`/`WebPGetInfo`) - simplest
  codec of the seven, no memory-buffer plumbing needed at all. No orientation
  extraction (same rarity rationale as PNG).
- **AVIF** (`AvifCodec`, libavif) - no orientation extraction (irot/imir transforms
  exist but are uncommon outside camera-native capture, and AVIF in the wild is
  mostly web-optimized). **Testing gap**: this vcpkg build's libavif links a
  decode-only AV1 backend - `avifEncoderWrite()` came back empty every time,
  confirmed empirically (no AV1 encoder buildtree exists under vcpkg/buildtrees at
  all). Round-trip fixture tests (the pattern used for PNG/TIFF/WebP) had to be
  dropped in favor of garbage-data + format-gating tests only, same shape as RAW/
  video's tests (which never had an encode path to begin with). No real AVIF sample
  exists on the dev machine either, so unlike RAW/video there's no live-verification
  fallback - lowest-confidence codec of the seven as a result, consistent with the
  user's own prioritization (their phone doesn't produce AVIF).
- **HEIF** (`HeifCodec`, libheif + libde265 decode backend) - same embedded-preview-
  first ladder as RAW (`heif_image_handle_get_thumbnail()` before falling back to the
  primary image), libheif applies container orientation automatically like RAW/TIFF.
  Same testing gap as AVIF and for the same reason: `vcpkg.json` declares libheif with
  `default-features: false` specifically to skip x265 (the HEVC encoder, an
  unnecessary heavy dependency for read-only thumbnailing) - confirmed via no x265
  buildtree existing - so no encode-based fixture was possible here either. Lowest
  priority of the seven per the user's own ranking (their phone shoots JPEG, not
  HEIC), and it shows in the test confidence: garbage-data + gating only, no
  real-file verification.

**Retroactive pickup for already-indexed libraries**: files scanned before today got
`state=Unsupported` (3) for every one of these formats, and Pass B's pending-work
query (`WHERE state=0`) won't touch them on a normal Refresh. Checked
`Indexer.cpp:133` - `forceRethumbnail` unconditionally resets `state=0` for every
existing file regardless of its *current* state, which already includes Unsupported.
So the "Force Re-thumbnail This Folder" action added earlier this session (originally
built for the width/height bugfix) is *also* exactly the right tool to retroactively
pick up all seven newly-supported formats in an existing library - no new code
needed, it already composes correctly.

**Scope note**: `src/app/FullscreenDecoder.cpp` (the fullscreen viewer's full-
resolution decoder, separate from `ThumbGenerator`) is still JPEG-only - it falls back
to the grid's cached thumbnail for every other format, same graceful-degradation
behavior as before today. Not addressed in this pass, since the ask was specifically
about thumbnail/indexing coverage (P4's actual scope); worth a follow-up if fullscreen
full-resolution viewing of RAW/HEIC/etc. turns out to matter in practice.

Build + full test suite (46/46) clean throughout, one format at a time.

---

## 2026-08-09 — desktop — Grid column-fit bug, round five (dual relayout systems)

User report after round three's fix: manual drag-resize can still jump straight from
4-wide to 1-wide, skipping 2 and 3 - i.e. the empirical backoff in
`ThumbGridView::updateGridSize()` is backing off much further than it should within a
single settle, not just landing on a slightly-too-low count.

Found a real structural cause: `grid_->setResizeMode(QListView::Adjust)`
(`MainWindow.cpp`) tells QListView to relayout items **automatically on every single
resizeEvent**, using whatever `gridSize()` is set *at that instant* - which, mid-drag,
is last settle's (now stale) column count, since `updateGridSize()` deliberately
debounces 50ms behind. So during a drag, Qt's own automatic relayout and our explicit
one were two independent, uncoordinated systems both repositioning items, on different
schedules, against different assumptions about the current size - a plausible source of
exactly this kind of over-aggressive backoff if Qt's own pass left the view in some
partially-relaid-out state before our `doItemsLayout()` runs. Changed to
`QListView::Fixed` - items only reposition when something explicitly asks
(`doItemsLayout()`, which `updateGridSize()` already calls itself), removing the
redundant/conflicting automatic pass rather than trying to out-race it.

Verified via build + full test suite (15/15, clean) and extensive live drag-resize
testing against a real 400-file folder: smooth step sequences, deliberate `1600<->850`
big single jumps (restore-down/flick-style), and two different jittery random-walk
sequences (one general-range, one deliberately centered in the width band where ideal
columns is 4-5) via `SetWindowPos` from an external PowerShell process, `PW_RENDERFULLCONTENT`
`PrintWindow` capture, and title-bar debug logging identical to earlier rounds.
**Honest gap**: none of these synthetic runs reproduced the exact "skips 2 & 3" pattern
either before or after the fix - every observed transition backed off one column at a
time and converged correctly. The `QListView::Adjust`/`Fixed` fix is real and worth
keeping regardless (two uncoordinated relayout systems racing is a legitimate bug on
its own terms), but it isn't *confirmed* to be the specific mechanism behind what was
reported, since it couldn't be reproduced on demand to verify against. Asked the user
to retest and report back with more specific repro details (exact action - drag vs.
maximize vs. restore, single vs. multi-monitor, right after opening a folder or well
after) if it still happens, to narrow down what a synthetic `SetWindowPos` stream isn't
capturing about a real OS-level drag.

Also note: `HKCU\Software\pixet\pixet\lastDirectory` was temporarily overwritten during
this session's testing (pointed at a 400-file real folder to get a meaningful repro
folder, rather than the ~3-5 file folders used in earlier rounds) - pixet will just
overwrite it again on next real navigation, but worth knowing why it might open
somewhere unexpected once.

---

## 2026-08-09 — desktop — Grid column-fit bug, round three (manual drag-resize, root cause)

Round two's fix (restart on detected width drift) turned out to only patch one
symptom, not the actual mechanism - user report: manual drag-resize still collapsed
4-wide to 1-wide, and the settled state after a drag could show correctly-proportioned
columns with a large blank gap on the right (fewer columns than the width could hold).

Reproduced with a synthetic drag: since a live in-app click-drag can't be scripted
without either stealing the user's input focus or simulating literal mouse-hardware
events (both ruled out - VS Code had focus most of this session), used
`SetWindowPos` from an external PowerShell process to stream ~50 resize steps at
20ms intervals, which is a reasonable stand-in for a real OS-level drag (Windows
delivers `WM_SIZE` on essentially every mouse-move sample regardless of source).
`PrintWindow` with `PW_RENDERFULLCONTENT` was needed to screenshot it, since a
correctly-unfocused pixet window sits behind VS Code and a plain screen-region capture
just grabs whatever's on top - confirmed the hard way when an early capture returned a
totally unrelated file manager window's content (the hwnd was legitimately pixet's the
whole time; only the *capture coordinates*, taken from a stale/wrong source, were off -
double-checked via `GetWindowThreadProcessId` + window title before concluding no other
app had actually been resized).

Put the title-bar debug logging back in a third time and found the real mechanism:
even with round two's synchronous `doItemsLayout()`-based fit, a **cross-process**
resize (a real drag comes from `explorer.exe`/`dwm.exe`, not this process) can still
get dispatched *between iterations* of the fit loop - "synchronous within one C++ call"
doesn't mean "atomic against sent window messages." Worse, the empirical
`renderedColumnCount()` check driving the backoff turned out to be a genuine
**feedback loop**, not just a race: fitting fewer columns means more rows, which can
cross the vertical scrollbar's show/hide threshold, which changes the viewport width,
which changes which column count fits - observed as a stable, *self-sustaining*
919px/931px oscillation (exactly one scrollbar-width apart) that never settled on its
own, deferring to the resize-debounce timer forever rather than converging.

Fixed at the actual root: `setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn)` in the
constructor. Reserving the scrollbar's space unconditionally means a column-count
choice can no longer change the viewport width as a side effect of itself, which
removes the feedback loop rather than reacting to it - most of round two's defensive
machinery (per-iteration width re-reads, post-loop drift detection deferring to the
debounce timer, a bounded `consecutiveDefers_` forcing eventual termination) turned out
to still be worth keeping as defense-in-depth against the underlying cross-process
timing hazard, just no longer fighting a fight they couldn't reliably win on their own.
`ThumbGridView.h`'s class comment now has the full five-round history.

Verified with the same synthetic drag sequence against a build with the fix: the debug
log showed every single fit converging in one pass (no `stale`/`defer` entries at all,
where before there'd been dozens in a stuck loop), and a `PrintWindow` screenshot
confirmed the grid fills cleanly with no leftover gap. Build + full test suite (15/15)
clean; debug logging fully removed again afterward.

## 2026-08-09 — desktop — Grid column-fit bug, round two (small folders)

The horizontal-fill bug came back: "space for 3 cells, shows only 1" on the folder
restored at startup. Put the title-bar debug logging back in (`ThumbGridView.cpp`,
same technique as the original hunt, removed again once done) and reproduced by
launching `pixet.exe` directly (needed `C:\Qt\6.8.3\msvc2022_64\bin` on PATH - it's
only on PATH inside the VS dev shell `scripts/build.ps1` enters, not a bare shell) and
reading its title. Real log for the repro folder (5 files):

```
U(vw=679,rc=5)>try(c=4)>verify(act=3)>try(c=3)>verify(act=2)>try(c=2)>verify(act=1)
>try(c=1)>R(w=667)>verify(act=1)>FIN(lfw=667,pending=0)>R(w=667)>U:jit(vw=667,lfw=667)
```

A **fourth** layer to the same underlying race documented on `updateGridSize()`: the
backoff sequence itself (columns 4→3→2→1) makes the grid *taller* each step (fewer
columns, same 5 items, more rows) - taller than the viewport is enough to cross the
vertical-scrollbar threshold, and on a folder this small, that can happen *mid-sequence*
(a big folder's scrollbar is already showing regardless of column count, so this never
triggered there). The scrollbar appearing shrank the viewport (679→667) between one
`applyColumns()` attempt's `setGridSize()` and its `QTimer::singleShot(0)` verification.
`finishFit()` then read `viewport()->width()` *fresh* (667, already changed) and
stamped that into `lastFitWidth_` - so the next resize's jitter guard compared 667
against itself and always looked "already fresh", even though the gridSize actually
applied (a single full-width column) was computed and verified against the stale 679,
never against 667. The grid got stuck at 1 column permanently.

Fix: `applyColumns()`'s deferred verify lambda now captures the viewport width at the
moment it was launched and compares against the current width when it actually fires;
a mismatch restarts clean via `applyColumns(idealColumns(), attemptsLeft - 1)` instead
of trusting a `renderedColumnCount()` reading that was measured against a since-stale
gridSize. Reproduced live before and after: before, stuck at 1 column even after the
scrollbar-driven resize settled; after, the log shows `drift(was=679,now=667)` followed
by a fresh `try(c=4)`→`verify(act=4)` that actually converges, confirmed visually via a
screenshot of the running app (4 columns, evenly filled, 5th item wrapped cleanly to
its own row). `ThumbGridView.h`'s class comment on `updateGridSize()` gained a fourth
numbered entry for this.

Build + full test suite (15/15) both clean.

## 2026-08-09 — desktop — Background reconciler + fullscreen aliasing fix

Two follow-ups from the P3 fullscreen work below.

**Fullscreen aliasing on the default (fit) view**: `FullscreenViewer::paintEvent` never
set `QPainter::SmoothPixmapTransform`. The fit-scaled decode only lands on a power-of-2
scaled-DCT step (`JpegCodec::decodeJpeg`'s `scale_num/scale_denom`), so it's usually
somewhat larger than the exact on-screen size - `drawPixmap` was then downscaling that
remainder with Qt's default (effectively nearest-neighbor) transform, visible as
aliasing even with no zoom involved. One-line fix: enable the render hint, matching
what `PreviewPane` already does via `Qt::SmoothTransformation`.

**Background reconciler** (`BackgroundReconciler.h/.cpp`, new): worried that the
width/height bug's residual bad data (see below) - and more generally, any file edited
outside pixet - would just sit stale until someone happened to hit Refresh on that
exact folder. Added a low-priority worker (`QThread::LowestPriority`) that continuously
cycles through every directory already in the `dirs` table, running the same Pass A/B
Refresh logic (`forceRescan=true, forceRethumbnail=false`) FolderIndexer already uses,
paced ~1.5s between directories with a 10-minute rest after each full pass - deliberately
slow, since this is hygiene, not urgency. Deliberately does *not* use the directory-mtime
shortcut, since that shortcut only catches added/removed/renamed files - a file edited
in place under the same name never moves its directory's own mtime, which is exactly
the drift this is meant to catch (mirrors the existing devlog note on why manual Refresh
already has to bypass that shortcut too). Coordinates with the on-demand indexer and any
running `pixet-index` via the same claims table Indexer already uses, under a distinct
owner id (`gui:bg:pid:N`) so it skips rather than contends with either. `MainWindow`
does a light incremental grid refresh (`refreshThumbStates()`, not a full reset) if the
directory the sweep just touched happens to be the one currently on screen.

Verified via build + full test suite (15/15); not live-verified in the running app this
round for the same reason as the last entry (VS Code had focus).

## 2026-08-09 — desktop — P3 fullscreen viewer

Double-click/Enter on a thumbnail opens a fullscreen viewer (`FullscreenViewer` +
`FullscreenDecoder`, `src/app/`). Baseline: fit-to-screen by default, click zooms to 1:1
centered on the click point, click-drag pans, click again zooms out, arrow keys move
next/prev with a ±2-row prefetch ring buffer, Escape closes. `FullscreenDecoder` is a
LIFO-queue worker (like `ThumbLoader`, unlike the side panel's latest-wins
`PreviewDecoder`) since the viewer needs several requests serviced at once - neighbor
prefetch plus an on-demand full-resolution decode for whichever row is current. Only
JPEG has a decoder right now; other formats fall back to the grid's cached thumbnail.

Follow-up round added six more asks in one pass:
- **Grid stays in sync.** `FullscreenViewer` emits `rowChanged(row)` on every
  navigation; `MainWindow` updates `grid_`'s current index + scrolls it into view, so
  closing fullscreen (however it happened) leaves the grid selection on the same image.
- **Double-click closes** the viewer, in addition to Escape.
- **Plain mouse wheel** navigates next/prev, same as arrow keys.
- **Ctrl+scroll zooms continuously**, centered on the cursor. This needed reworking the
  binary "fit vs. 1:1" model into a continuous one: `fitMode_` (recompute scale-to-fit
  every paint) vs. an explicit `scale_` + `centerImagePoint_` (which native-resolution
  pixel is centered in the viewport) once the user leaves fit mode via click or
  Ctrl+scroll. Turned out to *simplify* rendering rather than complicate it - one
  formula (`screenX = viewportCenter + (imagePoint - center) * scale`) now drives fit
  view, 1:1 click-zoom, and arbitrary Ctrl+scroll zoom alike, and the "never blank,
  upscale a lower-res placeholder while the real decode is in flight" behavior falls
  out for free instead of needing its own code path.
- **I key** toggles a bottom-left info overlay (filename/format/dimensions/size/taken-at/
  duration - same fields the main window's status bar shows). Not true EXIF-field
  cycling as floated ("maybe cycle through exif") - the app doesn't parse EXIF tags
  beyond orientation yet (`ExifInfo`), so there's nothing richer to cycle through
  without a real EXIF tag parser, which is out of scope for this pass.
- **F key** toggles borderless `showFullScreen()` vs. a maximized window with a title
  bar (set to the current image's full path, updated on every navigation) - persists
  across opens/closes as a session-level display preference rather than resetting each
  time.

Verified via build + full test suite (15/15 pass); not live-input-verified this round
since the user's foreground window was VS Code at the time (see the automation-safety
note from the earlier P3 baseline entry - skip synthetic input when the user is
actively in their editor, rely on code review + tests, say so plainly).

Still uncommitted, along with the P3 baseline below.

## 2026-08-09 — desktop — Fixed width/height bug + more QOL (path bar, force rethumbnail)

Follow-up to the QOL batch below. Four small asks turned up one real bug.

**Bug found and fixed**: the status bar's file dimensions were showing the *thumbnail's*
size (e.g. "160×120"), not the original image's. Root cause in
`ThumbGenerator::generateThumb` (`src/core/thumb/ThumbGenerator.cpp`): `ThumbResult.width/
height` were always the post-downscale thumbnail dimensions, and `Indexer.cpp`'s Pass B
write wrote that same value into *both* `thumbs.thumbs.w/h` (correct - that's the
thumbnail) *and* `files.width/height` (wrong - that's supposed to be the original).
Fixed by adding `JpegCodec::readJpegDimensions()` - a header-only read (`jpeg_read_header`,
no pixel decode, cheap) that gets the file's true native size independent of whatever
tier/scale the thumbnail decode used - stored as new `ThumbResult::origWidth/origHeight`,
which `Indexer.cpp` now writes to `files.width/height` instead. Added regression
assertions to `tests/test_thumbgen.cpp` (a 1600x1200 source must report `origWidth==1600`
even though its thumbnail is ≤320px) - would have caught this immediately.

**Important wrinkle this surfaced**: fixing the bug only affects *newly*-thumbnailed
files. `Refresh`/F5 (`forceRescan`) deliberately only re-thumbnails files whose
`(mtime, size)` actually changed - by design, so Refresh stays cheap on a large folder -
so already-indexed files keep their stale (wrong) width/height forever unless something
re-decodes them. Added a genuinely stronger option: `IndexOptions::forceRethumbnail`
(`src/core/scan/Indexer.{h,cpp}`) unconditionally re-thumbnails every file in the
directory regardless of whether it changed, threaded through
`FolderIndexer::indexFolder()` / `MainWindow::requestIndex` as a third bool. Exposed as
"Force Re-thumbnail This Folder" in the grid's right-click menu, distinct from the
existing lighter "Refresh (check for new/changed files)". This is also the fix for
already-corrupted `files.width/height` data from before today's bugfix - existing users
need to run it once per folder (or the whole tree via `pixet-index`) to pick up correct
dimensions; new indexing gets it right automatically.

**Other QOL** (`src/app/MainWindow.cpp` mainly):
- Path bar now also tracks the *selected file* (full path), not just the current
  directory - reverts to the directory when selection clears.
- Path bar selects-all with cursor at the end on focus (`QEvent::FocusIn` +
  `QTimer::singleShot(0, ...selectAll)` - doing it synchronously in the focus-in handler
  gets undone by the mouse-press that triggered the focus).
- Navigating to a folder (any way - tree click, bookmark, path bar) now expands that
  folder's own node in the tree, not just its ancestors, so its children are visible
  without an extra click.
- Status bar rewritten to show folder-level aggregates (`ThumbGridModel::imageCount()/
  videoCount()/totalBytes()`, computed for free during the existing per-directory row
  loop in `setDirectory()`) alongside the selected file's name/format/dimensions/size/
  taken-at/duration. Format name string added (`pixet::formatName()` in `db/Schema.cpp`
  - didn't exist anywhere before). Bit depth ("24bpp") sourced from the already-decoded
  preview `QImage::depth()` rather than a new DB column - no schema/migration needed
  (confirmed via research: this codebase has no migration mechanism at all yet, so
  avoiding a schema change here was deliberate).
- `.vscode/launch.json`: `externalConsole` (bool) → `console` (string) - VS Code
  deprecated the old key. `pixet`/`pixet-index` configs need different values
  (`internalConsole` vs `externalTerminal` - the CLI tool needs a real console to print
  to).

Verification: unit tests (15/15, including the two new dimension-regression checks) and
a live GUI pass confirmed the folder-aggregate status bar, path-bar file-tracking, and
selection border all work correctly against the real photo library. Did **not** get a
clean live confirmation of "Force Re-thumbnail" actually correcting a real file's
dimensions end-to-end - automation clicks started landing in the user's own VS Code
window (they were actively using the machine mid-test), so I stopped rather than risk
interfering. The underlying fix is covered by the unit tests either way; only the GUI
wiring for the new context menu action is unconfirmed live.

---

## 2026-08-09 — desktop — vcpkg cache fix + new GUI QOL features

Two follow-ups to the same day's earlier work.

**vcpkg cache bug**: the shared cache (previous entry below) verified as "working" too
early - a full cold rebuild populated the network share fine, but the *local*
`%LOCALAPPDATA%\vcpkg\archives` tier silently never got refreshed on rebuild. Root
cause: `default` needs an explicit `,readwrite` suffix exactly like `files` does -
`"clear;default;files,<path>,readwrite"` only makes `default` read-only. Confirmed via
`vcpkg install --debug`, which logs `Completed submission of X to N binary cache(s)` -
was reporting N=1 (network only) until fixed to `"clear;default,readwrite;files,...`,
after which it correctly reports N=2. `scripts/vcpkg-cache-env.ps1` updated; verified end
to end afterward (`build/release` reconfigured cold, both tiers populated: 20 local files,
44 network files).

**GUI QOL batch** (`src/app/MainWindow.{h,cpp}`, `ThumbGridView.{h,cpp}`,
`ThumbGridModel.{h,cpp}`, `main.cpp`):
- Thumbnail selection now draws a border instead of recoloring the cell - a custom
  `ThumbGridDelegate` clears `State_Selected` before delegating to the base paint, then
  draws the highlight color as an outline only.
- Grid right-click context menu with "Rescan This Folder" (same force-reindex path F5
  already had, just more discoverable).
- Status bar permanent label showing the selected item's name/dimensions/size/taken-at/
  duration - dimensions etc. only exist post-decode, so it also refreshes on
  `thumbsProgress`, not just on selection change.
- Path bar (`QLineEdit` above the tree) mirrors `currentPath_`; Enter navigates, and
  pasting a *file* path browses to its folder and selects that file once the grid has
  loaded it (`ThumbGridModel::rowForName()` + a pending-selection retried across
  `onFilesListed`).

Verification note: screenshot-and-click UI automation this session repeatedly gave
false readings before landing on a reliable method - raw `GetWindowRect`/`SetCursorPos`
coordinates from a non-DPI-aware PowerShell process don't match `UIAutomation`'s
physical-pixel rects (a ~1.24x mismatch), and `QLineEdit` accepted focus/clicks and
`WM_CHAR` text insertion fine but never fired `returnPressed()` for any synthetic Enter
key tried (`SendMessage(WM_KEYDOWN)`, `keybd_event`) - a tooling limitation, not
necessarily an app bug, but flagging since the path bar's Enter-to-navigate is the one
piece of this batch that was *not* independently confirmed working. Selection border,
context menu wiring, and status bar/preview were all confirmed via
`AutomationElement.GetClickablePoint()` + a real mouse click, which worked reliably once
adopted.

---

## 2026-08-09 — desktop — Shared vcpkg binary cache over the network (\\kioku\talsit)

User pushed the day's work to test-build on the other machine, then asked whether the
~28-minute cold vcpkg dependency build (full source build of ffmpeg/libheif/libraw/etc.)
on that machine's fresh clone can be avoided.

vcpkg already caches locally at `%LOCALAPPDATA%\vcpkg\archives` - a *second* configure on
the same machine only takes seconds. That cache just never leaves the machine that built
it, so a fresh clone elsewhere starts cold regardless. Fix: point `VCPKG_BINARY_SOURCES`
at an additional read/write cache tier so whichever machine builds a package first saves
the other from ever rebuilding it.

Added `scripts/vcpkg-cache-env.ps1`, dot-sourced by both `configure.ps1` (new - the
`cmake --preset` step, where vcpkg install actually runs) and `build.ps1`:

```
$env:VCPKG_BINARY_SOURCES = "clear;default;files,<path>,readwrite"
```

`default` keeps the local cache in play, `files,<path>,readwrite` adds the shared path as
a second tier vcpkg checks and writes through.

First attempt pointed `<path>` at the Nextcloud sync folder - user explicitly vetoed that
("I don't want it there, I will manually copy it across as needed") and asked for
`\\kioku\talsit\code\pixet\vcpkg-cache` instead: a plain network share, not synced
storage, so nothing moves between machines except when deliberately copied. Switched to
that path.

Verified: `configure.ps1 -Preset release` runs clean through the new script (restores all
22 packages from the local cache in ~4s rather than rebuilding), and the network path gets
created (`New-Item -Force`) and is writable. Did not force a genuine from-source rebuild
to prove the write-through mirroring specifically - that's standard, well-documented
vcpkg behavior once the `VCPKG_BINARY_SOURCES` syntax is confirmed valid and the target
path is reachable/writable, both of which are now confirmed.

**Not yet proven end-to-end**: the actual cross-machine payoff (build on desktop, confirm
laptop's fresh clone skips rebuilding the same packages) - that only shows itself the next
time a *new* dependency version needs building, since everything currently in
`vcpkg.json` is already cached locally on this machine.

Next: `SETUP.md` needs a note pointing at this for the other machine. Commit
`vcpkg-cache-env.ps1`, `configure.ps1`, `build.ps1`, push.

---

## 2026-08-09 — desktop — Fix: F5 build fails after a trivial edit ("type_traits" not found)

User report: edit a file, revert the edit, hit F5 → build fails. Got the actual error
this time (essential - I couldn't reproduce it blind from a description):

```
FAILED: [...] FolderTreeView.cpp.obj.ddi
"...\cl.exe" ... C:\Users\dmo\code\pixet\src\app\FolderTreeView.cpp ...
C:\Qt\6.8.3\msvc2022_64\include\QtCore/qglobal.h(13): fatal error C1083:
Cannot open include file: 'type_traits': No such file or directory
```

`type_traits` is a standard MSVC STL header - this is the exact "no `INCLUDE` env var
set" failure mode from way back in P0, just now happening *inside* VS Code's F5 flow
instead of a bare terminal. Root cause, once the actual compile command was visible:
none of its `-I`/`-external:I` flags point at the MSVC or Windows SDK headers - CMake's
Ninja+MSVC generator does not bake those into `build.ninja`, it expects them to already
be in the environment via `INCLUDE`/`LIB` (set by `vcvarsall.bat`/`Enter-VsDevShell`).

The `CMakePresets.json` `vendor` block (added a few sessions back specifically so VS
Code wouldn't need this) *can* make CMake Tools inject that environment automatically -
but, it turns out, only reliably for a build directory CMake Tools configured itself.
Every configure of this repo's `build/debug`/`build/release` actually happened via me
running `cmake --preset ...` directly from a terminal (necessary throughout this whole
session for command-line verification) - CMake Tools' extension was never the one that
configured these directories, so its F5 build task apparently doesn't apply the same
injection to them. The failure was latent the whole time: already-compiled `.obj` files
don't need `cl.exe` at all, so it only surfaces the moment something actually needs
recompiling - which a trivial edit-then-revert does (mtime changes, ninja recompiles),
explaining exactly the symptom reported.

**Fix: stopped depending on CMake Tools' environment injection working at all.** Added
`scripts/build.ps1`, which locates the VS install via `vswhere` and explicitly calls
`Enter-VsDevShell` itself before `cmake --build` - the exact sequence this session's own
manual PowerShell commands have used throughout, now scripted and deterministic instead
of conditional on VS Code extension internals. `.vscode/tasks.json`'s two build tasks
now run this script (`"type": "shell"` instead of `"type": "cmake"`) rather than
CMake Tools' own build command. `launch.json`'s `preLaunchTask` references need no
changes - same task labels, different implementation underneath.

**Verification:** the bug itself is confirmed directly by the user's pasted error output
above, not inferred - that's what made the root cause (missing `INCLUDE`) unambiguous.
For the fix, reverted `FolderTreeView.cpp` to committed state, added a blank line,
reverted again (the exact sequence reported), then ran `scripts/build.ps1 -Preset
release` from a cold PowerShell process (no dev shell pre-established - this tool's
shell state doesn't persist between calls, so every invocation already *is* a fresh
process, a reasonable proxy for "VS Code spawns a plain terminal for this task").
Succeeded cleanly for both presets. Didn't separately re-run the *old*, broken
`"type": "cmake"` task to watch it fail first - the user's pasted output already is that
evidence, and every one of my own manual builds all session used an explicit
`Enter-VsDevShell` wrapper, so re-deriving the failure would've just meant deliberately
building without one, not new information.

Also updated `SETUP.md`'s VS Code section to describe why `scripts/build.ps1` exists
instead of just "CMake Tools handles it," so this doesn't need rediscovering blind on
the other machine if it configures its own build directory differently.

Rebuilt both configs clean via the new script, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Tree: don't reposition if already visible; another hscroll attempt

User feedback: (1) don't jump the tree to top if the newly-selected folder is already
visible (even mid-viewport) - only reposition when it's genuinely not reachable
without scrolling; (2) horizontal scroll was *still* resetting on a direct tree click
despite the previous `FolderTreeView` fix.

**Fix 1 - conditional repositioning.** `repositionTreeToTop` now checks
`tree_->visualRect(idx)` first: if the row is already within the viewport (extended by
a 3-row-height tolerance on each side), return immediately without touching scroll at
all. An invalid rect (row not yet part of the laid-out tree - still expanding
asynchronously) falls through to the existing positioning logic unchanged. **Verified
cleanly**: screenshotted before/after clicking a bookmark-equivalent target that was
already visible mid-viewport (`Pictures`, several rows down) - the tree's scroll
position was pixel-identical before and after, only the selection highlight moved.
This is a real, direct confirmation, not an inference from internal state.

**Fix 2 - horizontal scroll on direct click, take two.** Added `mouseReleaseEvent` to
`FolderTreeView`'s save/restore set (previously only `mousePressEvent`/`keyPressEvent`),
and - the more likely actual fix - the restore is now also reasserted via
`QTimer::singleShot(0, ...)` after the base class call returns, not just synchronously
right after it. Reasoning: if Qt's selection-driven auto-scroll is deferred rather than
happening synchronously inside the base class event handler, a synchronous
save-then-restore wrapped tightly around it is too early to catch it - it needs to be
reasserted after the event loop has had a chance to run whatever Qt deferred.

**Not independently re-confirmed this round** - a horizontal-scroll-preservation test
(Shift+wheel to establish non-zero scroll, then click elsewhere) got interrupted by an
unrelated Windows task-switcher overlay that the synthetic input sequence apparently
triggered, and repeated attempts to force synthetic mouse-wheel/keyboard combinations
in this environment have a track record of exactly this kind of interference this
session. Given fix 1 already got a clean, direct, real confirmation and the remaining
risk/time cost of continuing to fight synthetic input reliability, stopped here rather
than kept forcing it - the code change is a straightforward, defensible technique
(same save/restore pattern already proven for the bookmark-click path, now covering
release + deferred re-assert too), but this one specifically needs the user's own
direct confirmation rather than mine.

Rebuilt both configs clean, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Fix: horizontal scroll still reset on a direct tree click

User confirmed the previous fix helped but the horizontal-scroll-reset bug still
happened "on folder change." Real gap: the previous fix only covered navigation that
originates *outside* the tree (bookmark click, startup restore) - `MainWindow::navigateTo`'s
repositioning block explicitly only runs when `tree_->currentIndex() != idx`, which is
never true for a click *inside* the tree (Qt's own native click handling has already
changed `currentIndex` by the time our `currentChanged` signal handler runs). So a
direct click on a different tree row was hitting Qt's built-in auto-scroll-to-reveal
behavior with zero opportunity for us to intervene via any signal - by the time any of
our code sees the change, the horizontal reset has already happened.

Fix: added `FolderTreeView`, a small `QTreeView` subclass overriding `mousePressEvent`
and `keyPressEvent` to save `horizontalScrollBar()->value()`, call the base
implementation (which does Qt's normal click/keyboard-navigation handling, auto-scroll
side effect and all), then restore the saved value immediately after. This intercepts
at the only point that's actually before Qt's internal auto-scroll happens, regardless
of the exact internal call sequence Qt uses - no signal-based approach could have
worked here. `MainWindow` now constructs a `FolderTreeView` instead of a bare
`QTreeView`; the bookmark/startup-restore path (`repositionTreeToTop`) is unchanged
and still handles the "position at top" part for that separate case.

**Verification note:** given how unreliable coordinate-based UI automation proved
earlier this session (DPI scaling, focus-stealing, DWM compositing timing), didn't
attempt a pixel-precise repro of "scroll right, click elsewhere, still scrolled right"
this time - that would need reliably clicking a horizontal scrollbar's page-right
region first, another coordinate-precision dependency. Instead confirmed no regression
(clean build, 15/15 tests, a direct tree click still navigates and populates the grid
correctly) and relied on the fix being simple and correct by inspection - it's the
same save/restore pattern already verified working for the bookmark-click case, just
intercepting at `mousePressEvent`/`keyPressEvent` instead of after the fact. Asked the
user to confirm directly since they've been an accurate real-usage reporter of both
bugs this fix addresses.

Rebuilt both configs clean, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Tree: position selection at top on navigate, keep horizontal scroll

Two requests: (1) when navigating via a bookmark or app-startup restore, the target
folder should land at the top of the tree, not wherever `EnsureVisible` happens to
leave it; (2) navigating shouldn't reset horizontal scroll back to the left, hiding
whatever of a long name had been scrolled into view.

**Fix, in `MainWindow::navigateTo`/`repositionTreeToTop`:** this only runs for
navigation that didn't originate from a click already inside the tree (a direct tree
click already has `currentIndex() == idx`, so the block is skipped entirely - this
scoping already existed and turned out to be exactly right, no changes needed there).
Walk and `expand()` every ancestor first, `setCurrentIndex(idx)`, then position via
`scrollTo(idx, QAbstractItemView::PositionAtTop)` - **not** the plain `scrollTo(idx)`
used before, whose default `EnsureVisible` hint is also what was resetting horizontal
scroll (it scrolls to reveal the row's start). Horizontal position is saved before and
restored after, so that side effect is gone without losing the "position at top" part.

**This took a lot longer to actually verify than to write, for two compounding
reasons, both worth remembering:**

1. **QFileSystemModel populates directories asynchronously**, so a `scrollTo()` called
   immediately after `expand()` is frequently computed against an incomplete row count
   - the position looks right for a moment, then drifts as more siblings stream in.
   Signal-driven retries via `QFileSystemModel::directoryLoaded` help but weren't
   sufficient by themselves for a deeply nested path (6 ancestor levels) under a
   directory with ~35 siblings (a real dev-machine home folder full of app-config
   dirs) - empirically needed fixed-delay fallback retries out to a few seconds
   (`{300, 800, 1500, 3000}` ms) to reliably converge. A shallower 4-level path under
   a directory with only ~7 children converged almost immediately - the settling time
   scales with both nesting depth and sibling count at each level, not just one or the
   other.
2. **The verification methodology itself was broken for a while and produced a false
   negative.** Every earlier screenshot in this session launched the app *in the
   background* (other windows on top) and only foregrounded it right before
   capturing. Under that sequence, Qt's internal state was provably correct
   (`visualRect()`/scrollbar value checked directly via a temporary debug-title
   diagnostic - valid, non-zero rect, positioned at y=0) while the screenshot still
   showed the old, unscrolled tree content. Foregrounding (and, to reliably survive a
   browser tab that kept stealing focus back mid-test, pinning the window
   `HWND_TOPMOST`) *before* the navigation happens, so the window is genuinely visible
   and composited by DWM the whole time, was necessary to get a screenshot that
   actually reflects the app's real state. A backgrounded window's content changes are
   evidently not guaranteed to be reflected live in a screen capture even after
   bringing it forward afterward.

Net effect of both lessons together: an early round of this fix looked "confirmed
working" from a screenshot that was actually stale, then looked "still broken" from a
different screenshot that turned out to be testing the wrong bookmark (an earlier
miscalibrated click had landed on a different, shallower bookmark and saved *that* to
`lastDirectory`, so the next launch wasn't testing the deep path at all). Sorted out
by checking Qt's actual internal state directly rather than trusting either screenshot,
and by controlling window visibility state deliberately throughout each test rather
than only at capture time.

Rebuilt both configs clean, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Fix: folder tree truncates long names, no horizontal scroll

User confirmed bookmarks work and P2's bug fixes hold up. New report: deeply
nested/long folder names in the left-panel tree get truncated with no way to scroll to
see the rest.

Root cause: `QTreeView` defaults to `header()->stretchLastSection() == true`, which
forces the one visible column (the other `QFileSystemModel` columns are hidden) to
always exactly fill the viewport width - it can never grow wider, so a long name just
gets elided and there's nothing to scroll to. Fixed in `MainWindow`'s tree setup:
`setStretchLastSection(false)`, column 0 resize mode `ResizeToContents` (grows to fit
the longest currently-visible item, e.g. as folders are expanded), horizontal
scrollbar policy `ScrollBarAsNeeded`, and `setTextElideMode(Qt::ElideNone)` so nothing
is ever silently cut - the user's expectation was "let me scroll to it," not "shorten
it for me."

Verified (window never resized/moved during capture, per the earlier lesson): a
26-character name (`SolidWorks_Flexnet_Server`) now renders in full with no ellipsis,
confirming the column is genuinely sizing to content rather than being force-fit.
Didn't chase a screenshot of the scrollbar itself appearing (would need a
pathologically long real folder name to force it) - the mechanism is a standard,
well-understood Qt pattern (disable stretch + ResizeToContents + ScrollBarAsNeeded),
not something that needs pixel-level proof on top of confirming elision is actually
gone.

Rebuilt both configs clean, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Fix: wheel scroll still smooth; thumbnails clipped to a sliver

User tried the previous session's fixes: wheel scrolling was still smooth, and the
"only the tiny top part [of each thumbnail] shows until I hover" bug was still there.
Both were real, separate bugs from the pagination-scroll and streaming-file-list work.

**Wheel scroll fix.** `ThumbGridView::wheelEvent` computed `notches = angleDelta().y() /
120` and fell through to the default (smooth) `QListView::wheelEvent` whenever that was
zero. Modern mice and touchpads commonly deliver many small fractional wheel deltas
instead of one clean 120-unit notch per click - so `notches` was 0 almost every time,
and the override almost never actually fired. Fixed by accumulating delta across events
in a member (`accumulatedDelta_`) and only consuming a full 120 once the accumulation
reaches it, **never** falling through to the base implementation regardless of delta
size (that fallthrough was the bug).

**Thumbnail clipping fix - likely the real explanation for the very first "top 10-20%"
report too, not just the threading bug.** Root cause: `grid_->setUniformItemSizes(true)`.
That flag tells Qt to compute an item's size once (from an early item) and trust it for
every cell thereafter, never recomputing. Since thumbnails arrive asynchronously, the
first real layout pass happens while most cells' `Qt::DecorationRole` is still an empty
`QVariant` (no thumbnail yet) - so the cached "uniform" size ends up computed without
accounting for a decoration at all. Every thumbnail that streams in afterward gets
clipped to that too-small stale size - reading exactly as "only a tiny sliver at the
top" - until something (a hover) forces Qt to relayout from scratch. Fix: removed
`setUniformItemSizes(true)` entirely. `setGridSize()` already gives fixed, explicit
cell dimensions, so the performance case for trusting a cached size doesn't really
apply here anyway.

Also added, as defense in depth (cheap, and removes any doubt regardless of the exact
Qt-internal mechanism): `ThumbGridModel::refreshThumbStates()` now batches its
`dataChanged` into one range-covering signal instead of one per changed row, and
`MainWindow` explicitly calls `grid_->viewport()->update()` after both
`ThumbLoader::thumbReady` and `refreshThumbStates()`.

**A verification-methodology lesson worth keeping:** the previous session's "confirmed
via screenshot" checks were unreliable in a way I didn't catch at the time - every
screenshot was preceded by a `MoveWindow` call to reposition/resize the window for
capture, and resizing a window forces Qt to fully relayout and repaint regardless of
whatever bug is actually present. That almost certainly masked this exact clipping bug
in the earlier "verified" screenshots. This time, verified by launching the app,
deliberately **never** resizing or moving it, and only using `SetForegroundWindow` (plus
the Alt-key foreground-lock workaround) to bring it forward for the screenshot -
foregrounding an already-fully-visible, non-minimized window doesn't force DWM to
recomposite content that hasn't changed, so this is a much more faithful test of what
the user actually sees. Caught the real transitional sequence cleanly this time:
filenames-only (~2s), then full-size non-clipped thumbnails streaming in (~3.7s), no
hover needed. **Lesson: never resize/move the window as part of a verification
screenshot - it can hide exactly the class of layout/repaint bug being tested for.**

Rebuilt both configs clean, 15/15 tests pass in each.

---

## 2026-08-09 — desktop — Pagination scrolling + streaming file list on cold navigate

Two user-requested UX changes.

**Pagination scrolling.** `QListView`'s default wheel handling scrolls by a pixel
amount scaled by the OS's "lines per notch" setting, which reads as smooth/continuous
over a thumbnail grid. Added `ThumbGridView` (`QListView` subclass) that overrides
`wheelEvent` to move the vertical scrollbar by exactly one grid row per wheel notch,
and sets `ScrollPerItem` for keyboard/scrollbar-arrow consistency too. `MainWindow`
now constructs a `ThumbGridView` instead of a bare `QListView`.

**Immediate file list on cold navigate.** This is the streaming behavior P2 explicitly
deferred ("no incremental placeholder-then-fill UX yet... would need Indexer to expose
per-batch progress, not just per-directory, which it doesn't yet"). Implemented that
missing piece:
- `pixet_core::Indexer` gains `IndexCallbacks` (replacing the old bare `onProgress`
  function parameter): `onFilesListed(dirId, dirPath)` fires once, right after Pass A's
  transaction commits - the file list is final at that point even though thumbnails are
  still pending. `onProgress` now also fires after every Pass B batch commit (previously
  only once per directory, useless for a GUI's single non-recursive folder), so
  thumbnails becoming available is observable incrementally, not just at the very end.
- `FolderIndexer` (GUI wrapper) exposes this as two signals: `filesListed(path)` and
  `thumbsProgress(path)`.
- `ThumbGridModel` gained `refreshThumbStates()` alongside the existing `setDirectory()`.
  The distinction matters: `setDirectory()` does a full `beginResetModel()`/
  `endResetModel()` (correct exactly once, right after Pass A, since nothing meaningful
  is on screen yet to lose) - `refreshThumbStates()` re-checks `thumb_id`/`state` for
  *already-loaded* rows and only emits targeted `dataChanged()` for ones that actually
  changed, so already-displayed thumbnails are never touched. Calling `setDirectory()`
  on every Pass B batch instead would have reset the model repeatedly and made
  thumbnails visibly flicker in and out - the whole reason this needed a second method
  rather than just calling the existing one more often.
- `MainWindow` wires `filesListed` → `setDirectory()` (once) and `thumbsProgress` →
  `refreshThumbStates()` (repeatedly); `onIndexerFinished` now also calls
  `refreshThumbStates()` (catches any trailing batch) instead of `setDirectory()`,
  since a reset at the very end would undo all the incremental work.

`pixet-index` updated for the `IndexCallbacks` signature change (mechanical - same
lambda, just assigned to `callbacks.onProgress` instead of passed positionally). It
incidentally now gets more frequent progress prints too (per-batch instead of
per-directory), a harmless side effect.

**Verification:** rebuilt both configs clean, 15/15 tests pass in each. Tried twice to
screenshot-capture the transient "filenames visible, thumbnails still filling in" state
on a cold 245- and 462-file folder and missed the window both times (indexing at
~69-80 files/sec completes in a few seconds, and screen-capture tooling overhead in
this environment - process launch, foreground-lock workaround, P/Invoke JIT - eats
1-2s+ before the first frame lands reliably). Did not keep fighting it: the mechanism
is correct by construction (`onFilesListed` structurally fires before any decode work
starts; `refreshThumbStates()` never resets the model, so it cannot regress
already-shown content even if never observed mid-flight) and the underlying pipeline
was already rigorously verified in the previous entry. This one's easy to confirm by
just watching it - flagging instead of burning more time on automation.

---

## 2026-08-09 — desktop — Release build: first real numbers

Built and benchmarked `release` (RelWithDebInfo) for the first time. Configure was ~30s
(vcpkg restored every dependency from its binary cache instead of rebuilding from
source - the ~28min P1 debug build only pays that cost once per machine, not per
build type).

**Cold-run throughput, same 2,678-file folder as the P1 debug benchmark:**
- Debug: 38.8s → ~69 files/sec
- Release: 34.5s → **~78 files/sec** (~12% faster)

Smaller win than a typical debug-vs-release gap because the workload is dominated by
the embedded-preview path (96.5% of files, per P1) - mostly file I/O and a tiny JPEG
decode, not the kind of hot CPU-bound loop `-O2` transforms dramatically. The `-O2`ed
scaled-DCT/resize/encode path helps, but it's a minority of the work here.

Also: release `pixet.exe` uses noticeably less memory at launch (~74MB vs ~97-99MB
debug) - expected, no debug CRT/iterator-checking overhead.

Scroll/navigation responsiveness itself should be identical or better between configs -
that was the threading bug fixed in the previous entry, orthogonal to optimization
level, and applies the same way in both.

---

## 2026-08-09 — desktop — Critical fix: workers were never actually threaded

User reported real-use bugs: navigating to a folder froze the UI for 2-3s, scrolling
managed maybe 2-3 repaints/sec, thumbnails only partially rendered until an unrelated
interaction (hover) forced a repaint, and thumbnails visibly overlapped each other.

**Root cause (one bug explaining the first three symptoms): `QObject::moveToThread()`
silently no-ops on an object that already has a parent.** `ThumbLoader`, `PreviewDecoder`,
and `FolderIndexer` were all constructed as `new Worker(this)` in `MainWindow.cpp` -
passing `this` (MainWindow) as the QObject parent. Each worker's constructor then called
`moveToThread(&thread_)`, which Qt silently refuses (just a stderr warning - invisible,
since `pixet` is a `WIN32` subsystem app with no console) when the object has a parent.
Net effect: **every "background" worker had genuinely been running on the UI thread the
entire time.** Every `connect()` to them resolved to `Qt::DirectConnection` (same
apparent thread) instead of `Qt::QueuedConnection`, so:
- Navigating to a folder ran `FolderIndexer::indexFolder` (Pass A+B, real disk I/O and
  JPEG decode for every file) synchronously, blocking the UI for exactly as long as
  indexing took - the reported 2-3s freeze.
- Every `ThumbLoader::request` during scroll decoded synchronously too, blocking
  painting on every single thumbnail - the reported 2-3 updates/sec.
- The partial-repaint-until-hover symptom was very likely a side effect of decode work
  happening synchronously inside what should have been a quick signal handler,
  interfering with Qt's own paint scheduling.

Fix: construct all three workers with **no parent** (`std::make_unique<ThumbLoader>()`,
not `new ThumbLoader(this)`), and switched `MainWindow`'s owning members from raw
`Worker*` to `std::unique_ptr<Worker>` for manual lifetime management (parent-based
auto-deletion isn't available once there's no parent). Declaration order in
`MainWindow.h` already had them after `gridModel_`, which - given members are destroyed
in reverse declaration order - means workers stop cleanly *before* the model/view they
signal into gets torn down. Left that ordering as-is since it's already correct.

**Second, separate bug: overlapping thumbnails.** Stored thumbnails are up to 320px
(P1's target); the grid displays them at a 150px icon size, but nothing ever scaled the
pixmap down before handing it to `Qt::DecorationRole` - relying on Qt's default item
delegate to auto-fit an oversized raw `QPixmap` into the icon box, which it doesn't
reliably do, so decorations bled into neighboring cells. Fixed in `ThumbLoader`: decode
straight to the target size (cheap scaled-DCT path, `ThumbLoader::kThumbIconSize = 150`)
and do a final exact `QPixmap::scaled(..., KeepAspectRatio)` pass, since `decodeJpeg`'s
DCT scale steps only land *close* to a target, not exact. `MainWindow`'s
`grid_->setIconSize()` now derives from the same `kThumbIconSize` constant instead of a
second hardcoded `150` - one source of truth for how big a grid cell actually is.

**Verification note:** screenshot/click automation was unreliable last session (DPI
scaling, see the P2 entry below) - this time verified without depending on pixel
coordinates at all:
- **Responsiveness**: wiped the cache, seeded a fresh 245-file folder, launched, and
  polled `SendMessageTimeout(hwnd, WM_NULL, ..., SMTO_ABORTIFHUNG, 500ms)` every 150ms
  for 6 seconds while on-demand indexing ran. Every single response was **sub-millisecond
  (0.3-2ms), zero hangs** - the UI thread was never blocked, for the entire duration
  indexing was actively running.
- **Correctness**: after indexing finished, queried the DB directly - 245 files, 237
  done, 8 unsupported, 0 failed, 237 thumb blobs. Exact match with this same folder's
  known-good numbers from the P1 benchmark entry - confirms indexing did real, correct
  work off-thread, not that it silently skipped anything.
- **Overlap fix**: screenshot (with the window properly foregrounded this time) shows
  clean, distinct, non-overlapping thumbnails with filenames correctly positioned below
  each cell.
- Scrolling smoothness itself wasn't re-measured numerically, but shared the identical
  root cause as the navigate-freeze (synchronous decode on the UI thread during
  `ThumbLoader::request`), which is now fixed the same way.

This class of bug (parented QObject silently failing to move threads) is worth
remembering for any *future* worker-thread class: **never pass a parent to a QObject
you're about to `moveToThread()`.**

---

## 2026-08-08 — desktop — P2: GUI shell (folder tree, bookmarks, grid, preview, on-demand index)

Implemented the P2 scope: `MainWindow` now has a real folder tree, a DB-backed
bookmarks list, a virtualized thumbnail grid, a side preview pane, and on-demand
indexing wired to navigation - the "browse a folder and it just works, no prior
`pixet-index` run needed" behavior the whole plan is built around.

**New pieces (`src/app/`):**
- `ThumbGridModel` - `QAbstractListModel` over `files` for the current dir. Rows come
  from a synchronous main-thread query (small/fast - deliberately not async, see
  below). `DecorationRole` returns a cached `QPixmap` if we have one, else emits
  `thumbNeeded(fileId, thumbId)` and returns a placeholder.
- `ThumbLoader` - `QObject` moved to its own `QThread`, own read-only `Database`
  connection (created lazily, on the worker thread - a `Database` must be opened and
  used by the same thread). Requests are a LIFO stack deduped by file id, so whatever
  was most recently requested (typically wherever the user just scrolled to) decodes
  first. Decodes thumb blobs through `pixet_core`'s own `decodeJpeg`, not Qt's image
  plugins (see `QtInterop.h` - avoids a runtime plugin-discovery dependency).
- `PreviewDecoder` - same threading shape, but decodes the *original* file at preview
  resolution (not an upscale of the thumbnail). Cancel-on-supersede via an
  `std::atomic<qint64> latestRequestId_` stamped synchronously on the UI thread the
  instant a new request comes in, checked both before and after the (possibly slow)
  decode - a superseded in-flight decode bails without emitting.
- `FolderIndexer` - thin `QObject`/`QThread` wrapper that calls P1's `Indexer` exactly
  as `pixet-index` does, just non-recursive and triggered by navigation instead of a
  CLI arg. This is what makes browsing self-indexing: `MainWindow::navigateTo` shows
  whatever's cached immediately, then always fires `requestIndex` in the background,
  which is a fast no-op via the freshness check when nothing changed.
- `PreviewPane` - plain `QLabel`-backed widget; keeps the undecoded-resolution
  `QPixmap` around so a window resize rescales in place instead of triggering a
  redecode.

**Real bug found via code inspection while wiring paths, not by luck:** `dirs.path`
in the DB is written via `pixet-index`'s `GetFullPathNameW`-based normalization
(backslash-separated, no trailing slash), but `QFileSystemModel` can hand back
forward-slash paths. A GUI query built from an unnormalized path would silently miss
rows that are actually there. Fixed by extracting that normalization out of
`pixet-index/main.cpp` into `pixet_core` as `util/PathUtil.h` (`normalizePath`), so
the CLI and the GUI funnel every path through the identical function before it ever
touches the DB. `pixet-index` updated to use it too (removed its local copy).

**Scope decisions not already in the plan:**
- MainWindow's own DB connection is **read-write**, not read-only as the plan's
  "GUI opens read-only connections" line suggests. Reasoning: bookmarks CRUD needs
  writes anyway, and a read-only `sqlite3_open_v2` fails outright if `index.db`
  doesn't exist yet (first launch, cold machine) - opening read-write bootstraps the
  schema for free. The *hot* paths the plan actually cares about (thumbnail blob
  reads under `ThumbLoader`, Pass A/B writes under `FolderIndexer`) do each get their
  own dedicated connection off the UI thread, which is the part that actually matters
  for not blocking the GUI.
- Grid metadata queries (`ThumbGridModel::setDirectory`) run synchronously on the UI
  thread rather than being handed to a worker. This is a deliberate simplification -
  it's a single indexed `SELECT` against a small warm table, microseconds in
  practice. The genuinely expensive operations (thumbnail blob decode, Pass A/B disk
  walking) are the ones actually moved off-thread.
- No incremental "placeholder now, thumbnails stream in during Pass B" UX yet for a
  *newly*-indexed folder - `FolderIndexer::finished` triggers one full grid reload
  after Pass A+B both complete for that folder. Already-cached folders are unaffected
  (still instant). At P1's ~69 files/sec single-threaded, a typical few-hundred-photo
  folder finishes in a few seconds - acceptable for a first cut; true Pass-A-then-
  streaming-Pass-B would need Indexer to expose per-batch progress, not just
  per-directory, which it doesn't yet.
- No explicit cancellation of off-screen `ThumbLoader`/`FolderIndexer` requests - the
  grid only re-requests currently-painted cells so stale entries just stop being
  reissued and drain naturally. Revisit if scrolling perf ever demands it.
- Double-click / Enter on a grid item does nothing yet - fullscreen viewing is
  explicitly P3, didn't want a dead-end stub.

**Testing note for the next session (this cost real time to figure out):** GUI
screenshot/click automation via PowerShell + Win32 APIs (`SetCursorPos`,
`mouse_event`, `MoveWindow`) in this environment is **unreliable** because the
display runs at 125% scaling (`GetDpiForWindow` = 120) and Qt6 apps are
Per-Monitor-V2 DPI-aware by default (`GetProcessDpiAwareness` = 2) - correct/desired
for crisp rendering, but it means coordinates from external, differently-DPI-aware
Win32 automation don't line up with the app's own logical pixel space, and screenshots
can visually mislead about where widgets actually are. **Ground truth is the app's
own widget geometry**, not screenshot pixel-counting - when in doubt, temporarily log
`widget->geometry()` (e.g. via window title, since these are `WIN32` subsystem apps
with no console) rather than trying to reverse-engineer scaling factors from a
screenshot. Confirmed this way: the splitter layout (bookmarks+tree / grid / preview)
is correctly proportioned, none of the three panels collapse to zero.

**Verified:**
- Clean build, 15/15 unit tests still passing.
- On-demand indexing end-to-end, twice, against real folders in the Nextcloud test
  tree with the cache fully wiped first: navigated to a folder with zero cache,
  confirmed real (different, changing) photo thumbnails populate the grid with no
  prior `pixet-index` run - this is the actual point of P2.
- Widget geometry confirmed correct via direct introspection (see above) - all three
  panels present and properly sized.
- Not yet verified by direct interaction (blocked on the DPI/automation issue above,
  not a known app bug): grid-selection-to-preview-pane click flow, bookmarks
  add/click/remove, Refresh. These all use the same wiring patterns already confirmed
  working elsewhere (identical async architecture to the proven thumbnail path) but
  should get an actual manual click-through on the next session, on either machine,
  since a human with a real mouse doesn't have this problem.
- Next: P3 - fullscreen viewer (ring buffer, prefetch, thumb-upscale fallback,
  keyboard navigation).

---

## 2026-08-08 — desktop — P1 throughput benchmark (real data) - the P1 gate

Ran `pixet-index` against `C:\Users\dmo\Nextcloud\InstantUpload\Camera\2026` - real
phone/camera uploads, not synthetic data. 2,678 files, 33.9GB (2,522 jpg / 142 mp4 /
14 dng). First attempt was contaminated (the folder had already been indexed from an
earlier pass in this session, so it reported a false "0.0s, all fresh-skipped" - wiped
`%LOCALAPPDATA%\pixet\{index,thumbs}.db` and reran clean to get an honest cold number.

**Result: 2,678 files in 38.8s = ~69 files/sec, single-threaded.**
- 2,434 embedded-preview (96.5% of the 2,522 JPEGs - the EXIF-thumbnail fast path is
  doing almost all the work, exactly as the plan bet it would)
- 88 decoded (main-image scaled-DCT path - JPEGs without a usable embedded preview)
- 156 unsupported (142 mp4 + 14 dng, exactly matching the file listing - correctly
  deferred to P4, not errors, and cheap: format is checked before any file read, so
  these don't cost real I/O/decode time)
- 0 failed

Rough extrapolation to 800GB: this sample averages ~12.6MB/file (skewed up by the
videos); at that ratio 800GB is ~63k files, which at 69 files/sec is **~15 minutes**
single-threaded. Treat this as order-of-magnitude, not a promise - it's one 8-month
slice of a phone's camera roll, not the full multi-year archive, and older parts of a
"real" photo library could have a different RAW/JPEG ratio. But the extrapolation is
fairly robust to that specific uncertainty, since RAW/video files are cheap
(Unsupported, rejected pre-read) rather than slow in P1 - they just don't get
thumbnailed yet.

**Gate verdict: pass.** Format mix (94% JPEG in this sample) confirms JPEG-only was the
right P1 scope call - RAW doesn't need to move up from P4. ~69 files/s single-threaded
is fast enough that P1 isn't the bottleneck for "index while browsing" (P2's on-demand
FolderIndexer only ever touches one folder at a time, a few hundred files at most).
Multithreading is real, available headroom (decode is CPU-bound and embarrassingly
parallel across files) but not an urgent blocker - reasonable to defer past P2 unless a
full 800GB `pixet-index` run in practice turns out to feel slow.
- Next: P2 - GUI shell (MainWindow, folder tree, bookmarks, thumbnail grid, on-demand
  FolderIndexer wired to navigation).

## 2026-08-08 — desktop — P1: core indexer + JPEG ladder, unit-tested and smoke-tested

Implemented the P1 scope from the plan: schema, claims, directory walker, the JPEG
extraction ladder, batched writes, and `pixet-index` wired to actually run it. All new
code lives in `pixet_core` (`src/core/{db,scan,meta,decode,thumb,util}/`) so the GUI's
future on-demand `FolderIndexer` reuses the exact same path, per the plan.

**Real bug caught by the schema unit test, not by eyeballing:** unqualified
`CREATE TABLE IF NOT EXISTS thumbs(...)` targets the `main` database, not the attached
`thumbs` alias, even though the table name matches the alias name. It was silently
creating the thumbs table inside `index.db` instead of `thumbs.db`. Every subsequent
`INSERT INTO thumbs.thumbs(...)` from the indexer would have failed at runtime. Fixed to
`CREATE TABLE IF NOT EXISTS thumbs.thumbs(...)`, added a regression test that asserts the
table does *not* leak into `main`. This is exactly the kind of bug that only shows up the
first time you actually write a thumbnail - worth remembering if `thumbs.db` ever looks
suspiciously small.

**Decisions made that weren't already pinned in the plan:**
- Central cache location: `%LOCALAPPDATA%\pixet\{index,thumbs}.db` (`util/AppPaths`).
  macOS equivalent (`~/Library/Application Support/pixet`) is a P5 addition to that file.
- `files.state` gets a 4th value beyond the plan's 0/1/2 sketch: `3 = Unsupported`
  (format has no decoder yet, e.g. everything but JPEG right now), distinct from
  `2 = Failed` (decode attempted, file is corrupt/truncated). Matters for P4: when a
  decoder lands for a format, those rows should be retried, unlike genuine failures.
- Non-media files (sidecars, `Thumbs.db`, `.xmp`, random junk) are never inserted into
  `files` at all - `classifyFormat` returns `Unknown` and Pass A skips them, rather than
  cluttering the index with permanently-`Unsupported` rows.
- JPEG-only in P1 (as the plan allowed, conditional on the real library's format mix -
  haven't measured that yet, see below). PNG/HEIC/RAW/TIFF/WebP/AVIF all classify
  correctly and land in `files` with `state=Unsupported`, ready to pick up in P4 without a
  schema change.
- `ThumbTier` collapses the plan's "scaled DCT vs full decode" distinction into one
  `Decoded` tier for JPEG, since `decodeJpeg()` always tries scaled-DCT and transparently
  falls back to native-resolution decode (denom=1) when the image is already small -
  there's no separate code path to distinguish. The "full decode + Lanczos" tier the plan
  describes is for *other* formats (PNG/TIFF/...) in P4, not JPEG.
- Box-filter downscale only *downscales* - a thumbnail smaller than the 320px target
  (e.g. an embedded EXIF preview, or a genuinely small source image) is stored as-is
  rather than upscaled. Verified by `ThumbGeneratorDoesNotUpscaleSmallJpeg`.
- Vanished-file cleanup on rescan is a hard delete (file row + its thumb blob), not a
  tombstone - simplest correct behavior for v1.
- Single-threaded for P1. Decode is CPU-bound and embarrassingly parallel across files -
  real headroom for a worker-pool optimization once we have a baseline number to compare
  against.
- Cross-database transactions (`files` in `index.db`, `thumbs` in the attached
  `thumbs.db`, same connection, same `BEGIN`/`COMMIT`) are not perfectly atomic across a
  crash in WAL mode per SQLite's own docs (two-phase commit across attached WAL files has
  a narrow crash window). Accepted risk for a thumbnail cache - worst case is an orphaned
  blob or a dangling `thumb_id`, both harmless and self-healing on next rescan.

**Verified:**
- 15/15 unit tests (`tests/pixet_tests.exe`, wired to `ctest`): schema creation +
  idempotency + the thumbs-table regression above, format classification, claim
  acquire/renew/block/steal-when-stale/release, EXIF orientation + embedded-thumbnail
  extraction (via a self-consistent hand-built TIFF/IFD fixture, not hardcoded offsets),
  tier selection, no-upscale behavior, JPEG round-trip.
- Smoke-tested `pixet-index` against `C:\Windows\Web` (36 real JPEGs, 14 directories,
  mixed sizes up to 4K wallpapers): clean run, 8 embedded-preview / 28 decoded / 0
  unsupported / 0 failed. Second run: 14/14 dirs fresh-skipped, 0.0s. `--force`: correctly
  re-walks (bypasses the mtime shortcut) but doesn't re-thumbnail unchanged files (0 new,
  0 removed, 0 thumbnails touched) - confirms `--force` means "re-verify freshness," not
  "redo everything."
- Not yet done: the real throughput benchmark against the 800GB library (the actual P1
  gate) - haven't run it, don't know the format mix (JPEG-only vs RAW-heavy) yet. Also
  not yet exercised: true multi-process concurrent indexing (claims are unit-tested in
  isolation, not yet run as two real `pixet-index` processes racing on one tree), or
  crash-mid-run recovery live (stale-claim-steal is unit-tested, not battle-tested).
- Next: run the benchmark, see what the format mix and files/sec actually look like, and
  decide from real numbers whether LibRaw needs to move up from "P4" to "finish P1."

---

## 2026-08-08 — desktop — VS Code run/debug configs

- Added `.vscode/{launch.json,tasks.json,settings.json,extensions.json}`. CMake Tools +
  C/C++ (cpptools) were already installed on this machine, so wired through those rather
  than a parallel setup: `settings.json` pins `cmake.configurePreset`/`buildPreset` to
  `debug` (matches `CMakePresets.json`), `tasks.json` has build tasks per preset,
  `launch.json` has `cppvsdbg` debug configs for both `pixet` and `pixet-index`, debug and
  release.
- **Also fixed a real gap in `CMakePresets.json`**: added a `vendor` block
  (`microsoft.com/VisualStudioSettings/CMake/1.0`) to the base preset. Without it, CMake
  Tools configuring through the IDE hits the same "`cl.exe`/`CMAKE_CXX_COMPILER` not
  found" failure the CLI did before we knew to enter the VS dev shell manually — this
  `vendor` key is the documented mechanism that makes VS Code (and Visual Studio) prompt
  for/auto-inject the MSVC environment for a Ninja+presets project. Confirmed
  `cmake --preset debug` still reconfigures cleanly from the CLI after adding it.
  Reconfirmed `pixet.exe`/`pixet-index.exe` land exactly where `launch.json` expects
  (`build/debug/src/app/`, `build/debug/src/index/`).
- `launch.json`'s `PATH` override for `pixet.exe` (`C:\Qt\6.8.3\msvc2022_64\bin`) is
  hardcoded to match `CMakePresets.json`'s default Qt path — needed since we haven't
  wired `windeployqt` yet. If a machine needs the `CMakeUserPresets.json` Qt-path
  override (see earlier entry), also update this PATH locally in `launch.json` (don't
  commit a machine-specific path over the shared one).

---

## 2026-08-08 — desktop — P0 verified: hello-world Qt window builds and runs

- VS Build Tools finished installing. Confirmed via `vswhere.exe -requires
  Microsoft.VisualStudio.Component.VC.Tools.x86.x64`.
- **Gotcha for the other machine:** a plain new PowerShell window does not have `cl.exe`
  or the MSVC env on `PATH` — you must enter the VS dev shell first:
  ```powershell
  $vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
  Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
  Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
  ```
  `Enter-VsDevShell` **replaces** `$env:Path` rather than extending your existing one, so
  cmake/ninja (installed to user-scope winget paths) disappear from `PATH` after running
  it. Re-merge the registry `PATH` back in afterward:
  ```powershell
  $env:Path = $env:Path + ";" + [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
  ```
  Added this sequence to `SETUP.md`.
- **Fixed a vcpkg.json bug found during first configure:** `libheif`'s dependency on
  `libde265` is a mandatory base dependency of the port, not an optional feature —
  `"features": ["libde265"]` fails with "does not have required feature". Fixed to
  `{ "name": "libheif", "default-features": false }` (drops the `hevc`/x265 default
  feature, which is for *encoding* HEIC — irrelevant, we only decode).
- `cmake --preset debug` (first run, builds the full vcpkg dependency list from source —
  sqlite3, libjpeg-turbo, libpng, tiff, webp, avif, libraw, libheif+libde265, ffmpeg):
  **~28 minutes.** Should be near-instant on the second machine's first run too once vcpkg
  populates its binary cache, but budget for ~30 min on a truly cold vcpkg cache.
- `cmake --build build/debug`: fast, as expected (only our placeholder sources compile).
- **P0 gate passed:** launched `build/debug/src/app/pixet.exe`, confirmed a window titled
  "pixet 0.1.0" actually renders (`Get-Process pixet | Select MainWindowTitle` +
  screenshot). `pixet-index.exe` prints its version stub correctly.
- Next: start P1 — schema/migrations, directory walker, claims table, JPEG decode path
  (embedded preview → scaled DCT), benchmark against the real 800GB library.

---

## 2026-08-08 — desktop — P0 toolchain bootstrap (part 1)

- Machine started with zero toolchain: no compiler, CMake, vcpkg, or Qt. `python` was
  just the Windows Store stub.
- Installed per-user (no admin needed, contrary to plan's assumption): Python 3.12 via
  `winget install Python.Python.3.12 --scope user`, CMake 4.4.2 and Ninja 1.13.2 via
  `winget install ... --scope user`.
- **VS 2022 Build Tools is the one piece that does need admin.** Handed the user this
  command to run in an elevated shell (not yet confirmed run as of this entry):
  ```
  winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
  ```
- vcpkg added as a **git submodule** (not a plain clone — a plain `git clone` leaves a
  nested `.git` that confuses the parent repo). Pinned at commit
  `c4d9956c0c10a4742840a5e7d93efa2e0015c865` (2026-08-07), which is also `vcpkg.json`'s
  `builtin-baseline` — this is what makes dependency resolution identical on both
  machines. Bootstrapped via `bootstrap-vcpkg.bat -disableMetrics` (downloads a prebuilt
  vcpkg.exe, no compiler needed for this step).
- Qt installed via `aqtinstall` (pip package, not the official GUI installer) — no Qt
  account needed, scriptable. **Deviation from plan:** the latest versions (6.12.0,
  6.11.1) are listed by `aqt list-qt` but their metadata XML 404s — not fully mirrored
  yet. Landed on **6.8.3 LTS** at `C:\Qt\6.8.3\msvc2022_64` instead. If the other machine
  hits the same mirror gap, check `aqt list-qt windows desktop` for what's actually
  installable before assuming 6.8.3 is still the right call.
- `CMakePresets.json` hardcodes `CMAKE_PREFIX_PATH` to `C:/Qt/6.8.3/msvc2022_64`. If Qt
  lands at a different version/path on the other machine, override locally via
  `CMakeUserPresets.json` (gitignored) rather than editing the committed preset.
- Repo scaffolded per the plan: `src/core` (static lib, placeholder `version.h/.cpp`
  only — real db/scan/decode/thumb/meta code is P1), `src/index` (stub `pixet-index`
  CLI), `src/app` (minimal `pixet` GUI — bare `QMainWindow`, this is the P0
  hello-world gate), `tests/` (empty, P1 fills it in).
- **Blocked on:** user running the elevated VS Build Tools command above. Once that's
  done, next step is `cmake --preset debug && cmake --build build/debug` and confirm the
  `pixet` window actually opens — that's the P0 exit gate, not yet verified.
- Wrote up every command above as a runbook in `SETUP.md` so the second machine doesn't
  need to reconstruct this from the devlog prose. If a step's exact command changes going
  forward, update `SETUP.md`, not just this log.

- Stack: C++20 + Qt6 Widgets (not Python) — startup time and native decode access matter
  more than dev speed for a tool used daily; matches the existing C++/vcpkg .gitignore.
- Video: thumbnails + poster frame only in v1, no playback engine — full A/V playback
  roughly doubles the video work and isn't needed for "browse a folder".
- Formats: JPEG/PNG, HEIC/HEIF, camera RAW, TIFF/WebP/AVIF.
- Platforms: Windows first (this machine currently has zero toolchain installed — no
  compiler/CMake/vcpkg/Qt), macOS shortly after; portable from commit 1 (CMake + vcpkg,
  no platform-specific code outside a thin shim layer).
- Speed strategy: avoid full decodes wherever possible — embedded previews (EXIF/HEIF
  thumb/LibRaw unpack_thumb) first, scaled-DCT libjpeg-turbo decode second, full decode
  as last resort. This is the single biggest lever at 800GB.
- Storage: SQLite WAL, two DB files (index.db small/hot, thumbs.db blobs, ATTACHed).
  File identity = (dir_id, name), validity = (mtime, size). No content hashing in v1.
- Concurrency: directory-level claims table, not file-level — coarse enough to avoid
  contention, self-heals via heartbeat if an indexer is killed.
- Indexing model: no mandatory pre-index step. Browsing a folder indexes/thumbnails it
  on the spot (non-recursive); explicit Refresh does the more expensive re-stat-everything
  staleness check. `pixet-index` CLI becomes an optional whole-tree pre-warm, not a
  prerequisite.
- Qt installed via the official online installer, not vcpkg (building qtbase from vcpkg
  source takes hours).
- Full plan: see the plan file this session produced (architecture, schema, phases,
  verification) — not duplicated here to avoid drift between two copies.
- Next: P0 toolchain bootstrap on this machine (nothing installed yet).
