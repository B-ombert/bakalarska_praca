#include "sync/google_calendar_sync_service.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <set>
#include <unordered_set>
#include <utility>

#include "calendar_sync_internal.h"
#include "utils/datetime_utils.h"
#include "utils/provider_utils.h"

namespace {

constexpr std::size_t kRemotePersistChunkSize = 250;

std::mutex g_googleRangeMutex;
std::map<long long, std::set<std::pair<long long, long long>>> g_googleVisitedRanges;

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

std::string BuildGoogleInstancesUrl(const Calendar& calendar, const std::string& providerMasterId, const long long instanceStart) {
    constexpr long long kInstanceLookupWindowSeconds = 86400;
    const long long windowStart = instanceStart - kInstanceLookupWindowSeconds;
    const long long windowEnd = instanceStart + kInstanceLookupWindowSeconds;
    return BuildGoogleEventsCollectionUrl(calendar) + "/" + sync_internal::EncodeSegment(providerMasterId) +
           "/instances?showDeleted=true&timeMin=" + AccessToken::UrlEncode(epochToIso(windowStart)) +
           "&timeMax=" + AccessToken::UrlEncode(epochToIso(windowEnd));
}

std::optional<std::string> ResolveGoogleInstanceId(const Calendar& calendar,
                                                   const Event& master,
                                                   const long long originalStart,
                                                   const std::string& accessToken) {
    if (master.providerEventId.empty() || originalStart == 0) {
        return std::nullopt;
    }

    const auto payload = sync_internal::ParseResponseJson(
        PerformHttpRequest(sync_internal::BuildAuthorizedRequest(
            BuildGoogleInstancesUrl(calendar, master.providerEventId, originalStart),
            accessToken)));
    if (!payload.has_value() || !payload->contains("items") || !(*payload)["items"].is_array()) {
        return std::nullopt;
    }

    for (const auto& item : (*payload)["items"]) {
        Event candidate = Event::ParseGoogleJsonEvent(item);
        if (candidate.instanceStart == originalStart && !candidate.providerEventId.empty()) {
            return candidate.providerEventId;
        }
    }

    return std::nullopt;
}

GoogleSyncMemoryRange QuarterBucketForEpoch(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    const int year = tm.tm_year + 1900;
    const int quarterStartMonth = (tm.tm_mon / 3) * 3 + 1;
    const int nextQuarterMonth = quarterStartMonth + 3;
    const int endYear = nextQuarterMonth > 12 ? year + 1 : year;
    const int endMonth = nextQuarterMonth > 12 ? nextQuarterMonth - 12 : nextQuarterMonth;
    return GoogleSyncMemoryRange{
        MakeUtcEpoch(year, quarterStartMonth, 1),
        MakeUtcEpoch(endYear, endMonth, 1)
    };
}

GoogleSyncMemoryRange PreviousQuarterBucket(const GoogleSyncMemoryRange& bucket) {
    const std::tm tm = EpochToUtcTm(bucket.startEpoch);
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1 - 3;
    if (month < 1) {
        month += 12;
        --year;
    }
    const long long start = MakeUtcEpoch(year, month, 1);
    return GoogleSyncMemoryRange{start, bucket.startEpoch};
}

GoogleSyncMemoryRange NextQuarterBucket(const GoogleSyncMemoryRange& bucket) {
    const std::tm tm = EpochToUtcTm(bucket.endEpoch);
    int year = tm.tm_year + 1900;
    int month = tm.tm_mon + 1 + 3;
    if (month > 12) {
        month -= 12;
        ++year;
    }
    return GoogleSyncMemoryRange{bucket.endEpoch, MakeUtcEpoch(year, month, 1)};
}

std::vector<GoogleSyncMemoryRange> BucketsForRange(const long long startEpoch, const long long endEpoch) {
    std::vector<GoogleSyncMemoryRange> ranges;
    if (startEpoch >= endEpoch) {
        return ranges;
    }

    GoogleSyncMemoryRange bucket = QuarterBucketForEpoch(startEpoch);
    while (bucket.startEpoch < endEpoch) {
        ranges.push_back(bucket);
        bucket = NextQuarterBucket(bucket);
    }
    return ranges;
}

std::vector<GoogleSyncMemoryRange> CurrentQuarterWithNeighbours() {
    const GoogleSyncMemoryRange current = QuarterBucketForEpoch(std::time(nullptr));
    return {PreviousQuarterBucket(current), current, NextQuarterBucket(current)};
}

std::vector<GoogleSyncMemoryRange> RememberedOrDefaultRangesForCalendar(const long long calendarId) {
    std::lock_guard<std::mutex> lock(g_googleRangeMutex);
    const auto it = g_googleVisitedRanges.find(calendarId);
    if (it == g_googleVisitedRanges.end() || it->second.empty()) {
        return CurrentQuarterWithNeighbours();
    }

    std::vector<GoogleSyncMemoryRange> ranges;
    ranges.reserve(it->second.size());
    for (const auto& [start, end] : it->second) {
        ranges.push_back(GoogleSyncMemoryRange{start, end});
    }
    return ranges;
}

void RememberRangeForCalendar(const long long calendarId, const GoogleSyncMemoryRange& range) {
    std::lock_guard<std::mutex> lock(g_googleRangeMutex);
    g_googleVisitedRanges[calendarId].insert({range.startEpoch, range.endEpoch});
}

void FlushRemoteEventBuffer(EventRepository& eventRepo, std::vector<Event>& buffer) {
    if (buffer.empty()) {
        return;
    }

    eventRepo.runBulkSavepoint("google_remote_event_chunk", [&]() {
        for (auto& event : buffer) {
            sync_internal::PersistRemoteEvent(eventRepo, std::move(event));
        }
    });
    buffer.clear();
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

bool FetchGoogleRangeImpl(const Calendar& calendar,
                          const long long startEpoch,
                          const long long endEpoch,
                          const AccessTokenProvider& accessTokenProvider,
                          const std::function<void(Event)>& onEvent,
                          std::unordered_set<std::string>* seenProviderIds = nullptr) {
    std::string pageToken;

    do {
        std::string url = BuildGoogleEventsCollectionUrl(calendar) +
                          "?showDeleted=false&maxResults=250" +
                          "&timeMin=" + AccessToken::UrlEncode(epochToIso(startEpoch)) +
                          "&timeMax=" + AccessToken::UrlEncode(epochToIso(endEpoch));

        if (!pageToken.empty()) {
            url += "&pageToken=" + AccessToken::UrlEncode(pageToken);
        }

        const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
        if (accessToken.empty()) {
            return false;
        }

        const auto payload = sync_internal::ParseResponseJson(
            PerformHttpRequest(sync_internal::BuildAuthorizedRequest(url, accessToken)));
        if (!payload.has_value()) {
            return false;
        }

        if (payload->contains("items") && (*payload)["items"].is_array()) {
            for (const auto& item : (*payload)["items"]) {
                Event event = Event::ParseGoogleJsonEvent(item);
                event.calendarId = calendar.id;
                event.deletedAt = event.status == "cancelled" ? std::time(nullptr) : 0;
                event.syncStatus = SYNCED;
                if (seenProviderIds != nullptr && !event.providerEventId.empty()) {
                    seenProviderIds->insert(event.providerEventId);
                }
                if (seenProviderIds != nullptr && !event.providerMasterId.empty()) {
                    seenProviderIds->insert(event.providerMasterId);
                }
                if (onEvent) {
                    onEvent(std::move(event));
                }
            }
        }

        pageToken = payload->value("nextPageToken", "");
    } while (!pageToken.empty());

    return true;
}

void ReconcileGoogleRangeSnapshot(EventRepository& eventRepo,
                                  const Calendar& calendar,
                                  const GoogleSyncMemoryRange& range,
                                  const std::unordered_set<std::string>& seenProviderIds) {
    for (const auto& event : eventRepo.getEventsInRange(calendar.id, range.startEpoch, range.endEpoch)) {
        if (event.syncStatus != SYNCED || event.providerEventId.empty()) {
            continue;
        }

        const bool eventSeen = seenProviderIds.count(event.providerEventId) > 0;
        const bool masterSeen = !event.providerMasterId.empty() && seenProviderIds.count(event.providerMasterId) > 0;
        if (!eventSeen && !masterSeen) {
            eventRepo.deleteByProviderIdentity(calendar.id, event.providerEventId);
        }
    }
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

bool GoogleCalendarSyncService::deleteRemoteCalendar(Calendar& calendar,
                                                     const std::string& accessToken,
                                                     RepositoryHolder& repository) {
    if (calendar.providerCalendarId.empty()) {
        return repository.calendarRepository.deleteById(calendar.id);
    }

    const std::string url = calendar.isShared
        ? "https://www.googleapis.com/calendar/v3/users/me/calendarList/" +
              sync_internal::EncodeSegment(calendar.providerCalendarId)
        : "https://www.googleapis.com/calendar/v3/calendars/" +
              sync_internal::EncodeSegment(calendar.providerCalendarId);
    const HttpResponse response = PerformHttpRequestWithResponse(
        sync_internal::BuildAuthorizedRequest(url, accessToken, DELETE_));
    if (response.statusCode == 404 || (response.statusCode >= 200 && response.statusCode < 300)) {
        return repository.calendarRepository.deleteById(calendar.id);
    }

    return false;
}

bool GoogleCalendarSyncService::fetchAndStoreRemoteEventsInRange(
    const Calendar& calendar,
    const long long startEpoch,
    const long long endEpoch,
    const AccessTokenProvider& accessTokenProvider,
    RepositoryHolder& repository) {
    bool allSucceeded = true;
    for (const auto& range : BucketsForRange(startEpoch, endEpoch)) {
        std::vector<Event> remoteEventBuffer;
        std::unordered_set<std::string> seenProviderIds;
        const bool success = FetchGoogleRangeImpl(
            calendar,
            range.startEpoch,
            range.endEpoch,
            accessTokenProvider,
            [&](Event event) {
                remoteEventBuffer.push_back(std::move(event));
                if (remoteEventBuffer.size() >= kRemotePersistChunkSize) {
                    FlushRemoteEventBuffer(repository.eventRepository, remoteEventBuffer);
                }
            },
            &seenProviderIds);
        FlushRemoteEventBuffer(repository.eventRepository, remoteEventBuffer);
        if (success) {
            ReconcileGoogleRangeSnapshot(repository.eventRepository, calendar, range, seenProviderIds);
            RememberRangeForCalendar(calendar.id, range);
        }
        allSucceeded = allSucceeded && success;
    }

    return allSucceeded;
}

std::vector<GoogleSyncMemoryRange> GoogleCalendarSyncService::DefaultSyncRanges() {
    return CurrentQuarterWithNeighbours();
}

std::vector<GoogleSyncMemoryRange> GoogleCalendarSyncService::RememberedRangesForCalendar(const long long calendarId) {
    return RememberedOrDefaultRangesForCalendar(calendarId);
}

std::string GoogleCalendarSyncService::providerName() const {
    return kProviderGoogle;
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
    const AccessTokenProvider& accessTokenProvider,
    const Calendar& calendar,
    const std::vector<Event>& events) {
    std::vector<EventBatchRequest> requests;
    constexpr size_t kGoogleBatchLimit = 100;

    for (size_t offset = 0; offset < events.size(); offset += kGoogleBatchLimit) {
        const size_t end = std::min(events.size(), offset + kGoogleBatchLimit);
        const std::string boundary = "calendar_batch_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(offset);
        std::ostringstream body;
        std::vector<long long> localEventIds;
        const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
        if (accessToken.empty()) {
            continue;
        }

        for (size_t index = offset; index < end; ++index) {
            const Event& event = events[index];
            const bool isInsert = event.syncStatus == PENDING_INSERT;
            const bool isUpdate = event.syncStatus == PENDING_UPDATE && !event.providerEventId.empty();
            const bool isDelete = event.syncStatus == PENDING_DELETE && !event.providerEventId.empty();
            if (!isInsert && !isUpdate && !isDelete) {
                continue;
            }

            std::string method;
            std::string target;
            json payload;
            if (isInsert) {
                method = "POST";
                target = BuildGoogleEventsCollectionTarget(calendar);
                payload = exportEventPayload(event);
            }
            else if (isUpdate) {
                method = "PATCH";
                target = BuildGoogleEventItemTarget(calendar, event.providerEventId);
                payload = exportEventPayload(event);
            }
            else {
                method = "DELETE";
                target = BuildGoogleEventItemTarget(calendar, event.providerEventId);
            }

            localEventIds.push_back(event.id);
            if (isInsert) {
                payload["extendedProperties"]["private"]["local_id"] = std::to_string(event.id);
            }

            body << "--" << boundary << "\r\n"
                 << "Content-Type: application/http\r\n"
                 << "Content-ID: " << event.id << "\r\n\r\n"
                 << method << " " << target << " HTTP/1.1\r\n";
            if (isDelete) {
                body << "\r\n";
            }
            else {
                body << "Content-Type: application/json; charset=UTF-8\r\n\r\n"
                     << payload.dump() << "\r\n";
            }
        }

        body << "--" << boundary << "--\r\n";

        if (localEventIds.empty()) {
            continue;
        }

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

void GoogleCalendarSyncService::fetchRemoteChangesStreaming(
    const Calendar& calendar,
    const AccessTokenProvider& accessTokenProvider,
    CalendarRepository& repository,
    const std::function<void(Event)>& onEvent) {
    (void)repository;
    for (const auto& range : RememberedOrDefaultRangesForCalendar(calendar.id)) {
        std::unordered_set<std::string> seenProviderIds;
        const bool success = FetchGoogleRangeImpl(
            calendar,
            range.startEpoch,
            range.endEpoch,
            accessTokenProvider,
            onEvent,
            &seenProviderIds);
        if (success) {
            ReconcileGoogleRangeSnapshot(eventRepo, calendar, range, seenProviderIds);
            RememberRangeForCalendar(calendar.id, range);
        }
    }
}

bool GoogleCalendarSyncService::fetchRemoteRangeStreaming(
    const Calendar& calendar,
    const long long startEpoch,
    const long long endEpoch,
    const AccessTokenProvider& accessTokenProvider,
    CalendarRepository& repository,
    const std::function<void(Event)>& onEvent) {
    (void)repository;
    bool allSucceeded = true;
    for (const auto& range : BucketsForRange(startEpoch, endEpoch)) {
        std::unordered_set<std::string> seenProviderIds;
        const bool success = FetchGoogleRangeImpl(
            calendar,
            range.startEpoch,
            range.endEpoch,
            accessTokenProvider,
            onEvent,
            &seenProviderIds);
        if (success) {
            ReconcileGoogleRangeSnapshot(eventRepo, calendar, range, seenProviderIds);
            RememberRangeForCalendar(calendar.id, range);
        }
        allSucceeded = allSucceeded && success;
    }
    return allSucceeded;
}

bool GoogleCalendarSyncService::uploadRecurrenceOverride(
    const AccessTokenProvider& accessTokenProvider,
    const Calendar& calendar,
    const RecurrenceOverride& overrideEntry) {
    const auto master = eventRepo.getById(overrideEntry.masterEventId);
    if (!master.has_value() || master->providerEventId.empty() || overrideEntry.originalStart == 0) {
        return false;
    }

    const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
    if (accessToken.empty()) {
        return false;
    }

    const auto instanceId = ResolveGoogleInstanceId(calendar, *master, overrideEntry.originalStart, accessToken);
    if (!instanceId.has_value()) {
        return false;
    }

    HttpRequest request;
    if (overrideEntry.type == RecurrenceOverrideType::CANCELLED) {
        request = sync_internal::BuildAuthorizedRequest(
            buildEventItemUrl(calendar, *instanceId),
            accessToken,
            DELETE_);
    }
    else {
        if (overrideEntry.replacementEventId == 0) {
            return false;
        }
        auto replacement = eventRepo.getById(overrideEntry.replacementEventId);
        if (!replacement.has_value()) {
            return false;
        }

        replacement->providerEventId = *instanceId;
        replacement->providerMasterId = master->providerEventId;
        replacement->type = EventType::EXCEPTION;
        replacement->recurrenceRule.clear();
        request = sync_internal::BuildAuthorizedRequest(
            buildEventItemUrl(calendar, *instanceId),
            accessToken,
            PATCH,
            exportEventPayload(*replacement));
    }

    const HttpResponse response = PerformHttpRequestWithResponse(request);
    const bool accepted = response.statusCode == 404 || (response.statusCode >= 200 && response.statusCode < 300);
    if (accepted && overrideEntry.type == RecurrenceOverrideType::MODIFIED && overrideEntry.replacementEventId != 0) {
        auto replacement = eventRepo.getById(overrideEntry.replacementEventId);
        if (replacement.has_value()) {
            replacement->providerEventId = *instanceId;
            replacement->providerMasterId = master->providerEventId;
            replacement->type = EventType::EXCEPTION;
            replacement->syncStatus = SYNCED;
            eventRepo.updateById(*replacement);
        }
    }

    return accepted;
}
