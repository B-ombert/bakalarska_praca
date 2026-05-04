#pragma once

#include <optional>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/calendar.h"

class CalendarRepository {
public:
    explicit CalendarRepository(SQLite::Database& db);

    long long upsert(const Calendar& c);

    std::vector<Calendar> getAll();
    std::vector<Calendar> getByAccount(long long accountId);
    std::optional<Calendar> getById(long long id);
    bool deleteById(long long id);

    std::vector<Calendar> getByProvider(const std::string& provider);
    std::optional<Calendar> getByProviderId(long long accountId, const std::string& providerCalendarId);

private:
    Calendar mapRow(SQLite::Statement& query);

    SQLite::Database& db;
};
