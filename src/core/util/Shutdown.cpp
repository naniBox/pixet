#include "Shutdown.h"

#include <atomic>

namespace pixet {

namespace {
std::atomic<bool> g_shutdownRequested{false};
} // namespace

void requestShutdown() { g_shutdownRequested.store(true, std::memory_order_relaxed); }

bool shutdownRequested() { return g_shutdownRequested.load(std::memory_order_relaxed); }

} // namespace pixet
