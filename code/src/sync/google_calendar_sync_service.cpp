#include "sync/google_calendar_sync_service.h"

#include <ctime>

#include "calendar_sync_internal.h"

namespace {

std::string BuildGoogleEventsCollectionUrl(const Calendar& calendar) {
    return "https://www.googleapis.com/calendar/v3/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::vector<Calendar> FetchGoogleCalendarsImpl(const std::string& accessToken) {
    const auto payload = sync_internal::ParseResponseJson(
        PerformHttpRequest(sync_internal::BuildAuthorizedRequest(
            "https://www.googleapis.com/calendar/v3/users/me/calendarList",
            accessToken
        ))
    );

    return payload.has_value() ? sync_internal::ParseCalendarCollection(*payload, false) : std::vector<Calendar>{};
}

std::vector<Event> FetchGoogleChangesImpl(const Calendar& calendar,
                                          const std::string& accessToken,
                                          CalendarRepository& repository) {
    std::vector<Event> changes;
    std::string nextSyncToken;
    std::string pageToken;

    do {
        std::string url = BuildGoogleEventsCollectionUrl(calendar) +
                          "?showDeleted=true&maxResults=250";

        if (!calendar.syncToken.empty() && pageToken.empty()) {
            url += "&syncToken=" + AccessToken::UrlEncode(calendar.syncToken);
        }

        if (!pageToken.empty()) {
            url += "&pageToken=" + AccessToken::UrlEncode(pageToken);
        }

        const auto payload = sync_internal::ParseResponseJson(
            PerformHttpRequest(sync_internal::BuildAuthorizedRequest(url, accessToken))
        );
        if (!payload.has_value()) {
            break;
        }

        if (payload->contains("items") && (*payload)["items"].is_array()) {
            for (const auto& item : (*payload)["items"]) {
                Event event = Event::ParseGoogleJsonEvent(item);
                event.calendarId = calendar.id;
                event.deletedAt = event.status == "cancelled" ? std::time(nullptr) : 0;
                event.syncStatus = SYNCED;
                changes.push_back(std::move(event));
            }
        }

        pageToken = payload->value("nextPageToken", "");
        if (payload->contains("nextSyncToken") && (*payload)["nextSyncToken"].is_string()) {
            nextSyncToken = (*payload)["nextSyncToken"].get<std::string>();
        }
    } while (!pageToken.empty());

    if (!nextSyncToken.empty()) {
        Calendar updated = calendar;
        updated.syncToken = nextSyncToken;
        updated.lastSyncedAt = std::time(nullptr);
        repository.upsert(updated);
    }

    return changes;
}

} // namespace

GoogleCalendarSyncService::GoogleCalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo)
    : CalendarSyncService(calendarRepo, eventRepo) {}

std::vector<Calendar> GoogleCalendarSyncService::fetchRemoteCalendars(const std::string& accessToken) {
    return FetchGoogleCalendarsImpl(accessToken);
}

bool GoogleCalendarSyncService::uploadCalendar(Calendar& calendar,
                                               const std::string& accessToken,
                                               RepositoryHolder& repository) {
    return sync_internal::UploadCalendarImpl(calendar, accessToken, repository, false);
}

std::vector<Event> GoogleCalendarSyncService::fetchRemoteChanges(const Calendar& calendar,
                                                                 const std::string& accessToken,
                                                                 CalendarRepository& repository) {
    return FetchGoogleChangesImpl(calendar, accessToken, repository);
}

std::string GoogleCalendarSyncService::providerName() const {
    return "GOOGLE";
}

json GoogleCalendarSyncService::exportEventPayload(const Event& event) {
    Event copy = event;
    return copy.exportToGoogleJson();
}

Event GoogleCalendarSyncService::parseRemoteEvent(const json& payload) {
    return Event::ParseGoogleJsonEvent(payload);
}

std::string GoogleCalendarSyncService::buildEventsCollectionUrl(const Calendar& calendar) const {
    return BuildGoogleEventsCollectionUrl(calendar);
}

std::string GoogleCalendarSyncService::buildEventItemUrl(const Calendar& calendar,
                                                         const std::string& providerEventId) const {
    return BuildGoogleEventsCollectionUrl(calendar) + "/" + sync_internal::EncodeSegment(providerEventId);
}
