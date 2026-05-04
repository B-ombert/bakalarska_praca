#pragma once

#include <string>

#include "utils/datetime_utils.h"
#include "utils/json.hpp"
#include "utils/types.h"

using json = nlohmann::json;

struct Event {
    long long id = -1;
    long long calendarId = 0;

    std::string providerEventId;
    std::string providerMasterId;

    long long instanceStart = 0;

    EventType type = EventType::SINGLE;

    std::string title;
    std::string description;
    std::string location;
    std::string timezone;

    long long startDateTime = 0;
    long long endDateTime = 0;
    bool allDay = false;

    std::string status = "confirmed";
    std::string recurrenceRule;

    long long deletedAt = 0;
    int syncStatus = PENDING_INSERT;

    long long lastModified = 0;
    long long createdAt = 0;
    long long updatedAt = 0;

    long long GetDisplayStartEpoch() const;
    long long GetDisplayEndEpoch() const;
    long long GetDisplayStartEpoch(const std::string& timezone) const;
    long long GetDisplayEndEpoch(const std::string& timezone) const;

    static Event fromIcal(const std::string& icalBody);
    static Event fromJson(Platform platform, const std::string& jsonBody);
    static Event ParseGoogleJsonEvent(const json& j);

    json ExportToJson(Platform platform) const;
    json exportToGoogleJson();
    json exportToOutlookJson();
    json ExportToIcal() const;
};
