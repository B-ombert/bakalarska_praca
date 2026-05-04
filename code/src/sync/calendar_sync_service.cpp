#include "sync/calendar_sync_service.h"

#include <unordered_map>

#include "calendar_sync_internal.h"

CalendarSyncService::CalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo)
    : calendarRepo(calendarRepo), eventRepo(eventRepo) {}

void CalendarSyncService::syncCalendarsIncremental(const long long accountId,
                                                   const std::vector<Calendar>& remoteCalendars,
                                                   const std::string& accessToken) {
    std::vector<Calendar> localCalendars = calendarRepo.getByAccount(accountId);
    std::unordered_map<std::string, Calendar*> localMap;
    std::unordered_map<std::string, bool> remoteIds;

    for (auto& calendar : localCalendars) {
        localMap[calendar.providerCalendarId] = &calendar;
    }

    for (const auto& remote : remoteCalendars) {
        Calendar merged = remote;
        merged.accountId = accountId;
        remoteIds[remote.providerCalendarId] = true;

        const auto existingIt = localMap.find(remote.providerCalendarId);
        if (existingIt != localMap.end()) {
            const Calendar& local = *existingIt->second;
            merged.id = local.id;
            merged.syncToken = local.syncToken;
            merged.lastSyncedAt = local.lastSyncedAt;
            merged.createdAt = local.createdAt;
        }

        calendarRepo.upsert(merged);

        if (merged.id == 0) {
            const auto persisted = calendarRepo.getByProviderId(merged.accountId, merged.providerCalendarId);
            if (persisted.has_value()) {
                merged = *persisted;
            }
        }

        const std::vector<Event> changes = fetchRemoteChanges(merged, accessToken, calendarRepo);
        for (const auto& event : changes) {
            sync_internal::PersistRemoteEvent(eventRepo, event);
        }
    }

    for (const auto& local : localCalendars) {
        if (remoteIds.find(local.providerCalendarId) != remoteIds.end()) {
            continue;
        }

        calendarRepo.deleteById(local.id);
    }
}

void CalendarSyncService::syncPendingEvents(const std::string& accessToken, const Calendar& calendar) {
    const std::vector<Event> pendingEvents = eventRepo.getPendingRemoteEvents(calendar.id);

    for (const auto& pending : pendingEvents) {
        HttpRequest req;
        std::optional<json> payload;

        if (pending.syncStatus == PENDING_INSERT) {
            payload = exportEventPayload(pending);
            req = sync_internal::BuildAuthorizedRequest(buildEventsCollectionUrl(calendar), accessToken, POST, payload);
        }
        else if (pending.syncStatus == PENDING_UPDATE && !pending.providerEventId.empty()) {
            payload = exportEventPayload(pending);
            req = sync_internal::BuildAuthorizedRequest(
                buildEventItemUrl(calendar, pending.providerEventId), accessToken, PATCH, payload);
        }
        else if (pending.syncStatus == PENDING_DELETE) {
            if (pending.providerEventId.empty()) {
                eventRepo.deleteEvent(pending.id);
                continue;
            }
            req = sync_internal::BuildAuthorizedRequest(
                buildEventItemUrl(calendar, pending.providerEventId), accessToken, DELETE_);
        }
        else {
            continue;
        }

        const std::string response = PerformHttpRequest(req);

        if (pending.syncStatus == PENDING_DELETE) {
            if (response.empty()) {
                eventRepo.deleteEvent(pending.id);
            }
            continue;
        }

        const auto parsed = sync_internal::ParseResponseJson(response);
        if (!parsed.has_value()) {
            continue;
        }

        Event synced = parseRemoteEvent(*parsed);
        synced.id = pending.id;
        synced.calendarId = calendar.id;
        synced.createdAt = pending.createdAt == 0 ? std::time(nullptr) : pending.createdAt;
        synced.updatedAt = std::time(nullptr);
        synced.lastModified = synced.updatedAt;
        synced.deletedAt = 0;
        synced.syncStatus = SYNCED;

        if (synced.providerEventId.empty()) {
            synced.providerEventId = pending.providerEventId;
        }

        eventRepo.upsert(synced);
    }
}

void CalendarSyncService::fetchAndStoreRemoteEvents(const Calendar& calendar,
                                                    const std::string& accessToken,
                                                    RepositoryHolder& repository) {
    const std::vector<Event> changes = fetchRemoteChanges(calendar, accessToken, repository.calendarRepository);
    for (const auto& event : changes) {
        sync_internal::PersistRemoteEvent(repository.eventRepository, event);
    }
}
