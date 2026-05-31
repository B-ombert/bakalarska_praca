#include "sync/calendar_sync_service.h"

#include <unordered_map>
#include <unordered_set>
#include <ctime>
#include <cstddef>
#include <utility>

#include "calendar_sync_internal.h"

namespace {

constexpr std::size_t kMaxPendingEventsPerCalendarPass = 500;
constexpr std::size_t kRemotePersistChunkSize = 250;

bool IsSamePendingUploadSnapshot(const Event& current, const Event& pending) {
    return current.syncStatus == pending.syncStatus &&
           current.providerEventId == pending.providerEventId &&
           current.providerMasterId == pending.providerMasterId &&
           current.recurrenceGroupId == pending.recurrenceGroupId &&
           current.instanceStart == pending.instanceStart &&
           current.type == pending.type &&
           current.title == pending.title &&
           current.description == pending.description &&
           current.location == pending.location &&
           current.timezone == pending.timezone &&
           current.startDateTime == pending.startDateTime &&
           current.endDateTime == pending.endDateTime &&
           current.allDay == pending.allDay &&
           current.status == pending.status &&
           current.recurrenceRule == pending.recurrenceRule &&
           current.deletedAt == pending.deletedAt;
}

bool CanCompletePendingUpload(EventRepository& eventRepo, const Event& pending) {
    const auto current = eventRepo.getById(pending.id);
    return current.has_value() && IsSamePendingUploadSnapshot(*current, pending);
}

void FlushRemoteEventBuffer(EventRepository& eventRepo, std::vector<Event>& buffer) {
    if (buffer.empty()) {
        return;
    }

    eventRepo.runBulkSavepoint("remote_event_chunk", [&]() {
        for (auto& event : buffer) {
            sync_internal::PersistRemoteEvent(eventRepo, std::move(event));
        }
    });
    buffer.clear();
}

} // namespace

CalendarSyncService::CalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo)
    : calendarRepo(calendarRepo), eventRepo(eventRepo) {}

void CalendarSyncService::syncCalendarsIncremental(const long long accountId,
                                                   const std::vector<Calendar>& remoteCalendars,
                                                   const std::string& accessToken,
                                                   const bool allowInitialEventBootstrap) {
    syncCalendarsIncremental(
        accountId,
        remoteCalendars,
        [&accessToken]() { return accessToken; },
        allowInitialEventBootstrap);
}

void CalendarSyncService::syncCalendarsIncremental(const long long accountId,
                                                   const std::vector<Calendar>& remoteCalendars,
                                                   const AccessTokenProvider& accessTokenProvider,
                                                   const bool allowInitialEventBootstrap) {
    std::vector<Calendar> localCalendars = calendarRepo.getByAccount(accountId);
    std::unordered_map<std::string, Calendar*> localMap;
    std::unordered_map<std::string, bool> remoteIds;
    std::unordered_set<std::string> pendingDeletedCalendarIds;

    for (const auto& calendar : calendarRepo.getPendingRemoteCalendars(accountId)) {
        if (calendar.syncStatus == PENDING_DELETE && !calendar.providerCalendarId.empty()) {
            pendingDeletedCalendarIds.insert(calendar.providerCalendarId);
        }
    }

    for (auto& calendar : localCalendars) {
        if (!calendar.providerCalendarId.empty()) {
            localMap[calendar.providerCalendarId] = &calendar;
        }
    }

    for (const auto& remote : remoteCalendars) {
        if (remote.deletedAt != 0 || !remote.syncEnabled) {
            continue;
        }

        if (pendingDeletedCalendarIds.count(remote.providerCalendarId) > 0) {
            continue;
        }

        Calendar merged = remote;
        merged.accountId = accountId;
        remoteIds[remote.providerCalendarId] = true;

        const auto existingIt = localMap.find(remote.providerCalendarId);
        if (existingIt != localMap.end()) {
            const Calendar& local = *existingIt->second;
            merged.id = local.id;
            merged.colorHex = local.colorHex;
            if (local.syncStatus == PENDING_UPDATE) {
                merged.name = local.name;
                merged.description = local.description;
                merged.timezone = local.timezone;
                merged.syncStatus = local.syncStatus;
            }
        }

        calendarRepo.upsert(merged);

        if (merged.id == 0 || merged.colorHex.empty()) {
            const auto persisted = calendarRepo.getByProviderId(merged.accountId, merged.providerCalendarId);
            if (persisted.has_value()) {
                merged = *persisted;
            }
        }

        if (!allowInitialEventBootstrap) {
            continue;
        }

        std::vector<Event> remoteEventBuffer;
        fetchRemoteChangesStreaming(
            merged,
            accessTokenProvider,
            calendarRepo,
            [&](Event event) {
                remoteEventBuffer.push_back(std::move(event));
                if (remoteEventBuffer.size() >= kRemotePersistChunkSize) {
                    FlushRemoteEventBuffer(eventRepo, remoteEventBuffer);
                }
            });
        FlushRemoteEventBuffer(eventRepo, remoteEventBuffer);
    }

    for (const auto& local : localCalendars) {
        if (remoteIds.find(local.providerCalendarId) != remoteIds.end()) {
            continue;
        }

        calendarRepo.deleteById(local.id);
    }
}

