#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include "SQLiteCpp/Backup.h"

inline void ConfigureSqliteConnection(SQLite::Database& db) {
    db.exec("PRAGMA foreign_keys = ON");
    db.exec("PRAGMA journal_mode = WAL");
    db.exec("PRAGMA busy_timeout = 5000");
    db.exec("PRAGMA synchronous = NORMAL");
}

inline void BindInt64(SQLite::Statement& statement, const int index, const long long value) {
    statement.bind(index, static_cast<std::int64_t>(value));
}

template <typename Func>
auto RunInSavepoint(SQLite::Database& db, const std::string& name, Func&& func) -> decltype(func()) {
    const std::string savepoint = "SAVEPOINT " + name;
    const std::string release = "RELEASE SAVEPOINT " + name;
    const std::string rollback = "ROLLBACK TO SAVEPOINT " + name;

    db.exec(savepoint);
    try {
        if constexpr (std::is_void_v<decltype(func())>) {
            func();
            db.exec(release);
        }
        else {
            auto result = func();
            db.exec(release);
            return result;
        }
    }
    catch (...) {
        try {
            db.exec(rollback);
        }
        catch (...) {
        }

        try {
            db.exec(release);
        }
        catch (...) {
        }

        throw;
    }
}
