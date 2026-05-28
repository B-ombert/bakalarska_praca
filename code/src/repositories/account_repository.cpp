#include "repositories/account_repository.h"
#include "utils/sqlite_utils.h"

namespace {

constexpr const char* kAccountSelectColumns =
    "id, name, provider, provider_user_id, email, refresh_token";

Account MapAccountRow(SQLite::Statement& query) {
    Account a;
    a.id = query.getColumn(0).getInt64();
    a.name = query.getColumn(1).getString();
    a.provider = query.getColumn(2).getString();
    a.providerUserId = query.getColumn(3).getString();
    a.email = query.getColumn(4).getString();
    a.refreshToken = query.getColumn(5).getString();
    return a;
}

} // namespace

long long AccountRepository::Upsert(const Account &a) {
    return RunInSavepoint(db, "account_upsert", [&]() -> long long {
        SQLite::Statement find(db,
            "SELECT id FROM accounts WHERE provider = ? AND provider_user_id = ?"
            );

        find.bind(1, a.provider);
        find.bind(2, a.providerUserId);

        if (find.executeStep()) {
            long long id = find.getColumn(0).getInt64();

            SQLite::Statement update(db, "UPDATE accounts SET name = ?, email = ?, refresh_token = ? WHERE id = ?");

            update.bind(1, a.name);
            update.bind(2, a.email);
            update.bind(3, a.refreshToken);
            BindInt64(update, 4, id);
            update.exec();

            return id;
        }

        SQLite::Statement insert(db, "INSERT INTO accounts (name, provider, provider_user_id, email, refresh_token) "
                                     "VALUES (?, ?, ?, ?, ?)"
                                     );
        insert.bind(1, a.name);
        insert.bind(2, a.provider);
        insert.bind(3, a.providerUserId);
        insert.bind(4, a.email);
        insert.bind(5, a.refreshToken);
        insert.exec();

        return db.getLastInsertRowid();
    });
}

std::vector<Account> AccountRepository::GetAllAccounts() {
    std::vector<Account> accounts;

    SQLite::Statement query(
        db,
        std::string("SELECT ") + kAccountSelectColumns + " FROM accounts "
        "ORDER BY CASE WHEN provider = 'LOCAL' THEN 0 ELSE 1 END, name COLLATE NOCASE ASC, id ASC");

    while (query.executeStep()) {
        accounts.push_back(MapAccountRow(query));
    }

    return accounts;
}

std::optional<Account> AccountRepository::GetById(const long long id) {
    SQLite::Statement query(db, std::string("SELECT ") + kAccountSelectColumns + " FROM accounts WHERE id = ?");
    BindInt64(query, 1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return MapAccountRow(query);
}

bool AccountRepository::DeleteById(const long long id) {
    return RunInSavepoint(db, "account_delete", [&]() {
        SQLite::Statement query(db, "DELETE FROM accounts WHERE id = ?");
        BindInt64(query, 1, id);
        return query.exec() > 0;
    });
}

std::string AccountRepository::GetRefreshToken(const Account &a) {
    SQLite::Statement query(db, "SELECT refresh_token FROM accounts WHERE id = ?");
    BindInt64(query, 1, a.id);

    if (query.executeStep()) {
        return query.getColumn(0).getString();
    }
    return "";
}
