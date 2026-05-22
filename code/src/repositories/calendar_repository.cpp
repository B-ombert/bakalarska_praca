#include "repositories/calendar_repository.h"
#include "utils/calendar_colors.h"
#include "utils/sqlite_utils.h"

namespace {

constexpr const char* kCalendarSelectColumns =
    "id, account_id, provider_calendar_id, name, description, timezone, "
    "color_hex, "
    "is_primary, is_read_only, is_shared, sync_enabled, sync_status, deleted_at";

void BindProviderCalendarId(SQLite::Statement& query, const int index, const std::string& providerCalendarId) {
    if (providerCalendarId.empty()) {
        query.bind(index);
        return;
    }

    query.bind(index, providerCalendarId);
}

}

CalendarRepository::CalendarRepository(SQLite::Database &db) : db(db) {}

long long CalendarRepository::upsert(const Calendar &c) {
    return RunInSavepoint(db, "calendar_upsert", [&]() -> long long {
        if (c.id > 0 && getById(c.id).has_value()) {
            updateById(c);
            return c.id;
        }

        SQLite::Statement query(db,
            "INSERT INTO calendars("
            "account_id, "
            "provider_calendar_id, "
            "name, "
            "description, "
            "timezone, "
            "color_hex, "
            "is_primary, "
            "is_read_only, "
            "is_shared, "
            "sync_enabled, "
            "sync_status, "
            "deleted_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(account_id, provider_calendar_id) DO UPDATE SET "
            "name = excluded.name, "
            "description = excluded.description, "
            "timezone = excluded.timezone, "
            "color_hex = CASE "
            "WHEN calendars.color_hex IS NULL OR calendars.color_hex = '' THEN excluded.color_hex "
            "ELSE calendars.color_hex END, "
            "is_primary = excluded.is_primary, "
            "is_read_only = excluded.is_read_only, "
            "is_shared = excluded.is_shared, "
            "sync_enabled = excluded.sync_enabled, "
            "sync_status = excluded.sync_status, "
            "deleted_at = excluded.deleted_at"
            );

        query.bind(1, c.accountId);
        BindProviderCalendarId(query, 2, c.providerCalendarId);
        query.bind(3, c.name);
        query.bind(4, c.description);
        query.bind(5, c.timezone);
        query.bind(6, NormalizeCalendarColor(c.colorHex.empty() ? RandomCalendarColor() : c.colorHex));
        query.bind(7, (int)c.isPrimary);
        query.bind(8, (int)c.isReadOnly);
        query.bind(9, (int)c.isShared);
        query.bind(10, (int)c.syncEnabled);
        query.bind(11, c.syncStatus);
        query.bind(12, c.deletedAt);

        query.exec();

        return db.getLastInsertRowid();
    });
}

bool CalendarRepository::updateById(const Calendar& c) {
    return RunInSavepoint(db, "calendar_update_by_id", [&]() {
        SQLite::Statement query(
            db,
            "UPDATE calendars SET "
            "account_id = ?, provider_calendar_id = ?, name = ?, description = ?, timezone = ?, "
            "color_hex = ?, "
            "is_primary = ?, is_read_only = ?, is_shared = ?, sync_enabled = ?, sync_status = ?, "
            "deleted_at = ? "
            "WHERE id = ?");
        query.bind(1, c.accountId);
        BindProviderCalendarId(query, 2, c.providerCalendarId);
        query.bind(3, c.name);
        query.bind(4, c.description);
        query.bind(5, c.timezone);
        query.bind(6, NormalizeCalendarColor(c.colorHex));
        query.bind(7, static_cast<int>(c.isPrimary));
        query.bind(8, static_cast<int>(c.isReadOnly));
        query.bind(9, static_cast<int>(c.isShared));
        query.bind(10, static_cast<int>(c.syncEnabled));
        query.bind(11, c.syncStatus);
        query.bind(12, c.deletedAt);
        query.bind(13, c.id);
        return query.exec() > 0;
    });
}

std::optional<Calendar> CalendarRepository::getById(long long id) {
    SQLite::Statement query(db,
        std::string("SELECT ") + kCalendarSelectColumns + " FROM calendars "
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

std::vector<Calendar> CalendarRepository::getByAccount(long long accountId) {
    std::vector<Calendar> calendars;

    SQLite::Statement query(db,
        std::string("SELECT ") + kCalendarSelectColumns + " FROM calendars WHERE account_id = ? AND deleted_at = 0"
        );

    query.bind(1, accountId);

    while (query.executeStep()) {
        calendars.push_back(mapRow(query));
    }

    return calendars;
}

std::vector<Calendar> CalendarRepository::getPendingRemoteCalendars(const long long accountId) {
    std::vector<Calendar> calendars;

    SQLite::Statement query(
        db,
        std::string("SELECT ") + kCalendarSelectColumns + " FROM calendars "
        "WHERE account_id = ? AND sync_status != ? "
        "ORDER BY id");

    query.bind(1, accountId);
    query.bind(2, SYNCED);

    while (query.executeStep()) {
        calendars.push_back(mapRow(query));
    }

    return calendars;
}

std::optional<Calendar> CalendarRepository::getByProviderId(const long long accountId, const std::string &providerCalendarId) {
    SQLite::Statement query(db,
        std::string("SELECT ") + kCalendarSelectColumns + " FROM calendars "
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

SQLite::Database& CalendarRepository::database() {
    return db;
}

Calendar CalendarRepository::mapRow(SQLite::Statement &query) {
    Calendar c;

    int col = 0;

    c.id = query.getColumn(col++).getInt64();

    c.accountId = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;

    c.providerCalendarId =
        query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    c.name = query.getColumn(col++).getString();
    c.description = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    c.timezone = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    c.colorHex = NormalizeCalendarColor(query.getColumn(col).isNull() ? "" : query.getColumn(col).getString()); col++;

    c.isPrimary  = query.getColumn(col++).getInt() != 0;
    c.isReadOnly = query.getColumn(col++).getInt() != 0;
    c.isShared = query.getColumn(col++).getInt() != 0;
    c.syncEnabled = query.getColumn(col++).getInt() != 0;
    c.syncStatus = query.getColumn(col++).getInt();

    c.deletedAt =
        query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

    return c;
}
