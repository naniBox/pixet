# pixet devlog

Running log of decisions and status, kept because development happens across two
machines. Newest entry on top. Append, don't rewrite history.

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