SyncPendingEventsResult CalendarSyncService::syncPendingCalendarsForAccount(
    const AccessTokenProvider& accessTokenProvider,
    const long long accountId) {
    SyncPendingEventsResult result;
    RepositoryHolder repository{calendarRepo, eventRepo};

    const std::vector<Calendar> pendingCalendars = calendarRepo.getPendingRemoteCalendars(accountId);
    for (auto calendar : pendingCalendars) {
        if (calendar.isReadOnly) {
            continue;
        }

        ++result.pendingEventCount;
        const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
        if (accessToken.empty()) {
            continue;
        }

        bool accepted = false;
        if (calendar.syncStatus == PENDING_INSERT || calendar.syncStatus == PENDING_UPDATE) {
            accepted = uploadCalendar(calendar, accessToken, repository);
        }
        else if (calendar.syncStatus == PENDING_DELETE) {
            accepted = deleteRemoteCalendar(calendar, accessToken, repository);
        }

        if (accepted) {
            ++result.acceptedEventCount;
        }
    }

    return result;
}

SyncPendingEventsResult CalendarSyncService::syncPendingEventsForAccount(const std::string& accessToken, const long long accountId) {
    return syncPendingEventsForAccount([&accessToken]() { return accessToken; }, accountId);
}

SyncPendingEventsResult CalendarSyncService::syncPendingEventsForAccount(const AccessTokenProvider& accessTokenProvider, const long long accountId) {
    SyncPendingEventsResult result;
    const std::vector<Calendar> calendars = calendarRepo.getByAccount(accountId);
    for (const auto& calendar : calendars) {
        if (calendar.isReadOnly || !calendar.syncEnabled || calendar.providerCalendarId.empty()) {
            continue;
        }

        const SyncPendingEventsResult calendarResult = syncPendingEvents(accessTokenProvider, calendar);
        result.pendingEventCount += calendarResult.pendingEventCount;
        result.acceptedEventCount += calendarResult.acceptedEventCount;
    }
    return result;
}

SyncPendingEventsResult CalendarSyncService::syncPendingEvents(const std::string& accessToken, const Calendar& calendar) {
    return syncPendingEvents([&accessToken]() { return accessToken; }, calendar);
}

SyncPendingEventsResult CalendarSyncService::syncPendingEvents(const AccessTokenProvider& accessTokenProvider, const Calendar& calendar) {
    if (calendar.isReadOnly || !calendar.syncEnabled || calendar.providerCalendarId.empty()) {
        return {};
    }

    if (!eventRepo.hasPendingRemoteEvents(calendar.id) &&
        !eventRepo.hasPendingRecurrenceOverrides(calendar.id)) {
        return {};
    }

    const std::vector<Event> pendingEvents =
        eventRepo.getPendingRemoteEvents(calendar.id, kMaxPendingEventsPerCalendarPass);
    SyncPendingEventsResult result;
    std::vector<Event> batchEvents;

    for (const auto& pending : pendingEvents) {
        if (pending.syncStatus == PENDING_DELETE && pending.providerEventId.empty()) {
            eventRepo.deleteEvent(pending.id);
            continue;
        }

        if (pending.syncStatus == PENDING_INSERT ||
            pending.syncStatus == PENDING_DELETE ||
            (pending.syncStatus == PENDING_UPDATE && !pending.providerEventId.empty())) {
            batchEvents.push_back(pending);
            ++result.pendingEventCount;
        }
    }

    auto resetMicrosoftSyncTokenAfterUpload = [&]() {};

    if (batchEvents.empty()) {
        resetMicrosoftSyncTokenAfterUpload();
        return result;
    }

    std::unordered_map<long long, Event> pendingById;
    for (const auto& event : batchEvents) {
        pendingById[event.id] = event;
    }

    std::vector<EventBatchUploadResult> results;
    const std::vector<EventBatchRequest> batchRequests = buildEventBatchRequests(accessTokenProvider, calendar, batchEvents);
    for (const auto& batchRequest : batchRequests) {
        const HttpResponse response = PerformHttpRequestWithResponse(batchRequest.request);
        auto parsedResults = parseEventBatchResponse(response, batchRequest.localEventIds);
        results.insert(results.end(), parsedResults.begin(), parsedResults.end());
    }

    for (const auto& uploadResult : results) {
        const auto pendingIt = pendingById.find(uploadResult.localEventId);
        if (pendingIt == pendingById.end()) {
            continue;
        }

        const Event& pending = pendingIt->second;
        const bool requestSucceeded = uploadResult.httpStatus >= 200 && uploadResult.httpStatus < 300;
        const bool deletedRemoteAlreadyMissing =
            pending.syncStatus == PENDING_DELETE && uploadResult.httpStatus == 404;
        if (!requestSucceeded && !deletedRemoteAlreadyMissing) {
            continue;
        }

        if (!CanCompletePendingUpload(eventRepo, pending)) {
            continue;
        }

        if (pending.syncStatus == PENDING_DELETE) {
            eventRepo.deleteEvent(pending.id);
            ++result.acceptedEventCount;
            continue;
        }

        Event synced = pending;
        if (uploadResult.responseBody.is_object() && !uploadResult.responseBody.empty()) {
            synced = parseRemoteEvent(uploadResult.responseBody);
            synced.id = pending.id;
            synced.calendarId = calendar.id;
            synced.createdAt = pending.createdAt == 0 ? std::time(nullptr) : pending.createdAt;
            synced.deletedAt = 0;
        }

        if (synced.providerEventId.empty()) {
            synced.providerEventId = pending.providerEventId;
        }
        if (pending.syncStatus == PENDING_INSERT && synced.providerEventId.empty()) {
            continue;
        }
        if (!pending.recurrenceRule.empty() && synced.recurrenceRule.empty()) {
            synced.recurrenceRule = pending.recurrenceRule;
            synced.type = pending.type;
        }

        synced.updatedAt = std::time(nullptr);
        synced.syncStatus = SYNCED;
        if (!eventRepo.updateById(synced)) {
            eventRepo.upsert(synced);
        }
        ++result.acceptedEventCount;
    }

    resetMicrosoftSyncTokenAfterUpload();

    for (const auto& overrideEntry : eventRepo.getPendingRecurrenceOverrides(calendar.id)) {
        ++result.pendingEventCount;
        if (uploadRecurrenceOverride(accessTokenProvider, calendar, overrideEntry)) {
            eventRepo.markRecurrenceOverrideSynced(overrideEntry.id);
            ++result.acceptedEventCount;
        }
    }

    resetMicrosoftSyncTokenAfterUpload();
    return result;
}

