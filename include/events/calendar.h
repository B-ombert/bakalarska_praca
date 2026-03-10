#pragma once
#include "event.h"

struct Calendar {
    long long id;

    std::string provider;
    std::string providerCalendarId;

    std::string name;

    std::string syncToken;

    long long createdAt = 0;
    long long updatedAt = 0;
};

class CalendarRepository {
    public:
    explicit CalendarRepository(SQLite::Database& db);

    long long insert(const Calendar& c);

    bool update(const Calendar& c);

    std::vector<Calendar> getAll();
    std::optional<Calendar> getById(long long id);

    std::optional<Calendar> getByProviderId(
        const std::string& provider,
        const std::string& providerCalendarId
    );

    private:
    Calendar mapRow(SQLite::Statement& query);

    SQLite::Database& db;
};