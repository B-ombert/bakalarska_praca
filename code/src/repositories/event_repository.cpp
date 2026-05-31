#include "repositories/event_repository.h"

#include <ctime>

#include "utils/sqlite_utils.h"

namespace {

constexpr const char* kEventSelectColumns =
    "id, calendar_id, provider_event_id, provider_master_id, recurrence_group_id, instance_start, type, "
    "title, description, location, timezone, "
    "start_datetime, end_datetime, all_day, "
    "status, recurrence_rule, deleted_at, sync_status, "
    "created_at, updated_at";

void BindProviderEventId(SQLite::Statement& query, const int index, const std::string& providerEventId) {
    if (providerEventId.empty()) {
        query.bind(index);
        return;
    }

    query.bind(index, providerEventId);
}

std::string EscapeLikePattern(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '%' || ch == '_' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

long long InsertEventRow(SQLite::Database& db, const Event& e) {
    SQLite::Statement query(db,
        "INSERT INTO events ("
        "calendar_id, provider_event_id, provider_master_id, recurrence_group_id, instance_start, type, "
        "title, description, location, timezone, "
        "start_datetime, end_datetime, all_day, "
        "status, recurrence_rule, "
        "deleted_at, sync_status, "
        "created_at, updated_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

    int i = 1;
    BindInt64(query, i++, e.calendarId);
    BindProviderEventId(query, i++, e.providerEventId);
    query.bind(i++, e.providerMasterId);
    query.bind(i++, e.recurrenceGroupId);
    BindInt64(query, i++, e.instanceStart);
    query.bind(i++, static_cast<int>(e.type));

    query.bind(i++, e.title);
    query.bind(i++, e.description);
    query.bind(i++, e.location);
    query.bind(i++, e.timezone);

    BindInt64(query, i++, e.startDateTime);
    BindInt64(query, i++, e.endDateTime);
    query.bind(i++, static_cast<int>(e.allDay));

    query.bind(i++, e.status);
    query.bind(i++, e.recurrenceRule);

    BindInt64(query, i++, e.deletedAt);
    query.bind(i++, e.syncStatus);

    BindInt64(query, i++, e.createdAt);
    BindInt64(query, i++, e.updatedAt);

    query.exec();
    return db.getLastInsertRowid();
}

bool CalendarUploadsToProvider(SQLite::Database& db, const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT provider_calendar_id, sync_enabled "
        "FROM calendars "
        "WHERE id = ?");
    BindInt64(query, 1, calendarId);
    if (!query.executeStep()) {
        return false;
    }

    const std::string providerCalendarId = query.getColumn(0).isNull()
        ? ""
        : query.getColumn(0).getString();
    const bool syncEnabled = !query.getColumn(1).isNull() && query.getColumn(1).getInt() != 0;
    return syncEnabled && !providerCalendarId.empty();
}

}

EventRepository::EventRepository(SQLite::Database &db) : db(db) {}

RecurrenceOverride EventRepository::mapRecurrenceOverrideRow(SQLite::Statement& query) {
    RecurrenceOverride overrideEntry;
    int col = 0;
    overrideEntry.id = query.getColumn(col++).getInt64();
    overrideEntry.masterEventId = query.getColumn(col++).getInt64();
    overrideEntry.originalStart = query.getColumn(col++).getInt64();
    overrideEntry.type = static_cast<RecurrenceOverrideType>(query.getColumn(col++).getInt());
    overrideEntry.replacementEventId = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    overrideEntry.syncStatus = query.getColumn(col).isNull() ? PENDING_INSERT : query.getColumn(col).getInt(); col++;
    overrideEntry.deletedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    overrideEntry.createdAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64(); col++;
    overrideEntry.updatedAt = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();
    return overrideEntry;
}

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

long long EventRepository::upsertRecurrenceOverride(const RecurrenceOverride& overrideEntry) {
    return RunInSavepoint(db, "recurrence_override_upsert", [&]() -> long long {
        const long long now = std::time(nullptr);
        if (overrideEntry.id > 0) {
            SQLite::Statement update(
                db,
                "UPDATE recurrence_overrides SET "
                "master_event_id = ?, original_start = ?, type = ?, replacement_event_id = ?, "
                "sync_status = ?, deleted_at = ?, created_at = ?, updated_at = ? "
                "WHERE id = ?");
            int i = 1;
            BindInt64(update, i++, overrideEntry.masterEventId);
            BindInt64(update, i++, overrideEntry.originalStart);
            update.bind(i++, static_cast<int>(overrideEntry.type));
            if (overrideEntry.replacementEventId == 0) {
                update.bind(i++);
            }
            else {
                BindInt64(update, i++, overrideEntry.replacementEventId);
            }
            update.bind(i++, overrideEntry.syncStatus);
            BindInt64(update, i++, overrideEntry.deletedAt);
            BindInt64(update, i++, overrideEntry.createdAt == 0 ? now : overrideEntry.createdAt);
            BindInt64(update, i++, now);
            BindInt64(update, i++, overrideEntry.id);
            if (update.exec() > 0) {
                return overrideEntry.id;
            }
        }

        SQLite::Statement existing(
            db,
            "SELECT id FROM recurrence_overrides WHERE master_event_id = ? AND original_start = ?");
        BindInt64(existing, 1, overrideEntry.masterEventId);
        BindInt64(existing, 2, overrideEntry.originalStart);
        if (existing.executeStep()) {
            RecurrenceOverride updated = overrideEntry;
            updated.id = existing.getColumn(0).getInt64();
            return upsertRecurrenceOverride(updated);
        }

        SQLite::Statement insert(
            db,
            "INSERT INTO recurrence_overrides ("
            "master_event_id, original_start, type, replacement_event_id, "
            "sync_status, deleted_at, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        int i = 1;
        BindInt64(insert, i++, overrideEntry.masterEventId);
        BindInt64(insert, i++, overrideEntry.originalStart);
        insert.bind(i++, static_cast<int>(overrideEntry.type));
        if (overrideEntry.replacementEventId == 0) {
            insert.bind(i++);
        }
        else {
            BindInt64(insert, i++, overrideEntry.replacementEventId);
        }
        insert.bind(i++, overrideEntry.syncStatus);
        BindInt64(insert, i++, overrideEntry.deletedAt);
        BindInt64(insert, i++, overrideEntry.createdAt == 0 ? now : overrideEntry.createdAt);
        BindInt64(insert, i++, now);
        insert.exec();
        return db.getLastInsertRowid();
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
            "recurrence_group_id = ?, "
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
            "created_at = ?, "
            "updated_at = ? "
            "WHERE id = ?");

        int i = 1;
        BindInt64(query, i++, e.calendarId);
        BindProviderEventId(query, i++, e.providerEventId);
        query.bind(i++, e.providerMasterId);
        query.bind(i++, e.recurrenceGroupId);
        BindInt64(query, i++, e.instanceStart);
        query.bind(i++, static_cast<int>(e.type));
        query.bind(i++, e.title);
        query.bind(i++, e.description);
        query.bind(i++, e.location);
        query.bind(i++, e.timezone);
        BindInt64(query, i++, e.startDateTime);
        BindInt64(query, i++, e.endDateTime);
        query.bind(i++, static_cast<int>(e.allDay));
        query.bind(i++, e.status);
        query.bind(i++, e.recurrenceRule);
        BindInt64(query, i++, e.deletedAt);
        query.bind(i++, e.syncStatus);
        BindInt64(query, i++, e.createdAt);
        BindInt64(query, i++, e.updatedAt);
        BindInt64(query, i++, e.id);

        return query.exec() > 0;
    });
}

std::optional<long long> EventRepository::moveEventToCalendar(
    const Event& movedEvent,
    const long long targetCalendarId) {
    if (movedEvent.id <= 0 || targetCalendarId <= 0) {
        return std::nullopt;
    }

    return RunInSavepoint(db, "event_move_to_calendar", [&]() -> std::optional<long long> {
        const auto original = getById(movedEvent.id);
        if (!original.has_value()) {
            return std::nullopt;
        }

        const long long now = static_cast<long long>(std::time(nullptr));
        const bool sourceUploads = CalendarUploadsToProvider(db, original->calendarId);
        const bool targetUploads = CalendarUploadsToProvider(db, targetCalendarId);
        const int movedSyncStatus = targetUploads ? PENDING_INSERT : SYNCED;

        Event copy = movedEvent;
        copy.id = -1;
        copy.calendarId = targetCalendarId;
        copy.providerEventId.clear();
        copy.providerMasterId.clear();
        copy.deletedAt = 0;
        copy.syncStatus = movedSyncStatus;
        copy.createdAt = now;
        copy.updatedAt = now;

        if (copy.recurrenceRule.empty()) {
            copy.type = EventType::SINGLE;
            copy.instanceStart = copy.startDateTime;
        }
        else {
            copy.type = EventType::MASTER;
            copy.instanceStart = 0;
        }

        const long long newEventId = InsertEventRow(db, copy);

        SQLite::Statement updateMasterOverrides(
            db,
            "UPDATE recurrence_overrides "
            "SET master_event_id = ?, sync_status = ?, deleted_at = 0, updated_at = ? "
            "WHERE master_event_id = ?");
        BindInt64(updateMasterOverrides, 1, newEventId);
        updateMasterOverrides.bind(2, movedSyncStatus);
        BindInt64(updateMasterOverrides, 3, now);
        BindInt64(updateMasterOverrides, 4, original->id);
        updateMasterOverrides.exec();

        SQLite::Statement updateReplacementOverrides(
            db,
            "UPDATE recurrence_overrides "
            "SET replacement_event_id = ?, sync_status = ?, deleted_at = 0, updated_at = ? "
            "WHERE replacement_event_id = ?");
        BindInt64(updateReplacementOverrides, 1, newEventId);
        updateReplacementOverrides.bind(2, movedSyncStatus);
        BindInt64(updateReplacementOverrides, 3, now);
        BindInt64(updateReplacementOverrides, 4, original->id);
        updateReplacementOverrides.exec();

        if (sourceUploads && !original->providerEventId.empty() && original->syncStatus != PENDING_INSERT) {
            SQLite::Statement markOriginalDeleted(
                db,
                "UPDATE events "
                "SET deleted_at = ?, sync_status = ?, updated_at = ? "
                "WHERE id = ?");
            BindInt64(markOriginalDeleted, 1, now);
            markOriginalDeleted.bind(2, PENDING_DELETE);
            BindInt64(markOriginalDeleted, 3, now);
            BindInt64(markOriginalDeleted, 4, original->id);
            markOriginalDeleted.exec();
        }
        else {
            SQLite::Statement deleteOriginal(db, "DELETE FROM events WHERE id = ?");
            BindInt64(deleteOriginal, 1, original->id);
            deleteOriginal.exec();
        }

        return newEventId;
    });
}

Event EventRepository::mapRow(SQLite::Statement &query) {
    Event e;

    int col = 0;

    e.id = query.getColumn(col++).getInt64();
    e.calendarId = query.getColumn(col++).getInt64();

    e.providerEventId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.providerMasterId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;
    e.recurrenceGroupId = query.getColumn(col).isNull() ? "" : query.getColumn(col).getString(); col++;

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

    e.createdAt    = query.getColumn(col++).getInt64();
    e.updatedAt    = query.getColumn(col).isNull() ? 0 : query.getColumn(col).getInt64();

    return e;
}

std::optional<Event> EventRepository::getById(long long id) {
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events WHERE id = ?");
    BindInt64(query, 1, id);

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

    BindInt64(query, 1, calendarId);
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

        BindInt64(query, 1, static_cast<long long>(time(nullptr)));
        query.bind(2, PENDING_DELETE);
        BindInt64(query, 3, id);

        return query.exec() > 0;
    });
}

