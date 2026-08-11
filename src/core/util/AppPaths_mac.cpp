#include "AppPaths.h"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace pixet {

namespace {

// $HOME is the right primary source - it's what every other tool on the system honors,
// including when a user has deliberately overridden it - but it can legitimately be absent
// for a process launched with a scrubbed environment, and appDataDir() throwing takes the
// whole app down (see below). The passwd database is the standard fallback.
std::string homeDir() {
    if (const char *env = std::getenv("HOME"); env && *env) return env;
    if (const struct passwd *pw = getpwuid(getuid()); pw && pw->pw_dir && *pw->pw_dir) return pw->pw_dir;
    throw std::runtime_error("could not resolve $HOME");
}

} // namespace

// Throws rather than returning empty on failure, matching AppPaths_win.cpp - and
// deliberately so. Nothing catches this (MainWindow, FolderIndexer, ThumbLoader,
// RawRenderer, BackgroundReconciler, pixet-index and prefs::settingsStore all call it
// bare), because a process that can't locate its own cache directory has nothing useful
// left to do.
std::string appDataDir() {
    // ~/Library/Application Support/pixet, NOT ~/.config - that's the Linux/XDG
    // convention, not macOS's. This is also where prefs::settingsStore() puts pixet.ini,
    // which it derives by appending "/pixet.ini" to this path.
    std::filesystem::path dir = std::filesystem::path(homeDir()) / "Library" / "Application Support" / "pixet";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // create_directories returns false with no error when the directory already existed,
    // so the existence check - not the return value - is what decides success here.
    // (~/Library/Application Support itself always exists, so unlike the Windows version's
    // single-level CreateDirectoryW this is never actually creating more than one level.)
    if (!std::filesystem::is_directory(dir)) {
        throw std::runtime_error("could not create app data directory: " + dir.string());
    }
    return dir.string();
}

std::string indexDbPath() { return appDataDir() + "/index.db"; }
std::string thumbsDbPath() { return appDataDir() + "/thumbs.db"; }

} // namespace pixet
