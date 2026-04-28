#include "ui/local_calendar_app.h"

#include <array>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
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
        ClearForm();
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

        toolbarSizer->Add(titleBlock, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 24);
        toolbarSizer->Add(monthViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(weekViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(dayViewButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(todayButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(previousButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(nextButton_, 0, wxRIGHT, 18);
        toolbarSizer->Add(monthTitleLabel_, 0, wxALIGN_CENTER_VERTICAL);

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

        auto* editorPane = new wxBoxSizer(wxVERTICAL);
        editorPane->Add(new wxStaticText(panel, wxID_ANY, "Event Details"), 0, wxALL, 10);

        titleCtrl_ = AddLabeledTextField(panel, editorPane, "Title");
        locationCtrl_ = AddLabeledTextField(panel, editorPane, "Location");
        allDayCtrl_ = new wxCheckBox(panel, wxID_ANY, "All day");
        editorPane->Add(allDayCtrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        startCtrl_ = AddLabeledTextField(panel, editorPane, "Start (UTC)");
        endCtrl_ = AddLabeledTextField(panel, editorPane, "End (UTC)");
        descriptionCtrl_ = AddLabeledTextField(panel, editorPane, "Description", wxTE_MULTILINE, 140);

        auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
        newButton_ = new wxButton(panel, wxID_ANY, "New Event");
        saveButton_ = new wxButton(panel, wxID_ANY, "Save");
        deleteButton_ = new wxButton(panel, wxID_ANY, "Delete");
        refreshButton_ = new wxButton(panel, wxID_ANY, "Refresh");
        buttonSizer->Add(newButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(saveButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(deleteButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(refreshButton_, 0);
        editorPane->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        statusLabel_ = new wxStaticText(panel, wxID_ANY, "Ready");
        editorPane->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        contentSizer->Add(leftPane, 5, wxEXPAND | wxALL, 12);
        contentSizer->Add(editorPane, 2, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

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
        const wxColour accent(26, 115, 232);

        monthViewButton_->SetBackgroundColour(surfaceBg);
        weekViewButton_->SetBackgroundColour(surfaceBg);
        dayViewButton_->SetBackgroundColour(surfaceBg);
        todayButton_->SetBackgroundColour(surfaceBg);
        previousButton_->SetBackgroundColour(surfaceBg);
        nextButton_->SetBackgroundColour(surfaceBg);
        saveButton_->SetBackgroundColour(accent);
        saveButton_->SetForegroundColour(*wxWHITE);
    }

    wxTextCtrl* AddLabeledTextField(wxWindow* parent,
                                    wxBoxSizer* sizer,
                                    const wxString& label,
                                    const long style = 0,
                                    const int minHeight = -1) {
        sizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
        auto* control = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(-1, minHeight), style);
        sizer->Add(control, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return control;
    }

    void BindEvents() {
        monthViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::MONTH, 0); });
        weekViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::WEEK, 1); });
        dayViewButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SwitchView(CalendarViewMode::DAY, 2); });
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(-1); });
        nextButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(1); });
        newButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNew, this);
        saveButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnSave, this);
        deleteButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnDelete, this);
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        allDayCtrl_->Bind(wxEVT_CHECKBOX, &LocalCalendarFrame::OnAllDayChanged, this);
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

    void RefreshEvents() {
        events_ = eventRepository_.getByCalendar(localCalendarId_);
        RefreshViewState();
        statusLabel_->SetLabel(wxString::Format("Loaded %zu event(s)", events_.size()));
    }

    void RefreshViewState() {
        RefreshHeaderTitle();
        RefreshMonthGrid();
        RefreshTimelineViews();
    }

    void RefreshHeaderTitle() {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            monthTitleLabel_->SetLabel(FormatMonthTitle(visibleYear_, visibleMonth_));
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            monthTitleLabel_->SetLabel(FormatWeekTitle(StartOfUtcWeek(selectedDayEpoch_)));
        }
        else {
            monthTitleLabel_->SetLabel(FormatDayHeader(selectedDayEpoch_));
        }
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
            monthCells_[cellIndex]->UpdateCell(
                dayEpoch,
                dayNumber,
                inCurrentMonth,
                IsSameUtcDay(dayEpoch, today),
                IsSameUtcDay(dayEpoch, selectedDayEpoch_),
                EventsForDay(dayEpoch));
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
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
    }

    void FocusDay(const long long dayEpoch, const bool refreshView = true, const bool clearForm = false) {
        selectedDayEpoch_ = dayEpoch;
        SyncVisibleMonthToSelectedDay();
        if (refreshView) {
            RefreshViewState();
        }
        if (clearForm) {
            ClearForm();
        }
    }

    void ClearForm() {
        selectedEventId_ = -1;
        titleCtrl_->Clear();
        locationCtrl_->Clear();
        descriptionCtrl_->Clear();
        allDayCtrl_->SetValue(false);
        startCtrl_->SetValue(FormatUtcDateTimeInput(selectedDayEpoch_ + 9 * 3600, false));
        endCtrl_->SetValue(FormatUtcDateTimeInput(selectedDayEpoch_ + 10 * 3600, false));
        statusLabel_->SetLabel("Creating new event");
    }

    void LoadEventIntoForm(const Event& event) {
        selectedEventId_ = event.id;
        titleCtrl_->SetValue(event.title);
        locationCtrl_->SetValue(event.location);
        descriptionCtrl_->SetValue(event.description);
        allDayCtrl_->SetValue(event.allDay);
        startCtrl_->SetValue(FormatUtcDateTimeInput(event.startDateTime, event.allDay));
        endCtrl_->SetValue(FormatUtcDateTimeInput(event.endDateTime, event.allDay));
        FocusDay(StartOfUtcDay(event.startDateTime), true, false);
        statusLabel_->SetLabel(wxString::Format("Editing event #%lld", event.id));
    }

    Event BuildEventFromForm() {
        Event event{};
        event.id = selectedEventId_;
        event.calendarId = localCalendarId_;
        event.title = titleCtrl_->GetValue().ToStdString();
        event.location = locationCtrl_->GetValue().ToStdString();
        event.description = descriptionCtrl_->GetValue().ToStdString();
        event.allDay = allDayCtrl_->GetValue();
        event.status = "confirmed";
        event.type = EventType::SINGLE;
        event.deletedAt = 0;
        event.syncStatus = SYNCED;
        event.lastModified = std::time(nullptr);
        event.updatedAt = event.lastModified;
        event.createdAt = event.lastModified;
        event.startDateTime = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), event.allDay);
        event.endDateTime = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), event.allDay);
        NormalizeAllDayEventRange(event);

        if (selectedEventId_ > 0) {
            auto existing = eventRepository_.getById(selectedEventId_);
            if (existing) {
                event.createdAt = existing->createdAt;
                event.providerEventId = existing->providerEventId;
                event.providerMasterId = existing->providerMasterId;
                event.instanceStart = existing->instanceStart;
                event.recurrenceRule = existing->recurrenceRule;
            }
        }
        else {
            event.instanceStart = event.startDateTime;
            event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
        }

        return event;
    }

    bool ValidateEvent(const Event& event) {
        const auto validationMessage = ValidateEventForUi(event);
        if (!validationMessage.has_value()) {
            return true;
        }

        wxMessageBox(*validationMessage, "Validation", wxOK | wxICON_WARNING, this);
        return false;
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
    }

    void SwitchView(const CalendarViewMode mode, const int pageIndex) {
        currentViewMode_ = mode;
        calendarBook_->SetSelection(pageIndex);
        RefreshHeaderTitle();
    }

    void ShiftCurrentPeriod(const int direction) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            ShiftVisibleMonth(direction);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            FocusDay(selectedDayEpoch_ + direction * 7LL * kSecondsPerDay, true, true);
            return;
        }
        else {
            FocusDay(selectedDayEpoch_ + direction * 1LL * kSecondsPerDay, true, true);
            return;
        }

        RefreshViewState();
        ClearForm();
    }

    bool PersistEvent(const Event& event) {
        try {
            eventRepository_.upsert(event);
            RefreshEvents();
            LoadEventIntoForm(eventRepository_.getByProviderInstance(event.providerEventId, event.instanceStart).value_or(event));
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
            ClearForm();
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

        LoadEventIntoForm(*selectedEvent);
        OpenEventDialog(*selectedEvent);
    }

    void OpenNewEventDialog(const long long dayEpoch, const EventDraftDefaults& defaults) {
        FocusDay(dayEpoch, true, true);
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
        FocusDay(monthCellEpochs_[index], true, true);
    }

    void HandleMonthCellCreateEvent(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        OpenNewAllDayEventDialog(monthCellEpochs_[index]);
    }

    void OnToday(wxCommandEvent&) {
        FocusDay(StartOfUtcDay(std::time(nullptr)), true, true);
    }

    void OnNew(wxCommandEvent&) {
        ClearForm();
        OpenEventDialog();
    }

    void OnSave(wxCommandEvent&) {
        Event event = BuildEventFromForm();
        if (!ValidateEvent(event)) {
            return;
        }

        PersistEvent(event);
    }

    void OnDelete(wxCommandEvent&) {
        if (selectedEventId_ <= 0) {
            wxMessageBox("Select an event first.", "Delete", wxOK | wxICON_INFORMATION, this);
            return;
        }

        DeleteEventById(selectedEventId_);
    }

    void OnRefresh(wxCommandEvent&) {
        RefreshEvents();
    }

    void OnAllDayChanged(wxCommandEvent&) {
        ApplyAllDayToggleToInputs(startCtrl_, endCtrl_, allDayCtrl_->GetValue());
    }

    SQLite::Database db_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
    long long localCalendarId_ = 0;
    long long selectedDayEpoch_ = 0;
    long long selectedEventId_ = -1;
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
    wxSimplebook* calendarBook_ = nullptr;
    TimelineViewPanel* weekTimeline_ = nullptr;
    TimelineViewPanel* dayTimeline_ = nullptr;
    std::array<MonthCellPanel*, kMonthCellCount> monthCells_{};
    std::array<long long, kMonthCellCount> monthCellEpochs_{};

    wxTextCtrl* titleCtrl_ = nullptr;
    wxTextCtrl* locationCtrl_ = nullptr;
    wxTextCtrl* startCtrl_ = nullptr;
    wxTextCtrl* endCtrl_ = nullptr;
    wxTextCtrl* descriptionCtrl_ = nullptr;
    wxCheckBox* allDayCtrl_ = nullptr;
    wxButton* newButton_ = nullptr;
    wxButton* saveButton_ = nullptr;
    wxButton* deleteButton_ = nullptr;
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
