#include "repositories/event_repository.h"

#include <ctime>

EventRepository::EventRepository(SQLite::Database &db) : db(db) {}

long long EventRepository::upsert(const Event &e) {
    SQLite::Statement query(db,
        "INSERT INTO events ("
        "calendar_id, provider_event_id, provider_master_id, instance_start, type, "
        "title, description, location, "
        "start_datetime, end_datetime, all_day, "
        "status, recurrence_rule, "
        "deleted_at, sync_status, "
        "last_modified, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "

        "ON CONFLICT(calendar_id, provider_event_id, instance_start) DO UPDATE SET "
        "provider_master_id = excluded.provider_master_id, "
        "title = excluded.title, "
        "description = excluded.description, "
        "location = excluded.location, "
        "start_datetime = excluded.start_datetime, "
        "end_datetime = excluded.end_datetime, "
        "all_day = excluded.all_day, "
        "status = excluded.status, "
        "recurrence_rule = excluded.recurrence_rule, "
        "deleted_at = excluded.deleted_at, "
        "sync_status = CASE "
        "    WHEN events.sync_status = 0 THEN 2 "
        "    ELSE events.sync_status "
        "END, "
        "last_modified = excluded.last_modified, "
        "updated_at = excluded.updated_at;"
    );

    int i = 1;
    query.bind(i++, e.calendarId);
    query.bind(i++, e.providerEventId);
    query.bind(i++, e.providerMasterId);
    query.bind(i++, e.instanceStart);
    query.bind(i++, (int)(e.type));

    query.bind(i++, e.title);
    query.bind(i++, e.description);
    query.bind(i++, e.location);

    query.bind(i++, e.startDateTime);
    query.bind(i++, e.endDateTime);
    query.bind(i++, (int)e.allDay);

    query.bind(i++, e.status);
    query.bind(i++, e.recurrenceRule);

    query.bind(i++, e.deletedAt);
    query.bind(i++, e.syncStatus);

    query.bind(i++, e.lastModified);
    query.bind(i++, e.createdAt);
    query.bind(i++, e.updatedAt);

    query.exec();
    return db.getLastInsertRowid();
}

Event EventRepository::mapRow(SQLite::Statement &query) {
    Event e;

    int col = 0;

    e.id = query.getColumn(col++).getInt64();
    e.calendarId = query.getColumn(col++).getInt64();

    e.providerEventId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.providerMasterId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.instanceStart = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    e.type = (EventType)query.getColumn(col++).getInt();

    e.title = query.getColumn(col++).getString();
    e.description = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.location    = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.startDateTime = query.getColumn(col++).getInt64();
    e.endDateTime   = query.getColumn(col++).getInt64();
    e.allDay        = query.getColumn(col++).getInt();

    e.status = query.getColumn(col).isNull() ? "confirmed" : query.getColumn(col).getString(); col++;

    e.recurrenceRule = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.deletedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;

    e.syncStatus = query.getColumn(col).isNull() ? PENDING_INSERT : query.getColumn(col).getInt(); col++;

    e.lastModified = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    e.createdAt    = query.getColumn(col++).getInt64();
    e.updatedAt    = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

    return e;
}

std::optional<Event> EventRepository::getById(long long id) {
    SQLite::Statement query(db, "SELECT * FROM events WHERE id = ?");
    query.bind(1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

std::optional<Event> EventRepository::getByProviderId(const std::string &providerId) {
    SQLite::Statement query(db,
                    "SELECT * FROM events "
                            "WHERE provider_event_id = ?"
        );

    query.bind(1, providerId);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

bool EventRepository::softDelete(long long id) {
    SQLite::Statement query(db,
        "UPDATE events SET deleted_at = ?, sync_status = ? WHERE id = ?");

    query.bind(1, (long long)time(nullptr));
    query.bind(2, PENDING_DELETE);
    query.bind(3, id);

    return query.exec() > 0;
}

bool EventRepository::deleteEvent(long long id) {
    SQLite::Statement query(db, "DELETE FROM events WHERE id = ?");
    query.bind(1, id);

    return query.exec() > 0;
}

std::optional<Event> EventRepository::getByProviderInstance(const std::string &providerId, long long instanceStart) {
    SQLite::Statement query(db, "SELECT * FROM events "
                                "WHERE provider_event_id = ? AND instance_start = ?");

    query.bind(1, providerId);
    query.bind(2, instanceStart);

    if (!query.executeStep()) return std::nullopt;

    return mapRow(query);
}

std::vector<Event> EventRepository::getByCalendar(long long calendarId) {
    SQLite::Statement query(db,
        "SELECT * FROM events WHERE calendar_id = ? "
        "AND deleted_at = 0 ORDER BY start_datetime ASC"
        );

    query.bind(1, calendarId);

    std::vector<Event> events;

    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }

    return events;
}

std::vector<Event> EventRepository::getEventsInRange(long long calendarId, long long start, long long end) {
    SQLite::Statement query(db,
                    "SELECT * FROM events "
                            "WHERE calendar_id = ? "
                            "AND deleted_at = 0 "
                            "AND start_datetime < ? "
                            "AND end_datetime > ? "
                            "ORDER BY start_datetime ASC"
                            );
    query.bind(1, calendarId);
    query.bind(2, end);
    query.bind(3, start);

    std::vector<Event> events;
    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }
    return events;
}

std::vector<Event> EventRepository::getSyncedEvents() {
    SQLite::Statement query(db,
                    "SELECT * FROM events "
                            "WHERE sync_status = ? "
                            "ORDER BY last_modified ASC"
        );
    query.bind(1, SYNCED);

    std::vector<Event> pendingEvents;
    while (query.executeStep()) {
        pendingEvents.push_back(mapRow(query));
    }

    return pendingEvents;
}

std::vector<Event> EventRepository::getPendingRemoteEvents(long long calendarId) {
    SQLite::Statement query(db, "SELECT * FROM events "
                                "WHERE provider_event_id IS NOT NULL AND sync_status != ? AND calendar_id = ?"
        );
    query.bind(1, SYNCED);
    query.bind(2, calendarId);

    std::vector<Event> pendingEvents;
    while (query.executeStep()) {
        pendingEvents.push_back(mapRow(query));
    }

    return pendingEvents;
}
