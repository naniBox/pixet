#include "Database.h"

#include <sqlite3.h>

#include <stdexcept>

#include "Schema.h"

namespace pixet {

namespace {

void check(sqlite3 *db, int rc, const char *what) {
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE) {
        std::string msg = std::string(what) + ": " + sqlite3_errmsg(db);
        throw std::runtime_error(msg);
    }
}

} // namespace

// ---- Statement ----

Statement::Statement(sqlite3 *db, const std::string &sql) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &stmt_, nullptr);
    check(db, rc, "prepare");
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement &&other) noexcept : stmt_(other.stmt_) { other.stmt_ = nullptr; }

Statement &Statement::operator=(Statement &&other) noexcept {
    if (this != &other) {
        if (stmt_) sqlite3_finalize(stmt_);
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

void Statement::bind(int idx, int64_t v) { sqlite3_bind_int64(stmt_, idx, v); }

void Statement::bindDouble(int idx, double v) { sqlite3_bind_double(stmt_, idx, v); }

void Statement::bind(int idx, const std::string &v) {
    sqlite3_bind_text(stmt_, idx, v.data(), (int)v.size(), SQLITE_TRANSIENT);
}

void Statement::bind(int idx, const std::vector<uint8_t> &blob) {
    if (blob.empty()) {
        sqlite3_bind_zeroblob(stmt_, idx, 0);
    } else {
        sqlite3_bind_blob(stmt_, idx, blob.data(), (int)blob.size(), SQLITE_TRANSIENT);
    }
}

void Statement::bindNull(int idx) { sqlite3_bind_null(stmt_, idx); }

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("step: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}

void Statement::reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

int64_t Statement::columnInt64(int col) const { return sqlite3_column_int64(stmt_, col); }

double Statement::columnDouble(int col) const { return sqlite3_column_double(stmt_, col); }

std::string Statement::columnText(int col) const {
    const unsigned char *text = sqlite3_column_text(stmt_, col);
    int len = sqlite3_column_bytes(stmt_, col);
    return text ? std::string((const char *)text, len) : std::string();
}

std::vector<uint8_t> Statement::columnBlob(int col) const {
    const void *data = sqlite3_column_blob(stmt_, col);
    int len = sqlite3_column_bytes(stmt_, col);
    if (!data || len <= 0) return {};
    const uint8_t *bytes = (const uint8_t *)data;
    return std::vector<uint8_t>(bytes, bytes + len);
}

bool Statement::columnIsNull(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

// ---- Database ----

Database::Database(const std::string &indexDbPath, const std::string &thumbsDbPath, bool readOnly) {
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(indexDbPath.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("failed to open " + indexDbPath + ": " + msg);
    }

    exec("PRAGMA busy_timeout=10000;");
    exec("PRAGMA foreign_keys=OFF;");
    // Puts one schema into WAL, tolerating the case where it can't be done right now.
    //
    // Converting a rollback-journal database to WAL needs a brief EXCLUSIVE lock, so it fails
    // with SQLITE_BUSY if anything at all is mid-read. That must not be fatal, and finding out
    // the hard way is what this comment is for: a first version of the thumbs.db conversion
    // called exec() directly, and with one reader holding an open read transaction the pragma
    // threw from inside this constructor - which every caller runs at startup and no caller
    // wraps - turning a recoverable lock conflict into an immediate abort (`libc++abi:
    // terminating due to uncaught exception ... SQL: PRAGMA thumbs.journal_mode=WAL;`). Worse
    // than the problem it was added to fix.
    //
    // So: skip the work entirely if already WAL (the normal case after the first launch), drop
    // busy_timeout to 200ms for the attempt so a contended startup isn't a 10-second stall, and
    // on failure carry on in whatever mode the file is in. The conversion is persisted in the
    // database header, so any later launch that finds the file quiet converts it permanently.
    //
    // synchronous=NORMAL is applied only if WAL actually took: NORMAL is crash-safe under WAL
    // but risks corruption on power loss under a rollback journal, so the two settings have to
    // move together or not at all.
    auto ensureWal = [this](const char *schema) {
        std::string prefix = std::string("PRAGMA ") + schema + ".";
        auto currentMode = [&]() {
            auto q = prepare(prefix + "journal_mode;");
            return q.step() ? q.columnText(0) : std::string();
        };
        std::string mode = currentMode();
        if (mode != "wal") {
            exec("PRAGMA busy_timeout=200;");
            try {
                exec(prefix + "journal_mode=WAL;");
            } catch (const std::exception &) {
                // Something is reading it. Leave the file as it is and try again next launch.
            }
            exec("PRAGMA busy_timeout=10000;");
            mode = currentMode();
        }
        if (mode == "wal") exec(prefix + "synchronous=NORMAL;");
    };

    // main only here - the ATTACH hasn't happened yet, so `thumbs` doesn't exist to be named.
    if (!readOnly) ensureWal("main");

    // ATTACH needs a bound-free literal; escape single quotes defensively.
    std::string escaped;
    escaped.reserve(thumbsDbPath.size());
    for (char c : thumbsDbPath) {
        if (c == '\'') escaped += '\'';
        escaped += c;
    }
    exec("ATTACH DATABASE '" + escaped + "' AS thumbs;");

    // thumbs.db in WAL too, and this is a correctness matter, not tuning. The pragmas above
    // run before the ATTACH and are unqualified, so they only ever reach index.db; without
    // this call thumbs.db silently keeps SQLite's defaults (rollback journal,
    // synchronous=FULL). A latency benchmark makes those defaults look perfectly acceptable,
    // and it is measuring the wrong thing.
    //
    // On a rollback journal, a writer's COMMIT must promote RESERVED -> EXCLUSIVE, and
    // EXCLUSIVE cannot be acquired while any other connection holds a SHARED lock - i.e. while
    // anything at all is mid-read. So a *reader* of thumbs.db blocks a *writer's commit*, which
    // is the one lock interaction WAL exists to eliminate. Reproduced end to end: with one
    // open read transaction on thumbs.db, `pixet-index --force-rethumbnail` waits out the
    // full 10s busy_timeout and dies with "exec failed: database is locked / SQL: COMMIT;",
    // which without the exception boundaries around indexing would take the GUI down with it
    // from a background sweep. In WAL the identical run succeeds in 1.6s. That is the entire
    // difference between "the CLI and the GUI can be used at the same time" and not, and the
    // claims table exists precisely so that they can be.
    //
    // Note the failure is at COMMIT, not BEGIN, which is what made it confusing: BEGIN
    // IMMEDIATE only needs RESERVED, which readers don't block, so a competing *writer* fails
    // early and visibly while a competing *reader* fails late, at the commit, after a 10s stall.
    //
    // synchronous=NORMAL moves with it deliberately: NORMAL is crash-safe under WAL but risks
    // corruption on power loss under a rollback journal, so the two settings have to change
    // together: FULL is the correct partner for a rollback journal, NORMAL for WAL.
    //
    // Attempted on every open, so an existing thumbs.db converts on the first launch that
    // finds it uncontended - see ensureWal above for why the attempt is allowed to fail rather
    // than throw. Read-only connections can't convert it and don't need to; they inherit
    // whatever mode the file is already in, exactly as they already did for index.db.
    //
    // What this does NOT buy is cross-database atomicity for the indexer's main+thumbs
    // transactions: SQLite gives per-database, not cross-database, atomicity once any database
    // in the set is in WAL, and index.db already was. A torn commit therefore leaves either a
    // files.thumb_id pointing at a missing blob (re-thumbnailed on the next scan) or an
    // orphaned blob (reclaimed by compaction) - both self-healing, because thumbs.db is a
    // cache. Recovering real atomicity would mean taking index.db out of WAL, which would
    // reintroduce exactly the reader-blocks-writer failure fixed here.
    if (!readOnly) ensureWal("thumbs");

    // SQLite's own page cache defaults to ~2MB regardless of database size, which
    // barely covers one folder's worth of thumbnail blobs on a large library (measured:
    // 22MB for one 554-file real-world folder). thumbs.db is the one that actually
    // grows large (it holds the image bytes; index.db is comparatively tiny metadata),
    // so it gets the bigger budget. Both cache_size (SQLite's own page cache) and
    // mmap_size (lets SQLite read pages via the OS's memory-mapped file instead of
    // explicit read() calls) are per-connection, so every caller that opens a
    // Database - every worker thread in the app, and pixet-index - gets this for
    // free rather than needing to remember it individually. mmap_size is virtual
    // address space, not committed RAM (the OS pages it in on demand and can evict
    // under pressure), so being generous here on a 64-bit build is low-risk; an
    // engine built without mmap support just ignores the pragma rather than erroring.
    exec("PRAGMA cache_size = -8192;");         // 8MB for index.db (small; metadata only)
    exec("PRAGMA thumbs.cache_size = -65536;"); // 64MB for thumbs.db (measured ~40% faster
                                                 // on a real 1.1GB thumbs.db: 55ms -> 34ms
                                                 // reading one 554-file folder's worth of blobs)
    exec("PRAGMA mmap_size = 268435456;");         // 256MB
    exec("PRAGMA thumbs.mmap_size = 1073741824;"); // 1GB

    if (!readOnly) applySchema();
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

void Database::applySchema() {
    exec(kIndexSchemaSql);
    exec(kThumbsSchemaSql);
    runMigrations();
}

void Database::runMigrations() {
    int64_t version = 0;
    {
        auto sel = prepare("PRAGMA user_version");
        if (sel.step()) version = sel.columnInt64(0);
    }

    if (version < 1) {
        // FileState::DoneNeedsRender didn't exist before this - every RAW file already
        // sitting at Done was thumbnailed by code that couldn't tell "fast embedded
        // preview" and "full demosaic render" apart, so it's genuinely ambiguous which
        // one any given row actually got. Reclassify all of them back to
        // DoneNeedsRender so RawRenderer/`--render-raws` gives every one of them a real
        // (or confirming) pass - safe even for ones that already happened to be fully
        // rendered, since re-rendering is idempotent (some wasted CPU once, not an
        // incorrect result). Without this, any RAW file indexed before this migration
        // existed would sit at Done forever, invisible to the render-upgrade path, which
        // is exactly what happened in practice.
        exec("UPDATE files SET state=" + std::to_string((int64_t)FileState::DoneNeedsRender) +
             " WHERE fmt=" + std::to_string((int64_t)Format::Raw) +
             " AND state=" + std::to_string((int64_t)FileState::Done));
        exec("PRAGMA user_version = 1");
    }

    if (version < 2) {
        // FileState::Unsupported can only be produced today by ThumbGenerator's
        // per-format switch falling through to its default case - and every format
        // that can actually reach ThumbGenerator (i.e. every Format enum value except
        // Unknown, which never gets a files row at all - see Indexer's Pass A) has
        // explicit handling there now. So any row sitting at Unsupported today is
        // necessarily stale: it was marked that way by an older build, from before
        // that format's decoder existed (Video was the concrete case found in
        // practice), and normal re-scanning never revisits it since Pass
        // B only processes State::New (or DoneNeedsRender, for a --render-raws pass) -
        // an already-Unsupported row is invisible to it forever otherwise. Reclassify
        // back to New so Pass B gives every one of them a real pass with today's
        // actual format support. Safe even for a row that would genuinely still fail -
        // it just lands back on Unsupported/Failed again, no worse than before.
        exec("UPDATE files SET state=" + std::to_string((int64_t)FileState::New) +
             " WHERE state=" + std::to_string((int64_t)FileState::Unsupported));
        exec("PRAGMA user_version = 2");
    }

    if (version < 3) {
        // Adding *columns* needs an explicit ALTER, unlike the two data fixups above.
        // applySchema() runs CREATE TABLE IF NOT EXISTS, which is a no-op on a database that
        // already has the table - so a column added to the schema SQL reaches fresh
        // databases only, and every existing one would silently never gain it. (Database.h's
        // comment claimed the schema SQL handled new columns; true for a fresh file, wrong
        // for an existing one - corrected there.)
        //
        // Guarded on the column actually being absent rather than on the version alone: a
        // *fresh* database gets these from the CREATE and still arrives here at
        // user_version 0, where an unguarded ALTER fails with "duplicate column name".
        bool haveGps = false;
        {
            auto info = prepare("PRAGMA table_info(files)");
            while (info.step()) {
                if (info.columnText(1) == "gps_lat") {
                    haveGps = true;
                    break;
                }
            }
        }
        if (!haveGps) {
            exec("ALTER TABLE files ADD COLUMN gps_lat REAL");
            exec("ALTER TABLE files ADD COLUMN gps_lon REAL");
            exec("ALTER TABLE files ADD COLUMN gps_checked INTEGER NOT NULL DEFAULT 0");
        }
        exec("PRAGMA user_version = 3");
    }
}

void Database::exec(const std::string &sql) {
    char *errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string msg = errMsg ? errMsg : "unknown error";
        sqlite3_free(errMsg);
        throw std::runtime_error("exec failed: " + msg + "\nSQL: " + sql);
    }
}

Statement Database::prepare(const std::string &sql) { return Statement(db_, sql); }

void Database::beginTransaction() { exec("BEGIN IMMEDIATE;"); }
void Database::commit() { exec("COMMIT;"); }
void Database::rollback() { exec("ROLLBACK;"); }

int64_t Database::lastInsertRowId() const { return sqlite3_last_insert_rowid(db_); }
int Database::changes() const { return sqlite3_changes(db_); }

} // namespace pixet
