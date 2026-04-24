#include "repositories/account_repository.h"

long long AccountRepository::Upsert(const Account &a) {
    SQLite::Statement find(db,
        "SELECT id FROM accounts WHERE provider = ? AND provider_user_id = ?"
        );

    find.bind(1, a.provider);
    find.bind(2, a.providerUserId);

    if (find.executeStep()) {
        long long id = find.getColumn(0).getInt64();

        SQLite::Statement update(db, "UPDATE accounts SET refresh_token = ? WHERE id = ?");

        update.bind(1, a.refreshToken);
        update.bind(2, id);
        update.exec();

        return id;
    }

    SQLite::Statement insert(db, "INSERT INTO accounts (name, provider, provider_user_id, refresh_token) "
                                 "VALUES (?, ?, ?, ?)"
                                 );
    insert.bind(1, a.name);
    insert.bind(2, a.provider);
    insert.bind(3, a.providerUserId);
    insert.bind(4, a.refreshToken);
    insert.exec();

    return db.getLastInsertRowid();
}

std::string AccountRepository::GetRefreshToken(const Account &a) {
    SQLite::Statement query(db, "SELECT refresh_token FROM accounts WHERE id = ?");
    query.bind(1, a.id);

    if (query.executeStep()) {
        return query.getColumn(0).getString();
    }
    return "";
}

std::vector<Account> AccountRepository::GetAllAccountsFromPlatform(const std::string &platform) {
    std::vector<Account> accounts;

    SQLite::Statement query(db, "SELECT * FROM accounts WHERE provider = ?");
    query.bind(1, platform);

    while (query.executeStep()) {
        Account a;
        a.id = query.getColumn(0).getInt64();
        a.name = query.getColumn(1).getString();
        a.provider = query.getColumn(2).getString();
        a.providerUserId = query.getColumn(3).getString();
        a.refreshToken = query.getColumn(4).getString();

        accounts.push_back(std::move(a));
    }

    return accounts;
}
