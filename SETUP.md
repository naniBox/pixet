# pixet dev environment setup

Toolchain bootstrap for a fresh Windows machine. Run top to bottom. Everything except
step 3 (VS Build Tools) is per-user and needs no admin rights.

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

`build/debug/src/app/pixet.exe` should launch a window titled "pixet 0.1.0" — that's the
P0 exit gate (needs `C:\Qt\6.8.3\msvc2022_64\bin` on `PATH`, or run from inside an IDE
that sets it, since we haven't wired `windeployqt` yet). `build/debug/src/index/pixet-index.exe`
should print a version stub.

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

If `pixet.exe` fails to *launch* (not build) with a missing-DLL error, check
`launch.json`'s `PATH` override still points at your actual Qt install (see the
`CMakeUserPresets.json` note above if yours differs from `C:\Qt\6.8.3\msvc2022_64`).

## Troubleshooting

- **`winget install` command not found**: winget ships with Windows 11 by default: update
  App Installer from the Microsoft Store if missing.
- **A `winget install --scope user` step fails/needs admin anyway**: some packages don't
  support user-scope installs on every Windows build. Falling back to an elevated install
  for that one package is fine — it doesn't affect anything else in this doc.
- **PATH not picking up a freshly installed tool**: `winget`-installed tools update the
  registry's PATH but not your already-open shell's environment. Open a new terminal, or
  re-run the `$env:Path = [System.Environment]::GetEnvironmentVariable(...)` refresh line.
