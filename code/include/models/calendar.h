#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models/event.h"
#include "utils/json.hpp"
#include "utils/types.h"

using json = nlohmann::json;

struct Calendar {
    long long id = 0;
    long long accountId = 0;

    std::string providerCalendarId;

    std::string name;
    std::string description;
    std::string timezone;
    std::string colorHex;

    bool isPrimary = false;
    bool isReadOnly = false;
    bool isShared = false;
    bool syncEnabled = false;
    int syncStatus = SYNCED;

    long long deletedAt = 0;

    static Calendar parseGoogleCalendarJson(const json& cal);
    static Calendar parseOutlookCalendarJson(const json& cal);

    struct IcalImportResult {
        std::string name;
        std::string description;
        std::string timezone;
        std::vector<Event> events;
    };

    static std::optional<IcalImportResult> fromIcal(const std::string& icalBody, std::string* error = nullptr);

    json toGoogleJson();
    json toOutlookJson();
    std::string ExportToIcal(const std::vector<Event>& events) const;
};
