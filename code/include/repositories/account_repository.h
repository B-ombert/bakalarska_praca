#pragma once

#include <string>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/account.h"

class AccountRepository {
private:
    SQLite::Database& db;

public:
    explicit AccountRepository(SQLite::Database& db) : db(db) {}

    long long Upsert(const Account& a);
    std::vector<Account> GetAllAccountsFromPlatform(const std::string& platform);
    std::string GetRefreshToken(const Account& a);
};
