#include "events/event_occurrence_utils.h"

#include <algorithm>
#include <ctime>

#include "models/rrule.h"
#include "utils/datetime_utils.h"
#include "utils/timezone_utils.h"

long long CurrentLocalDisplayEpoch() {
    return ConvertUtcEpochToTimeZoneDisplayEpoch(
        GetCurrentLocalTimeZoneName(),
        static_cast<long long>(std::time(nullptr)));
}

VisibleRange ConvertDisplayRangeToUtc(const VisibleRange& displayRange) {
    const std::string displayTimezone = GetCurrentLocalTimeZoneName();
    const long long utcStart = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayRange.startEpoch)
        .value_or(displayRange.startEpoch);
    const long long utcEnd = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayRange.endEpoch)
        .value_or(displayRange.endEpoch);
    return VisibleRange{utcStart, utcEnd};
}

VisibleRange ExpandVisibleRangeForRecurrence(const VisibleRange& range, const CalendarViewMode mode) {
    int reserveDays = 2;
    if (mode == CalendarViewMode::MONTH) {
        reserveDays = 14;
    }
    else if (mode == CalendarViewMode::WEEK) {
        reserveDays = 7;
    }

    return VisibleRange{
        std::max(kMinCalendarEpoch, range.startEpoch - static_cast<long long>(reserveDays) * kSecondsPerDay),
        range.endEpoch + static_cast<long long>(reserveDays) * kSecondsPerDay};
}

std::string LimitedRecurrenceRule(const std::string& recurrenceRule, const long long untilEpoch) {
    if (recurrenceRule.empty()) {
        return "";
    }

    RRule rule = RRule().parseRRule(recurrenceRule);
    if (rule.freq == Frequency::UNKNOWN) {
        return recurrenceRule;
    }

    rule.hasCount = false;
    rule.count = 0;
    rule.hasUntil = true;
    rule.until = std::max(0LL, untilEpoch);
    return rule.toGoogleRRule();
}

std::string PreserveRecurrenceEndBoundary(
    const std::string& newRecurrenceRule,
    const std::string& previousRecurrenceRule) {
    if (newRecurrenceRule.empty() || previousRecurrenceRule.empty()) {
        return newRecurrenceRule;
    }

    RRule newRule = RRule().parseRRule(newRecurrenceRule);
    const RRule previousRule = RRule().parseRRule(previousRecurrenceRule);
    if (newRule.freq == Frequency::UNKNOWN || previousRule.freq == Frequency::UNKNOWN) {
        return newRecurrenceRule;
    }

    if (previousRule.hasUntil) {
        newRule.hasCount = false;
        newRule.count = 0;
        newRule.hasUntil = true;
        newRule.until = previousRule.until;
    }
    else if (previousRule.hasCount) {
        newRule.hasCount = true;
        newRule.count = previousRule.count;
    }

    return newRule.toGoogleRRule();
}

