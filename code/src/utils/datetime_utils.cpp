#include "utils/datetime_utils.h"

#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <wx/datetime.h>

namespace {

bool ParseIsoDateTimeParts(const std::string& iso,
                           int& year,
                           int& month,
                           int& day,
                           int& hour,
                           int& minute,
                           int& second,
                           int& offsetSeconds) {
    std::string normalized = iso;
    if (normalized.size() >= 8 &&
        std::isdigit(static_cast<unsigned char>(normalized[0])) &&
        std::isdigit(static_cast<unsigned char>(normalized[1])) &&
        std::isdigit(static_cast<unsigned char>(normalized[2])) &&
        std::isdigit(static_cast<unsigned char>(normalized[3])) &&
        std::isdigit(static_cast<unsigned char>(normalized[4])) &&
        std::isdigit(static_cast<unsigned char>(normalized[5])) &&
        std::isdigit(static_cast<unsigned char>(normalized[6])) &&
        std::isdigit(static_cast<unsigned char>(normalized[7])) &&
        (normalized.size() == 8 || normalized[8] == 'T' || normalized[8] == ' ')) {
        normalized.insert(4, "-");
        normalized.insert(7, "-");
    }

    if (normalized.size() < 10 || normalized[4] != '-' || normalized[7] != '-') {
        return false;
    }

    year = std::atoi(normalized.substr(0, 4).c_str());
    month = std::atoi(normalized.substr(5, 2).c_str());
    day = std::atoi(normalized.substr(8, 2).c_str());
    hour = 0;
    minute = 0;
    second = 0;
    offsetSeconds = 0;

    const size_t separatorPos = normalized.find_first_of("T ");
    if (separatorPos == std::string::npos) {
        return true;
    }

    const std::string timeAndZone = normalized.substr(separatorPos + 1);
    if (timeAndZone.size() < 5 || timeAndZone[2] != ':') {
        return false;
    }

    size_t zonePos = timeAndZone.find('Z');
    if (zonePos == std::string::npos) {
        const size_t plusPos = timeAndZone.find('+');
        const size_t minusPos = timeAndZone.find('-', 1);
        zonePos = (plusPos != std::string::npos) ? plusPos : minusPos;
        if (zonePos == std::string::npos) {
            zonePos = timeAndZone.size();
        }
    }

    std::string timePart = timeAndZone.substr(0, zonePos);
    const size_t fractionalPos = timePart.find('.');
    if (fractionalPos != std::string::npos) {
        timePart = timePart.substr(0, fractionalPos);
    }

    hour = std::atoi(timePart.substr(0, 2).c_str());
    minute = std::atoi(timePart.substr(3, 2).c_str());
    if (timePart.size() >= 8 && timePart[5] == ':') {
        second = std::atoi(timePart.substr(6, 2).c_str());
    }

    if (zonePos < timeAndZone.size()) {
        if (timeAndZone[zonePos] == 'Z') {
            offsetSeconds = 0;
        } else {
            const int sign = (timeAndZone[zonePos] == '-') ? -1 : 1;
            const std::string offsetPart = timeAndZone.substr(zonePos + 1);
            int offsetHours = 0;
            int offsetMinutes = 0;

            if (offsetPart.size() >= 5 && offsetPart[2] == ':') {
                offsetHours = std::atoi(offsetPart.substr(0, 2).c_str());
                offsetMinutes = std::atoi(offsetPart.substr(3, 2).c_str());
            } else if (offsetPart.size() >= 4) {
                offsetHours = std::atoi(offsetPart.substr(0, 2).c_str());
                offsetMinutes = std::atoi(offsetPart.substr(2, 2).c_str());
            }

            offsetSeconds = sign * (offsetHours * 3600 + offsetMinutes * 60);
        }
    }

    return true;
}

wxDateTime UtcDateTimeFromEpoch(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    return wxDateTime(
        tm.tm_mday,
        static_cast<wxDateTime::Month>(tm.tm_mon),
        tm.tm_year + 1900,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    );
}

} // namespace

long long MakeUtcEpoch(const int year, const int month, const int day, const int hour, const int minute, const int second) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return static_cast<long long>(_mkgmtime(&tm));
}

std::tm EpochToUtcTm(const long long epoch) {
    std::time_t value = static_cast<std::time_t>(epoch);
    std::tm tm{};

#ifdef _WIN32
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif

    return tm;
}

long long StartOfUtcDay(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    return MakeUtcEpoch(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

long long ParseUtcDateTimeInput(const std::string& value, const bool allDay) {
    std::tm tm{};
    std::istringstream input(value);

    if (allDay) {
        input >> std::get_time(&tm, "%Y-%m-%d");
    } else {
        input >> std::get_time(&tm, "%Y-%m-%d %H:%M");
    }

    if (input.fail()) {
        return -1;
    }

    return MakeUtcEpoch(
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    );
}

std::string FormatUtcDateTimeInput(const long long epoch, const bool allDay) {
    const std::tm tm = EpochToUtcTm(epoch);
    std::ostringstream output;
    output << std::put_time(&tm, allDay ? "%Y-%m-%d" : "%Y-%m-%d %H:%M");
    return output.str();
}

long long iso8601ToEpoch(const std::string& iso) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int offsetSeconds = 0;

    if (!ParseIsoDateTimeParts(iso, year, month, day, hour, minute, second, offsetSeconds)) {
        return 0;
    }

    return MakeUtcEpoch(year, month, day, hour, minute, second) - offsetSeconds;
}

long long iso8601ToEpochDate(const std::string& iso) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int offsetSeconds = 0;

    if (!ParseIsoDateTimeParts(iso, year, month, day, hour, minute, second, offsetSeconds)) {
        return 0;
    }

    return MakeUtcEpoch(year, month, day);
}

std::string epochToIso(const long long epoch) {
    const wxDateTime utc = UtcDateTimeFromEpoch(epoch);
    return utc.FormatISOCombined('T').ToStdString() + "Z";
}

std::string epochToIsoDate(const long long epoch) {
    const wxDateTime utc = UtcDateTimeFromEpoch(epoch);
    return utc.FormatISODate().ToStdString();
}
