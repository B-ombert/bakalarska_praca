#pragma once

#include <optional>
#include <string>
#include <vector>

#include "SQLiteCpp/Backup.h"
#include "models/calendar_sync_range.h"

class CalendarSyncRangeRepository {
public:
    explicit CalendarSyncRangeRepository(SQLite::Database& db);

    bool isRangeCovered(long long calendarId, long long startEpoch, long long endEpoch);
    void markRangeCovered(long long calendarId, long long startEpoch, long long endEpoch);
    std::optional<CalendarSyncRange> getExactRange(long long calendarId, long long startEpoch, long long endEpoch);
    std::vector<CalendarSyncRange> getRangesForCalendar(long long calendarId);
    std::vector<CalendarSyncRange> getMostRecentlyViewedRanges(long long calendarId, int limit);
    void upsertRange(const CalendarSyncRange& range);

private:
    CalendarSyncRange mapRow(SQLite::Statement& query);

    SQLite::Database& db;
};
