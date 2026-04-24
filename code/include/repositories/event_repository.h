#pragma once

#include <optional>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/event.h"

class EventRepository {
public:
    explicit EventRepository(SQLite::Database& db);

    long long upsert(const Event& e);
    bool softDelete(long long id);
    bool deleteEvent(long long id);

    Event mapRow(SQLite::Statement& query);
    std::optional<Event> getById(long long id);
    std::optional<Event> getByProviderId(const std::string& providerId);
    std::optional<Event> getByProviderInstance(const std::string& providerId, long long instanceStart);
    std::vector<Event> getByCalendar(long long calendarId);
    std::vector<Event> getEventsInRange(long long calendarId, long long start, long long end);
    std::vector<Event> getSyncedEvents();
    std::vector<Event> getPendingRemoteEvents(long long calendarId);

private:
    SQLite::Database& db;
};