int EventRepository::softDeleteAllByCalendar(const long long calendarId) {
    return RunInSavepoint(db, "event_soft_delete_all_by_calendar", [&]() {
        SQLite::Statement query(
            db,
            "UPDATE events "
            "SET deleted_at = ?, sync_status = ?, updated_at = ? "
            "WHERE calendar_id = ? AND deleted_at = 0");

        const long long now = static_cast<long long>(std::time(nullptr));
        BindInt64(query, 1, now);
        query.bind(2, PENDING_DELETE);
        BindInt64(query, 3, now);
        BindInt64(query, 4, calendarId);

        return query.exec();
    });
}

bool EventRepository::deleteEvent(long long id) {
    return RunInSavepoint(db, "event_delete", [&]() {
        SQLite::Statement query(db, "DELETE FROM events WHERE id = ?");
        BindInt64(query, 1, id);

        return query.exec() > 0;
    });
}

int EventRepository::deleteAllByCalendar(const long long calendarId) {
    return RunInSavepoint(db, "event_delete_all_by_calendar", [&]() {
        SQLite::Statement query(db, "DELETE FROM events WHERE calendar_id = ?");
        BindInt64(query, 1, calendarId);
        return query.exec();
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
        BindInt64(query, 1, calendarId);
        query.bind(2, providerEventId);
        query.bind(3, providerEventId);

        return query.exec();
    });
}

