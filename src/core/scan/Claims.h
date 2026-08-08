#pragma once

#include <cstdint>
#include <string>

namespace pixet {

class Database;

// Directory-level work claims so multiple pixet-index processes (and the GUI's
// on-demand FolderIndexer) can share one library without duplicating work.
// Coarse on purpose: one claim per directory, not per file.
class ClaimManager {
public:
    explicit ClaimManager(Database &db);

    static constexpr int64_t kDefaultStaleMs = 60'000;

    // Attempts to claim dirId for `owner`. Succeeds if unclaimed, already held by
    // `owner`, or the existing claim's heartbeat is older than staleMs (the prior
    // holder is presumed dead - self-healing after a crash/kill).
    bool tryClaim(int64_t dirId, const std::string &owner, int64_t nowMs, int64_t staleMs = kDefaultStaleMs);

    void heartbeat(int64_t dirId, const std::string &owner, int64_t nowMs);
    void release(int64_t dirId, const std::string &owner);

private:
    Database &db_;
};

} // namespace pixet
