#include "version.h"

namespace pixet {

// PIXET_VERSION comes from the top-level project(VERSION ...) via a compile definition -
// see src/core/CMakeLists.txt. Deliberately no #ifdef fallback: if the definition ever goes
// missing this should fail loudly at compile time rather than silently reporting a stale
// version that the .app bundle's metadata then disagrees with.
const char *version() { return PIXET_VERSION; }

} // namespace pixet
