#include "repositories/event_repository.h"

#include <ctime>

#include "utils/sqlite_utils.h"

namespace {

constexpr const char* kEventSelectColumns =
    "id, calendar_id, provider_event_id, provider_master_id, instance_start, type, "
    "title, description, location, timezone, "
    "start_datetime, end_datetime, all_day, "
    "status, recurrence_rule, deleted_at, sync_status, "
    "last_modified, created_at, updated_at";

void BindProviderEventId(SQLite::Statement& query, const int index, const std::string& providerEventId) {
    if (providerEventId.empty()) {
        query.bind(index);
        return;
    }

    query.bind(index, providerEventId);
}

long long InsertEventRow(SQLite::Database& db, const Event& e) {
    SQLite::Statement query(db,
        "INSERT INTO events ("
        "calendar_id, provider_event_id, provider_master_id, instance_start, type, "
        "title, description, location, timezone, "
        "start_datetime, end_datetime, all_day, "
        "status, recurrence_rule, "
        "deleted_at, sync_status, "
        "last_modified, created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    int i = 1;
    query.bind(i++, e.calendarId);
    BindProviderEventId(query, i++, e.providerEventId);
    query.bind(i++, e.providerMasterId);
    query.bind(i++, e.instanceStart);
    query.bind(i++, static_cast<int>(e.type));

    query.bind(i++, e.title);
    query.bind(i++, e.description);
    query.bind(i++, e.location);
    query.bind(i++, e.timezone);

    query.bind(i++, e.startDateTime);
    query.bind(i++, e.endDateTime);
    query.bind(i++, static_cast<int>(e.allDay));

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

}

EventRepository::EventRepository(SQLite::Database &db) : db(db) {}

long long EventRepository::upsert(const Event &e) {
    return RunInSavepoint(db, "event_upsert", [&]() -> long long {
        if (e.id > 0 && getById(e.id).has_value()) {
            updateById(e);
            return e.id;
        }

        if (!e.providerEventId.empty()) {
            const auto existing = getByProviderId(e.calendarId, e.providerEventId);
            if (existing.has_value()) {
                Event updated = e;
                updated.id = existing->id;
                if (updated.createdAt == 0) {
                    updated.createdAt = existing->createdAt;
                }
                updateById(updated);
                return updated.id;
            }
        }

        return InsertEventRow(db, e);
    });
}

long long EventRepository::upsertRemoteSnapshot(const Event& e) {
    return RunInSavepoint(db, "event_upsert_remote_snapshot", [&]() -> long long {
        if (e.id > 0 && getById(e.id).has_value()) {
            updateById(e);
            return e.id;
        }

        if (!e.providerEventId.empty()) {
            const auto existing = getByProviderId(e.calendarId, e.providerEventId);
            if (existing.has_value()) {
                Event updated = e;
                updated.id = existing->id;
                if (updated.createdAt == 0) {
                    updated.createdAt = existing->createdAt;
                }
                updateById(updated);
                return updated.id;
            }
        }

        return InsertEventRow(db, e);
    });
}

bool EventRepository::updateById(const Event& e) {
    return RunInSavepoint(db, "event_update_by_id", [&]() {
        SQLite::Statement query(
            db,
            "UPDATE events SET "
            "calendar_id = ?, "
            "provider_event_id = ?, "
            "provider_master_id = ?, "
            "instance_start = ?, "
            "type = ?, "
            "title = ?, "
            "description = ?, "
            "location = ?, "
            "timezone = ?, "
            "start_datetime = ?, "
            "end_datetime = ?, "
            "all_day = ?, "
            "status = ?, "
            "recurrence_rule = ?, "
            "deleted_at = ?, "
            "sync_status = ?, "
            "last_modified = ?, "
            "created_at = ?, "
            "updated_at = ? "
            "WHERE id = ?");

        int i = 1;
        query.bind(i++, e.calendarId);
        BindProviderEventId(query, i++, e.providerEventId);
        query.bind(i++, e.providerMasterId);
        query.bind(i++, e.instanceStart);
        query.bind(i++, static_cast<int>(e.type));
        query.bind(i++, e.title);
        query.bind(i++, e.description);
        query.bind(i++, e.location);
        query.bind(i++, e.timezone);
        query.bind(i++, e.startDateTime);
        query.bind(i++, e.endDateTime);
        query.bind(i++, static_cast<int>(e.allDay));
        query.bind(i++, e.status);
        query.bind(i++, e.recurrenceRule);
        query.bind(i++, e.deletedAt);
        query.bind(i++, e.syncStatus);
        query.bind(i++, e.lastModified);
        query.bind(i++, e.createdAt);
        query.bind(i++, e.updatedAt);
        query.bind(i++, e.id);

        return query.exec() > 0;
    });
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
    e.timezone    = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

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
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events WHERE id = ?");
    query.bind(1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

std::optional<Event> EventRepository::getByProviderId(const std::string &providerId) {
    SQLite::Statement query(db,
                    std::string("SELECT ") + kEventSelectColumns + " FROM events "
                            "WHERE provider_event_id = ?"
        );

    query.bind(1, providerId);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

std::optional<Event> EventRepository::getByProviderId(const long long calendarId, const std::string& providerId) {
    SQLite::Statement query(
        db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events "
        "WHERE calendar_id = ? AND provider_event_id = ?");

    query.bind(1, calendarId);
    query.bind(2, providerId);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRow(query);
}

bool EventRepository::softDelete(long long id) {
    return RunInSavepoint(db, "event_soft_delete", [&]() {
        SQLite::Statement query(db,
            "UPDATE events SET deleted_at = ?, sync_status = ? WHERE id = ?");

        query.bind(1, (long long)time(nullptr));
        query.bind(2, PENDING_DELETE);
        query.bind(3, id);

        return query.exec() > 0;
    });
}

bool EventRepository::deleteEvent(long long id) {
    return RunInSavepoint(db, "event_delete", [&]() {
        SQLite::Statement query(db, "DELETE FROM events WHERE id = ?");
        query.bind(1, id);

        return query.exec() > 0;
    });
}

int EventRepository::deleteByProviderIdentity(const long long calendarId, const std::string& providerEventId) {
    if (providerEventId.empty()) {
        return 0;
    }

    return RunInSavepoint(db, "event_delete_by_provider_identity", [&]() {
        SQLite::Statement query(
            db,
            "DELETE FROM events "
            "WHERE calendar_id = ? "
            "AND (provider_event_id = ? OR provider_master_id = ?)");
        query.bind(1, calendarId);
        query.bind(2, providerEventId);
        query.bind(3, providerEventId);

        return query.exec();
    });
}

std::optional<Event> EventRepository::getByProviderInstance(const std::string &providerId, long long instanceStart) {
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events "
                                "WHERE provider_event_id = ? AND instance_start = ?");

    query.bind(1, providerId);
    query.bind(2, instanceStart);

    if (!query.executeStep()) return std::nullopt;

    return mapRow(query);
}

std::vector<Event> EventRepository::getByCalendar(long long calendarId) {
    SQLite::Statement query(db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events WHERE calendar_id = ? "
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
                    std::string("SELECT ") + kEventSelectColumns + " FROM events "
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

std::vector<Event> EventRepository::getRecurringMasters(long long calendarId) {
    SQLite::Statement query(
        db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events "
        "WHERE calendar_id = ? "
        "AND deleted_at = 0 "
        "AND recurrence_rule IS NOT NULL "
        "AND recurrence_rule != '' "
        "ORDER BY start_datetime ASC");
    query.bind(1, calendarId);

    std::vector<Event> events;
    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }
    return events;
}

std::vector<Event> EventRepository::getSyncedEvents() {
    SQLite::Statement query(db,
                    std::string("SELECT ") + kEventSelectColumns + " FROM events "
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
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events "
                                "WHERE calendar_id = ? "
                                "AND sync_status != ? "
                                "AND (provider_event_id IS NOT NULL OR sync_status = ?)"
        );
    query.bind(1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);

    std::vector<Event> pendingEvents;
    while (query.executeStep()) {
        pendingEvents.push_back(mapRow(query));
    }

    return pendingEvents;
}

bool EventRepository::hasPendingRemoteEvents(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT 1 FROM events "
        "WHERE calendar_id = ? "
        "AND sync_status != ? "
        "AND (provider_event_id IS NOT NULL OR sync_status = ?) "
        "LIMIT 1");
    query.bind(1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);

    return query.executeStep();
}
