#pragma once

#include "sync/calendar_sync_service.h"

struct GoogleSyncMemoryRange {
    long long startEpoch = 0;
    long long endEpoch = 0;
};

class GoogleCalendarSyncService final : public CalendarSyncService {
public:
    GoogleCalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo);

    std::vector<Calendar> fetchRemoteCalendars(const std::string& accessToken) override;
    bool uploadCalendar(Calendar& calendar, const std::string& accessToken, RepositoryHolder& repository) override;
    bool deleteRemoteCalendar(Calendar& calendar, const std::string& accessToken, RepositoryHolder& repository) override;
    bool fetchAndStoreRemoteEventsInRange(
        const Calendar& calendar,
        long long startEpoch,
        long long endEpoch,
        const AccessTokenProvider& accessTokenProvider,
        RepositoryHolder& repository) override;
    static std::vector<GoogleSyncMemoryRange> DefaultSyncRanges();
    static std::vector<GoogleSyncMemoryRange> RememberedRangesForCalendar(long long calendarId);

protected:
    std::string providerName() const override;
    json exportEventPayload(const Event& event) override;
    Event parseRemoteEvent(const json& payload) override;
    std::string buildEventsCollectionUrl(const Calendar& calendar) const override;
    std::string buildEventItemUrl(const Calendar& calendar, const std::string& providerEventId) const override;
    std::vector<EventBatchRequest> buildEventBatchRequests(
        const AccessTokenProvider& accessTokenProvider,
        const Calendar& calendar,
        const std::vector<Event>& events) override;
    std::vector<EventBatchUploadResult> parseEventBatchResponse(
        const HttpResponse& response,
        const std::vector<long long>& localEventIds) override;
    void fetchRemoteChangesStreaming(
        const Calendar& calendar,
        const AccessTokenProvider& accessTokenProvider,
        CalendarRepository& repository,
        const std::function<void(Event)>& onEvent) override;
    bool fetchRemoteRangeStreaming(
        const Calendar& calendar,
        long long startEpoch,
        long long endEpoch,
        const AccessTokenProvider& accessTokenProvider,
        CalendarRepository& repository,
        const std::function<void(Event)>& onEvent) override;
    bool uploadRecurrenceOverride(
        const AccessTokenProvider& accessTokenProvider,
        const Calendar& calendar,
        const RecurrenceOverride& overrideEntry) override;
};