int EventRepository::deleteStoredInstancesForMaster(
    const long long calendarId,
    const long long masterEventId,
    const std::string& providerMasterId,
    const std::string& recurrenceGroupId) {
    if (providerMasterId.empty() && recurrenceGroupId.empty()) {
        return 0;
    }

    return RunInSavepoint(db, "event_delete_stored_instances_for_master", [&]() {
        SQLite::Statement query(
            db,
            "DELETE FROM events "
            "WHERE calendar_id = ? "
            "AND id != ? "
            "AND ("
            "    (? != '' AND provider_master_id = ?) "
            "    OR (? != '' AND recurrence_group_id = ?)"
            ")");
        BindInt64(query, 1, calendarId);
        BindInt64(query, 2, masterEventId);
        query.bind(3, providerMasterId);
        query.bind(4, providerMasterId);
        query.bind(5, recurrenceGroupId);
        query.bind(6, recurrenceGroupId);
        return query.exec();
    });
}

bool EventRepository::markRecurrenceOverrideSynced(const long long id) {
    return RunInSavepoint(db, "recurrence_override_mark_synced", [&]() {
        SQLite::Statement query(
            db,
            "UPDATE recurrence_overrides SET sync_status = ?, updated_at = ? WHERE id = ?");
        query.bind(1, SYNCED);
        BindInt64(query, 2, static_cast<long long>(std::time(nullptr)));
        BindInt64(query, 3, id);
        return query.exec() > 0;
    });
}

