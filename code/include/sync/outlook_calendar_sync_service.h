#pragma once

#include "sync/calendar_sync_service.h"

class OutlookCalendarSyncService final : public CalendarSyncService {
public:
    OutlookCalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo);

    std::vector<Calendar> fetchRemoteCalendars(const std::string& accessToken) override;
    bool uploadCalendar(Calendar& calendar, const std::string& accessToken, RepositoryHolder& repository) override;
    std::vector<Event> fetchRemoteChanges(
        const Calendar& calendar,
        const std::string& accessToken,
        CalendarRepository& repository) override;

protected:
    std::string providerName() const override;
    json exportEventPayload(const Event& event) override;
    Event parseRemoteEvent(const json& payload) override;
    std::string buildEventsCollectionUrl(const Calendar& calendar) const override;
    std::string buildEventItemUrl(const Calendar& calendar, const std::string& providerEventId) const override;
    std::vector<EventBatchRequest> buildEventBatchRequests(
        const std::string& accessToken,
        const Calendar& calendar,
        const std::vector<Event>& events) override;
    std::vector<EventBatchUploadResult> parseEventBatchResponse(
        const HttpResponse& response,
        const std::vector<long long>& localEventIds) override;
};
