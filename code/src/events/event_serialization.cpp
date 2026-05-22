#include "events/rrule.h"
#include "models/event.h"
#include "utils/timezone_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

#include <utils/json.hpp>

namespace {

std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    if (first == std::string::npos) {
        return "";
    }

    const auto last = value.find_last_not_of(" \r\n\t");
    return value.substr(first, last - first + 1);
}

std::string getIcalValue(const std::string& line) {
    const auto separator = line.find(':');
    if (separator == std::string::npos) {
        return "";
    }

    return line.substr(separator + 1);
}

std::vector<std::string> unfoldIcalLines(const std::string& body) {
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

std::string getIcalParameter(const std::string& line, const std::string& name) {
    const auto colon = line.find(':');
    const std::string header = colon == std::string::npos ? line : line.substr(0, colon);
    const std::string needle = name + "=";
    const auto param = header.find(needle);
    if (param == std::string::npos) {
        return "";
    }

    const auto valueStart = param + needle.size();
    const auto valueEnd = header.find(';', valueStart);
    return header.substr(valueStart, valueEnd == std::string::npos ? std::string::npos : valueEnd - valueStart);
}

std::string unescapeIcalText(std::string value) {
    size_t pos = 0;
    while ((pos = value.find("\\n", pos)) != std::string::npos) {
        value.replace(pos, 2, "\n");
        pos += 1;
    }

    pos = 0;
    while ((pos = value.find("\\,", pos)) != std::string::npos) {
        value.replace(pos, 2, ",");
        pos += 1;
    }

    pos = 0;
    while ((pos = value.find("\\;", pos)) != std::string::npos) {
        value.replace(pos, 2, ";");
        pos += 1;
    }

    return value;
}

std::string escapeIcalText(std::string value) {
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

std::string dateToBasicIso(const std::string& iso) {
    std::string normalized;
    for (const char ch : iso) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            normalized.push_back(ch);
        }
    }

    if (normalized.size() >= 8) {
        return normalized.substr(0, 8);
    }

    return "";
}

std::string dateTimeToBasicIso(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y%m%dT%H%M%S");
    return output.str();
}

std::string microsoftPatternTypeToFreq(const std::string& patternType) {
    if (patternType == "daily") return "DAILY";
    if (patternType == "weekly") return "WEEKLY";
    if (patternType == "absoluteMonthly" || patternType == "relativeMonthly") return "MONTHLY";
    if (patternType == "absoluteYearly" || patternType == "relativeYearly") return "YEARLY";
    return "";
}

std::string microsoftDayToRRule(const std::string& day) {
    if (day == "monday") return "MO";
    if (day == "tuesday") return "TU";
    if (day == "wednesday") return "WE";
    if (day == "thursday") return "TH";
    if (day == "friday") return "FR";
    if (day == "saturday") return "SA";
    if (day == "sunday") return "SU";
    return "";
}

std::string rruleDayToMicrosoft(const int day) {
    switch (day) {
        case 1: return "monday";
        case 2: return "tuesday";
        case 3: return "wednesday";
        case 4: return "thursday";
        case 5: return "friday";
        case 6: return "saturday";
        case 7: return "sunday";
        default: return "";
    }
}

std::string weekStartToMicrosoft(const int day) {
    const std::string converted = rruleDayToMicrosoft(day);
    return converted.empty() ? "monday" : converted;
}

