#include "event.h"

EventRepository::EventRepository(SQLite::Database &db) : db(db) {}

long long EventRepository::insert(const Event &e) {
    SQLite::Statement query(db,
        "INSERT INTO events"
        "(calendar_id, provider_id, provider_etag,"
        "title, description, location,"
        "start_datetime, end_datetime, all_day,"
        "status, recurrence_rule, recurrence_id,"
        "deleted_at, sync_status,"
        "last_modified, created_at, updated_at)"
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        );

    int i = 1;

    query.bind(i++, e.calendarId);
    query.bind(i++, e.providerEventId);
    query.bind(i++, e.providerEtag);
    query.bind(i++, e.title);
    query.bind(i++, e.description);
    query.bind(i++, e.location);
    query.bind(i++, e.startDateTime);
    query.bind(i++, e.endDateTime);
    query.bind(i++, (int)e.allDay);
    query.bind(i++, e.status);
    query.bind(i++, e.recurrenceRule);
    query.bind(i++, e.recurrenceId);
    query.bind(i++, e.deletedAt);
    query.bind(i++, e.syncStatus);
    query.bind(i++, e.lastModified);
    query.bind(i++, e.createdAt);
    query.bind(i++, e.updatedAt);

    query.exec();
    return db.getLastInsertRowid();
}

std::optional<Event> EventRepository::getById(long long id) {
    SQLite::Statement query(db, "SELECT * FROM events where id = ?");
    query.bind(1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    Event e;

    int col = 0;

    e.id = query.getColumn(col++).getInt64();
    e.calendarId = query.getColumn(col++).getInt64();

    e.providerEventId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.providerEtag = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.title = query.getColumn(col++).getString();
    e.description = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.location = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.startDateTime = query.getColumn(col++).getInt64();
    e.endDateTime = query.getColumn(col++).getInt64();
    e.allDay = query.getColumn(col++).getInt();

    e.status = query.getColumn(col).isNull() ? "confirmed" : query.getColumn(col).getString(); col++;

    e.recurrenceRule = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.recurrenceId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

    e.deletedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;

    e.syncStatus = query.getColumn(col).isNull() ? "synced" : query.getColumn(col).getString(); col++;

    e.lastModified = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    e.createdAt = query.getColumn(col++).getInt64();
    e.updatedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

    return e;
}

bool EventRepository::softDelete(long long id) {
    SQLite::Statement query(db,
        "UPDATE events SET deleted_at = ?, sync_status = ? WHERE id = ?");

    query.bind(1, (long long)time(nullptr));
    query.bind(2, id);

    return query.exec() > 0;
}

bool EventRepository::update(const Event &e) {
    SQLite::Statement query(db,
        "UPDATE events"
        "SET"
        "calendar_id = ?, provider_event_id = ?, provider_etag = ?,"
        "title = ?, description = ?, location = ?,"
        "start_datetime = ?, end_datetime = ?, all_day = ?,"
        "status = ?, recurrence_rule = ?, recurrence_id = ?,"
        "deleted_at = ?, sync_status = CASE"
        "WHEN sync_status = 'synced' THEN 'pending_update'"
        "ELSE sync_status"
        "END"
        "last_modified = ?, updated_at = ?"
        "WHERE id = ?"
        );

    int i = 1;

    query.bind(i++, e.calendarId);
    query.bind(i++, e.providerEventId);
    query.bind(i++, e.providerEtag);
    query.bind(i++, e.title);
    query.bind(i++, e.description);
    query.bind(i++, e.location);
    query.bind(i++, e.startDateTime);
    query.bind(i++, e.endDateTime);
    query.bind(i++, (int)e.allDay);
    query.bind(i++, e.status);
    query.bind(i++, e.recurrenceRule);
    query.bind(i++, e.recurrenceId);
    query.bind(i++, e.deletedAt);
    query.bind(i++, e.lastModified);
    query.bind(i++, e.updatedAt);
    query.bind(i++, e.id);

    return query.exec() > 0;
}

std::vector<Event> EventRepository::getByCalendar(long long calendarId) {
    SQLite::Statement query(db,
        "SELECT * FROM events WHERE calendar_id = ?"
        "AND deleted_at IS NULL ORDER BY start_datetime ASC"
        );

    query.bind(1, calendarId);

    std::vector<Event> events;

    while (query.executeStep()) {
        Event e;
        int col = 0;

        e.id = query.getColumn(col++).getInt64();
        e.calendarId = query.getColumn(col++).getInt64();

        e.providerEventId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
        e.providerEtag = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

        e.title = query.getColumn(col++).getString();
        e.description = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
        e.location = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

        e.startDateTime = query.getColumn(col++).getInt64();
        e.endDateTime = query.getColumn(col++).getInt64();
        e.allDay = query.getColumn(col++).getInt();

        e.status = query.getColumn(col).isNull() ? "confirmed" : query.getColumn(col).getString(); col++;

        e.recurrenceRule = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
        e.recurrenceId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

        e.deletedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;

        e.syncStatus = query.getColumn(col).isNull() ? "synced" : query.getColumn(col).getString(); col++;

        e.lastModified = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
        e.createdAt = query.getColumn(col++).getInt64();
        e.updatedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

        events.push_back(std::move(e));
    }

    return events;
}
