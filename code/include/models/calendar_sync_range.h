#pragma once

#include <string>

struct CalendarSyncRange {
    long long id = 0;
    long long calendarId = 0;
    long long startEpoch = 0;
    long long endEpoch = 0;
    long long syncedAt = 0;
    long long lastViewedAt = 0;
    std::string syncToken;
};