std::string FormatTimeZoneLocalIsoDateTime(const long long utcEpoch, const std::string& timezone) {
    const long long localEpoch = timezone.empty()
        ? utcEpoch
        : ConvertUtcEpochToTimeZoneDisplayEpoch(timezone, utcEpoch);
    const std::tm tm = EpochToUtcTm(localEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    return output.str();
}

std::string FormatTimeZoneOffsetSuffix(const std::string& timezone, const long long utcEpoch) {
    const auto offsetSeconds = GetUtcOffsetSeconds(timezone, utcEpoch);
    if (!offsetSeconds.has_value()) {
        return "Z";
    }

    const int totalSeconds = offsetSeconds.value();
    if (totalSeconds == 0) {
        return "Z";
    }

    const int absoluteSeconds = std::abs(totalSeconds);
    const int hours = absoluteSeconds / 3600;
    const int minutes = (absoluteSeconds % 3600) / 60;

    std::ostringstream output;
    output << (totalSeconds >= 0 ? '+' : '-')
           << std::setw(2) << std::setfill('0') << hours
           << ':'
           << std::setw(2) << std::setfill('0') << minutes;
    return output.str();
}

std::string FormatGoogleDateTime(const long long utcEpoch, const std::string& timezone) {
    return FormatTimeZoneLocalIsoDateTime(utcEpoch, timezone) + FormatTimeZoneOffsetSuffix(timezone, utcEpoch);
}

bool HasExplicitTimeZoneOffset(const std::string& value) {
    const size_t separator = value.find('T');
    if (separator == std::string::npos) {
        return false;
    }

    return value.find('Z', separator) != std::string::npos ||
           value.find('+', separator) != std::string::npos ||
           value.find('-', separator + 1) != std::string::npos;
}

long long ParseMicrosoftDateTime(const std::string& value, const bool allDay, const std::string& timezone) {
    if (allDay) {
        return iso8601ToEpochDate(value);
    }

    if (timezone.empty() || HasExplicitTimeZoneOffset(value)) {
        return iso8601ToEpoch(value);
    }

    const long long displayEpoch = iso8601ToEpoch(value);
    return ConvertTimeZoneDisplayEpochToUtcEpoch(timezone, displayEpoch).value_or(displayEpoch);
}

long long EventEpochInOwnTimeZone(const Event& event, const long long utcEpoch) {
    if (event.timezone.empty()) {
        return utcEpoch;
    }

    return ConvertUtcEpochToTimeZoneDisplayEpoch(event.timezone, utcEpoch);
}

json buildOutlookRecurrencePattern(const Event& event, const RRule& rule) {
    json pattern;
    pattern["interval"] = std::max(1, rule.interval);

    const std::tm startTm = EpochToUtcTm(EventEpochInOwnTimeZone(event, event.startDateTime));

    switch (rule.freq) {
        case Frequency::DAILY:
            pattern["type"] = "daily";
            break;

        case Frequency::WEEKLY: {
            pattern["type"] = "weekly";
            json days = json::array();
            if (!rule.byDay.empty()) {
                for (const int day : rule.byDay) {
                    const std::string converted = rruleDayToMicrosoft(day);
                    if (!converted.empty()) {
                        days.push_back(converted);
                    }
                }
            }
            if (days.empty()) {
                const int weekday = startTm.tm_wday == 0 ? 7 : startTm.tm_wday;
                days.push_back(rruleDayToMicrosoft(weekday));
            }
            pattern["daysOfWeek"] = days;
            pattern["firstDayOfWeek"] = weekStartToMicrosoft(rule.weekStart);
            break;
        }

        case Frequency::MONTHLY:
            pattern["type"] = "absoluteMonthly";
            pattern["dayOfMonth"] = !rule.byMonthDay.empty() ? rule.byMonthDay.front() : startTm.tm_mday;
            break;

        case Frequency::YEARLY:
            pattern["type"] = "absoluteYearly";
            pattern["dayOfMonth"] = !rule.byMonthDay.empty() ? rule.byMonthDay.front() : startTm.tm_mday;
            pattern["month"] = startTm.tm_mon + 1;
            break;

        case Frequency::UNKNOWN:
            return json();
    }

    return pattern;
}

json buildOutlookRecurrenceRange(const Event& event, const RRule& rule) {
    json range;
    range["startDate"] = epochToIsoDate(EventEpochInOwnTimeZone(event, event.startDateTime));

    if (rule.hasCount) {
        range["type"] = "numbered";
        range["numberOfOccurrences"] = rule.count;
    }
    else if (rule.hasUntil) {
        range["type"] = "endDate";
        range["endDate"] = epochToIsoDate(EventEpochInOwnTimeZone(event, rule.until));
    }
    else {
        range["type"] = "noEnd";
    }

    return range;
}

std::string microsoftRecurrenceToRRule(const json& recurrence) {
    if (!recurrence.is_object() ||
        !recurrence.contains("pattern") ||
        !recurrence["pattern"].is_object()) {
        return "";
    }

    const json& pattern = recurrence["pattern"];
    std::ostringstream rule;

    const std::string freq = microsoftPatternTypeToFreq(pattern.value("type", ""));
    if (freq.empty()) {
        return "";
    }

    rule << "RRULE:FREQ=" << freq;

    const int interval = pattern.value("interval", 1);
    if (interval > 1) {
        rule << ";INTERVAL=" << interval;
    }

    if (pattern.contains("daysOfWeek") && pattern["daysOfWeek"].is_array() && !pattern["daysOfWeek"].empty()) {
        bool first = true;
        rule << ";BYDAY=";
        for (const auto& day : pattern["daysOfWeek"]) {
            if (!day.is_string()) {
                continue;
            }

            const std::string code = microsoftDayToRRule(day.get<std::string>());
            if (code.empty()) {
                continue;
            }

            if (!first) {
                rule << ",";
            }
            rule << code;
            first = false;
        }
    }

    if (pattern.contains("dayOfMonth") && !pattern["dayOfMonth"].is_null()) {
        rule << ";BYMONTHDAY=" << pattern["dayOfMonth"].get<int>();
    }

    if (recurrence.contains("range") && recurrence["range"].is_object()) {
        const json& range = recurrence["range"];
        const std::string rangeType = range.value("type", "");

        if (rangeType == "numbered" && range.contains("numberOfOccurrences")) {
            rule << ";COUNT=" << range["numberOfOccurrences"].get<int>();
        }
        else if ((rangeType == "endDate" || rangeType == "numbered") && range.contains("endDate")) {
            const std::string until = dateToBasicIso(range["endDate"].get<std::string>());
            if (!until.empty()) {
                rule << ";UNTIL=" << until << "T235959Z";
            }
        }
    }

    return rule.str();
}

Event parseMicrosoftJsonEvent(const json& j) {
    Event e;

    if (j.contains("id") && j["id"].is_string()) {
        e.providerEventId = j["id"].get<std::string>();
    }

    if (j.contains("subject") && j["subject"].is_string()) {
        e.title = j["subject"].get<std::string>();
    }

    if (j.contains("body") && j["body"].is_object() &&
        j["body"].contains("content") && j["body"]["content"].is_string()) {
        e.description = j["body"]["content"].get<std::string>();
    }

    if (j.contains("location") && j["location"].is_object() &&
        j["location"].contains("displayName") && j["location"]["displayName"].is_string()) {
        e.location = j["location"]["displayName"].get<std::string>();
    }

    if (j.contains("isAllDay") && j["isAllDay"].is_boolean()) {
        e.allDay = j["isAllDay"].get<bool>();
    }

    if (j.contains("start") && j["start"].is_object() &&
        j["start"].contains("timeZone") && j["start"]["timeZone"].is_string()) {
        e.timezone = MicrosoftTimeZoneToIana(j["start"]["timeZone"].get<std::string>());
    }
    if (e.timezone.empty() &&
        j.contains("end") && j["end"].is_object() &&
        j["end"].contains("timeZone") && j["end"]["timeZone"].is_string()) {
        e.timezone = MicrosoftTimeZoneToIana(j["end"]["timeZone"].get<std::string>());
    }

    if (j.contains("start") && j["start"].is_object() &&
        j["start"].contains("dateTime") && j["start"]["dateTime"].is_string()) {
        const std::string start = j["start"]["dateTime"].get<std::string>();
        e.startDateTime = ParseMicrosoftDateTime(start, e.allDay, e.timezone);
    }

    if (j.contains("end") && j["end"].is_object() &&
        j["end"].contains("dateTime") && j["end"]["dateTime"].is_string()) {
        const std::string end = j["end"]["dateTime"].get<std::string>();
        e.endDateTime = ParseMicrosoftDateTime(end, e.allDay, e.timezone);
    }

    e.status = "confirmed";
    if (j.contains("isCancelled") && j["isCancelled"].is_boolean() && j["isCancelled"].get<bool>()) {
        e.status = "cancelled";
    }

    if (j.contains("recurrence") && j["recurrence"].is_object()) {
        e.recurrenceRule = microsoftRecurrenceToRRule(j["recurrence"]);
        if (!e.recurrenceRule.empty()) {
            e.type = EventType::MASTER;
        }
    }

    if (j.contains("seriesMasterId") && j["seriesMasterId"].is_string()) {
        e.providerMasterId = j["seriesMasterId"].get<std::string>();
    }

    if (j.contains("originalStart") && j["originalStart"].is_string()) {
        const std::string originalStart = j["originalStart"].get<std::string>();
        e.instanceStart = ParseMicrosoftDateTime(originalStart, e.allDay, e.timezone);
    }

    if (j.contains("type") && j["type"].is_string()) {
        const std::string eventType = j["type"].get<std::string>();

        if (eventType == "seriesMaster") {
            e.type = EventType::MASTER;
        }
        else if (eventType == "occurrence") {
            e.type = EventType::OCCURRENCE;
        }
        else if (eventType == "exception") {
            e.type = (e.status == "cancelled") ? EventType::CANCELLED_INSTANCE : EventType::EXCEPTION;
        }
        else if (eventType == "singleInstance") {
            e.type = EventType::SINGLE;
        }
    }

    return e;
}

bool parseBasicDateTimeParts(const std::string& value,
                             int& year,
                             int& month,
                             int& day,
                             int& hour,
                             int& minute,
                             int& second,
                             bool& utc) {
    std::string digits;
    utc = !value.empty() && value.back() == 'Z';
    for (const char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            digits.push_back(ch);
        }
    }

    if (digits.size() < 8) {
        return false;
    }

    year = std::atoi(digits.substr(0, 4).c_str());
    month = std::atoi(digits.substr(4, 2).c_str());
    day = std::atoi(digits.substr(6, 2).c_str());
    hour = digits.size() >= 10 ? std::atoi(digits.substr(8, 2).c_str()) : 0;
    minute = digits.size() >= 12 ? std::atoi(digits.substr(10, 2).c_str()) : 0;
    second = digits.size() >= 14 ? std::atoi(digits.substr(12, 2).c_str()) : 0;
    return true;
}

