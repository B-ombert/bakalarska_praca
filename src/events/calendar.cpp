#include "calendar.h"

CalendarRepository::CalendarRepository(SQLite::Database &db) : db(db) {}

long long CalendarRepository::insert(const Calendar &c) {
    SQLite::Statement query(db,
        "INSERT INTO calendars("
        "provider,"
        "provider_calendar_id,"
        "name,"
        "sync_token,"
        "created_at,"
        "updated_at)"
        "VALUES(?, ?, ?, ?, ?, ?)"
        );

    query.bind(1, c.provider);
    query.bind(2, c.providerCalendarId);
    query.bind(3, c.name);
    query.bind(4, c.syncToken);
    query.bind(5, c.createdAt);
    query.bind(6, c.updatedAt);

    query.exec();

    return db.getLastInsertRowid();
}

bool CalendarRepository::update(const Calendar &c) {
    SQLite::Statement query(db,
        "UPDATE calendars"
        "SET"
        "name = ?,"
        "sync_token = ?,"
        "updated_at = ?"
        "WHERE id = ?"
        );

    query.bind(1, c.name);
    query.bind(2, c.syncToken);
    query.bind(3, c.updatedAt);
    query.bind(4, c.id);

    return query.exec() > 0;
}

std::vector<Calendar> CalendarRepository::getAll() {
    SQLite::Statement query(db,
        "SELECT * FROM calendars"
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
        "SELECT * FROM calendars"
        "WHERE id = ?"
        );

    query.bind(1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

std::optional<Calendar> CalendarRepository::getByProviderId(const std::string &provider, const std::string &providerCalendarId) {
    SQLite::Statement query(db,
        "SELECT * FROM calendars"
        "WHERE provider = ?"
        "AND provider_calendar_id = ?"
        );

    query.bind(1, provider);
    query.bind(2, providerCalendarId);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

Calendar CalendarRepository::mapRow(SQLite::Statement &query) {
    Calendar c;

    c.id = query.getColumn("id").getInt64();

    c.provider = query.getColumn("provider").getString();

    c.providerCalendarId =
        query.getColumn("provider_calendar_id").isNull()
        ? ""
        : query.getColumn("provider_calendar_id").getString();

    c.name = query.getColumn("name").getString();

    c.syncToken =
        query.getColumn("sync_token").isNull()
        ? ""
        : query.getColumn("sync_token").getString();

    c.createdAt = query.getColumn("created_at").getInt64();

    c.updatedAt =
        query.getColumn("updated_at").isNull()
        ? 0
        : query.getColumn("updated_at").getInt64();

    return c;
}
