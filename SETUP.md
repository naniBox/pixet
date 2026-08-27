# pixet dev environment setup

Toolchain bootstrap for a fresh machine.

- **Windows** — steps 0-8 below. Run top to bottom. Everything except step 3 (VS Build
  Tools) is per-user and needs no admin rights. Steps 0-7 get you building and debugging;
  step 8 is only needed if you want to produce the installer.
- **macOS (Apple Silicon)** — jump to [macOS](#macos-apple-silicon) near the end. Much
  shorter: no admin, no dev-shell, and the compiler comes from the Command Line Tools.

See `devlog.md` for why these specific choices (Qt version, vcpkg pinning, etc.).

## 0. Clone

```powershell
git clone --recurse-submodules https://github.com/<you>/pixet.git
cd pixet
```

If you already cloned without `--recurse-submodules`:

```powershell
git submodule update --init
```

## 1. Python (needed only as a vehicle for `aqtinstall`, step 5)

```powershell
winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements
```

Installer adds Python to `PATH`, but **the change doesn't reach your current shell** —
open a new PowerShell window (or run the `$env:Path = ...` refresh below) before the next
steps.

```powershell
$env:Path = [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")
py --version   # sanity check
```

## 2. CMake + Ninja

```powershell
winget install -e --id Kitware.CMake --scope user --accept-package-agreements --accept-source-agreements
winget install -e --id Ninja-build.Ninja --scope user --accept-package-agreements --accept-source-agreements
```

Refresh `PATH` again (new shell, or re-run the `$env:Path = ...` line above), then:

```powershell
cmake --version
ninja --version
```

## 3. VS 2022 Build Tools — **the one step that needs admin**

Open PowerShell **as Administrator**, then:

```powershell
winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --quiet --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

Unattended, ~15-25 min, a few GB (MSVC v143 toolset + Windows 11 SDK). Verify afterward
(new shell):

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
```

should print an install path, not an error.

## 4. vcpkg

Already present as a git submodule (see step 0), pinned to the commit in
`vcpkg.json`'s `builtin-baseline` — that pin is what keeps dependency resolution
identical between machines, so don't `git submodule update --remote` casually.

```powershell
cd vcpkg
.\bootstrap-vcpkg.bat -disableMetrics
cd ..
```

Downloads a prebuilt `vcpkg.exe` — no compiler needed for this step, so it's fine to run
before step 3 finishes.

## 5. Qt6

Installed via `aqtinstall` (pip package), not the official GUI installer — no Qt account
needed, and it's scriptable.

```powershell
py -m pip install --user aqtinstall
$env:Path += ";C:\Users\$env:USERNAME\AppData\Roaming\Python\Python312\Scripts"
```

**Before installing, check what's actually available** — `aqt list-qt` can list versions
whose mirror metadata isn't live yet (hit this on the first machine: 6.12.0 and 6.11.1
both 404'd). Confirm the target version resolves:

```powershell
aqt list-qt windows desktop
```

Then install (adjust the version if 6.8.3 also turns out to be stale by the time you run
this — check `devlog.md` for the latest known-good version first):

```powershell
aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir C:\Qt
```

`qtbase` (the default module set) already bundles Widgets, Sql, and Concurrent — no
`--modules` flag needed.

**If you land on a different Qt version or path than `C:\Qt\6.8.3\msvc2022_64`**, don't
edit `CMakePresets.json` (it's committed and shared). Instead create
`CMakeUserPresets.json` in the repo root (gitignored) inheriting from the committed preset
and overriding `CMAKE_PREFIX_PATH`:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "debug",
      "inherits": "debug",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "C:/Qt/<your-version>/msvc2022_64" }
    }
  ]
}
```

## 6. Build and verify

A plain PowerShell window does **not** have `cl.exe`/MSVC on `PATH` even after step 3 —
you need to enter the VS dev shell first. `Enter-VsDevShell` **replaces** `$env:Path`
rather than extending it, so cmake/ninja (installed to user-scope winget paths in steps 1-2)
disappear from `PATH` afterward — merge the registry `PATH` back in:

```powershell
$vsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
Import-Module "$vsPath\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
$env:Path = $env:Path + ";" + [System.Environment]::GetEnvironmentVariable("Path","Machine") + ";" + [System.Environment]::GetEnvironmentVariable("Path","User")

