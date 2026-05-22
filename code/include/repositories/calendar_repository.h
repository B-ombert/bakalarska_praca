#pragma once

#include <optional>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/calendar.h"

class CalendarRepository {
public:
    explicit CalendarRepository(SQLite::Database& db);

    long long upsert(const Calendar& c);
    bool updateById(const Calendar& c);

    std::vector<Calendar> getByAccount(long long accountId);
    std::vector<Calendar> getPendingRemoteCalendars(long long accountId);
    std::optional<Calendar> getById(long long id);
    bool deleteById(long long id);

    std::optional<Calendar> getByProviderId(long long accountId, const std::string& providerCalendarId);
    SQLite::Database& database();

private:
    Calendar mapRow(SQLite::Statement& query);

    SQLite::Database& db;
};
