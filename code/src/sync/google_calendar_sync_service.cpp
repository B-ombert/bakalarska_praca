#include "sync/google_calendar_sync_service.h"

#include <algorithm>
#include <ctime>
#include <iterator>
#include <regex>
#include <sstream>

#include "calendar_sync_internal.h"

namespace {

std::string BuildGoogleEventsCollectionUrl(const Calendar& calendar) {
    return "https://www.googleapis.com/calendar/v3/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::string BuildGoogleEventsCollectionTarget(const Calendar& calendar) {
    return "/calendar/v3/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::string BuildGoogleEventItemTarget(const Calendar& calendar, const std::string& providerEventId) {
    return BuildGoogleEventsCollectionTarget(calendar) + "/" + sync_internal::EncodeSegment(providerEventId);
}

std::vector<EventBatchUploadResult> ParseGoogleBatchResponse(const std::string& responseBody) {
    std::vector<EventBatchUploadResult> results;
    static const std::regex contentIdPattern(R"(Content-ID:\s*<?(?:response-)?(\d+)>?)", std::regex::icase);
    static const std::regex statusPattern(R"(HTTP/\d(?:\.\d)?\s+(\d{3}))", std::regex::icase);

    std::sregex_iterator it(responseBody.begin(), responseBody.end(), contentIdPattern);
    const std::sregex_iterator end;
    for (; it != end; ++it) {
        const size_t partStart = static_cast<size_t>(it->position());
        const size_t partEnd = std::next(it) == end
            ? responseBody.size()
            : static_cast<size_t>(std::next(it)->position());
        const std::string part = responseBody.substr(partStart, partEnd - partStart);

        EventBatchUploadResult result;
        result.localEventId = std::stoll((*it)[1].str());

        std::smatch statusMatch;
        if (std::regex_search(part, statusMatch, statusPattern)) {
            result.httpStatus = std::stoi(statusMatch[1].str());
        }

        const size_t jsonStart = part.find('{');
        const size_t jsonEnd = part.rfind('}');
        if (jsonStart != std::string::npos && jsonEnd != std::string::npos && jsonEnd >= jsonStart) {
            const auto parsed = sync_internal::ParseResponseJson(part.substr(jsonStart, jsonEnd - jsonStart + 1));
            if (parsed.has_value()) {
                result.responseBody = *parsed;
            }
        }

        results.push_back(std::move(result));
    }

    if (!results.empty()) {
        return results;
    }

    std::sregex_iterator statusIt(responseBody.begin(), responseBody.end(), statusPattern);
    for (; statusIt != end; ++statusIt) {
        const size_t partStart = static_cast<size_t>(statusIt->position());
        const size_t partEnd = std::next(statusIt) == end
            ? responseBody.size()
            : static_cast<size_t>(std::next(statusIt)->position());
        const std::string part = responseBody.substr(partStart, partEnd - partStart);

        EventBatchUploadResult result;
        result.httpStatus = std::stoi((*statusIt)[1].str());

        const size_t jsonStart = part.find('{');
        const size_t jsonEnd = part.rfind('}');
        if (jsonStart != std::string::npos && jsonEnd != std::string::npos && jsonEnd >= jsonStart) {
            const auto parsed = sync_internal::ParseResponseJson(part.substr(jsonStart, jsonEnd - jsonStart + 1));
            if (parsed.has_value()) {
                result.responseBody = *parsed;
            }
        }

        results.push_back(std::move(result));
    }

    return results;
}

void FillMissingLocalIdsFromRequestOrder(std::vector<EventBatchUploadResult>& results,
                                         const std::vector<long long>& localEventIds) {
    if (localEventIds.empty()) {
        return;
    }

    for (size_t index = 0; index < results.size() && index < localEventIds.size(); ++index) {
        if (results[index].localEventId == 0) {
            results[index].localEventId = localEventIds[index];
        }
    }
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

std::vector<EventBatchRequest> GoogleCalendarSyncService::buildEventBatchRequests(
    const std::string& accessToken,
    const Calendar& calendar,
    const std::vector<Event>& events) {
    std::vector<EventBatchRequest> requests;
    constexpr size_t kGoogleBatchLimit = 100;

    for (size_t offset = 0; offset < events.size(); offset += kGoogleBatchLimit) {
        const size_t end = std::min(events.size(), offset + kGoogleBatchLimit);
        const std::string boundary = "calendar_batch_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(offset);
        std::ostringstream body;
        std::vector<long long> localEventIds;

        for (size_t index = offset; index < end; ++index) {
            const Event& event = events[index];
            if (event.syncStatus != PENDING_INSERT &&
                !(event.syncStatus == PENDING_UPDATE && !event.providerEventId.empty())) {
                continue;
            }
            localEventIds.push_back(event.id);

            json payload = exportEventPayload(event);
            if (event.syncStatus == PENDING_INSERT) {
                payload["extendedProperties"]["private"]["local_id"] = std::to_string(event.id);
            }

            const std::string method = event.syncStatus == PENDING_INSERT ? "POST" : "PATCH";
            const std::string target = event.syncStatus == PENDING_INSERT
                ? BuildGoogleEventsCollectionTarget(calendar)
                : BuildGoogleEventItemTarget(calendar, event.providerEventId);

            body << "--" << boundary << "\r\n"
                 << "Content-Type: application/http\r\n"
                 << "Content-ID: " << event.id << "\r\n\r\n"
                 << method << " " << target << " HTTP/1.1\r\n"
                 << "Content-Type: application/json; charset=UTF-8\r\n\r\n"
                 << payload.dump() << "\r\n";
        }

        body << "--" << boundary << "--\r\n";

        HttpRequest req;
        req.url = "https://www.googleapis.com/batch/calendar/v3";
        req.verb = POST;
        req.headers.push_back("Authorization: Bearer " + accessToken);
        req.headers.push_back("Content-Type: multipart/mixed; boundary=" + boundary);
        req.postData = body.str();

        requests.push_back(EventBatchRequest{std::move(req), std::move(localEventIds)});
    }

    return requests;
}

std::vector<EventBatchUploadResult> GoogleCalendarSyncService::parseEventBatchResponse(
    const HttpResponse& response,
    const std::vector<long long>& localEventIds) {
    if (response.statusCode < 200 || response.statusCode >= 300) {
        return {};
    }

    auto results = ParseGoogleBatchResponse(response.body);
    FillMissingLocalIdsFromRequestOrder(results, localEventIds);
    return results;
}
