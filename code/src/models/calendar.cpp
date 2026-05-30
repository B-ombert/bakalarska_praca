#include "models/calendar.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <utils/json.hpp>

#include "utils/timezone_utils.h"
#include "utils/calendar_colors.h"
#include "utils/provider_utils.h"

namespace {

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \r\n\t");
    return value.substr(first, last - first + 1);
}

std::string IcalPropertyName(const std::string& line) {
    const auto end = line.find_first_of(":;");
    return end == std::string::npos ? Trim(line) : Trim(line.substr(0, end));
}

std::string IcalPropertyValue(const std::string& line) {
    const auto separator = line.find(':');
    return separator == std::string::npos ? "" : line.substr(separator + 1);
}

std::vector<std::string> UnfoldIcalLines(const std::string& body) {
    std::vector<std::string> lines;
    std::stringstream input(body);
    std::string line;

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && (line.front() == ' ' || line.front() == '\t') && !lines.empty()) {
            lines.back() += line.substr(1);
            continue;
        }

        lines.push_back(std::move(line));
    }

    return lines;
}

void SetIcalError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

std::string EscapeIcalText(std::string value) {
    size_t pos = 0;
    while ((pos = value.find("\\", pos)) != std::string::npos) {
        value.replace(pos, 1, "\\\\");
        pos += 2;
    }
    pos = 0;
    while ((pos = value.find("\n", pos)) != std::string::npos) {
        value.replace(pos, 1, "\\n");
        pos += 2;
    }
    pos = 0;
    while ((pos = value.find(",", pos)) != std::string::npos) {
        value.replace(pos, 1, "\\,");
        pos += 2;
    }
    pos = 0;
    while ((pos = value.find(";", pos)) != std::string::npos) {
        value.replace(pos, 1, "\\;");
        pos += 2;
    }
    return value;
}

} // namespace

json Calendar::toGoogleJson() {
    json j;

    j["summary"] = name;

    if (!description.empty()) {
        j["description"] = description;
    }

    if (!timezone.empty()) {
        j["timeZone"] = timezone;
    }

    return j;
}

json Calendar::toOutlookJson() {
    json j;

    j["name"] = name;

    if (!description.empty()) {
        j["description"] = description;
    }

    if (!timezone.empty()) {
        j["timeZone"] = IanaTimeZoneToMicrosoft(timezone);
    }

    return j;
}

Calendar Calendar::parseGoogleCalendarJson(const json& cal) {
    Calendar c;

    if (cal.value("deleted", false) || cal.value("selected", true) == false) {
        c.deletedAt = std::time(nullptr);
        c.syncEnabled = false;
    }

    c.providerCalendarId = cal.value("id", "");
    c.name = cal.value("summary", "");
    c.description = cal.value("description", "");
    c.timezone = cal.value("timeZone", "");
    const std::string accessRole = cal.value("accessRole", "");
    c.isPrimary = cal.value("primary", false);
    c.isReadOnly = accessRole != "writer" && accessRole != "owner";
    c.isShared = !accessRole.empty() && accessRole != "owner";
    c.syncEnabled = true;

    return c;
}

