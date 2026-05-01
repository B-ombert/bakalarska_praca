#include "ui/calendar_ui_shared.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "utils/datetime_utils.h"

namespace {

const std::array<const char*, 12>& MonthNames() {
    static const std::array<const char*, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    return months;
}

} // namespace

std::string FormatMonthTitle(const int year, const int month) {
    return FormatMonthName(month) + " " + std::to_string(year);
}

std::string FormatMonthName(const int month) {
    return MonthNames()[month - 1];
}

std::string FormatDayHeader(const long long dayEpoch) {
    const std::tm tm = EpochToUtcTm(dayEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%A, %d %B %Y");
    return output.str();
}

std::string FormatShortDayHeader(const long long dayEpoch) {
    const std::tm tm = EpochToUtcTm(dayEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%a %d %b");
    return output.str();
}

std::string FormatWeekTitle(const long long weekStartEpoch) {
    const long long weekEndEpoch = weekStartEpoch + 6 * kSecondsPerDay;
    const std::tm startTm = EpochToUtcTm(weekStartEpoch);
    const std::tm endTm = EpochToUtcTm(weekEndEpoch);

    std::ostringstream output;
    output << std::put_time(&startTm, "%d %b")
           << " - " << std::put_time(&endTm, "%d %b %Y");
    return output.str();
}

std::string FormatTimeLabel(const long long epoch) {
    return FormatUtcDateTimeInput(epoch, false).substr(11, 5);
}

std::string BuildTimelineEventLabel(const Event& event, const long long dayEpoch) {
    if (event.allDay) {
        return event.title.empty() ? "All day" : event.title;
    }

    const long long clippedStart = std::max(event.startDateTime, dayEpoch);
    const long long clippedEnd = std::min(event.endDateTime, dayEpoch + kSecondsPerDay);

    std::ostringstream output;
    output << event.title << "\n" << FormatTimeLabel(clippedStart) << " - " << FormatTimeLabel(clippedEnd);
    return output.str();
}

std::string BuildMonthEventLabel(const Event& event) {
    if (event.allDay) {
        return event.title;
    }
    return event.title + " " + FormatTimeLabel(event.startDateTime);
}

void NormalizeAllDayEventRange(Event& event) {
    if (!event.allDay) {
        return;
    }

    event.startDateTime = StartOfUtcDay(event.startDateTime);
    event.endDateTime = StartOfUtcDay(event.endDateTime);

    if (event.endDateTime <= event.startDateTime) {
        event.endDateTime = event.startDateTime + kSecondsPerDay;
    }
}

long long EventDisplayEndDay(const Event& event) {
    const long long safeEndEpoch = std::max(event.startDateTime, event.endDateTime - 1);
    return StartOfUtcDay(safeEndEpoch);
}

bool SpansMultipleDays(const Event& event) {
    return StartOfUtcDay(event.startDateTime) != EventDisplayEndDay(event);
}

int DaysInMonth(const int year, const int month) {
    static const std::array<int, 12> daysPerMonth = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month != 2) {
        return daysPerMonth[month - 1];
    }

    const bool leapYear = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leapYear ? 29 : 28;
}

int MonthGridOffset(const int year, const int month) {
    const long long firstDayEpoch = MakeUtcEpoch(year, month, 1);
    const std::tm tm = EpochToUtcTm(firstDayEpoch);
    return (tm.tm_wday + 6) % 7;
}

bool IsSameUtcDay(const long long lhs, const long long rhs) {
    return StartOfUtcDay(lhs) == StartOfUtcDay(rhs);
}

long long StartOfUtcWeek(const long long epoch) {
    const long long dayEpoch = StartOfUtcDay(epoch);
    const std::tm tm = EpochToUtcTm(dayEpoch);
    const int mondayOffset = (tm.tm_wday + 6) % 7;
    return dayEpoch - static_cast<long long>(mondayOffset) * kSecondsPerDay;
}

std::string MakeLocalProviderEventId(const long long startDateTime) {
    const auto now = static_cast<long long>(std::time(nullptr));
    return "local-" + std::to_string(now) + "-" + std::to_string(startDateTime);
}

std::optional<std::string> ValidateEventForUi(const Event& event) {
    if (event.title.empty()) {
        return "Title is required.";
    }

    if (event.startDateTime < 0 || event.endDateTime < 0) {
        return "Use YYYY-MM-DD for all-day events or YYYY-MM-DD HH:MM for timed events. The earliest supported date is 1970-01-01.";
    }

    if (event.endDateTime < event.startDateTime) {
        return "End must be after start.";
    }

    return std::nullopt;
}

void ApplyAllDayToggleToInputs(wxTextCtrl* startCtrl, wxTextCtrl* endCtrl, const bool allDay) {
    const long long startEpoch = ParseUtcDateTimeInput(startCtrl->GetValue().ToStdString(), false);
    const long long endEpoch = ParseUtcDateTimeInput(endCtrl->GetValue().ToStdString(), false);

    if (startEpoch >= 0) {
        startCtrl->SetValue(FormatUtcDateTimeInput(startEpoch, allDay));
    }
    if (endEpoch >= 0) {
        endCtrl->SetValue(FormatUtcDateTimeInput(endEpoch, allDay));
    }
}
