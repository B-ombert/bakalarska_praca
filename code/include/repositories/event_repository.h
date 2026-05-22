#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/event.h"
#include "models/recurrence_override.h"
#include "utils/sqlite_utils.h"

class EventRepository {
public:
    explicit EventRepository(SQLite::Database& db);

    long long upsert(const Event& e);
    long long upsertRecurrenceOverride(const RecurrenceOverride& overrideEntry);
    long long upsertRemoteSnapshot(const Event& e);
    bool updateById(const Event& e);
    bool softDelete(long long id);
    bool deleteEvent(long long id);
    int deleteByProviderIdentity(long long calendarId, const std::string& providerEventId);
    int deleteStoredInstancesForMaster(long long calendarId, long long masterEventId, const std::string& providerMasterId, const std::string& recurrenceGroupId);
    bool markRecurrenceOverrideSynced(long long id);

    Event mapRow(SQLite::Statement& query);
    std::optional<Event> getById(long long id);
    std::optional<Event> getByProviderId(const std::string& providerId);
    std::optional<Event> getByProviderId(long long calendarId, const std::string& providerId);
    std::optional<Event> getByProviderInstance(const std::string& providerId, long long instanceStart);
    std::vector<Event> getByCalendar(long long calendarId);
    std::vector<Event> getEventsInRange(long long calendarId, long long start, long long end);
    std::vector<Event> getRecurringMasters(long long calendarId);
    std::vector<Event> getRecurringMastersStartingBefore(long long calendarId, long long end);
    std::vector<Event> getPendingRemoteEvents(long long calendarId);
    std::vector<Event> getPendingRemoteEvents(long long calendarId, std::size_t limit);
    int countPendingRemoteEvents(long long calendarId);
    bool hasPendingRemoteEvents(long long calendarId);
    std::optional<RecurrenceOverride> getRecurrenceOverrideById(long long id);
    std::vector<RecurrenceOverride> getRecurrenceOverridesForMaster(long long masterEventId, long long start, long long end);
    std::vector<RecurrenceOverride> getPendingRecurrenceOverrides(long long calendarId);
    int countPendingRecurrenceOverrides(long long calendarId);
    bool hasPendingRecurrenceOverrides(long long calendarId);

    template <typename Func>
    auto runBulkSavepoint(const std::string& name, Func&& func) -> decltype(func()) {
        return RunInSavepoint(db, name, std::forward<Func>(func));
    }

private:
    SQLite::Database& db;
    RecurrenceOverride mapRecurrenceOverrideRow(SQLite::Statement& query);
};
