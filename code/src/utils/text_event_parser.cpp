#include "utils/text_event_parser.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "utils/datetime_utils.h"
#include "utils/timezone_utils.h"

namespace {

constexpr long long kSecondsPerDayLocal = 86400;

struct ParsedParts {
    std::string title;
    std::string dateText;
    std::string timeText;
    std::string location;
    std::string recurrenceText;
};

struct ParsedDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

struct ParsedTime {
    int hour = 0;
    int minute = 0;
};

struct KeywordMatch {
    std::string keyword;
    std::size_t position = std::string::npos;
    std::size_t end = std::string::npos;
};

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool IsWordBoundary(const char ch) {
    return !std::isalnum(static_cast<unsigned char>(ch)) && ch != '_';
}

bool StartsWithCaseInsensitive(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           ToLowerAscii(value.substr(0, prefix.size())) == ToLowerAscii(prefix);
}

std::vector<std::string> SplitWords(const std::string& value) {
    std::vector<std::string> words;
    std::string current;
    for (const char ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

bool TryParseInt(const std::string& value, int& parsed) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    parsed = std::stoi(value);
    return true;
}

int MonthFromText(const std::string& value) {
    static const std::unordered_map<std::string, int> months = {
        {"january", 1}, {"jan", 1},
        {"february", 2}, {"feb", 2},
        {"march", 3}, {"mar", 3},
        {"april", 4}, {"apr", 4},
        {"may", 5},
        {"june", 6}, {"jun", 6},
        {"july", 7}, {"jul", 7},
        {"august", 8}, {"aug", 8},
        {"september", 9}, {"sep", 9}, {"sept", 9},
        {"october", 10}, {"oct", 10},
        {"november", 11}, {"nov", 11},
        {"december", 12}, {"dec", 12},
    };

    std::string normalized = ToLowerAscii(Trim(value));
    if (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }

    int numeric = 0;
    if (TryParseInt(normalized, numeric)) {
        return numeric;
    }

    const auto it = months.find(normalized);
    return it == months.end() ? 0 : it->second;
}

int WeekdayFromText(const std::string& value) {
    static const std::unordered_map<std::string, int> weekdays = {
        {"sunday", 0}, {"sun", 0},
        {"monday", 1}, {"mon", 1},
        {"tuesday", 2}, {"tue", 2}, {"tues", 2},
        {"wednesday", 3}, {"wed", 3},
        {"thursday", 4}, {"thu", 4}, {"thur", 4}, {"thurs", 4},
        {"friday", 5}, {"fri", 5},
        {"saturday", 6}, {"sat", 6},
    };

    const auto it = weekdays.find(ToLowerAscii(Trim(value)));
    return it == weekdays.end() ? -1 : it->second;
}

bool IsValidDate(const int year, const int month, const int day) {
    if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }

    const std::tm tm = EpochToUtcTm(MakeUtcEpoch(year, month, day));
    return tm.tm_year + 1900 == year &&
           tm.tm_mon + 1 == month &&
           tm.tm_mday == day;
}

ParsedDate DateFromEpoch(const long long displayEpoch) {
    const std::tm tm = EpochToUtcTm(displayEpoch);
    return ParsedDate{tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

long long AddDaysToDisplayEpoch(const long long displayEpoch, const int days) {
    return StartOfUtcDay(displayEpoch) + static_cast<long long>(days) * kSecondsPerDayLocal;
}

std::optional<KeywordMatch> FindNextKeyword(const std::string& input, const std::size_t start) {
    static constexpr std::array<std::string_view, 4> keywords = {"EVERY", "ON", "AT", "IN"};

    std::optional<KeywordMatch> best;
    for (std::size_t i = start; i < input.size(); ++i) {
        if (i > 0 && !IsWordBoundary(input[i - 1])) {
            continue;
        }

        for (const auto keyword : keywords) {
            const std::size_t length = keyword.size();
            if (i + length > input.size()) {
                continue;
            }
            if (ToLowerAscii(input.substr(i, length)) != ToLowerAscii(std::string(keyword))) {
                continue;
            }
            if (i + length < input.size() && !IsWordBoundary(input[i + length])) {
                continue;
            }

            KeywordMatch match{std::string(keyword), i, i + length};
            if (!best.has_value() || match.position < best->position) {
                best = match;
            }
        }
    }

    return best;
}

ParsedParts SplitInputByKeywords(const std::string& input) {
    const std::string trimmed = Trim(input);
    if (trimmed.empty()) {
        throw std::invalid_argument("Event title is empty.");
    }

    const auto firstKeyword = FindNextKeyword(trimmed, 0);
    ParsedParts parts;
    parts.title = Trim(firstKeyword.has_value() ? trimmed.substr(0, firstKeyword->position) : trimmed);
    if (parts.title.empty()) {
        throw std::invalid_argument("Event title is empty.");
    }

    std::size_t cursor = firstKeyword.has_value() ? firstKeyword->position : trimmed.size();
    while (cursor < trimmed.size()) {
        const auto keyword = FindNextKeyword(trimmed, cursor);
        if (!keyword.has_value()) {
            break;
        }

        const std::size_t valueStart = keyword->end;
        const auto nextKeyword = FindNextKeyword(trimmed, valueStart);
        const std::size_t valueEnd = nextKeyword.has_value() ? nextKeyword->position : trimmed.size();
        const std::string value = Trim(trimmed.substr(valueStart, valueEnd - valueStart));

        if (keyword->keyword == "ON") {
            parts.dateText = value;
        }
        else if (keyword->keyword == "AT") {
            parts.timeText = value;
        }
        else if (keyword->keyword == "IN") {
            parts.location = value;
        }
        else if (keyword->keyword == "EVERY") {
            parts.recurrenceText = value;
        }

        cursor = valueEnd;
    }

    return parts;
}

ParsedDate ParseWeekdayDate(const std::string& text,
                            const long long referenceDisplayEpoch,
                            const std::string& fieldName) {
    std::string value = Trim(text);
    bool next = false;
    if (StartsWithCaseInsensitive(value, "next ")) {
        next = true;
        value = Trim(value.substr(5));
    }

    const int targetWeekday = WeekdayFromText(value);
    if (targetWeekday < 0) {
        throw std::invalid_argument("Invalid " + fieldName + " value: unknown weekday.");
    }

    const std::tm reference = EpochToUtcTm(referenceDisplayEpoch);
    const int currentWeekday = reference.tm_wday;
    int daysAhead = (targetWeekday - currentWeekday + 7) % 7;
    if (daysAhead == 0) {
        daysAhead = 7;
    }
    if (next) {
        daysAhead += 7;
    }

    return DateFromEpoch(AddDaysToDisplayEpoch(referenceDisplayEpoch, daysAhead));
}

ParsedDate ParseNumericDate(std::string text,
                            const long long referenceDisplayEpoch,
                            const std::string& fieldName) {
    text = Trim(text);
    for (char& ch : text) {
        if (ch == '/') {
            ch = '.';
        }
    }

    const ParsedDate reference = DateFromEpoch(referenceDisplayEpoch);
    int day = 0;
    int month = reference.month;
    int year = reference.year;

    const auto dot = text.find('.');
    if (dot == std::string::npos) {
        if (!TryParseInt(text, day)) {
            throw std::invalid_argument("Invalid " + fieldName + " value: day must be numeric or a weekday.");
        }
    }
    else {
        if (!TryParseInt(Trim(text.substr(0, dot)), day)) {
            throw std::invalid_argument("Invalid " + fieldName + " value: day is not numeric.");
        }

        std::string rest = Trim(text.substr(dot + 1));
        const auto words = SplitWords(rest);
        if (words.empty()) {
            throw std::invalid_argument("Invalid " + fieldName + " value: month is missing after day.");
        }

        month = MonthFromText(words[0]);
        if (month < 1 || month > 12) {
            throw std::invalid_argument("Invalid " + fieldName + " value: month is invalid.");
        }

        if (words.size() > 1 && !TryParseInt(words[1], year)) {
            throw std::invalid_argument("Invalid " + fieldName + " value: year is invalid.");
        }
        if (words.size() > 2) {
            throw std::invalid_argument("Invalid " + fieldName + " value: unexpected text after year.");
        }
    }

    if (!IsValidDate(year, month, day)) {
        throw std::invalid_argument("Invalid " + fieldName + " value: date does not exist.");
    }
    return ParsedDate{year, month, day};
}

ParsedDate ParseDateValue(const std::string& text,
                          const long long referenceDisplayEpoch,
                          const std::string& fieldName) {
    const std::string value = Trim(text);
    if (value.empty()) {
        return DateFromEpoch(referenceDisplayEpoch);
    }

    const std::string lower = ToLowerAscii(value);
    if (lower == "today") {
        return DateFromEpoch(referenceDisplayEpoch);
    }
    if (lower == "tomorrow") {
        return DateFromEpoch(AddDaysToDisplayEpoch(referenceDisplayEpoch, 1));
    }
    if (StartsWithCaseInsensitive(value, "next ") || WeekdayFromText(value) >= 0) {
        return ParseWeekdayDate(value, referenceDisplayEpoch, fieldName);
    }
    return ParseNumericDate(value, referenceDisplayEpoch, fieldName);
}

ParsedTime ParseTimeValue(std::string text) {
    text = Trim(text);
    if (text.empty()) {
        throw std::invalid_argument("Invalid AT value: time is missing.");
    }

    auto words = SplitWords(text);
    if (words.empty()) {
        throw std::invalid_argument("Invalid AT value: time is missing.");
    }

    std::string timePart = words[0];
    std::string meridiem;
    if (words.size() > 1) {
        meridiem = ToLowerAscii(words[1]);
    }
    if (words.size() > 2) {
        throw std::invalid_argument("Invalid AT value: unexpected text after time.");
    }
    if (meridiem != "" && meridiem != "am" && meridiem != "pm") {
        throw std::invalid_argument("Invalid AT value: AM/PM marker is invalid.");
    }

    int hour = 0;
    int minute = 0;
    const auto colon = timePart.find(':');
    if (colon == std::string::npos) {
        if (!TryParseInt(timePart, hour)) {
            throw std::invalid_argument("Invalid AT value: hour is not numeric.");
        }
    }
    else {
        if (!TryParseInt(timePart.substr(0, colon), hour) ||
            !TryParseInt(timePart.substr(colon + 1), minute)) {
            throw std::invalid_argument("Invalid AT value: hour or minute is not numeric.");
        }
    }

    if (!meridiem.empty()) {
        if (hour < 1 || hour > 12) {
            throw std::invalid_argument("Invalid AT value: AM/PM hour must be from 1 to 12.");
        }
        if (meridiem == "am") {
            hour = hour == 12 ? 0 : hour;
        }
        else if (hour != 12) {
            hour += 12;
        }
    }

    if (hour < 0 || hour > 23) {
        throw std::invalid_argument("Invalid AT value: hour must be from 0 to 23.");
    }
    if (minute < 0 || minute > 59) {
        throw std::invalid_argument("Invalid AT value: minute must be from 0 to 59.");
    }

    return ParsedTime{hour, minute};
}

long long ToUtcEpoch(const ParsedDate& date,
                     const ParsedTime& time,
                     const std::string& timezone) {
    const long long displayEpoch = MakeUtcEpoch(date.year, date.month, date.day, time.hour, time.minute);
    return ConvertTimeZoneDisplayEpochToUtcEpoch(timezone, displayEpoch).value_or(displayEpoch);
}

std::string FrequencyToRRuleValue(const std::string& value) {
    const std::string normalized = ToLowerAscii(Trim(value));
    if (normalized == "day" || normalized == "daily") {
        return "DAILY";
    }
    if (normalized == "week" || normalized == "weekly") {
        return "WEEKLY";
    }
    if (normalized == "month" || normalized == "monthly") {
        return "MONTHLY";
    }
    if (normalized == "year" || normalized == "yearly") {
        return "YEARLY";
    }
    return "";
}

std::size_t FindCaseInsensitiveWord(const std::string& input, const std::string& word) {
    const std::string lowerInput = ToLowerAscii(input);
    const std::string lowerWord = ToLowerAscii(word);
    for (std::size_t pos = lowerInput.find(lowerWord);
         pos != std::string::npos;
         pos = lowerInput.find(lowerWord, pos + 1)) {
        const bool beforeOk = pos == 0 || IsWordBoundary(input[pos - 1]);
        const std::size_t after = pos + word.size();
        const bool afterOk = after >= input.size() || IsWordBoundary(input[after]);
        if (beforeOk && afterOk) {
            return pos;
        }
    }
    return std::string::npos;
}

std::string BuildRecurrenceRule(const std::string& recurrenceText,
                                const ParsedDate& eventDate,
                                const long long referenceDisplayEpoch,
                                const std::string& timezone) {
    const std::string trimmed = Trim(recurrenceText);
    if (trimmed.empty()) {
        return "";
    }

    const auto words = SplitWords(trimmed);
    if (words.empty()) {
        return "";
    }

    const std::string frequency = FrequencyToRRuleValue(words[0]);
    if (frequency.empty()) {
        throw std::invalid_argument("Invalid EVERY value: frequency must be day, week, month, or year.");
    }

    const std::string rest = Trim(trimmed.substr(words[0].size()));
    if (rest.empty()) {
        throw std::invalid_argument("Invalid EVERY value: recurrence must end with '<count> times' or 'UNTIL <date>'.");
    }

    const std::size_t untilPos = FindCaseInsensitiveWord(rest, "UNTIL");
    if (untilPos != std::string::npos) {
        const std::string beforeUntil = Trim(rest.substr(0, untilPos));
        if (!beforeUntil.empty()) {
            throw std::invalid_argument("Invalid EVERY value: unexpected text before UNTIL.");
        }

        const std::string untilText = Trim(rest.substr(untilPos + 5));
        if (untilText.empty()) {
            throw std::invalid_argument("Invalid EVERY UNTIL value: date is missing.");
        }

        const ParsedDate untilDate = ParseDateValue(untilText, referenceDisplayEpoch, "EVERY UNTIL");
        const long long eventDisplayDay = MakeUtcEpoch(eventDate.year, eventDate.month, eventDate.day);
        const long long untilDisplayDay = MakeUtcEpoch(untilDate.year, untilDate.month, untilDate.day);
        if (untilDisplayDay < eventDisplayDay) {
            throw std::invalid_argument("Invalid EVERY UNTIL value: date cannot be before the event date.");
        }

        const long long untilUtc = ToUtcEpoch(untilDate, ParsedTime{23, 59}, timezone);
        return "RRULE:FREQ=" + frequency + ";UNTIL=" + epochToIso(untilUtc);
    }

    const auto restWords = SplitWords(rest);
    if (restWords.size() != 2 || ToLowerAscii(restWords[1]) != "times") {
        throw std::invalid_argument("Invalid EVERY value: expected '<count> times' or 'UNTIL <date>'.");
    }

    int count = 0;
    if (!TryParseInt(restWords[0], count) || count <= 0) {
        throw std::invalid_argument("Invalid EVERY value: count must be a positive integer.");
    }

    return "RRULE:FREQ=" + frequency + ";COUNT=" + std::to_string(count);
}

} // namespace

Event ParseTextEventInput(const std::string& input) {
    const auto now = std::chrono::system_clock::now();
    const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    return ParseTextEventInput(input, epoch, GetCurrentLocalTimeZoneName());
}

Event ParseTextEventInput(const std::string& input,
                          const long long referenceEpoch,
                          const std::string& timezone) {
    const std::string effectiveTimezone = timezone.empty() ? GetCurrentLocalTimeZoneName() : timezone;
    const long long referenceDisplayEpoch =
        ConvertUtcEpochToTimeZoneDisplayEpoch(effectiveTimezone, referenceEpoch);

    const ParsedParts parts = SplitInputByKeywords(input);
    const ParsedDate date = ParseDateValue(parts.dateText, referenceDisplayEpoch, "ON");

    Event event;
    event.title = parts.title;
    event.location = parts.location;
    event.timezone = effectiveTimezone;
    event.status = "confirmed";
    event.syncStatus = PENDING_INSERT;
    event.recurrenceRule = BuildRecurrenceRule(parts.recurrenceText, date, referenceDisplayEpoch, effectiveTimezone);
    if (!event.recurrenceRule.empty()) {
        event.type = EventType::MASTER;
    }

    if (Trim(parts.timeText).empty()) {
        event.allDay = true;
        event.startDateTime = MakeUtcEpoch(date.year, date.month, date.day);
        event.endDateTime = event.startDateTime + kSecondsPerDayLocal;
        return event;
    }

    const ParsedTime time = ParseTimeValue(parts.timeText);
    event.allDay = false;
    event.startDateTime = ToUtcEpoch(date, time, effectiveTimezone);
    event.endDateTime = event.startDateTime + 3600;
    return event;
}
