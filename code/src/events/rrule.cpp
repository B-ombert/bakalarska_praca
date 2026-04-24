#include "events/rrule.h"

#include "utils/datetime_utils.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

Frequency parseFrequency(const std::string& value) {
    if (value == "DAILY") return Frequency::DAILY;
    if (value == "WEEKLY") return Frequency::WEEKLY;
    if (value == "MONTHLY") return Frequency::MONTHLY;
    if (value == "YEARLY") return Frequency::YEARLY;
    return Frequency::UNKNOWN;
}

std::string frequencyToString(const Frequency freq) {
    switch (freq) {
        case Frequency::DAILY: return "DAILY";
        case Frequency::WEEKLY: return "WEEKLY";
        case Frequency::MONTHLY: return "MONTHLY";
        case Frequency::YEARLY: return "YEARLY";
        case Frequency::UNKNOWN: return "UNKNOWN";
    }

    return "UNKNOWN";
}

std::vector<std::string> splitString(const std::string& input, const char delimiter) {
    std::vector<std::string> parts;
    std::stringstream ss(input);
    std::string part;

    while (std::getline(ss, part, delimiter)) {
        parts.push_back(part);
    }

    return parts;
}

int parseWeekdayCode(const std::string& value) {
    if (value == "MO") return 1;
    if (value == "TU") return 2;
    if (value == "WE") return 3;
    if (value == "TH") return 4;
    if (value == "FR") return 5;
    if (value == "SA") return 6;
    if (value == "SU") return 7;
    return 0;
}

std::string weekdayCodeToString(const int value) {
    switch (value) {
        case 1: return "MO";
        case 2: return "TU";
        case 3: return "WE";
        case 4: return "TH";
        case 5: return "FR";
        case 6: return "SA";
        case 7: return "SU";
        default: return "";
    }
}

} // namespace

RRule RRule::parseRRule(const std::string& rule) {
    RRule parsed;

    std::string normalized = rule;
    if (normalized.rfind("RRULE:", 0) == 0) {
        normalized = normalized.substr(6);
    }

    for (const auto& part : splitString(normalized, ';')) {
        const auto separator = part.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = part.substr(0, separator);
        const std::string value = part.substr(separator + 1);

        if (key == "FREQ") {
            parsed.freq = parseFrequency(value);
        }
        else if (key == "INTERVAL") {
            parsed.interval = std::max(1, std::atoi(value.c_str()));
        }
        else if (key == "COUNT") {
            parsed.count = std::atoi(value.c_str());
            parsed.hasCount = true;
        }
        else if (key == "UNTIL") {
            parsed.until = iso8601ToEpoch(value);
            parsed.hasUntil = parsed.until > 0;
        }
        else if (key == "BYDAY") {
            for (const auto& day : splitString(value, ',')) {
                const int parsedDay = parseWeekdayCode(day);
                if (parsedDay != 0) {
                    parsed.byDay.push_back(parsedDay);
                }
            }
        }
        else if (key == "BYMONTHDAY") {
            for (const auto& day : splitString(value, ',')) {
                const int parsedDay = std::atoi(day.c_str());
                if (parsedDay != 0) {
                    parsed.byMonthDay.push_back(parsedDay);
                }
            }
        }
        else if (key == "WKST") {
            const int parsedDay = parseWeekdayCode(value);
            if (parsedDay != 0) {
                parsed.weekStart = parsedDay;
            }
        }
    }

    return parsed;
}

std::string RRule::toGoogleRRule() {
    std::ostringstream os;
    os << "RRULE:FREQ=" << frequencyToString(freq);

    if (interval > 1) {
        os << ";INTERVAL=" << interval;
    }

    if (hasCount) {
        os << ";COUNT=" << count;
    }

    if (hasUntil) {
        os << ";UNTIL=" << epochToIso(until);
    }

    if (!byDay.empty()) {
        os << ";BYDAY=";
        for (size_t i = 0; i < byDay.size(); ++i) {
            if (i > 0) {
                os << ",";
            }
            os << weekdayCodeToString(byDay[i]);
        }
    }

    if (!byMonthDay.empty()) {
        os << ";BYMONTHDAY=";
        for (size_t i = 0; i < byMonthDay.size(); ++i) {
            if (i > 0) {
                os << ",";
            }
            os << byMonthDay[i];
        }
    }

    if (weekStart != 1) {
        os << ";WKST=" << weekdayCodeToString(weekStart);
    }

    return os.str();
}

std::string RRule::toOutlookRRule() {
    return toGoogleRRule();
}
