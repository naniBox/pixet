#pragma once

#include <cstdint>

namespace pixet {

// The running process's OS id - used to build claim-owner strings (e.g.
// "gui:pid:1234") so concurrent indexers/GUI instances can tell each other apart.
// No identity/security meaning beyond that.
int64_t currentProcessId();

} // namespace pixet