std::vector<EventBatchRequest> CalendarSyncService::buildEventBatchRequests(
    const AccessTokenProvider& accessTokenProvider,
    const Calendar& calendar,
    const std::vector<Event>& events) {
    std::vector<EventBatchRequest> requests;

    for (const auto& pending : events) {
        std::optional<json> payload = exportEventPayload(pending);
        HttpRequest req;
        const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
        if (accessToken.empty()) {
            continue;
        }
        if (pending.syncStatus == PENDING_INSERT) {
            req = sync_internal::BuildAuthorizedRequest(buildEventsCollectionUrl(calendar), accessToken, POST, payload);
        }
        else if (pending.syncStatus == PENDING_UPDATE && !pending.providerEventId.empty()) {
            req = sync_internal::BuildAuthorizedRequest(
                buildEventItemUrl(calendar, pending.providerEventId), accessToken, PATCH, payload);
        }
        else if (pending.syncStatus == PENDING_DELETE && !pending.providerEventId.empty()) {
            req = sync_internal::BuildAuthorizedRequest(
                buildEventItemUrl(calendar, pending.providerEventId), accessToken, DELETE_);
        }
        else {
            continue;
        }

        requests.push_back(EventBatchRequest{std::move(req), {pending.id}});
    }

    return requests;
}

std::vector<EventBatchUploadResult> CalendarSyncService::parseEventBatchResponse(
    const HttpResponse& response,
    const std::vector<long long>& localEventIds) {
    std::vector<EventBatchUploadResult> results;
    if (localEventIds.size() != 1) {
        return results;
    }

    EventBatchUploadResult result;
    result.localEventId = localEventIds.front();
    result.httpStatus = response.statusCode;
    const auto parsed = sync_internal::ParseResponseJson(response.body);
    if (parsed.has_value()) {
        result.responseBody = *parsed;
    }
    results.push_back(std::move(result));
    return results;
}

bool CalendarSyncService::uploadRecurrenceOverride(
    const AccessTokenProvider&,
    const Calendar&,
    const RecurrenceOverride&) {
    return false;
}

bool CalendarSyncService::fetchRemoteRangeStreaming(
    const Calendar&,
    const long long,
    const long long,
    const AccessTokenProvider&,
    CalendarRepository&,
    const std::function<void(Event)>&) {
    return false;
}

bool CalendarSyncService::fetchAndStoreRemoteEventsInRange(
    const Calendar& calendar,
    const long long startEpoch,
    const long long endEpoch,
    const AccessTokenProvider& accessTokenProvider,
    RepositoryHolder& repository) {
    std::vector<Event> remoteEventBuffer;
    const bool success = fetchRemoteRangeStreaming(
        calendar,
        startEpoch,
        endEpoch,
        accessTokenProvider,
        repository.calendarRepository,
        [&](Event event) {
            remoteEventBuffer.push_back(std::move(event));
            if (remoteEventBuffer.size() >= kRemotePersistChunkSize) {
                FlushRemoteEventBuffer(repository.eventRepository, remoteEventBuffer);
            }
        });
    FlushRemoteEventBuffer(repository.eventRepository, remoteEventBuffer);
    return success;
}
