#include "Database.h"

#include <sqlite3.h>

#include <stdexcept>

#include "Schema.h"
#include "../util/StringUtil.h"

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

Database::Database(const std::wstring &indexDbPath, const std::wstring &thumbsDbPath, bool readOnly) {
    std::string indexPathUtf8 = toUtf8(indexDbPath);
    std::string thumbsPathUtf8 = toUtf8(thumbsDbPath);

    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(indexPathUtf8.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "sqlite3_open_v2 failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("failed to open " + indexPathUtf8 + ": " + msg);
    }

    exec("PRAGMA busy_timeout=10000;");
    exec("PRAGMA foreign_keys=OFF;");
    if (!readOnly) {
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=NORMAL;");
    }

    // ATTACH needs a bound-free literal; escape single quotes defensively.
    std::string escaped;
    escaped.reserve(thumbsPathUtf8.size());
    for (char c : thumbsPathUtf8) {
        if (c == '\'') escaped += '\'';
        escaped += c;
    }
    exec("ATTACH DATABASE '" + escaped + "' AS thumbs;");

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
        // existed would sit at Done forever, invisible to the render-upgrade path -
        // exactly what happened in practice (see devlog).
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
        // practice - see devlog), and normal re-scanning never revisits it since Pass
        // B only processes State::New (or DoneNeedsRender, for a --render-raws pass) -
        // an already-Unsupported row is invisible to it forever otherwise. Reclassify
        // back to New so Pass B gives every one of them a real pass with today's
        // actual format support. Safe even for a row that would genuinely still fail -
        // it just lands back on Unsupported/Failed again, no worse than before.
        exec("UPDATE files SET state=" + std::to_string((int64_t)FileState::New) +
             " WHERE state=" + std::to_string((int64_t)FileState::Unsupported));
        exec("PRAGMA user_version = 2");
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