std::optional<Event> EventRepository::getByProviderInstance(const std::string &providerId, long long instanceStart) {
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events "
                                "WHERE provider_event_id = ? AND instance_start = ?");

    query.bind(1, providerId);
    BindInt64(query, 2, instanceStart);

    if (!query.executeStep()) return std::nullopt;

    return mapRow(query);
}

std::vector<Event> EventRepository::getByCalendar(long long calendarId) {
    SQLite::Statement query(db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events WHERE calendar_id = ? "
        "AND deleted_at = 0 ORDER BY start_datetime ASC"
        );

    BindInt64(query, 1, calendarId);

    std::vector<Event> events;

    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }

    return events;
}

std::vector<Event> EventRepository::searchByTitle(const std::string& keyword, const EventTitleSearchMode mode) {
    if (keyword.empty()) {
        return {};
    }

    const std::string escapedKeyword = EscapeLikePattern(keyword);
    const std::string pattern = mode == EventTitleSearchMode::STARTS_WITH
        ? escapedKeyword + "%"
        : "%" + escapedKeyword + "%";

    SQLite::Statement query(
        db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events "
        "WHERE deleted_at = 0 "
        "AND title LIKE ? ESCAPE '\\' COLLATE NOCASE "
        "ORDER BY start_datetime ASC, title ASC");
    query.bind(1, pattern);

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
    BindInt64(query, 1, calendarId);
    BindInt64(query, 2, end);
    BindInt64(query, 3, start);

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
    BindInt64(query, 1, calendarId);

    std::vector<Event> events;
    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }
    return events;
}

std::vector<Event> EventRepository::getRecurringMastersStartingBefore(const long long calendarId, const long long end) {
    SQLite::Statement query(
        db,
        std::string("SELECT ") + kEventSelectColumns + " FROM events "
        "WHERE calendar_id = ? "
        "AND deleted_at = 0 "
        "AND recurrence_rule IS NOT NULL "
        "AND recurrence_rule != '' "
        "AND start_datetime < ? "
        "ORDER BY start_datetime ASC");
    BindInt64(query, 1, calendarId);
    BindInt64(query, 2, end);

    std::vector<Event> events;
    while (query.executeStep()) {
        events.push_back(mapRow(query));
    }
    return events;
}

std::vector<Event> EventRepository::getPendingRemoteEvents(const long long calendarId) {
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events "
                                "WHERE calendar_id = ? "
                                "AND sync_status != ? "
                                "AND (provider_event_id IS NOT NULL OR sync_status = ?) "
                                "ORDER BY updated_at ASC, id ASC"
        );
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);

    std::vector<Event> pendingEvents;
    while (query.executeStep()) {
        pendingEvents.push_back(mapRow(query));
    }

    return pendingEvents;
}

