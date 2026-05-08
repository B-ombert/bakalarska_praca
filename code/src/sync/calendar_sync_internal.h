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

inline void PersistRemoteEvent(EventRepository& repository, Event event) {
    const auto existing = FindExistingEvent(repository, event);

    if (event.deletedAt != 0) {
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
    event.lastModified = now;
    event.syncStatus = SYNCED;

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
        repository.calendarRepository.upsert(calendar);
    }

    return true;
}

} // namespace sync_internal
