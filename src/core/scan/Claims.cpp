#include "Claims.h"

#include "../db/Database.h"

namespace pixet {

ClaimManager::ClaimManager(Database &db) : db_(db) {}

bool ClaimManager::tryClaim(int64_t dirId, const std::string &owner, int64_t nowMs, int64_t staleMs) {
    {
        auto ins = db_.prepare("INSERT OR IGNORE INTO claims(dir_id, owner, heartbeat) VALUES(?,?,?)");
        ins.bind(1, dirId);
        ins.bind(2, owner);
        ins.bind(3, nowMs);
        ins.step();
        if (db_.changes() == 1) return true;
    }
    {
        // Already ours (re-entrant claim / renewal).
        auto renew = db_.prepare("UPDATE claims SET heartbeat=? WHERE dir_id=? AND owner=?");
        renew.bind(1, nowMs);
        renew.bind(2, dirId);
        renew.bind(3, owner);
        renew.step();
        if (db_.changes() == 1) return true;
    }
    {
        // Held by someone else - steal it only if their heartbeat has gone stale.
        int64_t staleBefore = nowMs - staleMs;
        auto steal = db_.prepare("UPDATE claims SET owner=?, heartbeat=? WHERE dir_id=? AND heartbeat<?");
        steal.bind(1, owner);
        steal.bind(2, nowMs);
        steal.bind(3, dirId);
        steal.bind(4, staleBefore);
        steal.step();
        if (db_.changes() == 1) return true;
    }
    return false;
}

void ClaimManager::heartbeat(int64_t dirId, const std::string &owner, int64_t nowMs) {
    auto stmt = db_.prepare("UPDATE claims SET heartbeat=? WHERE dir_id=? AND owner=?");
    stmt.bind(1, nowMs);
    stmt.bind(2, dirId);
    stmt.bind(3, owner);
    stmt.step();
}

void ClaimManager::release(int64_t dirId, const std::string &owner) {
    auto stmt = db_.prepare("DELETE FROM claims WHERE dir_id=? AND owner=?");
    stmt.bind(1, dirId);
    stmt.bind(2, owner);
    stmt.step();
}

} // namespace pixet