std::vector<Event> EventRepository::getPendingRemoteEvents(const long long calendarId, const std::size_t limit) {
    SQLite::Statement query(db, std::string("SELECT ") + kEventSelectColumns + " FROM events "
                                "WHERE calendar_id = ? "
                                "AND sync_status != ? "
                                "AND (provider_event_id IS NOT NULL OR sync_status = ?) "
                                "ORDER BY updated_at ASC, id ASC "
                                "LIMIT ?"
        );
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);
    BindInt64(query, 4, static_cast<long long>(limit));

    std::vector<Event> pendingEvents;
    while (query.executeStep()) {
        pendingEvents.push_back(mapRow(query));
    }

    return pendingEvents;
}

int EventRepository::countPendingRemoteEvents(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT COUNT(*) FROM events "
        "WHERE calendar_id = ? "
        "AND sync_status != ? "
        "AND (provider_event_id IS NOT NULL OR sync_status = ?)");
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);

    if (!query.executeStep()) {
        return 0;
    }

    return query.getColumn(0).getInt();
}

bool EventRepository::hasPendingRemoteEvents(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT 1 FROM events "
        "WHERE calendar_id = ? "
        "AND sync_status != ? "
        "AND (provider_event_id IS NOT NULL OR sync_status = ?) "
        "LIMIT 1");
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);

    return query.executeStep();
}

std::optional<RecurrenceOverride> EventRepository::getRecurrenceOverrideById(const long long id) {
    SQLite::Statement query(
        db,
        "SELECT id, master_event_id, original_start, type, replacement_event_id, "
        "sync_status, deleted_at, created_at, updated_at "
        "FROM recurrence_overrides "
        "WHERE id = ?");
    BindInt64(query, 1, id);

    if (!query.executeStep()) {
        return std::nullopt;
    }

    return mapRecurrenceOverrideRow(query);
}

std::vector<RecurrenceOverride> EventRepository::getRecurrenceOverridesForMaster(
    const long long masterEventId,
    const long long start,
    const long long end) {
    SQLite::Statement query(
        db,
        "SELECT id, master_event_id, original_start, type, replacement_event_id, "
        "sync_status, deleted_at, created_at, updated_at "
        "FROM recurrence_overrides "
        "WHERE master_event_id = ? "
        "AND deleted_at = 0 "
        "AND original_start >= ? "
        "AND original_start < ? "
        "ORDER BY original_start ASC");
    BindInt64(query, 1, masterEventId);
    BindInt64(query, 2, start);
    BindInt64(query, 3, end);

    std::vector<RecurrenceOverride> overrides;
    while (query.executeStep()) {
        overrides.push_back(mapRecurrenceOverrideRow(query));
    }

    return overrides;
}

int EventRepository::countPendingRecurrenceOverrides(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT COUNT(*) "
        "FROM recurrence_overrides ro "
        "JOIN events master ON master.id = ro.master_event_id "
        "WHERE master.calendar_id = ? "
        "AND ro.deleted_at = 0 "
        "AND ro.sync_status != ?");
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);

    if (!query.executeStep()) {
        return 0;
    }

    return query.getColumn(0).getInt();
}

bool EventRepository::hasPendingRecurrenceOverrides(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT 1 "
        "FROM recurrence_overrides ro "
        "JOIN events master ON master.id = ro.master_event_id "
        "WHERE master.calendar_id = ? "
        "AND ro.deleted_at = 0 "
        "AND ro.sync_status != ? "
        "LIMIT 1");
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);

    return query.executeStep();
}

std::vector<RecurrenceOverride> EventRepository::getPendingRecurrenceOverrides(const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT ro.id, ro.master_event_id, ro.original_start, ro.type, ro.replacement_event_id, "
        "ro.sync_status, ro.deleted_at, ro.created_at, ro.updated_at "
        "FROM recurrence_overrides ro "
        "JOIN events master ON master.id = ro.master_event_id "
        "WHERE master.calendar_id = ? "
        "AND ro.deleted_at = 0 "
        "AND ro.sync_status != ? "
        "ORDER BY ro.original_start ASC");
    BindInt64(query, 1, calendarId);
    query.bind(2, SYNCED);

    std::vector<RecurrenceOverride> overrides;
    while (query.executeStep()) {
        overrides.push_back(mapRecurrenceOverrideRow(query));
    }

    return overrides;
}
