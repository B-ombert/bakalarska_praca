#include "sync/outlook_calendar_sync_service.h"

#include <algorithm>
#include <ctime>

#include "calendar_sync_internal.h"

namespace {

std::string BuildOutlookEventsCollectionUrl(const Calendar& calendar) {
    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::string BuildOutlookEventsCollectionBatchUrl(const Calendar& calendar) {
    return "/me/calendars/" + sync_internal::EncodeSegment(calendar.providerCalendarId) + "/events";
}

std::string BuildOutlookEventItemBatchUrl(const Calendar& calendar, const std::string& providerEventId) {
    return BuildOutlookEventsCollectionBatchUrl(calendar) + "/" + sync_internal::EncodeSegment(providerEventId);
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

std::vector<EventBatchRequest> OutlookCalendarSyncService::buildEventBatchRequests(
    const std::string& accessToken,
    const Calendar& calendar,
    const std::vector<Event>& events) {
    std::vector<EventBatchRequest> batchRequests;
    constexpr size_t kMicrosoftBatchLimit = 20;

    for (size_t offset = 0; offset < events.size(); offset += kMicrosoftBatchLimit) {
        const size_t end = std::min(events.size(), offset + kMicrosoftBatchLimit);
        json requestItems = json::array();
        std::vector<long long> localEventIds;

        for (size_t index = offset; index < end; ++index) {
            const Event& event = events[index];
            if (event.syncStatus != PENDING_INSERT &&
                !(event.syncStatus == PENDING_UPDATE && !event.providerEventId.empty())) {
                continue;
            }
            localEventIds.push_back(event.id);

            json body = exportEventPayload(event);
            if (event.syncStatus == PENDING_INSERT) {
                body["transactionId"] = std::to_string(event.id);
            }

            requestItems.push_back({
                {"id", std::to_string(event.id)},
                {"method", event.syncStatus == PENDING_INSERT ? "POST" : "PATCH"},
                {"url", event.syncStatus == PENDING_INSERT
                    ? BuildOutlookEventsCollectionBatchUrl(calendar)
                    : BuildOutlookEventItemBatchUrl(calendar, event.providerEventId)},
                {"headers", {{"Content-Type", "application/json"}}},
                {"body", body}
            });
        }

        if (requestItems.empty()) {
            continue;
        }

        HttpRequest req;
        req.url = "https://graph.microsoft.com/v1.0/$batch";
        req.verb = POST;
        req.headers.push_back("Authorization: Bearer " + accessToken);
        req.headers.push_back("Content-Type: application/json");
        req.postData = json{{"requests", requestItems}}.dump();

        batchRequests.push_back(EventBatchRequest{std::move(req), std::move(localEventIds)});
    }

    return batchRequests;
}

std::vector<EventBatchUploadResult> OutlookCalendarSyncService::parseEventBatchResponse(
    const HttpResponse& response,
    const std::vector<long long>&) {
    std::vector<EventBatchUploadResult> results;
    if (response.statusCode < 200 || response.statusCode >= 300) {
        return results;
    }

    const auto payload = sync_internal::ParseResponseJson(response.body);
    if (!payload.has_value() || !payload->contains("responses") || !(*payload)["responses"].is_array()) {
        return results;
    }

    for (const auto& item : (*payload)["responses"]) {
        EventBatchUploadResult result;
        if (item.contains("id") && item["id"].is_string()) {
            result.localEventId = std::stoll(item["id"].get<std::string>());
        }
        result.httpStatus = item.value("status", 0);
        if (item.contains("body") && item["body"].is_object()) {
            result.responseBody = item["body"];
        }
        results.push_back(std::move(result));
    }

    return results;
}
