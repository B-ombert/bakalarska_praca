#pragma once

#include <ctime>
#include <iostream>
#include <optional>

#include "repositories/repository_holder.h"
#include "utils/access_token.h"
#include "utils/http.h"
#include "utils/json.hpp"

namespace sync_internal {

inline HttpRequest BuildAuthorizedRequest(const std::string& url,
                                          const std::string& accessToken,
                                          const int verb = GET,
                                          const std::optional<json>& payload = std::nullopt) {
    HttpRequest req;
    req.url = url;
    req.verb = verb;
    req.headers.push_back("Authorization: Bearer " + accessToken);

    if (payload.has_value()) {
        req.headers.push_back("Content-Type: application/json");
        req.postData = payload->dump();
    }

    return req;
}

inline std::string EncodeSegment(const std::string& value) {
    return AccessToken::UrlEncode(value);
}

inline std::optional<json> ParseResponseJson(const std::string& response) {
    if (response.empty()) {
        return std::nullopt;
    }

    try {
        return json::parse(response);
    }
    catch (const std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return std::nullopt;
    }
}

inline std::optional<Event> FindExistingEvent(EventRepository& repository, const Event& event) {
    if (event.providerEventId.empty()) {
        return std::nullopt;
    }

    if (event.instanceStart != 0) {
        auto instance = repository.getByProviderInstance(event.providerEventId, event.instanceStart);
        if (instance.has_value()) {
            return instance;
        }
    }

    if (event.calendarId != 0) {
        return repository.getByProviderId(event.calendarId, event.providerEventId);
    }

    return repository.getByProviderId(event.providerEventId);
}

inline RecurrenceOverride MakeRemoteOverride(const Event& master,
                                             const Event& occurrence,
                                             const RecurrenceOverrideType type,
                                             const long long replacementEventId = 0) {
    const long long now = std::time(nullptr);
    RecurrenceOverride overrideEntry;
    overrideEntry.masterEventId = master.id;
    overrideEntry.originalStart = occurrence.instanceStart;
    overrideEntry.type = type;
    overrideEntry.replacementEventId = replacementEventId;
    overrideEntry.syncStatus = SYNCED;
    overrideEntry.deletedAt = 0;
    overrideEntry.createdAt = now;
    overrideEntry.updatedAt = now;
    return overrideEntry;
}

inline void PersistRemoteEvent(EventRepository& repository, Event event) {
    const auto existing = FindExistingEvent(repository, event);
    const bool remoteDelete =
        event.deletedAt != 0 ||
        event.type == EventType::CANCELLED_INSTANCE ||
        event.status == "cancelled";

    if (existing.has_value() && existing->syncStatus != SYNCED) {
        return;
    }

    if (!event.providerMasterId.empty() && event.instanceStart != 0) {
        const auto master = repository.getByProviderId(event.calendarId, event.providerMasterId);
        if (master.has_value()) {
            if (master->syncStatus != SYNCED) {
                return;
            }

            if (remoteDelete) {
                if (!event.providerEventId.empty()) {
                    repository.deleteByProviderIdentity(event.calendarId, event.providerEventId);
                }
                repository.upsertRecurrenceOverride(
                    MakeRemoteOverride(*master, event, RecurrenceOverrideType::CANCELLED));
                return;
            }

            if (event.type == EventType::EXCEPTION) {
                if (existing.has_value()) {
                    event.id = existing->id;
                    if (event.createdAt == 0) {
                        event.createdAt = existing->createdAt;
                    }
                }
                else if (event.instanceStart != 0) {
                    for (const auto& overrideEntry : repository.getRecurrenceOverridesForMaster(
                             master->id,
                             event.instanceStart,
                             event.instanceStart + 1)) {
                        if (overrideEntry.type != RecurrenceOverrideType::MODIFIED ||
                            overrideEntry.replacementEventId == 0) {
                            continue;
                        }

                        const auto replacement = repository.getById(overrideEntry.replacementEventId);
                        if (!replacement.has_value() || replacement->syncStatus != SYNCED) {
                            continue;
                        }

                        event.id = replacement->id;
                        if (event.createdAt == 0) {
                            event.createdAt = replacement->createdAt;
                        }
                        break;
                    }
                }

                const long long now = std::time(nullptr);
                if (event.createdAt == 0) {
                    event.createdAt = now;
                }
                event.updatedAt = now;
                event.syncStatus = SYNCED;
                event.deletedAt = 0;
                event.recurrenceRule.clear();

                const long long replacementId = repository.upsertRemoteSnapshot(event);
                repository.upsertRecurrenceOverride(
                    MakeRemoteOverride(*master, event, RecurrenceOverrideType::MODIFIED, replacementId));
                return;
            }
        }
    }

    if (remoteDelete) {
        repository.deleteByProviderIdentity(event.calendarId, event.providerEventId);
        return;
    }

    if (existing.has_value()) {
        event.id = existing->id;
        if (event.createdAt == 0) {
            event.createdAt = existing->createdAt;
        }
    }

    const long long now = std::time(nullptr);
    if (event.createdAt == 0) {
        event.createdAt = now;
    }
    event.updatedAt = now;
    event.syncStatus = SYNCED;
    if (!event.recurrenceRule.empty() && event.providerMasterId.empty() && event.recurrenceGroupId.empty()) {
        event.recurrenceGroupId = event.providerEventId;
    }

    repository.upsertRemoteSnapshot(event);
}

inline std::vector<Calendar> ParseCalendarCollection(const json& payload, const bool microsoft) {
    std::vector<Calendar> calendars;
    const char* key = microsoft ? "value" : "items";

    if (!payload.contains(key) || !payload[key].is_array()) {
        return calendars;
    }

    for (const auto& item : payload[key]) {
        calendars.push_back(
            microsoft ? Calendar::parseOutlookCalendarJson(item)
                      : Calendar::parseGoogleCalendarJson(item)
        );
    }

    return calendars;
}

inline bool UploadCalendarImpl(Calendar& calendar,
                               const std::string& accessToken,
                               RepositoryHolder& repository,
                               const bool microsoft) {
    std::string url = microsoft
        ? "https://graph.microsoft.com/v1.0/me/calendars"
        : "https://www.googleapis.com/calendar/v3/calendars";
    int verb = POST;

    if (!calendar.providerCalendarId.empty()) {
        verb = PATCH;
        url += "/" + EncodeSegment(calendar.providerCalendarId);
    }

    const json body = microsoft ? calendar.toOutlookJson() : calendar.toGoogleJson();
    const auto payload = ParseResponseJson(
        PerformHttpRequest(BuildAuthorizedRequest(url, accessToken, verb, body))
    );

    if (!payload.has_value()) {
        return false;
    }

    if (calendar.providerCalendarId.empty() && payload->contains("id") && (*payload)["id"].is_string()) {
        calendar.providerCalendarId = (*payload)["id"].get<std::string>();
    }

    calendar.syncStatus = SYNCED;
    calendar.deletedAt = 0;
    repository.calendarRepository.upsert(calendar);

    return true;
}

} // namespace sync_internal