cmake --preset debug
cmake --build build/debug
```

The first `cmake --preset debug` also triggers vcpkg building the full dependency list
from source (sqlite3, libjpeg-turbo, libpng, tiff, webp, avif, libraw, libheif+libde265,
ffmpeg) — **budget ~30 minutes** on a cold vcpkg binary cache. `cmake --build` itself is
fast once dependencies are built.

**To avoid that 30 minutes**, use `scripts/configure.ps1`/`scripts/build.ps1` instead of
calling `cmake` directly (see step 7 below) — they point `VCPKG_BINARY_SOURCES` at
`\\kioku\talsit\code\pixet\vcpkg-cache`, a shared network cache both machines can read
from and write to. If the other machine already built a given package version, this
machine restores it from the share instead of rebuilding from source. It's a plain
network path, not synced storage — nothing moves between machines except through this
cache lookup, so there's no risk of it silently going stale or ballooning a sync folder.

`build/debug/src/app/pixet.exe` should launch a window titled "pixet <version>", where the
version comes from `project(pixet VERSION ...)` in the top-level `CMakeLists.txt` — that is
the single source of truth every version string in the repo reads from (`src/core/version.cpp`
via the `PIXET_VERSION` compile definition, the macOS bundle's `CFBundleShortVersionString`,
and the installer filename), so it is the one place to bump.

A locally built `pixet.exe` needs `C:\Qt\6.8.3\msvc2022_64\bin` on `PATH`, or to be run from
an IDE that sets it (`.vscode/launch.json` does). `windeployqt` *is* wired up now, but only in
the installer path (step 8) — dev builds deliberately stay un-deployed so an incremental
rebuild never has to re-copy Qt. `build/debug/src/index/pixet-index.exe --help` should print
its usage.

Then run the tests:

```powershell
ctest --test-dir build/debug --output-on-failure
```

`build\debug\tests\pixet_tests.exe` is the same binary run directly, with more readable
output on failure.

## 7. VS Code run/debug (recommended over the manual CLI steps above)

The repo has `.vscode/launch.json`, `tasks.json`, `settings.json` committed. Install the
recommended extensions (VS Code will prompt via `.vscode/extensions.json`, or manually:
`ms-vscode.cmake-tools`, `ms-vscode.cpptools`), open the folder, and press **F5** — pick
"pixet (debug)" or "pixet-index (debug)".

The F5 build task (`.vscode/tasks.json`) runs `scripts/build.ps1`, which explicitly
enters the VS dev shell itself before calling `cmake --build` — same
`Enter-VsDevShell`/PATH-merge dance as step 6, just scripted. This is deliberate, not
just convenience: `CMakePresets.json`'s `vendor` block *can* make CMake Tools
auto-inject the MSVC environment on its own, but only reliably for a build directory
CMake Tools configured itself. If the build directory was ever configured by running
`cmake --preset` directly from a terminal (as this doc's step 6 does, and as this repo's
own history did), F5 can silently build with no `INCLUDE`/`LIB` set at all — invisible
until a file actually needs recompiling, then fails with something like
`Cannot open include file: 'type_traits'`. `scripts/build.ps1` sidesteps that
inconsistency entirely rather than depending on it.

Prefer `scripts/configure.ps1 -Preset debug` over a bare `cmake --preset debug` for the
same reason step 6's manual dance exists, plus it wires up the shared vcpkg cache
(`scripts/vcpkg-cache-env.ps1`) — see the note under step 6 above.

If `pixet.exe` fails to *launch* (not build) with a missing-DLL error, check
`launch.json`'s `PATH` override still points at your actual Qt install (see the
`CMakeUserPresets.json` note above if yours differs from `C:\Qt\6.8.3\msvc2022_64`).

## 8. Building an installer you can give to someone else (Windows)

Only needed when you want to hand pixet to someone who doesn't have a build tree — the
Windows counterpart to macOS' `deploy-mac.sh` (step 6 of the macOS section).

One extra tool, per-user and no admin:

```powershell
winget install -e --id JRSoftware.InnoSetup --scope user --accept-package-agreements --accept-source-agreements
```

`scripts/deploy-windows.ps1` looks for the compiler at
`%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe`, which is where the user-scope winget
install puts it. If you installed Inno Setup machine-wide instead (Program Files), edit the
`$iscc` path in that script.

```powershell
./scripts/deploy-windows.ps1
```

That is the whole thing — it does not reuse an existing `build/release` tree, it drives the
full sequence itself:

1. **Configure + build release with `-DPIXET_DEBUG_MENU=OFF`.** The `&Debug` menu ("Copy
   Grid Debug Info") is deliberately kept in local release builds, but has no business in an
   installer handed to someone else. Building through this script is the only way it gets
   compiled out.
2. **Stage into `build/win-deploy`** (wiped and recreated each run, so a stale DLL from a
   previous Qt version can never leak into a build). Windows needs *two* sets of DLLs
   carried, unlike macOS where the vcpkg triplet is static and only Qt needs bundling:
   vcpkg's own (sqlite3, ffmpeg, libraw, libheif, ...) are already copied next to
   `pixet.exe` by vcpkg's applocal step during every normal build, and Qt's are added here.
   `pixet-index.exe` is staged alongside — it links only `pixet_core`, not Qt, so it needs
   nothing extra.
3. **Run `windeployqt`, then trim its output** to what pixet actually loads — the same
   treatment [deploy-mac.sh](scripts/deploy-mac.sh) gives the `.app`, and for the same
   reason: windeployqt's defaults assume an app that might render through any backend and
   decode images with Qt, and pixet does neither. 115 MB staged becomes 73 MB, which is a
   51 MB installer down to 40 MB:

   | Dropped | Saves | Why it's safe |
   |---|---|---|
   | `opengl32sw.dll` (`--no-opengl-sw`) | 20 MB | Mesa's software GL rasterizer, a fallback for machines with no usable GL driver. Only a Qt Quick / `QOpenGLWidget` app can reach it; pixet links `Qt6::Widgets` alone and paints through the raster engine. |
   | `dxcompiler.dll` + `dxil.dll` (`--no-system-dxc-compiler`) | 13.5 MB | Shader compilers for Qt RHI's D3D12 backend. Nothing here uses RHI. |
   | `d3dcompiler_47.dll` (`--no-system-d3d-compiler`) | 4.7 MB | Same, for RHI's D3D11 backend. |
   | `imageformats/`, `iconengines/`, `generic/`, `networkinformation/`, `tls/` | 1.3 MB | Every decode is pixet's own codec via `rgbImageToQImage`, the app icon is a `.ico`/`.png` resource, and pixet makes no network calls. Kept as an allowlist (`platforms`, `styles`) rather than a delete-list, so a future Qt adding new default plugins can't silently re-inflate the installer. |
   | `Qt6Network.dll`, `Qt6Svg.dll` | 2.2 MB | Derived, not hardcoded: both are dead only as a *consequence* of the plugins above, so the script re-derives the answer from import tables (`dumpbin /dependents`) each run rather than naming them. |

   `platforms\qwindows.dll` is what makes the app start at all and
   `styles\qmodernwindowsstyle.dll` is the native Windows 11 look — dropping either is
   visible immediately, so both are on the keep list. The script asserts `Qt6Core.dll` and
   `platforms\qwindows.dll` survived, so a silently no-op'd deploy fails here rather than on
   someone else's machine.
4. **Compile the installer** with `ISCC.exe` from `scripts/pixet.iss`, passing the version
   from `CMakeLists.txt` via `/DMyAppVersion=`. Output: `build/pixet-<version>-setup.exe`.

What the resulting installer does, all of it per-user (`PrivilegesRequired=lowest`, so no
UAC prompt):

- Presents the Apache 2.0 license from the repo root `LICENSE` and will not continue until
  it is accepted — "I do not accept" is preselected and Next stays disabled. This is the
  Windows half of showing the license; macOS has no installer to host a page like it and
  asks on first run instead (see [src/app/LicenseDialog.h](src/app/LicenseDialog.h)).
- Installs into `{userpf}\pixet` (`%LOCALAPPDATA%\Programs\pixet`), with a Start Menu entry
  and an optional desktop shortcut.
- Keeps a fixed `AppId`, so installing a newer build upgrades in place instead of leaving a
  second copy. Don't regenerate that GUID.
- Optionally registers pixet as an *available* handler for the still-image formats it
  decodes (JPEG, PNG, HEIC, RAW, TIFF, WebP, AVIF — video is deliberately excluded, since
  pixet already has a "use the system video player" preference). This does **not** steal any
  existing default: Windows hash-protects the user's chosen default by design, so the user
  still has to pick pixet once via Open-with or Settings. All entries are HKCU and are
  removed on uninstall.
- Ships `pixet-index.exe` in the same folder as the GUI, so the standalone indexer can't get
  separated from the app it shares a database format with. It's useful on its own for
  pre-indexing a library from Task Scheduler, or running `--render-raws` as a separate pass:

  ```powershell
  & "$env:LOCALAPPDATA\Programs\pixet\pixet-index.exe" --help
  ```

- Runs the Visual C++ redistributable installer before finishing. `pixet.exe` genuinely
  needs it (`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` — this build's CRT is
  dynamic, not static), and Microsoft's installer no-ops quickly when an equal-or-newer
  runtime is already present.

  Worth knowing, because nothing in this repo says so: **`vc_redist.x64.exe` is staged by
  `windeployqt`, not by `deploy-windows.ps1`.** windeployqt copies it out of the VS install
  alongside the Qt DLLs (look for `Updating vc_redist.x64.exe.` in its output), which is the
  only reason `pixet.iss`' `Source: ..\build\win-deploy\vc_redist.x64.exe` line resolves.
  So it is an implicit dependency on windeployqt's behaviour: if a future Qt stops doing
  that, or someone adds `--no-compiler-runtime`, ISCC starts failing with a
  missing-source-file error and `deploy-windows.ps1` is where the fetch would have to be
  added.

The recipient gets no Gatekeeper-style friction (unlike the macOS DMG), but the installer is
unsigned, so SmartScreen will show a "Windows protected your PC" warning on first run until
enough people install it — "More info" then "Run anyway" gets past it.

### The full pre-release check

```powershell
./scripts/win-e2e.ps1
```

Chains what this project's workflow does by hand after any real change, stopping at the first
failure: configure + build debug, run `pixet_tests.exe`, then `deploy-windows.ps1` (release
build + installer), then the release `pixet_tests.exe`. Nothing new in it — it just calls the
scripts above in order.

## macOS (Apple Silicon)

Substantially shorter than the Windows path: clang ships with the Command Line Tools, so
there's no dev-shell dance and nothing needs admin rights.

### 1. Prerequisites

```bash
xcode-select --install          # Command Line Tools - full Xcode is NOT required
brew install cmake ninja nasm
brew install autoconf automake libtool   # insurance: some vcpkg ports use autotools
```

`nasm` is genuinely required, not precautionary: vcpkg's `vcpkg_find_acquire_program(NASM)`
only auto-downloads on Windows hosts, so the ffmpeg port fails without a system nasm.

### 2. vcpkg

```bash
git submodule update --init
./vcpkg/bootstrap-vcpkg.sh -disableMetrics    # the .sh, not the .bat from step 4 above
```

### 3. Qt 6.8.3

Same version and same `aqtinstall` route as Windows, to keep the two machines comparable:

```bash
pip3 install --user aqtinstall
aqt list-qt mac desktop                       # confirm 6.8.3 resolves before installing
aqt install-qt mac desktop 6.8.3 clang_64 --outputdir ~/Qt
```

That lands at `~/Qt/6.8.3/macos`, which is exactly what `CMakePresets.json`'s `mac-base`
expects — no `CMakeUserPresets.json` needed. (The directory is named `macos` even though
the architecture argument is `clang_64`.) The official build is a universal
x86_64+arm64 binary with a macOS 12 floor, which is what makes a distributable app
possible at all — see step 6.

If you land on a different version, don't edit the committed preset; add a gitignored
`CMakeUserPresets.json` overriding `CMAKE_PREFIX_PATH`, exactly as the Windows note above
describes.

### 4. Build and verify

```bash
./scripts/configure.sh mac-debug     # first run also builds every vcpkg dependency
./scripts/build.sh mac-debug
ctest --test-dir build/mac-debug
open build/mac-debug/src/app/pixet.app
```

Budget **~30 minutes and 10-15GB** for that first configure — it builds sqlite3,
libjpeg-turbo, libpng, tiff, webp, avif, libraw, libheif+libde265 and ffmpeg from source.
`vcpkg/buildtrees` is not cleaned up automatically and is safe to delete afterwards.

Unlike the Windows scripts, `configure.sh`/`build.sh` do **not** use the `\\kioku` shared
binary cache. A cache entry's ABI hash includes the compiler and triplet, so nothing an
MSVC/x64-windows build ever pushed there is a hit for an Apple-clang/arm64-osx build. Set
`PIXET_VCPKG_CACHE_DIR` to a mounted share if you ever have a second Mac.

### 5. VS Code

Pick the `mac-debug` preset when CMake Tools prompts. `launch.json` has macOS entries using
LLDB — install `vadimcn.vscode-lldb` alongside the C++ extensions. Note the debug target is
inside the bundle: `build/mac-debug/src/app/pixet.app/Contents/MacOS/pixet`.

### 6. Building something you can give to someone else

The macOS counterpart to step 8 above (which does the same job on Windows, via Inno Setup).

```bash
./scripts/deploy-mac.sh
```

Produces `build/pixet-<version>-arm64.dmg`: a release build with the `&Debug` menu compiled
out, Qt frameworks embedded by `macdeployqt`, ad-hoc signed, drag-to-Applications DMG. The
codec libraries are statically linked (see `triplets/arm64-osx-pixet.cmake`), so Qt is the
only thing that needs embedding.

The script also trims the bundle, which `macdeployqt` alone does not do — 91 MB down to 68 MB
(33 MB compressed), all of it verified by launching the result with `~/Qt` moved aside:

| Step | Saves | Why it's safe |
|---|---|---|
| Drop the `imageformats` + `iconengines` plugins and `QtSvg` | 2.7 MB | Every decode is pixet's own codec via `rgbImageToQImage` — the app never asks Qt to read an image file. `QtSvg` is derived as dead, not hardcoded, so the check keeps working if the plugin set changes. |
| Thin universal Qt binaries to `arm64` | 23 MB | Qt's official macOS binaries are universal; ours aren't (the vcpkg triplet is `arm64-osx`), so the x86_64 halves could never execute. Skipped automatically if `CMAKE_OSX_ARCHITECTURES` is ever widened. |
| `strip -x` both executables | 3 MB | Distribution-only, so local builds stay debuggable in lldb. |

`QtDBus` looks unused and is not — `QtGui` links it directly in the official macOS build.
`Qt6::Sql` and `Qt6::Concurrent` *were* genuinely unused and have been removed from the link
line, which is what stops `macdeployqt` deploying the Postgres and Mimer SQL driver plugins to
users of a local photo viewer.

**`pixet-index` ships inside the bundle** at `Contents/MacOS/pixet-index`, signed with it, so
drag-installing carries it along and it can't get separated from the GUI it shares a database
format with. It links only system frameworks — no Qt, no rpath, no bundle — so it runs from
anywhere. To use it by name:

```bash
sudo ln -sf /Applications/pixet.app/Contents/MacOS/pixet-index /usr/local/bin/pixet-index
pixet-index --help
pixet-index ~/Pictures/some-folder --no-recurse
```

It shares `~/Library/Application Support/pixet/{index.db,thumbs.db}` with the GUI, so a
whole-tree pre-warm from the command line is immediately visible in the app. Both can run at
once — directory-level claims keep them off each other's work.

**What the recipient experiences, and why.** An ad-hoc-signed app is fine to run locally but
a downloaded DMG picks up the `com.apple.quarantine` attribute, and Gatekeeper refuses
anything that isn't Developer-ID-signed *and* notarized. So they must:

1. open it once and let it be refused, then
2. **System Settings → Privacy & Security → "Open Anyway"**.

The old Control-click → Open shortcut stopped working as a bypass in macOS 15. The terminal
equivalent is `xattr -dr com.apple.quarantine /Applications/pixet.app`.

To remove that friction entirely you need an Apple Developer Program membership ($99/year),
after which it's environment variables only — no script edits:

```bash
PIXET_CODESIGN_IDENTITY="Developer ID Application: NAME (TEAMID)" \
PIXET_NOTARY_PROFILE=my-profile ./scripts/deploy-mac.sh
```

The build targets **macOS 12+** and **arm64 only**. Both are deliberate and both are set in
one place each: `CMAKE_OSX_DEPLOYMENT_TARGET` in the top-level `CMakeLists.txt` and
`VCPKG_OSX_DEPLOYMENT_TARGET` in `triplets/arm64-osx-pixet.cmake` — they must agree. Adding
Intel means `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` plus an `x64-osx` dependency build.

### 7. First-launch permissions

macOS will ask for access to `~/Pictures`, `~/Documents` and `~/Downloads` the first time
pixet reads them. Two things worth knowing so neither reads as a bug:

- **The prompts come back after a rebuild.** TCC keys the grant to the code signature, and
  an ad-hoc signature is different on every build. A Developer ID signature is stable and
  fixes this as a side effect.
- **Folders you never grant are skipped, not fatal.** `pixet-index` reports them in its
  summary as `unreadable`, and a whole-`$HOME` index will legitimately have a nonzero count
  there. If you want to index broadly without answering prompts one at a time, grant Full
  Disk Access to the app in System Settings → Privacy & Security.

## Troubleshooting

- **`winget install` command not found**: winget ships with Windows 11 by default: update
  App Installer from the Microsoft Store if missing.
- **A `winget install --scope user` step fails/needs admin anyway**: some packages don't
  support user-scope installs on every Windows build. Falling back to an elevated install
  for that one package is fine — it doesn't affect anything else in this doc.
- **PATH not picking up a freshly installed tool**: `winget`-installed tools update the
  registry's PATH but not your already-open shell's environment. Open a new terminal, or
  re-run the `$env:Path = [System.Environment]::GetEnvironmentVariable(...)` refresh line.
- **`deploy-windows.ps1` says `ISCC.exe (Inno Setup) not found`**: either Inno Setup isn't
  installed (step 8) or it went to a machine-wide location; the script only looks in
  `%LOCALAPPDATA%\Programs\Inno Setup 6`.
- **`deploy-windows.ps1` fails compiling the installer on a missing `vc_redist.x64.exe`**:
  that file comes from `windeployqt`, not from the deploy script — check its output for
  `Updating vc_redist.x64.exe.` (see step 8). Dropping a copy into `build\win-deploy` by hand
  does not survive, since the script wipes that folder at the start of every run.
- **The app builds but won't launch with a missing-DLL error**: a dev build is not
  self-contained. Either put `C:\Qt\6.8.3\msvc2022_64\bin` on `PATH`, launch via F5 (step
  7), or use the installer output (step 8), which is the only fully deployed build.
