; Inno Setup script for pixet's Windows installer. Compiled by deploy-windows.ps1, which
; passes the real version via /DMyAppVersion=X.Y.Z (see CMakeLists.txt's project(VERSION) -
; the single source of truth every other version string in this repo already reads from).
; The #ifndef fallback below only matters if someone runs ISCC.exe on this file directly.
;
; Counterpart to scripts/deploy-mac.sh's DMG - see that script's own header for why macOS
; needs no equivalent of Windows' DLL-bundling problem (its vcpkg triplet is static) while
; Windows' x64-windows triplet is dynamic, so a Windows deploy has *two* sets of DLLs to
; carry: vcpkg's (sqlite3, ffmpeg, libraw, ...), already copied next to pixet.exe by
; vcpkg's own applocal step at build time, and Qt's, added by windeployqt in
; deploy-windows.ps1 before this script ever runs - by the time ISCC.exe sees it, the
; staged folder is already fully self-contained and this script just packages it as-is.
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif

#define MyAppName "pixet"
#define MyAppPublisher "naniBox"
#define MyAppExeName "pixet.exe"
#define MyAppURL "https://github.com/naniBox/pixet"

[Setup]
; Stable across every release - this is what lets Inno Setup detect "pixet is already
; installed" and offer an upgrade instead of a side-by-side second copy. Generated once;
; never regenerate it. The leading {{ is not a typo - Inno Setup treats a bare {...} as
; a constant reference even here, so a literal opening brace needs escaping (closing
; braces are unambiguous and don't).
AppId={{B6935046-EE96-4735-B8F5-BAC4E4ED4F22}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
; Per-user install (no admin/UAC prompt) into the per-user Program Files equivalent -
; matches how the app has run as a local dev build all along, and how Inno Setup itself
; was installed for this session (winget, no elevation). {autopf} would need admin;
; {userpf} doesn't.
DefaultDirName={userpf}\{#MyAppName}
DefaultGroupName={#MyAppName}
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\src\app\pixet.ico
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Staged by deploy-windows.ps1 - the whole self-contained folder (exe + every DLL).
OutputDir=..\build
OutputBaseFilename=pixet-{#MyAppVersion}-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Tells Explorer/the shell to refresh its file-association caches after install (and
; after uninstall, when the [Registry] entries below get torn down) - without this an
; already-open Explorer window can keep showing stale "Open with" suggestions until a
; manual refresh or reboot.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "fileassoc"; Description: "Register {#MyAppName} as an app for opening image files (JPEG, PNG, HEIC, RAW, TIFF, WebP, AVIF)"; GroupDescription: "File associations:"

[Files]
; recursesubdirs/createallsubdirs: windeployqt drops the platforms/imageformats/styles
; plugin subfolders alongside the DLLs, not just flat files next to the exe.
;
; vc_redist.x64.exe is excluded from this bulk copy and staged separately below - pixet.exe
; genuinely needs it (confirmed via dumpbin /dependents: MSVCP140.dll, VCRUNTIME140.dll,
; VCRUNTIME140_1.dll are real, non-Qt, non-vcpkg dependencies - this build's CRT is dynamic,
; not static), but the installer only needs the *installer* itself during setup, not a copy
; of it sitting unused in {app} afterward.
Source: "..\build\win-deploy\*"; DestDir: "{app}"; Excludes: "vc_redist.x64.exe"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\build\win-deploy\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; Registers pixet as an *available* handler for every still-image format it recognizes
; (see src/core/db/Schema.cpp's classifyFormat() - the exact same list, video formats
; deliberately excluded: pixet already has a "use the system video player instead"
; preference, so muscling into video's Open-with list isn't wanted here). This is the
; standard Windows "Default Programs" registration (Capabilities + RegisteredApplications -
; see Microsoft's own docs on the pattern), which is what makes pixet show up as a proper
; named entry in Settings > Apps > Default apps > "Choose default apps by file type",
; not just as a raw .exe buried in "Choose another app".
;
; This does NOT make pixet the default for anything. Windows 8 and later hash-protects a
; user's chosen default (the UserChoice registry key) specifically to stop installers from
; silently overwriting it - by design, with no supported workaround - so opening one of
; these file types still uses whatever already handles it today until the user picks
; pixet themselves, once, via Open-with or Settings. All HKCU (per-user, no admin),
; matching PrivilegesRequired=lowest above; entries are removed on uninstall.
Root: HKCU; Subkey: "Software\Classes\pixet.Image"; ValueType: string; ValueName: ""; ValueData: "pixet Image"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\pixet.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\Classes\pixet.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc

Root: HKCU; Subkey: "Software\Classes\Applications\{#MyAppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: fileassoc; Flags: uninsdeletekey

Root: HKCU; Subkey: "Software\pixet\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#MyAppName}"; Tasks: fileassoc; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\pixet\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Fast native photo/video viewer"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpe"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cr2"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cr3"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".nef"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".arw"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dng"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".orf"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".rw2"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".raf"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pef"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".srw"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "pixet.Image"; Tasks: fileassoc
Root: HKCU; Subkey: "Software\pixet\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avif"; ValueData: "pixet.Image"; Tasks: fileassoc

; RegisteredApplications is a shared key other apps also write entries into - deletes
; only pixet's own value on uninstall (uninsdeletevalue), never the whole key
; (uninsdeletekey would be wrong here and could wipe other apps' registrations).
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "pixet"; ValueData: "Software\pixet\Capabilities"; Tasks: fileassoc; Flags: uninsdeletevalue

[Run]
; /install /quiet /norestart is safe to run even when an equal-or-newer VC++ runtime is
; already present - Microsoft's own installer detects that and exits quickly rather than
; reinstalling, so there's no real cost to always running this unconditionally instead of
; adding a Pascal Script version check.
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent
