#include "events/event_load_operations.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

#include <SQLiteCpp/SQLiteCpp.h>

#include "repositories/event_repository.h"
#include "utils/calendar_colors.h"
#include "utils/datetime_utils.h"
#include "utils/provider_utils.h"
#include "utils/sqlite_utils.h"
#include "utils/timezone_utils.h"

namespace {

void HashCombine(std::uint64_t& seed, const std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

std::uint64_t ComputeEventListFingerprint(const std::vector<Event>& events) {
    std::uint64_t seed = events.size();
    for (const auto& event : events) {
        HashCombine(seed, static_cast<std::uint64_t>(event.id));
        HashCombine(seed, static_cast<std::uint64_t>(event.calendarId));
        HashCombine(seed, static_cast<std::uint64_t>(event.GetDisplayStartEpoch()));
        HashCombine(seed, static_cast<std::uint64_t>(event.GetDisplayEndEpoch()));
        HashCombine(seed, static_cast<std::uint64_t>(event.deletedAt));
        HashCombine(seed, static_cast<std::uint64_t>(event.syncStatus));
        HashCombine(seed, static_cast<std::uint64_t>(std::hash<std::string>{}(event.title)));
        HashCombine(seed, static_cast<std::uint64_t>(std::hash<std::string>{}(event.recurrenceRule)));
        HashCombine(seed, static_cast<std::uint64_t>(std::hash<std::string>{}(event.colorHex)));
    }
    return seed;
}

void AddEventMasterMetadata(
    const Event& event,
    std::unordered_map<std::string, RemoteMasterMetadata>& remoteMasterMetadata) {
    if (event.providerEventId.empty()) {
        return;
    }

    remoteMasterMetadata[event.providerEventId] = RemoteMasterMetadata{
        event.title,
        event.description,
        event.location,
        event.timezone,
        event.recurrenceRule,
        event.allDay};
}

void LoadCalendarEvents(
    EventRepository& eventRepository,
    const Calendar& calendar,
    const VisibleRange& bufferedUtcRange,
    const std::string& calendarColor,
    std::unordered_map<long long, Event>& uniqueEvents,
    std::unordered_map<std::string, RemoteMasterMetadata>& remoteMasterMetadata,
    std::unordered_map<long long, std::unordered_set<std::string>>& remoteOccurrenceMasterIdsByCalendar) {
    auto calendarEvents = eventRepository.getEventsInRange(
        calendar.id,
        bufferedUtcRange.startEpoch,
        bufferedUtcRange.endEpoch);
    auto recurringMasters = eventRepository.getRecurringMastersStartingBefore(
        calendar.id,
        bufferedUtcRange.endEpoch);

    for (auto& event : calendarEvents) {
        event.colorHex = calendarColor;
        AddEventMasterMetadata(event, remoteMasterMetadata);
        if (!event.providerMasterId.empty()) {
            remoteOccurrenceMasterIdsByCalendar[event.calendarId].insert(event.providerMasterId);
        }
        uniqueEvents[event.id] = std::move(event);
    }

    for (auto& event : recurringMasters) {
        event.colorHex = calendarColor;
        AddEventMasterMetadata(event, remoteMasterMetadata);
        uniqueEvents[event.id] = std::move(event);
    }
}

bool ShouldSkipMicrosoftMasterWithStoredOccurrences(
    const Event& event,
    const std::unordered_map<long long, std::string>& calendarProviders,
    const std::unordered_map<long long, std::unordered_set<std::string>>& remoteOccurrenceMasterIdsByCalendar) {
    const auto providerIt = calendarProviders.find(event.calendarId);
    const bool isMicrosoftCalendar =
        providerIt != calendarProviders.end() && providerIt->second == kProviderMicrosoft;
    if (!isMicrosoftCalendar ||
        event.type != EventType::MASTER ||
        event.recurrenceRule.empty()) {
        return false;
    }

    const auto occurrenceMastersIt = remoteOccurrenceMasterIdsByCalendar.find(event.calendarId);
    return occurrenceMastersIt != remoteOccurrenceMasterIdsByCalendar.end() &&
           occurrenceMastersIt->second.count(event.providerEventId) > 0;
}

void AddRecurringOccurrences(
    EventRepository& eventRepository,
    const Event& master,
    const VisibleRange& bufferedRange,
    const VisibleRange& bufferedUtcRange,
    EventLoadResult& result,
    long long& nextProjectedOccurrenceId) {
    std::unordered_set<long long> suppressedInstanceStarts;
    std::unordered_set<long long> suppressedDisplayDays;
    const std::string displayTimezone = master.timezone.empty()
        ? GetCurrentLocalTimeZoneName()
        : master.timezone;
    const long long overrideRangeStart =
        std::min(bufferedRange.startEpoch, bufferedUtcRange.startEpoch) - kSecondsPerDay;
    const long long overrideRangeEnd =
        std::max(bufferedRange.endEpoch, bufferedUtcRange.endEpoch) + kSecondsPerDay;

    for (const auto& overrideEntry : eventRepository.getRecurrenceOverridesForMaster(
             master.id,
             overrideRangeStart,
             overrideRangeEnd)) {
        if (overrideEntry.type == RecurrenceOverrideType::CANCELLED ||
            overrideEntry.type == RecurrenceOverrideType::MODIFIED) {
            suppressedInstanceStarts.insert(overrideEntry.originalStart);
            suppressedDisplayDays.insert(StartOfUtcDay(overrideEntry.originalStart));
            suppressedDisplayDays.insert(StartOfUtcDay(
                ConvertUtcEpochToTimeZoneDisplayEpoch(displayTimezone, overrideEntry.originalStart)));
        }
    }

    auto occurrences = ExpandRecurringEventForRange(
        master,
        bufferedRange,
        suppressedInstanceStarts,
        suppressedDisplayDays);
    for (auto& occurrence : occurrences) {
        const long long projectedId = nextProjectedOccurrenceId--;
        occurrence.id = projectedId;
        result.projectedOccurrenceMasterIds[projectedId] = master.id;
        result.projectedOccurrences[projectedId] = occurrence;
    }
    result.events.insert(
        result.events.end(),
        std::make_move_iterator(occurrences.begin()),
        std::make_move_iterator(occurrences.end()));
}

void FinalizeEventLoadResult(EventLoadResult& result, const VisibleRange& visibleRange) {
    std::sort(result.events.begin(), result.events.end(), [](const Event& lhs, const Event& rhs) {
        if (lhs.GetDisplayStartEpoch() != rhs.GetDisplayStartEpoch()) {
            return lhs.GetDisplayStartEpoch() < rhs.GetDisplayStartEpoch();
        }
        if (lhs.GetDisplayEndEpoch() != rhs.GetDisplayEndEpoch()) {
            return lhs.GetDisplayEndEpoch() < rhs.GetDisplayEndEpoch();
        }
        return lhs.id < rhs.id;
    });

    result.fingerprint = ComputeEventListFingerprint(result.events);
    result.visibleCount = static_cast<size_t>(std::count_if(
        result.events.begin(),
        result.events.end(),
        [&](const Event& event) {
            return event.GetDisplayStartEpoch() < visibleRange.endEpoch &&
                   event.GetDisplayEndEpoch() > visibleRange.startEpoch;
        }));
    HashCombine(result.fingerprint, static_cast<std::uint64_t>(visibleRange.startEpoch));
    HashCombine(result.fingerprint, static_cast<std::uint64_t>(visibleRange.endEpoch));
}

} // namespace

EventLoadResult LoadEventsForVisibleRange(const EventLoadRequest& request) {
    EventLoadResult result;

    try {
        SQLite::Database db(request.dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        ConfigureSqliteConnection(db);
        EventRepository eventRepository(db);
        std::unordered_map<long long, Event> uniqueEvents;
        std::unordered_map<std::string, RemoteMasterMetadata> remoteMasterMetadata;
        std::unordered_map<long long, std::unordered_set<std::string>> remoteOccurrenceMasterIdsByCalendar;

        for (const auto& calendar : request.calendarSnapshot) {
            if (request.visibleCalendarIds.count(calendar.id) == 0) {
                continue;
            }

            LoadCalendarEvents(
                eventRepository,
                calendar,
                request.bufferedUtcRange,
                NormalizeCalendarColor(calendar.colorHex),
                uniqueEvents,
                remoteMasterMetadata,
                remoteOccurrenceMasterIdsByCalendar);
        }

        long long nextProjectedOccurrenceId = -1;
        for (const auto& [_, event] : uniqueEvents) {
            Event hydratedEvent = BuildEffectiveRemoteEvent(event, remoteMasterMetadata);
            if (ShouldSkipMicrosoftMasterWithStoredOccurrences(
                    hydratedEvent,
                    request.calendarProviders,
                    remoteOccurrenceMasterIdsByCalendar)) {
                continue;
            }

            if (!hydratedEvent.recurrenceRule.empty() && hydratedEvent.providerMasterId.empty()) {
                AddRecurringOccurrences(
                    eventRepository,
                    hydratedEvent,
                    request.bufferedRange,
                    request.bufferedUtcRange,
                    result,
                    nextProjectedOccurrenceId);
                continue;
            }

            if (hydratedEvent.GetDisplayStartEpoch() < request.bufferedRange.endEpoch &&
                hydratedEvent.GetDisplayEndEpoch() > request.bufferedRange.startEpoch) {
                result.events.push_back(std::move(hydratedEvent));
            }
        }

        FinalizeEventLoadResult(result, request.visibleRange);
    }
    catch (const std::exception& ex) {
        result.error = ex.what();
    }

    return result;
}
