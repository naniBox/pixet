#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace pixet {

// RAII prepared statement. Move-only.
class Statement {
public:
    Statement() = default;
    Statement(sqlite3 *db, const std::string &sql);
    ~Statement();

    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    Statement(Statement &&other) noexcept;
    Statement &operator=(Statement &&other) noexcept;

    void bind(int idx, int64_t v);
    // Deliberately a named method rather than a bind(int, double) overload: a bare integer
    // literal at any existing call site would become ambiguous between the two.
    void bindDouble(int idx, double v);
    void bind(int idx, const std::string &v);
    void bind(int idx, const std::vector<uint8_t> &blob);
    void bindNull(int idx);

    // Returns true if a row is available, false when the statement is exhausted.
    bool step();
    void reset();

    int64_t columnInt64(int col) const;
    double columnDouble(int col) const;
    std::string columnText(int col) const;
    std::vector<uint8_t> columnBlob(int col) const;
    bool columnIsNull(int col) const;

private:
    sqlite3_stmt *stmt_ = nullptr;
};

// Owns one sqlite3 connection with thumbs.db ATTACHed as `thumbs`.
// Not thread-safe - callers use one Database per thread/connection.
class Database {
public:
    Database(const std::string &indexDbPath, const std::string &thumbsDbPath, bool readOnly = false);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    void exec(const std::string &sql);
    Statement prepare(const std::string &sql);

    void beginTransaction();
    void commit();
    void rollback();

    int64_t lastInsertRowId() const;
    int changes() const;

    sqlite3 *handle() const { return db_; }

private:
    void applySchema();
    // One-time, idempotent schema and data migrations keyed off PRAGMA user_version.
    //
    // Covers both changes to what a stored value *means* (correcting existing rows in
    // place) and added columns. New *tables* are handled by the schema SQL itself, but new
    // columns are not: applySchema() runs CREATE TABLE IF NOT EXISTS, which does nothing at
    // all on a database that already has the table, so a column added to the schema SQL
    // reaches only freshly-created files unless an ALTER is added here too. See
    // Database.cpp for the specific migrations.
    void runMigrations();

    sqlite3 *db_ = nullptr;
};

} // namespace pixet