long long parseIcalDateTime(const std::string& value, const bool allDay, const std::string& timezone) {
    if (allDay) {
        return iso8601ToEpochDate(value);
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool utc = false;
    if (!parseBasicDateTimeParts(value, year, month, day, hour, minute, second, utc)) {
        return 0;
    }

    const long long displayOrUtcEpoch = MakeUtcEpoch(year, month, day, hour, minute, second);
    if (utc || timezone.empty()) {
        return displayOrUtcEpoch;
    }

    return ConvertTimeZoneDisplayEpochToUtcEpoch(timezone, displayOrUtcEpoch).value_or(displayOrUtcEpoch);
}

} // namespace

Event Event::fromIcal(const std::string& icalBody) {
    Event event;

    for (auto line : unfoldIcalLines(icalBody)) {
        line = trim(line);

        if (line.rfind("SUMMARY", 0) == 0) {
            event.title = unescapeIcalText(getIcalValue(line));
        }
        else if (line.rfind("DESCRIPTION", 0) == 0) {
            event.description = unescapeIcalText(getIcalValue(line));
        }
        else if (line.rfind("LOCATION", 0) == 0) {
            event.location = unescapeIcalText(getIcalValue(line));
        }
        else if (line.rfind("STATUS", 0) == 0) {
            event.status = trim(getIcalValue(line));
        }
        else if (line.rfind("UID", 0) == 0) {
            event.providerEventId = trim(getIcalValue(line));
        }
        else if (line.rfind("RRULE", 0) == 0) {
            event.recurrenceRule = trim(line);
            event.type = EventType::MASTER;
        }
        else if (line.rfind("DTSTART;VALUE=DATE:", 0) == 0) {
            event.allDay = true;
            event.startDateTime = parseIcalDateTime(getIcalValue(line), true, "");
        }
        else if (line.rfind("DTEND;VALUE=DATE:", 0) == 0) {
            event.allDay = true;
            event.endDateTime = parseIcalDateTime(getIcalValue(line), true, "");
        }
        else if (line.rfind("DTSTART", 0) == 0) {
            event.timezone = getIcalParameter(line, "TZID");
            event.startDateTime = parseIcalDateTime(getIcalValue(line), false, event.timezone);
        }
        else if (line.rfind("DTEND", 0) == 0) {
            const std::string timezone = getIcalParameter(line, "TZID");
            if (event.timezone.empty()) {
                event.timezone = timezone;
            }
            event.endDateTime = parseIcalDateTime(getIcalValue(line), false, timezone.empty() ? event.timezone : timezone);
        }
    }

    if (event.endDateTime == 0 && event.startDateTime != 0) {
        event.endDateTime = event.allDay ? event.startDateTime + 24LL * 60 * 60 : event.startDateTime;
    }
    if (event.recurrenceRule.empty()) {
        event.instanceStart = event.startDateTime;
    }

    return event;
}

