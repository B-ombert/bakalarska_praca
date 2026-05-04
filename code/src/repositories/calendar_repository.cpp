#include "repositories/calendar_repository.h"
#include "utils/sqlite_utils.h"

CalendarRepository::CalendarRepository(SQLite::Database &db) : db(db) {}

long long CalendarRepository::upsert(const Calendar &c) {
    return RunInSavepoint(db, "calendar_upsert", [&]() -> long long {
        SQLite::Statement query(db,
            "INSERT INTO calendars("
            "account_id, "
            "provider_calendar_id, "
            "name, "
            "timezone, "
            "is_primary, "
            "is_read_only, "
            "sync_enabled, "
            "sync_token, "
            "last_synced_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(account_id, provider_calendar_id) DO UPDATE SET "
            "name = excluded.name, "
            "timezone = excluded.timezone, "
            "is_primary = excluded.is_primary, "
            "is_read_only = excluded.is_read_only, "
            "sync_enabled = excluded.sync_enabled, "
            "sync_token = excluded.sync_token, "
            "last_synced_at = excluded.last_synced_at"
            );

        query.bind(1, c.accountId);
        query.bind(2, c.providerCalendarId);
        query.bind(3, c.name);
        query.bind(4, c.timezone);
        query.bind(5, (int)c.isPrimary);
        query.bind(6, (int)c.isReadOnly);
        query.bind(7, (int)c.syncEnabled);
        query.bind(8, c.syncToken);
        query.bind(9, c.lastSyncedAt);

        query.exec();

        return db.getLastInsertRowid();
    });
}

std::vector<Calendar> CalendarRepository::getAll() {
    SQLite::Statement query(db,
        "SELECT * FROM calendars "
        "ORDER BY name"
        );

    std::vector<Calendar> calendars;

    while (query.executeStep()) {
        calendars.push_back(mapRow(query));
    }

    return calendars;
}

std::optional<Calendar> CalendarRepository::getById(long long id) {
    SQLite::Statement query(db,
        "SELECT * FROM calendars "
        "WHERE id = ?"
        );

    query.bind(1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

bool CalendarRepository::deleteById(long long id) {
    return RunInSavepoint(db, "calendar_delete", [&]() {
        SQLite::Statement query(db, "DELETE FROM calendars WHERE id = ?");
        query.bind(1, id);

        return query.exec() > 0;
    });
}

std::vector<Calendar> CalendarRepository::getByProvider(const std::string &provider) {
    std::vector<Calendar> calendars;

    SQLite::Statement query(db,
    "SELECT c.id, c.account_id, c.provider_calendar_id, c.name, c.timezone, "
    "c.is_primary, c.is_read_only, c.sync_enabled, c.sync_token, c.last_synced_at "
    "FROM calendars c "
    "JOIN accounts a ON c.account_id = a.id "
    "WHERE a.provider = ? "
    "ORDER BY c.name"
        );

    query.bind(1, provider);

    while (query.executeStep()) {
        calendars.push_back(mapRow(query));
    }

    return calendars;
}

std::vector<Calendar> CalendarRepository::getByAccount(long long accountId) {
    std::vector<Calendar> calendars;

    SQLite::Statement query(db,
        "SELECT * FROM calendars WHERE account_id = ?"
        );

    query.bind(1, accountId);

    while (query.executeStep()) {
        calendars.push_back(mapRow(query));
    }

    return calendars;
}

std::optional<Calendar> CalendarRepository::getByProviderId(const long long accountId, const std::string &providerCalendarId) {
    SQLite::Statement query(db,
        "SELECT * FROM calendars "
        "WHERE account_id = ? "
        "AND provider_calendar_id = ?"
        );

    query.bind(1, accountId);
    query.bind(2, providerCalendarId);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

Calendar CalendarRepository::mapRow(SQLite::Statement &query) {
    Calendar c;

    int col = 0;

    c.id = query.getColumn(col++).getInt64();

    c.accountId = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;

    c.providerCalendarId =
        query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    c.name = query.getColumn(col++).getString();
    c.timezone = query.getColumn(col++).getString();

    c.isPrimary  = query.getColumn(col++).getInt() != 0;
    c.isReadOnly = query.getColumn(col++).getInt() != 0;
    c.syncEnabled = query.getColumn(col++).getInt() != 0;

    c.syncToken =
        query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    c.lastSyncedAt =
        query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

    return c;
}
