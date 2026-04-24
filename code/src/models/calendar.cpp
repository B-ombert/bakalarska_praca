#include "models/calendar.h"

#include <ctime>

#include <utils/json.hpp>

json Calendar::toGoogleJson() {
    json j;

    j["summary"] = name;

    if (!timezone.empty()) {
        j["timeZone"] = timezone;
    }

    return j;
}

json Calendar::toOutlookJson() {
    json j;

    j["name"] = name;

    if (!timezone.empty()) {
        j["timeZone"] = timezone;
    }

    return j;
}

Calendar Calendar::parseGoogleCalendarJson(const json& cal) {
    Calendar c;
    c.provider = "GOOGLE";

    c.providerCalendarId = cal.value("id", "");
    c.name = cal.value("summary", "");
    c.timezone = cal.value("timeZone", "");
    c.isPrimary = cal.value("primary", false);
    c.isReadOnly = cal.value("accessRole", "") == "reader";
    c.syncToken = cal.value("nextSyncToken", "");
    c.syncEnabled = true;

    c.createdAt = std::time(nullptr);
    c.updatedAt = std::time(nullptr);

    return c;
}

Calendar Calendar::parseOutlookCalendarJson(const json& cal) {
    Calendar c;
    c.provider = "MICROSOFT";

    c.providerCalendarId = cal.value("id", "");
    c.name = cal.value("name", "");
    c.timezone = cal.value("timeZone", "");
    c.isPrimary = cal.value("isDefaultCalendar", false);
    c.isReadOnly = !cal.value("canEdit", true);
    c.syncEnabled = true;

    c.createdAt = std::time(nullptr);
    c.updatedAt = std::time(nullptr);

    return c;
}
