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
    query.bind(i++, e.calendarId);
    BindProviderEventId(query, i++, e.providerEventId);
    query.bind(i++, e.providerMasterId);
    query.bind(i++, e.recurrenceGroupId);
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

    query.bind(i++, e.createdAt);
    query.bind(i++, e.updatedAt);

    query.exec();
    return db.getLastInsertRowid();
}

bool CalendarUploadsToProvider(SQLite::Database& db, const long long calendarId) {
    SQLite::Statement query(
        db,
        "SELECT provider_calendar_id, sync_enabled "
        "FROM calendars "
        "WHERE id = ?");
    query.bind(1, calendarId);
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
            update.bind(i++, overrideEntry.masterEventId);
            update.bind(i++, overrideEntry.originalStart);
            update.bind(i++, static_cast<int>(overrideEntry.type));
            if (overrideEntry.replacementEventId == 0) {
                update.bind(i++);
            }
            else {
                update.bind(i++, overrideEntry.replacementEventId);
            }
            update.bind(i++, overrideEntry.syncStatus);
            update.bind(i++, overrideEntry.deletedAt);
            update.bind(i++, overrideEntry.createdAt == 0 ? now : overrideEntry.createdAt);
            update.bind(i++, now);
            update.bind(i++, overrideEntry.id);
            if (update.exec() > 0) {
                return overrideEntry.id;
            }
        }

        SQLite::Statement existing(
            db,
            "SELECT id FROM recurrence_overrides WHERE master_event_id = ? AND original_start = ?");
        existing.bind(1, overrideEntry.masterEventId);
        existing.bind(2, overrideEntry.originalStart);
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
        insert.bind(i++, overrideEntry.masterEventId);
        insert.bind(i++, overrideEntry.originalStart);
        insert.bind(i++, static_cast<int>(overrideEntry.type));
        if (overrideEntry.replacementEventId == 0) {
            insert.bind(i++);
        }
        else {
            insert.bind(i++, overrideEntry.replacementEventId);
        }
        insert.bind(i++, overrideEntry.syncStatus);
        insert.bind(i++, overrideEntry.deletedAt);
        insert.bind(i++, overrideEntry.createdAt == 0 ? now : overrideEntry.createdAt);
        insert.bind(i++, now);
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
        query.bind(i++, e.calendarId);
        BindProviderEventId(query, i++, e.providerEventId);
        query.bind(i++, e.providerMasterId);
        query.bind(i++, e.recurrenceGroupId);
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
        query.bind(i++, e.createdAt);
        query.bind(i++, e.updatedAt);
        query.bind(i++, e.id);

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
        updateMasterOverrides.bind(1, newEventId);
        updateMasterOverrides.bind(2, movedSyncStatus);
        updateMasterOverrides.bind(3, now);
        updateMasterOverrides.bind(4, original->id);
        updateMasterOverrides.exec();

        SQLite::Statement updateReplacementOverrides(
            db,
            "UPDATE recurrence_overrides "
            "SET replacement_event_id = ?, sync_status = ?, deleted_at = 0, updated_at = ? "
            "WHERE replacement_event_id = ?");
        updateReplacementOverrides.bind(1, newEventId);
        updateReplacementOverrides.bind(2, movedSyncStatus);
        updateReplacementOverrides.bind(3, now);
        updateReplacementOverrides.bind(4, original->id);
        updateReplacementOverrides.exec();

        if (sourceUploads && !original->providerEventId.empty() && original->syncStatus != PENDING_INSERT) {
            SQLite::Statement markOriginalDeleted(
                db,
                "UPDATE events "
                "SET deleted_at = ?, sync_status = ?, updated_at = ? "
                "WHERE id = ?");
            markOriginalDeleted.bind(1, now);
            markOriginalDeleted.bind(2, PENDING_DELETE);
            markOriginalDeleted.bind(3, now);
            markOriginalDeleted.bind(4, original->id);
            markOriginalDeleted.exec();
        }
        else {
            SQLite::Statement deleteOriginal(db, "DELETE FROM events WHERE id = ?");
            deleteOriginal.bind(1, original->id);
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
        query.bind(1, calendarId);
        query.bind(2, masterEventId);
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
        query.bind(2, static_cast<long long>(std::time(nullptr)));
        query.bind(3, id);
        return query.exec() > 0;
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
    query.bind(1, calendarId);
    query.bind(2, end);

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
    query.bind(1, calendarId);
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
    query.bind(1, calendarId);
    query.bind(2, SYNCED);
    query.bind(3, PENDING_INSERT);
    query.bind(4, static_cast<long long>(limit));

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
    query.bind(1, calendarId);
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
    query.bind(1, calendarId);
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
    query.bind(1, id);

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
    query.bind(1, masterEventId);
    query.bind(2, start);
    query.bind(3, end);

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
    query.bind(1, calendarId);
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
    query.bind(1, calendarId);
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
    query.bind(1, calendarId);
    query.bind(2, SYNCED);

    std::vector<RecurrenceOverride> overrides;
    while (query.executeStep()) {
        overrides.push_back(mapRecurrenceOverrideRow(query));
    }

    return overrides;
}
