#include "sync/outlook_calendar_sync_service.h"

#include <ctime>

#include "calendar_sync_internal.h"

namespace {

std::string BuildOutlookEventsCollectionUrl(const Calendar& calendar) {
    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::string BuildOutlookDeltaUrl(const Calendar& calendar) {
    if (!calendar.syncToken.empty()) {
        return calendar.syncToken;
    }

    const long long now = std::time(nullptr);
    const std::string start = AccessToken::UrlEncode(epochToIso(now - (365LL * 24 * 60 * 60)));
    const std::string end = AccessToken::UrlEncode(epochToIso(now + (730LL * 24 * 60 * 60)));

    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/calendarView/delta?startDateTime=" + start +
           "&endDateTime=" + end;
}

std::vector<Calendar> FetchOutlookCalendarsImpl(const std::string& accessToken) {
    const auto payload = sync_internal::ParseResponseJson(
        PerformHttpRequest(sync_internal::BuildAuthorizedRequest(
            "https://graph.microsoft.com/v1.0/me/calendars",
            accessToken
        ))
    );

    return payload.has_value() ? sync_internal::ParseCalendarCollection(*payload, true) : std::vector<Calendar>{};
}

std::vector<Event> FetchOutlookChangesImpl(const Calendar& calendar,
                                           const std::string& accessToken,
                                           CalendarRepository& repository) {
    std::vector<Event> changes;
    std::string url = BuildOutlookDeltaUrl(calendar);
    std::string nextLink;
    std::string deltaLink;

    do {
        const auto payload = sync_internal::ParseResponseJson(
            PerformHttpRequest(sync_internal::BuildAuthorizedRequest(url, accessToken))
        );
        if (!payload.has_value()) {
            break;
        }

        if (payload->contains("value") && (*payload)["value"].is_array()) {
            for (const auto& item : (*payload)["value"]) {
                Event event;

                if (item.contains("@removed")) {
                    event.providerEventId = item.value("id", "");
                    event.providerMasterId = item.value("seriesMasterId", "");
                    if (item.contains("originalStart") && item["originalStart"].is_string()) {
                        event.instanceStart = iso8601ToEpoch(item["originalStart"].get<std::string>());
                    }
                    event.status = "cancelled";
                    event.deletedAt = std::time(nullptr);
                }
                else {
                    event = Event::fromJson(MICROSOFT, item.dump());
                    event.deletedAt = event.status == "cancelled" ? std::time(nullptr) : 0;
                }

                event.calendarId = calendar.id;
                event.syncStatus = SYNCED;
                changes.push_back(std::move(event));
            }
        }

        nextLink = payload->value("@odata.nextLink", "");
        if (payload->contains("@odata.deltaLink") && (*payload)["@odata.deltaLink"].is_string()) {
            deltaLink = (*payload)["@odata.deltaLink"].get<std::string>();
        }

        url = nextLink;
    } while (!url.empty());

    if (!deltaLink.empty()) {
        Calendar updated = calendar;
        updated.syncToken = deltaLink;
        updated.lastSyncedAt = std::time(nullptr);
        repository.upsert(updated);
    }

    return changes;
}

} // namespace

OutlookCalendarSyncService::OutlookCalendarSyncService(CalendarRepository& calendarRepo, EventRepository& eventRepo)
    : CalendarSyncService(calendarRepo, eventRepo) {}

std::vector<Calendar> OutlookCalendarSyncService::fetchRemoteCalendars(const std::string& accessToken) {
    return FetchOutlookCalendarsImpl(accessToken);
}

bool OutlookCalendarSyncService::uploadCalendar(Calendar& calendar,
                                                const std::string& accessToken,
                                                RepositoryHolder& repository) {
    return sync_internal::UploadCalendarImpl(calendar, accessToken, repository, true);
}

std::vector<Event> OutlookCalendarSyncService::fetchRemoteChanges(const Calendar& calendar,
                                                                  const std::string& accessToken,
                                                                  CalendarRepository& repository) {
    return FetchOutlookChangesImpl(calendar, accessToken, repository);
}

std::string OutlookCalendarSyncService::providerName() const {
    return "MICROSOFT";
}

json OutlookCalendarSyncService::exportEventPayload(const Event& event) {
    Event copy = event;
    return copy.exportToOutlookJson();
}

Event OutlookCalendarSyncService::parseRemoteEvent(const json& payload) {
    return Event::fromJson(MICROSOFT, payload.dump());
}

std::string OutlookCalendarSyncService::buildEventsCollectionUrl(const Calendar& calendar) const {
    return BuildOutlookEventsCollectionUrl(calendar);
}

std::string OutlookCalendarSyncService::buildEventItemUrl(const Calendar& calendar,
                                                          const std::string& providerEventId) const {
    return BuildOutlookEventsCollectionUrl(calendar) + "/" + sync_internal::EncodeSegment(providerEventId);
}