namespace {

int MondayWeekday(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    return tm.tm_wday == 0 ? 7 : tm.tm_wday;
}

long long TimeOffsetWithinDay(const long long epoch) {
    return epoch - StartOfUtcDay(epoch);
}

void HydrateFromRemoteMaster(
    Event& event,
    const std::unordered_map<std::string, RemoteMasterMetadata>& remoteMasterMetadata) {
    if (event.providerMasterId.empty()) {
        return;
    }

    const auto masterIt = remoteMasterMetadata.find(event.providerMasterId);
    if (masterIt == remoteMasterMetadata.end()) {
        return;
    }

    const RemoteMasterMetadata& metadata = masterIt->second;
    if (event.title.empty()) {
        event.title = metadata.title;
    }
    if (event.description.empty()) {
        event.description = metadata.description;
    }
    if (event.location.empty()) {
        event.location = metadata.location;
    }
    if (event.timezone.empty()) {
        event.timezone = metadata.timezone;
    }
    if (metadata.allDay) {
        event.allDay = true;
    }
    if (event.recurrenceRule.empty()) {
        event.recurrenceRule = metadata.recurrenceRule;
    }
}

bool MatchesRecurringDay(const Event& event, const RRule& rule, const long long dayEpoch) {
    const long long startDay = StartOfUtcDay(event.GetDisplayStartEpoch());
    if (dayEpoch < startDay) {
        return false;
    }

    const std::tm startTm = EpochToUtcTm(startDay);
    const std::tm dayTm = EpochToUtcTm(dayEpoch);
    switch (rule.freq) {
        case Frequency::DAILY: {
            const long long diffDays = (dayEpoch - startDay) / kSecondsPerDay;
            return diffDays % std::max(1, rule.interval) == 0;
        }
        case Frequency::WEEKLY: {
            const auto& days = rule.byDay.empty() ? std::vector<int>{MondayWeekday(startDay)} : rule.byDay;
            const long long diffWeeks = (StartOfUtcWeek(dayEpoch) - StartOfUtcWeek(startDay)) / (7LL * kSecondsPerDay);
            return diffWeeks >= 0 &&
                   diffWeeks % std::max(1, rule.interval) == 0 &&
                   std::find(days.begin(), days.end(), MondayWeekday(dayEpoch)) != days.end();
        }
        case Frequency::MONTHLY: {
            const int targetDay = !rule.byMonthDay.empty() ? rule.byMonthDay.front() : startTm.tm_mday;
            const int monthDiff = (dayTm.tm_year - startTm.tm_year) * 12 + (dayTm.tm_mon - startTm.tm_mon);
            return monthDiff >= 0 &&
                   monthDiff % std::max(1, rule.interval) == 0 &&
                   dayTm.tm_mday == targetDay;
        }
        case Frequency::YEARLY: {
            const int targetDay = !rule.byMonthDay.empty() ? rule.byMonthDay.front() : startTm.tm_mday;
            const int yearDiff = dayTm.tm_year - startTm.tm_year;
            return yearDiff >= 0 &&
                   yearDiff % std::max(1, rule.interval) == 0 &&
                   dayTm.tm_mon == startTm.tm_mon &&
                   dayTm.tm_mday == targetDay;
        }
        case Frequency::UNKNOWN:
            return false;
    }

    return false;
}

unsigned int CountOccurrencesBeforeDay(const Event& event, const RRule& rule, const long long dayEpoch) {
    const long long startDay = StartOfUtcDay(event.GetDisplayStartEpoch());
    if (dayEpoch <= startDay) {
        return 0;
    }

    unsigned int count = 0;
    for (long long currentDay = startDay; currentDay < dayEpoch; currentDay += kSecondsPerDay) {
        if (MatchesRecurringDay(event, rule, currentDay)) {
            ++count;
        }
    }
    return count;
}

} // namespace

Event BuildEffectiveRemoteEvent(
    Event event,
    const std::unordered_map<std::string, RemoteMasterMetadata>& remoteMasterMetadata) {
    HydrateFromRemoteMaster(event, remoteMasterMetadata);
    return event;
}

Event PreserveSeriesAnchorDate(const Event& master, const Event& editedEvent) {
    Event updated = editedEvent;
    const std::string displayTimezone = !updated.timezone.empty()
        ? updated.timezone
        : (!master.timezone.empty() ? master.timezone : GetCurrentLocalTimeZoneName());

    const long long masterDisplayStart = master.GetDisplayStartEpoch(displayTimezone);
    const long long masterDisplayDay = StartOfUtcDay(masterDisplayStart);

    if (updated.allDay) {
        const long long editedDisplayStart = updated.GetDisplayStartEpoch(displayTimezone);
        const long long editedDisplayEnd = updated.GetDisplayEndEpoch(displayTimezone);
        const long long durationDays = std::max(
            static_cast<long long>(kSecondsPerDay),
            StartOfUtcDay(std::max(editedDisplayStart, editedDisplayEnd - 1)) -
                StartOfUtcDay(editedDisplayStart) + kSecondsPerDay);
        updated.startDateTime = masterDisplayDay;
        updated.endDateTime = masterDisplayDay + durationDays;
        NormalizeAllDayEventRange(updated);
        return updated;
    }

    const long long editedDisplayStart = updated.GetDisplayStartEpoch(displayTimezone);
    const long long editedDisplayEnd = updated.GetDisplayEndEpoch(displayTimezone);
    const long long displayDuration = std::max(0LL, editedDisplayEnd - editedDisplayStart);
    const long long anchoredDisplayStart = masterDisplayDay + TimeOffsetWithinDay(editedDisplayStart);
    const long long anchoredDisplayEnd = anchoredDisplayStart + displayDuration;
    updated.startDateTime = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, anchoredDisplayStart)
        .value_or(anchoredDisplayStart);
    updated.endDateTime = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, anchoredDisplayEnd)
        .value_or(anchoredDisplayEnd);
    return updated;
}