long long Event::GetDisplayStartEpoch() const {
    return GetDisplayStartEpoch(GetCurrentLocalTimeZoneName());
}

long long Event::GetDisplayEndEpoch() const {
    return GetDisplayEndEpoch(GetCurrentLocalTimeZoneName());
}

long long Event::GetDisplayStartEpoch(const std::string& displayTimezone) const {
    if (allDay) {
        return startDateTime;
    }

    return ConvertUtcEpochToTimeZoneDisplayEpoch(displayTimezone, startDateTime);
}

long long Event::GetDisplayEndEpoch(const std::string& displayTimezone) const {
    if (allDay) {
        return endDateTime;
    }

    return ConvertUtcEpochToTimeZoneDisplayEpoch(displayTimezone, endDateTime);
}

Event Event::fromJson(const Platform platform, const std::string& jsonBody) {
    const json parsed = json::parse(jsonBody);

    switch (platform) {
        case GOOGLE:
            return ParseGoogleJsonEvent(parsed);
        case MICROSOFT:
            return parseMicrosoftJsonEvent(parsed);
    }

    return {};
}

json Event::ExportToJson(const Platform platform) const {
    Event copy = *this;

    switch (platform) {
        case GOOGLE:
            return copy.exportToGoogleJson();
        case MICROSOFT:
            return copy.exportToOutlookJson();
    }

    return json::object();
}

