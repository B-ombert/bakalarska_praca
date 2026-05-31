#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "models/calendar.h"
#include "models/event.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "repositories/repository_holder.h"

struct EventBatchUploadResult {
    long long localEventId = 0;
    int httpStatus = 0;
    json responseBody;
};

struct EventBatchRequest {
    HttpRequest request;
    std::vector<long long> localEventIds;
};

struct SyncPendingEventsResult {
    int pendingEventCount = 0;
    int acceptedEventCount = 0;
};

using AccessTokenProvider = std::function<std::string()>;

class CalendarSyncService {
public:
    CalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo);
    virtual ~CalendarSyncService() = default;

    virtual std::optional<std::vector<Calendar>> fetchRemoteCalendars(const std::string& accessToken) = 0;
    virtual bool uploadCalendar(Calendar& calendar, const std::string& accessToken, RepositoryHolder& repository) = 0;
    virtual bool deleteRemoteCalendar(Calendar& calendar, const std::string& accessToken, RepositoryHolder& repository) = 0;

    void syncCalendarsIncremental(long long accountId,
                                  const std::vector<Calendar>& remoteCalendars,
                                  const std::string& accessToken,
                                  bool allowInitialEventBootstrap = false);
    void syncCalendarsIncremental(long long accountId,
                                  const std::vector<Calendar>& remoteCalendars,
                                  const AccessTokenProvider& accessTokenProvider,
                                  bool allowInitialEventBootstrap = false);
    SyncPendingEventsResult syncPendingCalendarsForAccount(const AccessTokenProvider& accessTokenProvider, long long accountId);
    SyncPendingEventsResult syncPendingEventsForAccount(const std::string& accessToken, long long accountId);
    SyncPendingEventsResult syncPendingEventsForAccount(const AccessTokenProvider& accessTokenProvider, long long accountId);
    SyncPendingEventsResult syncPendingEvents(const std::string& accessToken, const Calendar& calendar);
    SyncPendingEventsResult syncPendingEvents(const AccessTokenProvider& accessTokenProvider, const Calendar& calendar);
    virtual bool fetchAndStoreRemoteEventsInRange(const Calendar& calendar,
                                                  long long startEpoch,
                                                  long long endEpoch,
                                                  const AccessTokenProvider& accessTokenProvider,
                                                  RepositoryHolder& repository);

protected:
    virtual std::string providerName() const = 0;
    virtual json exportEventPayload(const Event& event) = 0;
    virtual Event parseRemoteEvent(const json& payload) = 0;
    virtual std::string buildEventsCollectionUrl(const Calendar& calendar) const = 0;
    virtual std::string buildEventItemUrl(const Calendar& calendar, const std::string& providerEventId) const = 0;
    virtual std::vector<EventBatchRequest> buildEventBatchRequests(
        const AccessTokenProvider& accessTokenProvider,
        const Calendar& calendar,
        const std::vector<Event>& events);
    virtual std::vector<EventBatchUploadResult> parseEventBatchResponse(
        const HttpResponse& response,
        const std::vector<long long>& localEventIds);
    virtual void fetchRemoteChangesStreaming(
        const Calendar& calendar,
        const AccessTokenProvider& accessTokenProvider,
        CalendarRepository& repository,
        const std::function<void(Event)>& onEvent) = 0;
    virtual bool fetchRemoteRangeStreaming(
        const Calendar& calendar,
        long long startEpoch,
        long long endEpoch,
        const AccessTokenProvider& accessTokenProvider,
        CalendarRepository& repository,
        const std::function<void(Event)>& onEvent);
    virtual bool uploadRecurrenceOverride(
        const AccessTokenProvider& accessTokenProvider,
        const Calendar& calendar,
        const RecurrenceOverride& overrideEntry);

    CalendarRepository& calendarRepo;
    EventRepository& eventRepo;
};
