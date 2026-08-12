#include "DirRows.h"

#include "../db/Database.h"

namespace pixet {

int64_t upsertDir(Database &db, const std::string &path, int64_t parentId) {
    auto ins = db.prepare("INSERT OR IGNORE INTO dirs(parent_id, path, mtime, scanned_at) VALUES(?,?,0,0)");
    if (parentId < 0) ins.bindNull(1); else ins.bind(1, parentId);
    ins.bind(2, path);
    ins.step();

    auto sel = db.prepare("SELECT id FROM dirs WHERE path=?");
    sel.bind(1, path);
    sel.step();
    return sel.columnInt64(0);
}

} // namespace pixet