std::string Event::ExportToIcal() const {
    std::ostringstream ical;

    ical << "BEGIN:VEVENT\r\n";

    if (!title.empty()) {
        ical << "SUMMARY:" << escapeIcalText(title) << "\r\n";
    }

    if (!providerEventId.empty()) {
        ical << "UID:" << escapeIcalText(providerEventId) << "\r\n";
    }

    if (!description.empty()) {
        ical << "DESCRIPTION:" << escapeIcalText(description) << "\r\n";
    }

    if (!location.empty()) {
        ical << "LOCATION:" << escapeIcalText(location) << "\r\n";
    }

    if (allDay) {
        ical << "DTSTART;VALUE=DATE:" << dateToBasicIso(epochToIsoDate(startDateTime)) << "\r\n";
        ical << "DTEND;VALUE=DATE:" << dateToBasicIso(epochToIsoDate(endDateTime)) << "\r\n";
    }
    else if (!timezone.empty()) {
        const long long displayStart = ConvertUtcEpochToTimeZoneDisplayEpoch(timezone, startDateTime);
        const long long displayEnd = ConvertUtcEpochToTimeZoneDisplayEpoch(timezone, endDateTime);
        ical << "DTSTART;TZID=" << timezone << ":" << dateTimeToBasicIso(displayStart) << "\r\n";
        ical << "DTEND;TZID=" << timezone << ":" << dateTimeToBasicIso(displayEnd) << "\r\n";
    }
    else {
        ical << "DTSTART:" << dateTimeToBasicIso(startDateTime) << "Z\r\n";
        ical << "DTEND:" << dateTimeToBasicIso(endDateTime) << "Z\r\n";
    }

    if (!status.empty()) {
        ical << "STATUS:" << status << "\r\n";
    }

    if (!recurrenceRule.empty()) {
        ical << recurrenceRule << "\r\n";
    }

    ical << "END:VEVENT\r\n";

    return ical.str();
}

json Event::exportToGoogleJson() {
    json j;

    j["summary"] = title;

    if (!description.empty()) j["description"] = description;

    if (!location.empty()) j["location"] = location;

    if (allDay) {
        j["start"]["date"] = epochToIsoDate(startDateTime);
        j["end"]["date"] = epochToIsoDate(endDateTime);
        if (!timezone.empty()) {
            j["start"]["timeZone"] = timezone;
            j["end"]["timeZone"] = timezone;
        }
    }
    else {
        j["start"]["dateTime"] = FormatGoogleDateTime(startDateTime, timezone);
        j["end"]["dateTime"] = FormatGoogleDateTime(endDateTime, timezone);
        if (!timezone.empty()) {
            j["start"]["timeZone"] = timezone;
            j["end"]["timeZone"] = timezone;
        }
    }

    if (!status.empty()) j["status"] = status;

    if (!recurrenceRule.empty() && type == EventType::MASTER)
        j["recurrence"] = json::array( {recurrenceRule} );

    if (providerEventId.empty()) {
        j["extendedProperties"]["private"]["local_id"] = std::to_string(id);
    }

    return j;
}

