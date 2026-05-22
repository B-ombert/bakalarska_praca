#pragma once

#include <optional>
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
    std::vector<Account> GetAllAccounts();
    std::optional<Account> GetById(long long id);
    bool DeleteById(long long id);
    std::string GetRefreshToken(const Account& a);
};
