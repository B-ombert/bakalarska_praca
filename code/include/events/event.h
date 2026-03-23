#pragma once
#include <optional>

#include "types.h"
#include "SQLiteCpp/Backup.h"

class Event {
    public:

    long long id = -1;
    long long calendarId = 0;

    std::string providerEventId;
    std::string providerEtag;

    std::string title;
    std::string description;
    std::string location;

    long long startDateTime = 0;
    long long endDateTime = 0;
    bool allDay = false;

    std::string status = "confirmed";

    std::string recurrenceRule;
    std::string recurrenceId;

    long long deletedAt = 0;

    std::string syncStatus = "synced";

    long long lastModified=0;
    long long createdAt=0;
    long long updatedAt=0;

    Event();

    static Event fromIcal(const std::string& icalBody);
    static Event fromJson(Platform, const std::string& jsonBody);
    std::string ExportToJson(Platform) const;
    std::string ExportToIcal() const;
};

class EventRepository {
    public:
    explicit EventRepository(SQLite::Database& db);

    long long insert(const Event& e);
    bool update(const Event& e);
    bool softDelete(long long id);

    std::optional<Event> getById(long long id);
    std::vector<Event> getByCalendar(long long calendarId);

private:
    SQLite::Database& db;

};