json Event::exportToOutlookJson() {
    json j;

    j["subject"] = title;

    if (!description.empty()) {
        j["body"] = {
            {"contentType", "text"},
            {"content", description}
        };
    }

    if (!location.empty()) {
        j["location"] = {
            {"displayName", location}
        };
    }

    if (allDay) {
        j["isAllDay"] = true;

        j["start"] = {
            {"dateTime", epochToIsoDate(startDateTime)},
            {"timeZone", IanaTimeZoneToMicrosoft(timezone.empty() ? "Etc/UTC" : timezone)}
        };
        j["end"] = {
            {"dateTime", epochToIsoDate(endDateTime)},
            {"timeZone", IanaTimeZoneToMicrosoft(timezone.empty() ? "Etc/UTC" : timezone)}
        };
    }
    else {
        j["isAllDay"] = false;

        j["start"] = {
            {"dateTime", FormatTimeZoneLocalIsoDateTime(startDateTime, timezone)},
            {"timeZone", IanaTimeZoneToMicrosoft(timezone.empty() ? "Etc/UTC" : timezone)}
        };

        j["end"] = {
            {"dateTime", FormatTimeZoneLocalIsoDateTime(endDateTime, timezone)},
            {"timeZone", IanaTimeZoneToMicrosoft(timezone.empty() ? "Etc/UTC" : timezone)}
        };
    }

    if (status == "confirmed") j["showAs"] = "busy";
    else if (status == "cancelled") j["showAs"] = "free";

    if (!recurrenceRule.empty() && type == EventType::MASTER) {
        const RRule rule = RRule().parseRRule(recurrenceRule);
        const json pattern = buildOutlookRecurrencePattern(*this, rule);
        if (!pattern.is_null() && !pattern.empty()) {
            j["recurrence"] = {
                {"pattern", pattern},
                {"range", buildOutlookRecurrenceRange(*this, rule)}
            };
        }
    }

    return j;
}

Event Event::ParseGoogleJsonEvent(const json& j) {
    Event e;

    if (j.contains("id") && !j["id"].is_null()) {
        e.providerEventId = j["id"].get<std::string>();
    }

    if (j.contains("summary") && !j["summary"].is_null()) {
        e.title = j["summary"].get<std::string>();
    }

    if (j.contains("description") && !j["description"].is_null()) {
        e.description = j["description"].get<std::string>();
    }

    if (j.contains("location") && !j["location"].is_null()) {
        e.location = j["location"].get<std::string>();
    }

    if (j.contains("start")) {
        e.allDay = j["start"].contains("date");
        if (j["start"].contains("timeZone") && !j["start"]["timeZone"].is_null()) {
            e.timezone = j["start"]["timeZone"].get<std::string>();
        }

        if (e.allDay) {
            e.startDateTime = iso8601ToEpochDate(j["start"]["date"].get<std::string>());
        } else if (j["start"].contains("dateTime")) {
            e.startDateTime = iso8601ToEpoch(j["start"]["dateTime"].get<std::string>());
        }
    }

    if (j.contains("end")) {
        if (e.timezone.empty() && j["end"].contains("timeZone") && !j["end"]["timeZone"].is_null()) {
            e.timezone = j["end"]["timeZone"].get<std::string>();
        }
        if (e.allDay && j["end"].contains("date")) {
            e.endDateTime = iso8601ToEpochDate(j["end"]["date"].get<std::string>());
        } else if (j["end"].contains("dateTime")) {
            e.endDateTime = iso8601ToEpoch(j["end"]["dateTime"].get<std::string>());
        }
    }

    if (j.contains("status") && !j["status"].is_null())
        e.status = j["status"].get<std::string>();

    if (j.contains("recurrence") && j["recurrence"].is_array() && !j["recurrence"].empty()) {
        e.recurrenceRule = j["recurrence"][0].get<std::string>();
        e.type = EventType::MASTER;
    }

    if (j.contains("recurringEventId") && !j["recurringEventId"].is_null()) {
        e.providerMasterId = j["recurringEventId"].get<std::string>();

        if (j.contains("originalStartTime")) {
            if (j["originalStartTime"].contains("dateTime")) {
                e.instanceStart = iso8601ToEpoch(j["originalStartTime"]["dateTime"].get<std::string>());
            }
            else if (j["originalStartTime"].contains("date")) {
                e.instanceStart = iso8601ToEpochDate(j["originalStartTime"]["date"].get<std::string>());
            }
        }

        if (e.status == "cancelled") {
            e.type = EventType::CANCELLED_INSTANCE;
        }
        else {
            e.type = EventType::EXCEPTION;
        }
    }

    if (j.contains("extendedProperties") &&
        j["extendedProperties"].contains("private") &&
        j["extendedProperties"]["private"].contains("local_id") &&
        !j["extendedProperties"]["private"]["local_id"].is_null())
    {
        e.id = std::stoll(j["extendedProperties"]["private"]["local_id"].get<std::string>());
    }

    return e;
}
