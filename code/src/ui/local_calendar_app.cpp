#include "ui/local_calendar_app.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wx.h>

#include "models/calendar.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "ui/calendar_ui_shared.h"
#include "ui/event_editor_dialog.h"
#include "ui/month_cell_panel.h"
#include "ui/timeline_view_panel.h"
#include "utils/datetime_utils.h"

namespace {

struct MonthCellMeta {
    long long dayEpoch = 0;
    int dayNumber = 0;
    bool inCurrentMonth = true;
};

struct VisibleRange {
    long long startEpoch = 0;
    long long endEpoch = 0;
};

constexpr int kYearComboChunkSize = 120;
constexpr int kYearComboBackwardPadding = 20;

class LocalCalendarFrame final : public wxFrame {
public:
    explicit LocalCalendarFrame(const std::string& dbPath)
        : wxFrame(nullptr, wxID_ANY, "Calendar", wxDefaultPosition, wxSize(1440, 860)),
          db_(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
          eventRepository_(db_),
          calendarRepository_(db_) {
        localCalendarId_ = EnsureLocalCalendar();

        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        SyncVisibleMonthToSelectedDay();

        BuildLayout();
        ApplyLook();
        BindEvents();
        RefreshEvents();
    }

private:
    long long EnsureLocalCalendar() {
        auto existing = calendarRepository_.getByProviderId(0, "local-calendar");
        if (existing) {
            return existing->id;
        }

        Calendar localCalendar{};
        localCalendar.accountId = 0;
        localCalendar.providerCalendarId = "local-calendar";
        localCalendar.name = "Local Calendar";
        localCalendar.timezone = "UTC";
        localCalendar.syncEnabled = false;
        localCalendar.createdAt = std::time(nullptr);
        localCalendar.updatedAt = std::time(nullptr);

        calendarRepository_.upsert(localCalendar);
        existing = calendarRepository_.getByProviderId(0, "local-calendar");
        return existing ? existing->id : 0;
    }

    void BuildLayout() {
        auto* panel = new wxPanel(this);
        auto* rootSizer = new wxBoxSizer(wxVERTICAL);

        auto* toolbarSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* titleBlock = new wxBoxSizer(wxVERTICAL);
        auto* appTitle = new wxStaticText(panel, wxID_ANY, "Local Calendar");
        auto* appSubtitle = new wxStaticText(panel, wxID_ANY, "Month, week and day views with editable event cards");
        titleBlock->Add(appTitle, 0, wxBOTTOM, 2);
        titleBlock->Add(appSubtitle, 0);

        monthViewButton_ = new wxButton(panel, wxID_ANY, "Month");
        weekViewButton_ = new wxButton(panel, wxID_ANY, "Week");
        dayViewButton_ = new wxButton(panel, wxID_ANY, "Day");
        todayButton_ = new wxButton(panel, wxID_ANY, "Today");
        previousButton_ = new wxButton(panel, wxID_ANY, "<");
        nextButton_ = new wxButton(panel, wxID_ANY, ">");
        monthTitleLabel_ = new wxStaticText(panel, wxID_ANY, "");
        yearComboBox_ = new wxComboBox(panel, wxID_ANY, "", wxDefaultPosition, wxSize(110, -1), 0, nullptr, wxCB_READONLY);
        PopulateYearComboWindow(visibleYear_);

        toolbarSizer->Add(titleBlock, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 24);
        toolbarSizer->Add(monthViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(weekViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(dayViewButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(todayButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(previousButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(nextButton_, 0, wxRIGHT, 18);
        toolbarSizer->Add(monthTitleLabel_, 0, wxALIGN_CENTER_VERTICAL);
        toolbarSizer->Add(yearComboBox_, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, 8);

        auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* leftPane = new wxBoxSizer(wxVERTICAL);
        calendarBook_ = new wxSimplebook(panel, wxID_ANY);

        auto* monthPage = new wxPanel(calendarBook_);
        auto* monthSizer = new wxBoxSizer(wxVERTICAL);
        auto* weekdaySizer = new wxGridSizer(1, 7, 8, 8);
        const std::array<const char*, 7> weekdays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        for (const char* weekday : weekdays) {
            weekdaySizer->Add(new wxStaticText(monthPage, wxID_ANY, weekday), 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
        }
        monthSizer->Add(weekdaySizer, 0, wxEXPAND | wxALL, 10);

        auto* gridSizer = new wxGridSizer(6, 7, 8, 8);
        for (int index = 0; index < kMonthCellCount; ++index) {
            monthCells_[index] = new MonthCellPanel(
                monthPage,
                index,
                [this](const int cellIndex) { HandleMonthCellClicked(cellIndex); },
                [this](const int cellIndex) { HandleMonthCellCreateEvent(cellIndex); },
                [this](const long long eventId) { OpenEventById(eventId); });
            gridSizer->Add(monthCells_[index], 1, wxEXPAND);
        }
        monthSizer->Add(gridSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        monthPage->SetSizer(monthSizer);

        auto* weekPage = new wxPanel(calendarBook_);
        auto* weekSizer = new wxBoxSizer(wxVERTICAL);
        weekTimeline_ = new TimelineViewPanel(weekPage);
        weekSizer->Add(weekTimeline_, 1, wxEXPAND | wxALL, 10);
        weekPage->SetSizer(weekSizer);

        auto* dayPage = new wxPanel(calendarBook_);
        auto* daySizer = new wxBoxSizer(wxVERTICAL);
        dayTimeline_ = new TimelineViewPanel(dayPage);
        daySizer->Add(dayTimeline_, 1, wxEXPAND | wxALL, 10);
        dayPage->SetSizer(daySizer);

        calendarBook_->AddPage(monthPage, "Month");
        calendarBook_->AddPage(weekPage, "Week");
        calendarBook_->AddPage(dayPage, "Day");
        leftPane->Add(calendarBook_, 1, wxEXPAND);

        auto* sidePane = new wxBoxSizer(wxVERTICAL);
        sidePane->Add(new wxStaticText(panel, wxID_ANY, "Actions"), 0, wxALL, 10);

        auto* buttonSizer = new wxBoxSizer(wxVERTICAL);
        newButton_ = new wxButton(panel, wxID_ANY, "New Event");
        refreshButton_ = new wxButton(panel, wxID_ANY, "Refresh");
        newButton_->SetMinSize(wxSize(140, 36));
        buttonSizer->Add(newButton_, 0, wxEXPAND | wxBOTTOM, 8);
        buttonSizer->Add(refreshButton_, 0, wxEXPAND);
        sidePane->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        statusLabel_ = new wxStaticText(panel, wxID_ANY, "Ready");
        sidePane->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        sidePane->AddStretchSpacer();

        contentSizer->Add(leftPane, 5, wxEXPAND | wxALL, 12);
        contentSizer->Add(sidePane, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(toolbarSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        rootSizer->Add(contentSizer, 1, wxEXPAND);
        panel->SetSizer(rootSizer);

        weekTimeline_->SetMode(CalendarViewMode::WEEK);
        weekTimeline_->SetEventClickHandler([this](const long long eventId) { OpenEventById(eventId); });
        weekTimeline_->SetEmptySlotClickHandler([this](const long long dayEpoch, const int minuteOfDay) {
            OpenNewTimedEventDialog(dayEpoch, minuteOfDay);
        });

        dayTimeline_->SetMode(CalendarViewMode::DAY);
        dayTimeline_->SetEventClickHandler([this](const long long eventId) { OpenEventById(eventId); });
        dayTimeline_->SetEmptySlotClickHandler([this](const long long dayEpoch, const int minuteOfDay) {
            OpenNewTimedEventDialog(dayEpoch, minuteOfDay);
        });
    }

    void ApplyLook() {
        SetBackgroundColour(wxColour(246, 248, 252));
        if (GetParent() == nullptr) {
            SetMinSize(wxSize(1240, 760));
        }

        const wxColour surfaceBg(255, 255, 255);
        monthViewButton_->SetBackgroundColour(surfaceBg);
        weekViewButton_->SetBackgroundColour(surfaceBg);
        dayViewButton_->SetBackgroundColour(surfaceBg);
        todayButton_->SetBackgroundColour(surfaceBg);
        previousButton_->SetBackgroundColour(surfaceBg);
        nextButton_->SetBackgroundColour(surfaceBg);
        yearComboBox_->SetBackgroundColour(surfaceBg);
    }

    void BindEvents() {
        monthViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::MONTH, 0); });
        weekViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::WEEK, 1); });
        dayViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::DAY, 2); });
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(-1); });
        nextButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(1); });
        newButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNew, this);
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        yearComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnYearChanged, this);
    }

    std::vector<Event> EventsForDay(const long long dayEpoch) const {
        std::vector<Event> results;
        const long long dayEnd = dayEpoch + kSecondsPerDay;

        for (const auto& event : events_) {
            if (event.deletedAt != 0) {
                continue;
            }
            if (event.startDateTime < dayEnd && event.endDateTime > dayEpoch) {
                results.push_back(event);
            }
        }

        return results;
    }

    VisibleRange ComputeVisibleRange() const {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            const int firstOffset = MonthGridOffset(visibleYear_, visibleMonth_);
            const long long firstDayOfMonth = MakeUtcEpoch(visibleYear_, visibleMonth_, 1);
            const long long gridStart = firstDayOfMonth - static_cast<long long>(firstOffset) * kSecondsPerDay;
            return VisibleRange{gridStart, gridStart + static_cast<long long>(kMonthCellCount) * kSecondsPerDay};
        }

        if (currentViewMode_ == CalendarViewMode::WEEK) {
            const long long weekStart = StartOfUtcWeek(selectedDayEpoch_);
            return VisibleRange{weekStart, weekStart + 7LL * kSecondsPerDay};
        }

        return VisibleRange{selectedDayEpoch_, selectedDayEpoch_ + kSecondsPerDay};
    }

    void RefreshEvents() {
        const VisibleRange range = ComputeVisibleRange();
        events_ = eventRepository_.getEventsInRange(localCalendarId_, range.startEpoch, range.endEpoch);
        RefreshViewState();
        statusLabel_->SetLabel(wxString::Format("Loaded %zu visible event(s)", events_.size()));
    }

    void RefreshViewState() {
        RefreshHeaderTitle();
        RefreshMonthGrid();
        RefreshTimelineViews();
    }

    void RefreshHeaderTitle() {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            monthTitleLabel_->SetLabel(FormatMonthName(visibleMonth_));
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            monthTitleLabel_->SetLabel(FormatWeekTitle(StartOfUtcWeek(selectedDayEpoch_)));
        }
        else {
            monthTitleLabel_->SetLabel(FormatDayHeader(selectedDayEpoch_));
        }
        EnsureYearComboContains(visibleYear_);
        yearComboBox_->Enable(currentViewMode_ == CalendarViewMode::MONTH);
    }

    void RefreshMonthGrid() {
        const int firstOffset = MonthGridOffset(visibleYear_, visibleMonth_);
        const int daysCurrentMonth = DaysInMonth(visibleYear_, visibleMonth_);
        const int previousMonth = visibleMonth_ == 1 ? 12 : visibleMonth_ - 1;
        const int previousYear = visibleMonth_ == 1 ? visibleYear_ - 1 : visibleYear_;
        const int nextMonth = visibleMonth_ == 12 ? 1 : visibleMonth_ + 1;
        const int nextYear = visibleMonth_ == 12 ? visibleYear_ + 1 : visibleYear_;
        const int daysPreviousMonth = DaysInMonth(previousYear, previousMonth);
        const long long today = StartOfUtcDay(std::time(nullptr));
        std::array<MonthCellMeta, kMonthCellCount> cells{};
        std::array<std::vector<std::optional<MonthCellEventSegment>>, kMonthCellCount> cellRows{};

        for (int cellIndex = 0; cellIndex < kMonthCellCount; ++cellIndex) {
            int dayNumber = 0;
            bool inCurrentMonth = true;
            int cellMonth = visibleMonth_;
            int cellYear = visibleYear_;

            if (cellIndex < firstOffset) {
                dayNumber = daysPreviousMonth - firstOffset + cellIndex + 1;
                cellMonth = previousMonth;
                cellYear = previousYear;
                inCurrentMonth = false;
            }
            else if (cellIndex >= firstOffset + daysCurrentMonth) {
                dayNumber = cellIndex - (firstOffset + daysCurrentMonth) + 1;
                cellMonth = nextMonth;
                cellYear = nextYear;
                inCurrentMonth = false;
            }
            else {
                dayNumber = cellIndex - firstOffset + 1;
            }

            const long long dayEpoch = MakeUtcEpoch(cellYear, cellMonth, dayNumber);
            monthCellEpochs_[cellIndex] = dayEpoch;
            cells[cellIndex] = MonthCellMeta{dayEpoch, dayNumber, inCurrentMonth};
        }

        for (int weekIndex = 0; weekIndex < 6; ++weekIndex) {
            const int weekCellStart = weekIndex * 7;
            const long long weekStartEpoch = cells[weekCellStart].dayEpoch;
            const long long weekEndEpoch = cells[weekCellStart + 6].dayEpoch + kSecondsPerDay;

            std::vector<Event> spanningEvents;
            for (const auto& event : events_) {
                if (event.deletedAt != 0 || !SpansMultipleDays(event)) {
                    continue;
                }
                if (event.startDateTime < weekEndEpoch && event.endDateTime > weekStartEpoch) {
                    spanningEvents.push_back(event);
                }
            }

            std::sort(spanningEvents.begin(), spanningEvents.end(), [](const Event& lhs, const Event& rhs) {
                if (lhs.startDateTime != rhs.startDateTime) {
                    return lhs.startDateTime < rhs.startDateTime;
                }
                return lhs.endDateTime < rhs.endDateTime;
            });

            struct WeekSpan {
                long long eventId = -1;
                int row = 0;
                int startDayOffset = 0;
                int endDayOffset = 0;
                std::string label;
                bool continuesBefore = false;
                bool continuesAfter = false;
            };

            std::vector<std::vector<WeekSpan>> weekRows;
            for (const auto& event : spanningEvents) {
                const long long clippedStartDay = std::max(StartOfUtcDay(event.startDateTime), weekStartEpoch);
                const long long clippedEndDay = std::min(EventDisplayEndDay(event), weekEndEpoch - kSecondsPerDay);

                WeekSpan span;
                span.eventId = event.id;
                span.startDayOffset = static_cast<int>((clippedStartDay - weekStartEpoch) / kSecondsPerDay);
                span.endDayOffset = static_cast<int>((clippedEndDay - weekStartEpoch) / kSecondsPerDay);
                span.continuesBefore = event.startDateTime < weekStartEpoch;
                span.continuesAfter = event.endDateTime > weekEndEpoch;
                span.label = BuildMonthEventLabel(event);

                int assignedRow = 0;
                for (; assignedRow < static_cast<int>(weekRows.size()); ++assignedRow) {
                    bool overlapsExisting = false;
                    for (const auto& existing : weekRows[assignedRow]) {
                        if (!(span.endDayOffset < existing.startDayOffset || span.startDayOffset > existing.endDayOffset)) {
                            overlapsExisting = true;
                            break;
                        }
                    }
                    if (!overlapsExisting) {
                        break;
                    }
                }

                if (assignedRow == static_cast<int>(weekRows.size())) {
                    weekRows.emplace_back();
                }

                span.row = assignedRow;
                weekRows[assignedRow].push_back(span);

                for (int offset = span.startDayOffset; offset <= span.endDayOffset; ++offset) {
                    const int cellIndex = weekCellStart + offset;
                    if (static_cast<int>(cellRows[cellIndex].size()) <= span.row) {
                        cellRows[cellIndex].resize(span.row + 1);
                    }

                    MonthCellEventSegment segment;
                    segment.eventId = event.id;
                    segment.label = offset == span.startDayOffset ? span.label : "";
                    segment.continuesBefore = span.continuesBefore || offset > span.startDayOffset;
                    segment.continuesAfter = span.continuesAfter || offset < span.endDayOffset;
                    cellRows[cellIndex][span.row] = segment;
                }
            }

            for (int offset = 0; offset < 7; ++offset) {
                cellRows[weekCellStart + offset].resize(weekRows.size());
            }

            for (int offset = 0; offset < 7; ++offset) {
                const int cellIndex = weekCellStart + offset;
                auto dayEvents = EventsForDay(cells[cellIndex].dayEpoch);
                for (const auto& event : dayEvents) {
                    if (SpansMultipleDays(event)) {
                        continue;
                    }
                    MonthCellEventSegment segment;
                    segment.eventId = event.id;
                    segment.label = BuildMonthEventLabel(event);
                    cellRows[cellIndex].push_back(segment);
                }
            }
        }

        for (int cellIndex = 0; cellIndex < kMonthCellCount; ++cellIndex) {
            monthCells_[cellIndex]->UpdateCell(
                cells[cellIndex].dayEpoch,
                cells[cellIndex].dayNumber,
                cells[cellIndex].inCurrentMonth,
                IsSameUtcDay(cells[cellIndex].dayEpoch, today),
                IsSameUtcDay(cells[cellIndex].dayEpoch, selectedDayEpoch_),
                cellRows[cellIndex]);
        }
    }

    void RefreshTimelineViews() {
        const long long weekStart = StartOfUtcWeek(selectedDayEpoch_);
        weekTimeline_->SetSelectedDay(selectedDayEpoch_);
        weekTimeline_->SetRangeStart(weekStart);
        weekTimeline_->SetEvents(events_);

        dayTimeline_->SetSelectedDay(selectedDayEpoch_);
        dayTimeline_->SetRangeStart(selectedDayEpoch_);
        dayTimeline_->SetEvents(events_);
    }

    void SyncVisibleMonthToSelectedDay() {
        if (selectedDayEpoch_ < kMinCalendarEpoch) {
            selectedDayEpoch_ = kMinCalendarEpoch;
        }
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = std::max(kMinCalendarYear, tm.tm_year + 1900);
        visibleMonth_ = tm.tm_mon + 1;
    }

    void FocusDay(const long long dayEpoch, const bool refreshView = true) {
        selectedDayEpoch_ = std::max(kMinCalendarEpoch, dayEpoch);
        SyncVisibleMonthToSelectedDay();

        if (refreshView) {
            Freeze();
            RefreshEvents();
            Thaw();
        }
    }

    void ShiftVisibleMonth(const int delta) {
        visibleMonth_ += delta;
        while (visibleMonth_ < 1) {
            visibleMonth_ += 12;
            --visibleYear_;
        }
        while (visibleMonth_ > 12) {
            visibleMonth_ -= 12;
            ++visibleYear_;
        }
        if (visibleYear_ < kMinCalendarYear) {
            visibleYear_ = kMinCalendarYear;
            visibleMonth_ = 1;
        }
    }

    void PopulateYearComboWindow(const int targetYear) {
        const int clampedYear = std::max(kMinCalendarYear, targetYear);
        yearComboWindowStart_ = std::max(kMinCalendarYear, clampedYear - kYearComboBackwardPadding);
        yearComboWindowEnd_ = yearComboWindowStart_ + kYearComboChunkSize - 1;

        yearComboBox_->Freeze();
        yearComboBox_->Clear();
        for (int year = yearComboWindowStart_; year <= yearComboWindowEnd_; ++year) {
            yearComboBox_->Append(std::to_string(year));
        }
        yearComboBox_->SetValue(std::to_string(clampedYear));
        yearComboBox_->Thaw();
    }

    void EnsureYearComboContains(const int year) {
        const int clampedYear = std::max(kMinCalendarYear, year);
        const bool outsideWindow = clampedYear < yearComboWindowStart_ || clampedYear > yearComboWindowEnd_;
        const bool nearTop = clampedYear - yearComboWindowStart_ < 5 && yearComboWindowStart_ > kMinCalendarYear;
        const bool nearBottom = yearComboWindowEnd_ - clampedYear < 5;

        if (outsideWindow || nearTop || nearBottom) {
            PopulateYearComboWindow(clampedYear);
            return;
        }

        yearComboBox_->SetValue(std::to_string(clampedYear));
    }

    void SwitchView(const CalendarViewMode mode, const int pageIndex) {
        currentViewMode_ = mode;
        calendarBook_->SetSelection(pageIndex);
        RefreshEvents();
    }

    void ShiftCurrentPeriod(const int direction) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            ShiftVisibleMonth(direction);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            FocusDay(selectedDayEpoch_ + direction * 7LL * kSecondsPerDay, true);
            return;
        }
        else {
            FocusDay(selectedDayEpoch_ + direction * 1LL * kSecondsPerDay, true);
            return;
        }

        RefreshEvents();
    }

    bool PersistEvent(const Event& event) {
        try {
            eventRepository_.upsert(event);
            RefreshEvents();
            statusLabel_->SetLabel("Event saved");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Save failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    bool DeleteEventById(const long long eventId) {
        try {
            eventRepository_.deleteEvent(eventId);
            RefreshEvents();
            statusLabel_->SetLabel("Event deleted");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Delete failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    void OpenEventDialog(const std::optional<Event>& event = std::nullopt,
                         const std::optional<EventDraftDefaults>& defaults = std::nullopt) {
        EventEditorDialog dialog(this, event, selectedDayEpoch_, defaults);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (dialog.IsDeleteRequested()) {
            if (event.has_value()) {
                DeleteEventById(event->id);
            }
            return;
        }

        const auto builtEvent = dialog.BuildEvent(localCalendarId_);
        if (!builtEvent.has_value()) {
            return;
        }

        PersistEvent(*builtEvent);
    }

    void OpenEventById(const long long eventId) {
        auto selectedEvent = eventRepository_.getById(eventId);
        if (!selectedEvent.has_value()) {
            return;
        }

        FocusDay(StartOfUtcDay(selectedEvent->startDateTime), true);
        statusLabel_->SetLabel(wxString::Format("Editing event #%lld", selectedEvent->id));
        OpenEventDialog(*selectedEvent);
    }

    void OpenNewEventDialog(const long long dayEpoch, const EventDraftDefaults& defaults) {
        FocusDay(dayEpoch, true);
        statusLabel_->SetLabel("Creating new event");
        OpenEventDialog(std::nullopt, defaults);
    }

    void OpenNewAllDayEventDialog(const long long dayEpoch) {
        EventDraftDefaults defaults;
        defaults.startDateTime = dayEpoch;
        defaults.endDateTime = dayEpoch + kSecondsPerDay;
        defaults.allDay = true;
        OpenNewEventDialog(dayEpoch, defaults);
    }

    void OpenNewTimedEventDialog(const long long dayEpoch, const int minuteOfDay) {
        EventDraftDefaults defaults;
        defaults.startDateTime = dayEpoch + static_cast<long long>(minuteOfDay) * 60;
        defaults.endDateTime = defaults.startDateTime + 3600;
        defaults.allDay = false;
        OpenNewEventDialog(dayEpoch, defaults);
    }

    void HandleMonthCellClicked(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        currentViewMode_ = CalendarViewMode::DAY;
        calendarBook_->SetSelection(2);
        FocusDay(monthCellEpochs_[index], true);
    }

    void HandleMonthCellCreateEvent(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        OpenNewAllDayEventDialog(monthCellEpochs_[index]);
    }

    void OnToday(wxCommandEvent&) {
        FocusDay(StartOfUtcDay(std::time(nullptr)), true);
    }

    void OnNew(wxCommandEvent&) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            OpenNewAllDayEventDialog(selectedDayEpoch_);
            return;
        }

        const long long now = static_cast<long long>(std::time(nullptr));
        const long long baseDay = currentViewMode_ == CalendarViewMode::DAY
            ? selectedDayEpoch_
            : std::max(selectedDayEpoch_, StartOfUtcDay(now));
        const std::tm nowTm = EpochToUtcTm(std::max(now, kMinCalendarEpoch));
        const int minuteOfDay = nowTm.tm_hour * 60;
        OpenNewTimedEventDialog(baseDay, minuteOfDay);
    }

    void OnRefresh(wxCommandEvent&) {
        RefreshEvents();
    }

    void OnYearChanged(wxCommandEvent& event) {
        long selectedYear = visibleYear_;
        if (!event.GetString().ToLong(&selectedYear)) {
            return;
        }
        visibleYear_ = std::max(kMinCalendarYear, static_cast<int>(selectedYear));
        EnsureYearComboContains(visibleYear_);
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            RefreshViewState();
        }
    }

    SQLite::Database db_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
    long long localCalendarId_ = 0;
    long long selectedDayEpoch_ = 0;
    int visibleYear_ = 1970;
    int visibleMonth_ = 1;
    CalendarViewMode currentViewMode_ = CalendarViewMode::MONTH;
    std::vector<Event> events_;

    wxButton* monthViewButton_ = nullptr;
    wxButton* weekViewButton_ = nullptr;
    wxButton* dayViewButton_ = nullptr;
    wxButton* todayButton_ = nullptr;
    wxButton* previousButton_ = nullptr;
    wxButton* nextButton_ = nullptr;
    wxStaticText* monthTitleLabel_ = nullptr;
    wxComboBox* yearComboBox_ = nullptr;
    int yearComboWindowStart_ = kMinCalendarYear;
    int yearComboWindowEnd_ = kMinCalendarYear + kYearComboChunkSize - 1;
    wxSimplebook* calendarBook_ = nullptr;
    TimelineViewPanel* weekTimeline_ = nullptr;
    TimelineViewPanel* dayTimeline_ = nullptr;
    std::array<MonthCellPanel*, kMonthCellCount> monthCells_{};
    std::array<long long, kMonthCellCount> monthCellEpochs_{};

    wxButton* newButton_ = nullptr;
    wxButton* refreshButton_ = nullptr;
    wxStaticText* statusLabel_ = nullptr;
};

std::string g_dbPath;

class LocalCalendarApp final : public wxApp {
public:
    bool OnInit() override {
        auto* frame = new LocalCalendarFrame(g_dbPath);
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP_NO_MAIN(LocalCalendarApp);

} // namespace

int RunLocalCalendarUi(const std::string& dbPath) {
    g_dbPath = dbPath;

    int argc = 0;
    char** argv = nullptr;

    if (!wxEntryStart(argc, argv)) {
        return 1;
    }

    wxTheApp->CallOnInit();
    const int exitCode = wxTheApp->OnRun();
    wxTheApp->OnExit();
    wxEntryCleanup();

    return exitCode;
}
