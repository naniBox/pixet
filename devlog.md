# pixet devlog

Running log of decisions and status, kept because development happens across two
machines. Newest entry on top. Append, don't rewrite history.

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
