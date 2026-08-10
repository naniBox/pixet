#pragma once

#include <string>

namespace pixet {

// Central cache location, shared by every folder ever browsed/indexed -
// this is what makes revisiting a folder instant instead of per-folder caches.
// Windows: %LOCALAPPDATA%\pixet. Created on first access if missing. UTF-8.
std::string appDataDir();
std::string indexDbPath();
std::string thumbsDbPath();

} // namespace pixet
