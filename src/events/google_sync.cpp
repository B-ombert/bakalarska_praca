#include "google_sync.h"
#include <json.hpp>
#include "http.h"

void GoogleCalendarSync::syncCalendars(const std::string &accessToken) {
    HttpRequest request;
    request.url = "https://www.googleapis.com/calendar/v3/users/me/calendarList";
    request.headers = {
        "Authorization: Bearer " + accessToken,
        "Accept: application/json"
    };

    std::string response = PerformHttpRequest(request);

    auto json = nlohmann::json::parse(response);

    auto existing_calendars = calendars.getAll();

    for (const auto& item: json["items"]) {
        std::string providerId = item["id"];
        std::string name = item["summary"];

        auto existing = calendars.getByProviderId("GOOGLE", providerId);

        if (!existing) {
            Calendar cal;
            cal.provider = "GOOGLE";
            cal.providerCalendarId = providerId;
            cal.name = name;
            cal.createdAt = std::time(nullptr);

            calendars.insert(cal);
        }

        else {
            Calendar cal = *existing;
            cal.name = name;
            cal.updatedAt = std::time(nullptr);

            calendars.update(cal);
        }
    }
}
