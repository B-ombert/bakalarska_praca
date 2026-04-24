#pragma once

#include <string>

#include "utils/json.hpp"

using json = nlohmann::json;

struct Calendar {
    long long id = 0;
    long long accountId = 0;

    std::string provider;
    std::string providerCalendarId;

    std::string name;
    std::string timezone;

    bool isPrimary = false;
    bool isReadOnly = false;
    bool syncEnabled = false;

    std::string syncToken;
    long long lastSyncedAt = 0;

    long long createdAt = 0;
    long long updatedAt = 0;

    static Calendar parseGoogleCalendarJson(const json& cal);
    static Calendar parseOutlookCalendarJson(const json& cal);

    json toGoogleJson();
    json toOutlookJson();
};