std::optional<Calendar::IcalImportResult> Calendar::fromIcal(const std::string& icalBody, std::string* error) {
    const auto lines = UnfoldIcalLines(icalBody);
    if (lines.empty()) {
        SetIcalError(error, "The selected file is empty.");
        return std::nullopt;
    }

    bool inCalendar = false;
    bool inEvent = false;
    std::ostringstream eventBody;
    IcalImportResult result;
    result.name = "Imported calendar";
    result.description = "Imported from iCalendar file";
    result.timezone = GetCurrentLocalTimeZoneName();

    for (const auto& rawLine : lines) {
        const std::string line = Trim(rawLine);
        if (line.empty()) {
            continue;
        }

        const std::string property = IcalPropertyName(line);
        if (property == "BEGIN" && IcalPropertyValue(line) == "VCALENDAR") {
            inCalendar = true;
            continue;
        }
        if (property == "END" && IcalPropertyValue(line) == "VCALENDAR") {
            if (inEvent) {
                SetIcalError(error, "The iCalendar file ended while an event was still open.");
                return std::nullopt;
            }
            inCalendar = false;
            continue;
        }

        if (!inCalendar) {
            continue;
        }

        if (!inEvent && (property == "X-WR-CALNAME" || property == "NAME")) {
            const std::string name = Trim(IcalPropertyValue(line));
            if (!name.empty()) {
                result.name = name;
            }
            continue;
        }
        if (!inEvent && (property == "X-WR-CALDESC" || property == "DESCRIPTION")) {
            result.description = Trim(IcalPropertyValue(line));
            continue;
        }
        if (!inEvent && property == "X-WR-TIMEZONE") {
            const std::string timezone = NormalizeTimeZoneForProvider("", Trim(IcalPropertyValue(line)));
            if (!timezone.empty()) {
                result.timezone = timezone;
            }
            continue;
        }

        if (property == "BEGIN" && IcalPropertyValue(line) == "VEVENT") {
            if (inEvent) {
                SetIcalError(error, "Nested VEVENT blocks are not supported.");
                return std::nullopt;
            }
            inEvent = true;
            eventBody.str("");
            eventBody.clear();
            eventBody << line << "\r\n";
            continue;
        }

        if (inEvent) {
            eventBody << line << "\r\n";
            if (property == "END" && IcalPropertyValue(line) == "VEVENT") {
                inEvent = false;
                Event event = Event::fromIcal(eventBody.str());
                if (event.startDateTime == 0 || event.endDateTime == 0) {
                    SetIcalError(error, "One of the events has an invalid or missing start/end time.");
                    return std::nullopt;
                }
                if (event.title.empty()) {
                    event.title = "(Untitled)";
                }
                if (event.timezone.empty()) {
                    event.timezone = result.timezone;
                }
                event.status = event.status.empty() ? "confirmed" : event.status;
                event.type = event.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
                event.syncStatus = SYNCED;
                event.deletedAt = 0;
                event.createdAt = std::time(nullptr);
                event.updatedAt = event.createdAt;
                result.events.push_back(std::move(event));
            }
        }
    }

    if (inCalendar) {
        SetIcalError(error, "Missing END:VCALENDAR.");
        return std::nullopt;
    }
    if (result.events.empty()) {
        SetIcalError(error, "The iCalendar file does not contain any VEVENT entries.");
        return std::nullopt;
    }

    return result;
}

std::string Calendar::ExportToIcal(const std::vector<Event>& events) const {
    std::ostringstream ical;
    ical << "BEGIN:VCALENDAR\r\n";
    ical << "VERSION:2.0\r\n";
    ical << "PRODID:-//Local Calendar//CalendarApp//EN\r\n";
    ical << "CALSCALE:GREGORIAN\r\n";
    ical << "METHOD:PUBLISH\r\n";

    if (!name.empty()) {
        ical << "X-WR-CALNAME:" << EscapeIcalText(name) << "\r\n";
        ical << "NAME:" << EscapeIcalText(name) << "\r\n";
    }
    if (!description.empty()) {
        ical << "X-WR-CALDESC:" << EscapeIcalText(description) << "\r\n";
    }
    if (!timezone.empty()) {
        ical << "X-WR-TIMEZONE:" << timezone << "\r\n";
    }

    for (const auto& event : events) {
        if (event.deletedAt != 0) {
            continue;
        }
        ical << event.ExportToIcal();
    }

    ical << "END:VCALENDAR\r\n";
    return ical.str();
}

Calendar Calendar::parseOutlookCalendarJson(const json& cal) {
    Calendar c;

    c.providerCalendarId = cal.value("id", "");
    c.name = cal.value("name", "");
    c.description = cal.value("description", "");
    c.timezone = NormalizeTimeZoneForProvider(kProviderMicrosoft, cal.value("timeZone", ""));
    c.isPrimary = cal.value("isDefaultCalendar", false);
    c.isReadOnly = !cal.value("canEdit", true);
    c.isShared = cal.value("isShared", false) || cal.value("canShare", false);
    c.syncEnabled = true;

    return c;
}
