#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "models/event.h"
#include "ui/calendar_ui_shared.h"

struct VisibleRange {
    long long startEpoch = 0;
    long long endEpoch = 0;
};

struct RemoteMasterMetadata {
    std::string title;
    std::string description;
    std::string location;
    std::string timezone;
    std::string recurrenceRule;
    bool allDay = false;
};

VisibleRange ExpandVisibleRangeForRecurrence(const VisibleRange& range, CalendarViewMode mode);
VisibleRange ConvertDisplayRangeToUtc(const VisibleRange& displayRange);
long long CurrentLocalDisplayEpoch();

std::string LimitedRecurrenceRule(const std::string& recurrenceRule, long long untilEpoch);
std::string PreserveRecurrenceEndBoundary(const std::string& newRecurrenceRule,
                                          const std::string& previousRecurrenceRule);
Event PreserveSeriesAnchorDate(const Event& master, const Event& editedEvent);
Event BuildEffectiveRemoteEvent(
    Event event,
    const std::unordered_map<std::string, RemoteMasterMetadata>& remoteMasterMetadata);
std::vector<Event> ExpandRecurringEventForRange(
    const Event& event,
    const VisibleRange& range,
    const std::unordered_set<long long>& suppressedInstanceStarts = {},
    const std::unordered_set<long long>& suppressedDisplayDays = {});
