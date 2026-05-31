#include "sync/outlook_calendar_sync_service.h"

#include <algorithm>
#include <ctime>
#include <functional>
#include <iterator>
#include <optional>
#include <unordered_set>
#include <utility>

#include "calendar_sync_internal.h"
#include "repositories/calendar_sync_range_repository.h"
#include "utils/datetime_utils.h"
#include "utils/provider_utils.h"

namespace {

constexpr std::size_t kOutlookRangePersistChunkSize = 250;

struct OutlookBucket {
    long long startEpoch = 0;
    long long endEpoch = 0;
};

struct OutlookRemovedResolution {
    bool requestSucceeded = false;
    bool exists = false;
    Event event{};
};

HttpRequest BuildOutlookAuthorizedRequest(const std::string& url,
                                          const std::string& accessToken,
                                          const int verb = GET,
                                          const std::optional<json>& payload = std::nullopt) {
    HttpRequest request = sync_internal::BuildAuthorizedRequest(url, accessToken, verb, payload);
    request.headers.push_back("Prefer: IdType=\"ImmutableId\"");
    return request;
}

OutlookBucket BucketForEpoch(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    const int year = tm.tm_year + 1900;
    const int month = tm.tm_mon + 1;
    const int quarterStartMonth = ((month - 1) / 3) * 3 + 1;
    const long long start = MakeUtcEpoch(year, quarterStartMonth, 1);
    const int nextMonth = quarterStartMonth == 10 ? 1 : quarterStartMonth + 3;
    const int nextYear = quarterStartMonth == 10 ? year + 1 : year;
    return OutlookBucket{start, MakeUtcEpoch(nextYear, nextMonth, 1)};
}

OutlookBucket NextBucket(const OutlookBucket& bucket) {
    return BucketForEpoch(bucket.endEpoch);
}

OutlookBucket PreviousBucket(const OutlookBucket& bucket) {
    return BucketForEpoch(bucket.startEpoch - 1);
}

std::vector<OutlookBucket> BucketsForRange(const long long startEpoch, const long long endEpoch) {
    std::vector<OutlookBucket> buckets;
    if (startEpoch >= endEpoch) {
        return buckets;
    }

    OutlookBucket bucket = BucketForEpoch(startEpoch);
    while (bucket.startEpoch < endEpoch) {
        buckets.push_back(bucket);
        bucket = NextBucket(bucket);
    }
    return buckets;
}

std::string BuildOutlookEventsCollectionUrl(const Calendar& calendar) {
    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events";
}

std::string BuildOutlookEventsCollectionBatchUrl(const Calendar& calendar) {
    return "/me/calendars/" + sync_internal::EncodeSegment(calendar.providerCalendarId) + "/events";
}

std::string BuildOutlookEventItemBatchUrl(const Calendar& calendar, const std::string& providerEventId) {
    (void)calendar;
    return "/me/events/" + sync_internal::EncodeSegment(providerEventId);
}

std::string BuildOutlookEventItemUrl(const std::string& providerEventId) {
    return "https://graph.microsoft.com/v1.0/me/events/" + sync_internal::EncodeSegment(providerEventId);
}

std::string BuildOutlookInstancesUrl(const Calendar& calendar, const Event& master, const long long originalStart) {
    constexpr long long kInstanceLookupWindowSeconds = 86400;
    const long long windowStart = originalStart - kInstanceLookupWindowSeconds;
    const long long windowEnd = originalStart + kInstanceLookupWindowSeconds;
    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/events/" + sync_internal::EncodeSegment(master.providerEventId) +
           "/instances?startDateTime=" + AccessToken::UrlEncode(epochToIso(windowStart)) +
           "&endDateTime=" + AccessToken::UrlEncode(epochToIso(windowEnd));
}

std::optional<std::string> ResolveOutlookInstanceId(const Calendar& calendar,
                                                    const Event& master,
                                                    const long long originalStart,
                                                    const std::string& accessToken) {
    if (master.providerEventId.empty() || originalStart == 0) {
        return std::nullopt;
    }

    const auto payload = sync_internal::ParseResponseJson(
        PerformHttpRequest(BuildOutlookAuthorizedRequest(
            BuildOutlookInstancesUrl(calendar, master, originalStart),
            accessToken)));
    if (!payload.has_value() || !payload->contains("value") || !(*payload)["value"].is_array()) {
        return std::nullopt;
    }

    for (const auto& item : (*payload)["value"]) {
        Event candidate = Event::fromJson(MICROSOFT, item.dump());
        if (candidate.instanceStart == originalStart && !candidate.providerEventId.empty()) {
            return candidate.providerEventId;
        }
    }

    return std::nullopt;
}

OutlookRemovedResolution ResolveRemovedOutlookEvent(const Calendar& calendar,
                                                    const std::string& providerEventId,
                                                    const std::string& accessToken) {
    OutlookRemovedResolution result;
    if (providerEventId.empty()) {
        return result;
    }

    const HttpResponse response = PerformHttpRequestWithResponse(
        BuildOutlookAuthorizedRequest(BuildOutlookEventItemUrl(providerEventId), accessToken));
    if (response.statusCode == 404) {
        result.requestSucceeded = true;
        result.exists = false;
        return result;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        std::cerr << "Outlook removed-event verification failed with HTTP "
                  << response.statusCode << ": " << response.body << "\n";
        return result;
    }

    const auto payload = sync_internal::ParseResponseJson(response.body);
    if (!payload.has_value()) {
        return result;
    }

    result.requestSucceeded = true;
    result.exists = true;
    result.event = Event::fromJson(MICROSOFT, payload->dump());
    result.event.calendarId = calendar.id;
    result.event.deletedAt = result.event.status == "cancelled" ? std::time(nullptr) : 0;
    result.event.syncStatus = SYNCED;
    return result;
}

std::string BuildOutlookCalendarViewDeltaUrl(const Calendar& calendar,
                                             const long long startEpoch,
                                             const long long endEpoch) {
    return "https://graph.microsoft.com/v1.0/me/calendars/" +
           sync_internal::EncodeSegment(calendar.providerCalendarId) +
           "/calendarView/delta?startDateTime=" + AccessToken::UrlEncode(epochToIso(startEpoch)) +
           "&endDateTime=" + AccessToken::UrlEncode(epochToIso(endEpoch));
}

std::vector<Calendar> FetchOutlookCalendarsImpl(const std::string& accessToken) {
    const auto payload = sync_internal::ParseResponseJson(
        PerformHttpRequest(BuildOutlookAuthorizedRequest(
            "https://graph.microsoft.com/v1.0/me/calendars",
            accessToken
        ))
    );

    return payload.has_value() ? sync_internal::ParseCalendarCollection(*payload, true) : std::vector<Calendar>{};
}

bool FetchOutlookBucketDeltaImpl(const Calendar& calendar,
                                 const CalendarSyncRange& range,
                                 const AccessTokenProvider& accessTokenProvider,
                                 const std::function<void(Event)>& onEvent,
                                 std::string& fetchedDeltaLink,
                                 std::unordered_set<std::string>& seenProviderIds) {
    std::string url = range.syncToken.empty()
        ? BuildOutlookCalendarViewDeltaUrl(calendar, range.startEpoch, range.endEpoch)
        : range.syncToken;

    while (!url.empty()) {
        const std::string accessToken = accessTokenProvider ? accessTokenProvider() : "";
        if (accessToken.empty()) {
            return false;
        }

        const HttpResponse response = PerformHttpRequestWithResponse(
            BuildOutlookAuthorizedRequest(url, accessToken));
        if (response.statusCode == 403 || response.statusCode == 404) {
            // Some Outlook calendar list entries can be visible but not readable through calendarView/delta.
            // Do not mark the bucket as covered, otherwise a later permission/sync-token recovery would be skipped.
            return false;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            std::cerr << "Outlook calendarView delta failed with HTTP "
                      << response.statusCode << ": " << response.body << "\n";
            return false;
        }

        const auto payload = sync_internal::ParseResponseJson(response.body);
        if (!payload.has_value()) {
            return false;
        }

        if (payload->contains("value") && (*payload)["value"].is_array()) {
            for (const auto& item : (*payload)["value"]) {
                Event event;
                if (item.contains("@removed")) {
                    const std::string removedId = item.value("id", "");
                    const OutlookRemovedResolution resolution =
                        ResolveRemovedOutlookEvent(calendar, removedId, accessToken);
                    if (resolution.exists) {
                        event = resolution.event;
                    }
                    else if (resolution.requestSucceeded) {
                        event.providerEventId = removedId;
                        event.providerMasterId = item.value("seriesMasterId", "");
                        if (item.contains("originalStart") && item["originalStart"].is_string()) {
                            event.instanceStart = iso8601ToEpoch(item["originalStart"].get<std::string>());
                        }
                        event.type = EventType::SINGLE;
                        event.status = "cancelled";
                        event.deletedAt = std::time(nullptr);
                    }
                    else {
                        continue;
                    }
                }
                else {
                    event = Event::fromJson(MICROSOFT, item.dump());
                    event.deletedAt = event.status == "cancelled" ? std::time(nullptr) : 0;
                }
                event.calendarId = calendar.id;
                event.syncStatus = SYNCED;
                if (!event.providerEventId.empty()) {
                    seenProviderIds.insert(event.providerEventId);
                }
                if (!event.providerMasterId.empty()) {
                    seenProviderIds.insert(event.providerMasterId);
                }
                if (onEvent) {
                    onEvent(std::move(event));
                }
            }
        }

        if (payload->contains("@odata.deltaLink") && (*payload)["@odata.deltaLink"].is_string()) {
            fetchedDeltaLink = (*payload)["@odata.deltaLink"].get<std::string>();
        }

        url = payload->value("@odata.nextLink", "");
    }

    return !fetchedDeltaLink.empty();
}

void FlushOutlookRangeEventBuffer(EventRepository& eventRepo, std::vector<Event>& buffer) {
    if (buffer.empty()) {
        return;
    }

    eventRepo.runBulkSavepoint("outlook_range_event_chunk", [&]() {
        std::unordered_set<std::string> idsWithLiveSnapshot;
        for (const auto& event : buffer) {
            const bool remoteDelete =
                event.deletedAt != 0 ||
                event.type == EventType::CANCELLED_INSTANCE ||
                event.status == "cancelled";
            if (!remoteDelete && !event.providerEventId.empty()) {
                idsWithLiveSnapshot.insert(event.providerEventId);
            }
        }

        for (auto& event : buffer) {
            const bool remoteDelete =
                event.deletedAt != 0 ||
                event.type == EventType::CANCELLED_INSTANCE ||
                event.status == "cancelled";
            if (remoteDelete &&
                !event.providerEventId.empty() &&
                idsWithLiveSnapshot.count(event.providerEventId) > 0) {
                continue;
            }
            sync_internal::PersistRemoteEvent(eventRepo, std::move(event));
        }
    });
    buffer.clear();
}

void ReconcileOutlookBucketSnapshot(EventRepository& eventRepo,
                                    const Calendar& calendar,
                                    const CalendarSyncRange& range,
                                    const std::unordered_set<std::string>& seenProviderIds) {
    std::vector<Event> localEvents = eventRepo.getEventsInRange(calendar.id, range.startEpoch, range.endEpoch);
    auto recurringMasters = eventRepo.getRecurringMastersStartingBefore(calendar.id, range.endEpoch);
    localEvents.insert(
        localEvents.end(),
        std::make_move_iterator(recurringMasters.begin()),
        std::make_move_iterator(recurringMasters.end()));

    for (const auto& event : localEvents) {
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

bool FetchAndPersistOutlookBucket(CalendarSyncRangeRepository& rangeRepository,
                                  EventRepository& eventRepository,
                                  const Calendar& calendar,
                                  CalendarSyncRange range,
                                  const AccessTokenProvider& accessTokenProvider,
                                  const bool reconcileInitialSnapshot,
                                  const std::function<void(Event)>& onEvent = {}) {
    (void)reconcileInitialSnapshot;
    std::vector<Event> remoteEventBuffer;
    std::unordered_set<std::string> seenProviderIds;
    std::string deltaLink;

    const bool success = FetchOutlookBucketDeltaImpl(
        calendar,
        range,
        accessTokenProvider,
        [&](Event event) {
            if (onEvent) {
                onEvent(event);
            }
            remoteEventBuffer.push_back(std::move(event));
            if (remoteEventBuffer.size() >= kOutlookRangePersistChunkSize) {
                FlushOutlookRangeEventBuffer(eventRepository, remoteEventBuffer);
            }
        },
        deltaLink,
        seenProviderIds);
    FlushOutlookRangeEventBuffer(eventRepository, remoteEventBuffer);

    if (!success) {
        return false;
    }

    range.syncToken = deltaLink;
    range.syncedAt = static_cast<long long>(std::time(nullptr));
    range.lastViewedAt = range.syncedAt;
    rangeRepository.upsertRange(range);
    return true;
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

bool OutlookCalendarSyncService::deleteRemoteCalendar(Calendar& calendar,
                                                      const std::string& accessToken,
                                                      RepositoryHolder& repository) {
    if (calendar.providerCalendarId.empty()) {
        return repository.calendarRepository.deleteById(calendar.id);
    }

    const std::string url = "https://graph.microsoft.com/v1.0/me/calendars/" +
        sync_internal::EncodeSegment(calendar.providerCalendarId);
    const HttpResponse response = PerformHttpRequestWithResponse(
        BuildOutlookAuthorizedRequest(url, accessToken, DELETE_));
    if (response.statusCode == 404 || (response.statusCode >= 200 && response.statusCode < 300)) {
        return repository.calendarRepository.deleteById(calendar.id);
    }

    return false;
}

bool OutlookCalendarSyncService::fetchAndStoreRemoteEventsInRange(
    const Calendar& calendar,
    const long long startEpoch,
    const long long endEpoch,
    const AccessTokenProvider& accessTokenProvider,
    RepositoryHolder& repository) {
    CalendarSyncRangeRepository rangeRepository(repository.calendarRepository.database());
    bool allSucceeded = true;

    for (const auto& bucket : BucketsForRange(startEpoch, endEpoch)) {
        CalendarSyncRange range = rangeRepository
            .getExactRange(calendar.id, bucket.startEpoch, bucket.endEpoch)
            .value_or(CalendarSyncRange{
                0,
                calendar.id,
                bucket.startEpoch,
                bucket.endEpoch,
                0,
                static_cast<long long>(std::time(nullptr)),
                ""});
        range.lastViewedAt = static_cast<long long>(std::time(nullptr));

        const bool success = FetchAndPersistOutlookBucket(
            rangeRepository,
            repository.eventRepository,
            calendar,
            range,
            accessTokenProvider,
            true);
        allSucceeded = allSucceeded && success;
    }

    return allSucceeded;
}

std::string OutlookCalendarSyncService::providerName() const {
    return kProviderMicrosoft;
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
    (void)calendar;
    return BuildOutlookEventItemUrl(providerEventId);
}

std::vector<EventBatchRequest> OutlookCalendarSyncService::buildEventBatchRequests(
    const AccessTokenProvider& accessTokenProvider,
    const Calendar& calendar,
    const std::vector<Event>& events) {
    std::vector<EventBatchRequest> batchRequests;
    constexpr size_t kMicrosoftBatchLimit = 20;

    for (size_t offset = 0; offset < events.size(); offset += kMicrosoftBatchLimit) {
        const size_t end = std::min(events.size(), offset + kMicrosoftBatchLimit);
        json requestItems = json::array();
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
            std::string url;
            json body;
            if (isInsert) {
                method = "POST";
                url = BuildOutlookEventsCollectionBatchUrl(calendar);
                body = exportEventPayload(event);
                body["transactionId"] = std::to_string(event.id);
            }
            else if (isUpdate) {
                method = "PATCH";
                url = BuildOutlookEventItemBatchUrl(calendar, event.providerEventId);
                body = exportEventPayload(event);
            }
            else {
                method = "DELETE";
                url = BuildOutlookEventItemBatchUrl(calendar, event.providerEventId);
            }

            json requestItem = {
                {"id", std::to_string(event.id)},
                {"method", method},
                {"url", url}
            };
            json headers = {
                {"Prefer", "IdType=\"ImmutableId\""}
            };
            if (!isDelete) {
                headers["Content-Type"] = "application/json";
                requestItem["body"] = body;
            }
            requestItem["headers"] = headers;

            requestItems.push_back(std::move(requestItem));
            localEventIds.push_back(event.id);
        }

        if (requestItems.empty()) {
            continue;
        }

        HttpRequest req;
        req.url = "https://graph.microsoft.com/v1.0/$batch";
        req.verb = POST;
        req.headers.push_back("Authorization: Bearer " + accessToken);
        req.headers.push_back("Content-Type: application/json");
        req.headers.push_back("Prefer: IdType=\"ImmutableId\"");
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

void OutlookCalendarSyncService::fetchRemoteChangesStreaming(
    const Calendar& calendar,
    const AccessTokenProvider& accessTokenProvider,
    CalendarRepository& repository,
    const std::function<void(Event)>& onEvent) {
    (void)onEvent;
    CalendarSyncRangeRepository rangeRepository(repository.database());
    EventRepository eventRepository(repository.database());
    std::vector<CalendarSyncRange> ranges =
        rangeRepository.getMostRecentlyViewedRanges(calendar.id, 1);

    if (ranges.empty()) {
        const OutlookBucket currentBucket = BucketForEpoch(std::time(nullptr));
        ranges.push_back(CalendarSyncRange{
            0,
            calendar.id,
            currentBucket.startEpoch,
            currentBucket.endEpoch,
            0,
            static_cast<long long>(std::time(nullptr)),
            ""});
    }

    std::vector<CalendarSyncRange> syncRanges;
    const CalendarSyncRange current = ranges.front();
    syncRanges.push_back(current);

    const OutlookBucket currentBucket{current.startEpoch, current.endEpoch};
    const OutlookBucket previousBucket = PreviousBucket(currentBucket);
    const OutlookBucket nextBucket = NextBucket(currentBucket);

    if (auto previous = rangeRepository.getExactRange(calendar.id, previousBucket.startEpoch, previousBucket.endEpoch);
        previous.has_value()) {
        syncRanges.push_back(*previous);
    }
    if (auto next = rangeRepository.getExactRange(calendar.id, nextBucket.startEpoch, nextBucket.endEpoch);
        next.has_value()) {
        syncRanges.push_back(*next);
    }

    for (auto& range : syncRanges) {
        FetchAndPersistOutlookBucket(
            rangeRepository,
            eventRepository,
            calendar,
            range,
            accessTokenProvider,
            true,
            {});
    }
}

bool OutlookCalendarSyncService::fetchRemoteRangeStreaming(
    const Calendar& calendar,
    const long long startEpoch,
    const long long endEpoch,
    const AccessTokenProvider& accessTokenProvider,
    CalendarRepository& repository,
    const std::function<void(Event)>& onEvent) {
    CalendarSyncRangeRepository rangeRepository(repository.database());
    EventRepository eventRepository(repository.database());
    bool allSucceeded = true;

    for (const auto& bucket : BucketsForRange(startEpoch, endEpoch)) {
        CalendarSyncRange range = rangeRepository
            .getExactRange(calendar.id, bucket.startEpoch, bucket.endEpoch)
            .value_or(CalendarSyncRange{
                0,
                calendar.id,
                bucket.startEpoch,
                bucket.endEpoch,
                0,
                static_cast<long long>(std::time(nullptr)),
                ""});
        range.lastViewedAt = static_cast<long long>(std::time(nullptr));
        const bool success = FetchAndPersistOutlookBucket(
            rangeRepository,
            eventRepository,
            calendar,
            range,
            accessTokenProvider,
            true,
            onEvent);
        allSucceeded = allSucceeded && success;
    }

    return allSucceeded;
}

bool OutlookCalendarSyncService::uploadRecurrenceOverride(
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

    const auto instanceId = ResolveOutlookInstanceId(calendar, *master, overrideEntry.originalStart, accessToken);
    if (!instanceId.has_value()) {
        return false;
    }

    HttpRequest request;
    if (overrideEntry.type == RecurrenceOverrideType::CANCELLED) {
        request = BuildOutlookAuthorizedRequest(
            BuildOutlookEventItemUrl(*instanceId),
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
        request = BuildOutlookAuthorizedRequest(
            BuildOutlookEventItemUrl(*instanceId),
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
