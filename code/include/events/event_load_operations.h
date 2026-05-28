#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "events/event_occurrence_utils.h"
#include "models/calendar.h"
#include "models/event.h"

struct EventLoadResult {
    std::vector<Event> events;
    std::unordered_map<long long, Event> projectedOccurrences;
    std::unordered_map<long long, long long> projectedOccurrenceMasterIds;
    std::uint64_t fingerprint = 0;
    size_t visibleCount = 0;
    std::string error;
};

struct EventLoadRequest {
    std::string dbPath;
    std::vector<Calendar> calendarSnapshot;
    std::set<long long> visibleCalendarIds;
    std::unordered_map<long long, std::string> calendarProviders;
    VisibleRange visibleRange;
    VisibleRange bufferedRange;
    VisibleRange bufferedUtcRange;
};

EventLoadResult LoadEventsForVisibleRange(const EventLoadRequest& request);
