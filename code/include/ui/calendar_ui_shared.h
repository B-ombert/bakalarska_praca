#pragma once

#include <optional>
#include <string>

#include <wx/textctrl.h>

#include "models/event.h"

constexpr int kMonthCellCount = 35;
constexpr int kMinCalendarYear = 1970;
constexpr long long kMinCalendarEpoch = 0;
constexpr int kMinutesPerDay = 24 * 60;
constexpr int kSecondsPerDay = 86400;
constexpr int kTimelineHourHeight = 80;
constexpr int kTimelineHeaderHeight = 52;
constexpr int kTimelineAllDayLanePadding = 8;
constexpr int kTimelineAllDayRowHeight = 30;
constexpr int kTimelineAllDayMinRows = 2;
constexpr int kTimelineTimeLabelWidth = 74;

enum class CalendarViewMode {
    MONTH = 0,
    WEEK = 1,
    DAY = 2
};

struct EventDraftDefaults {
    long long startDateTime = 0;
    long long endDateTime = 0;
    bool allDay = false;
};

std::string FormatMonthTitle(int year, int month);
std::string FormatMonthName(int month);
std::string FormatDayHeader(long long dayEpoch);
std::string FormatShortDayHeader(long long dayEpoch);
std::string FormatWeekTitle(long long weekStartEpoch);
std::string FormatTimeLabel(long long epoch);
std::string BuildTimelineEventLabel(const Event& event, long long dayEpoch);
std::string BuildMonthEventLabel(const Event& event);
void NormalizeAllDayEventRange(Event& event);
long long EventDisplayEndDay(const Event& event);
bool SpansMultipleDays(const Event& event);
int DaysInMonth(int year, int month);
int MonthGridOffset(int year, int month);
bool IsSameUtcDay(long long lhs, long long rhs);
long long StartOfUtcWeek(long long epoch);
std::string MakeLocalProviderEventId(long long startDateTime);
std::optional<std::string> ValidateEventForUi(const Event& event);
void ApplyAllDayToggleToInputs(wxTextCtrl* startCtrl, wxTextCtrl* endCtrl, bool allDay);