std::vector<Event> ExpandRecurringEventForRange(
    const Event& event,
    const VisibleRange& range,
    const std::unordered_set<long long>& suppressedInstanceStarts,
    const std::unordered_set<long long>& suppressedDisplayDays) {
    std::vector<Event> occurrences;
    if (event.recurrenceRule.empty()) {
        return occurrences;
    }

    const RRule rule = RRule().parseRRule(event.recurrenceRule);
    if (rule.freq == Frequency::UNKNOWN) {
        return occurrences;
    }

    const std::string displayTimezone = event.timezone.empty()
        ? GetCurrentLocalTimeZoneName()
        : event.timezone;
    const long long displayStartEpoch = event.GetDisplayStartEpoch(displayTimezone);
    const long long duration = std::max(0LL, event.endDateTime - event.startDateTime);
    const long long startDay = StartOfUtcDay(displayStartEpoch);
    const long long scanStartDay = std::max(startDay, StartOfUtcDay(range.startEpoch));
    const long long scanEndDay = StartOfUtcDay(std::max(range.startEpoch, range.endEpoch - 1));
    unsigned int generatedCount = rule.hasCount
        ? CountOccurrencesBeforeDay(event, rule, scanStartDay)
        : 0;

    for (long long dayEpoch = scanStartDay; dayEpoch <= scanEndDay; dayEpoch += kSecondsPerDay) {
        if (!MatchesRecurringDay(event, rule, dayEpoch)) {
            continue;
        }
        if (rule.hasCount && generatedCount >= rule.count) {
            break;
        }
        if (rule.hasCount) {
            ++generatedCount;
        }

        Event occurrence = event;
        const long long occurrenceDisplayStart = dayEpoch + TimeOffsetWithinDay(displayStartEpoch);
        const long long occurrenceDisplayEnd = occurrenceDisplayStart + duration;
        if (event.allDay) {
            occurrence.startDateTime = occurrenceDisplayStart;
            occurrence.endDateTime = occurrenceDisplayEnd;
        }
        else {
            occurrence.startDateTime =
                ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, occurrenceDisplayStart).value_or(occurrenceDisplayStart);
            occurrence.endDateTime =
                ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, occurrenceDisplayEnd).value_or(occurrenceDisplayEnd);
        }
        occurrence.instanceStart = occurrence.startDateTime;
        occurrence.type = EventType::OCCURRENCE;

        const long long occurrenceDisplayDay = StartOfUtcDay(occurrenceDisplayStart);
        const long long occurrenceStoredDay = StartOfUtcDay(occurrence.instanceStart);
        if (suppressedInstanceStarts.count(occurrence.instanceStart) > 0 ||
            suppressedDisplayDays.count(occurrenceDisplayDay) > 0 ||
            suppressedDisplayDays.count(occurrenceStoredDay) > 0) {
            continue;
        }

        if (rule.hasUntil && occurrence.startDateTime > rule.until) {
            continue;
        }

        if (occurrence.startDateTime < range.endEpoch && occurrence.endDateTime > range.startEpoch) {
            occurrences.push_back(std::move(occurrence));
        }
    }

    return occurrences;
}
