#include "ui/local_calendar_app.h"

#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/frame.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wx.h>

#include "models/calendar.h"
#include "models/event.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "utils/datetime_utils.h"

namespace {

constexpr int kMonthCellCount = 42;

std::string FormatMonthTitle(const int year, const int month) {
    static const std::array<const char*, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    return std::string(months[month - 1]) + " " + std::to_string(year);
}

std::string FormatDayHeader(const long long dayEpoch) {
    const std::tm tm = EpochToUtcTm(dayEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%A, %d %B %Y");
    return output.str();
}

int DaysInMonth(const int year, const int month) {
    static const std::array<int, 12> daysPerMonth = {31,28,31,30,31,30,31,31,30,31,30,31};
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

std::string MakeLocalProviderEventId(const long long startDateTime) {
    const auto now = static_cast<long long>(std::time(nullptr));
    return "local-" + std::to_string(now) + "-" + std::to_string(startDateTime);
}

std::string BuildMonthCellLabel(const int dayNumber,
                                const bool inCurrentMonth,
                                const bool isToday,
                                const std::vector<Event>& dayEvents) {
    std::ostringstream label;
    if (isToday) {
        label << "[Today] ";
    }

    if (!inCurrentMonth) {
        label << "(" << dayNumber << ")";
    }
    else {
        label << dayNumber;
    }

    const size_t previewCount = std::min<size_t>(2, dayEvents.size());
    for (size_t index = 0; index < previewCount; ++index) {
        label << "\n";
        if (!dayEvents[index].allDay) {
            label << FormatUtcDateTimeInput(dayEvents[index].startDateTime, false).substr(11, 5) << " ";
        }
        label << dayEvents[index].title.substr(0, 14);
    }

    if (dayEvents.size() > previewCount) {
        label << "\n+" << (dayEvents.size() - previewCount) << " more";
    }

    return label.str();
}

class LocalCalendarFrame final : public wxFrame {
public:
    explicit LocalCalendarFrame(const std::string& dbPath)
        : wxFrame(nullptr, wxID_ANY, "Calendar", wxDefaultPosition, wxSize(1280, 760)),
          db_(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
          eventRepository_(db_),
          calendarRepository_(db_) {
        localCalendarId_ = EnsureLocalCalendar();
        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        const std::tm today = EpochToUtcTm(now);
        visibleYear_ = today.tm_year + 1900;
        visibleMonth_ = today.tm_mon + 1;

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
        auto* appSubtitle = new wxStaticText(panel, wxID_ANY, "Monthly grid and daily agenda backed by SQLite");
        titleBlock->Add(appTitle, 0, wxBOTTOM, 2);
        titleBlock->Add(appSubtitle, 0);

        monthViewButton_ = new wxButton(panel, wxID_ANY, "Month");
        dayViewButton_ = new wxButton(panel, wxID_ANY, "Day");
        todayButton_ = new wxButton(panel, wxID_ANY, "Today");
        previousButton_ = new wxButton(panel, wxID_ANY, "<");
        nextButton_ = new wxButton(panel, wxID_ANY, ">");
        monthTitleLabel_ = new wxStaticText(panel, wxID_ANY, "");

        toolbarSizer->Add(titleBlock, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 24);
        toolbarSizer->Add(monthViewButton_, 0, wxRIGHT, 6);
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
            auto* label = new wxStaticText(monthPage, wxID_ANY, weekday);
            weekdaySizer->Add(label, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
        }
        monthSizer->Add(weekdaySizer, 0, wxEXPAND | wxALL, 10);

        auto* gridSizer = new wxGridSizer(6, 7, 8, 8);
        for (int index = 0; index < kMonthCellCount; ++index) {
            monthCells_[index] = new wxButton(monthPage, wxID_ANY, "", wxDefaultPosition, wxSize(120, 92));
            gridSizer->Add(monthCells_[index], 1, wxEXPAND);
        }
        monthSizer->Add(gridSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        monthPage->SetSizer(monthSizer);

        auto* dayPage = new wxPanel(calendarBook_);
        auto* daySizer = new wxBoxSizer(wxVERTICAL);
        selectedDayLabel_ = new wxStaticText(dayPage, wxID_ANY, "");
        auto* dayHint = new wxStaticText(dayPage, wxID_ANY, "Select an event to edit it or create a new one for this day.");
        dayEventList_ = new wxListBox(dayPage, wxID_ANY);
        daySizer->Add(selectedDayLabel_, 0, wxALL, 10);
        daySizer->Add(dayHint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        daySizer->Add(dayEventList_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        dayPage->SetSizer(daySizer);

        calendarBook_->AddPage(monthPage, "Month");
        calendarBook_->AddPage(dayPage, "Day");

        leftPane->Add(calendarBook_, 1, wxEXPAND);

        auto* editorPane = new wxBoxSizer(wxVERTICAL);
        auto* editorTitle = new wxStaticText(panel, wxID_ANY, "Event Details");
        editorPane->Add(editorTitle, 0, wxALL, 10);

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

        contentSizer->Add(leftPane, 3, wxEXPAND | wxALL, 12);
        contentSizer->Add(editorPane, 2, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(toolbarSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        rootSizer->Add(contentSizer, 1, wxEXPAND);

        panel->SetSizer(rootSizer);
    }

    void ApplyLook() {
        SetBackgroundColour(wxColour(246, 248, 252));
        if (GetParent() == nullptr) {
            SetMinSize(wxSize(1100, 680));
        }

        const wxColour panelBg(246, 248, 252);
        const wxColour surfaceBg(255, 255, 255);
        const wxColour accent(26, 115, 232);

        GetChildren()[0]->SetBackgroundColour(panelBg);
        monthViewButton_->SetBackgroundColour(surfaceBg);
        dayViewButton_->SetBackgroundColour(surfaceBg);
        todayButton_->SetBackgroundColour(surfaceBg);
        previousButton_->SetBackgroundColour(surfaceBg);
        nextButton_->SetBackgroundColour(surfaceBg);
        saveButton_->SetBackgroundColour(accent);
        saveButton_->SetForegroundColour(*wxWHITE);

        for (auto* button : monthCells_) {
            button->SetBackgroundColour(surfaceBg);
            button->SetForegroundColour(wxColour(40, 40, 40));
        }
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
        monthViewButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnShowMonthView, this);
        dayViewButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnShowDayView, this);
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnPreviousPeriod, this);
        nextButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNextPeriod, this);
        newButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNew, this);
        saveButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnSave, this);
        deleteButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnDelete, this);
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        allDayCtrl_->Bind(wxEVT_CHECKBOX, &LocalCalendarFrame::OnAllDayChanged, this);
        dayEventList_->Bind(wxEVT_LISTBOX, &LocalCalendarFrame::OnDayEventSelected, this);

        for (int index = 0; index < kMonthCellCount; ++index) {
            monthCells_[index]->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnMonthCellClicked, this);
        }
    }

    std::vector<Event> EventsForDay(const long long dayEpoch) const {
        std::vector<Event> results;
        const long long dayEnd = dayEpoch + 86400;

        for (const auto& event : events_) {
            if (event.deletedAt != 0) {
                continue;
            }

            if (event.startDateTime < dayEnd && event.endDateTime >= dayEpoch) {
                results.push_back(event);
            }
        }

        return results;
    }

    void RefreshEvents() {
        events_ = eventRepository_.getByCalendar(localCalendarId_);
        RefreshMonthGrid();
        RefreshDayAgenda();
        statusLabel_->SetLabel(wxString::Format("Loaded %zu event(s)", events_.size()));
    }

    void RefreshMonthGrid() {
        monthTitleLabel_->SetLabel(FormatMonthTitle(visibleYear_, visibleMonth_));

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
            const auto dayEvents = EventsForDay(dayEpoch);
            const bool isToday = IsSameUtcDay(dayEpoch, today);

            monthCells_[cellIndex]->SetLabel(BuildMonthCellLabel(dayNumber, inCurrentMonth, isToday, dayEvents));
            monthCells_[cellIndex]->SetOwnForegroundColour(inCurrentMonth ? wxColour(34, 34, 34) : wxColour(140, 140, 140));

            if (IsSameUtcDay(dayEpoch, selectedDayEpoch_)) {
                monthCells_[cellIndex]->SetBackgroundColour(wxColour(221, 235, 255));
            }
            else {
                monthCells_[cellIndex]->SetBackgroundColour(*wxWHITE);
            }
        }
    }

    void RefreshDayAgenda() {
        selectedDayLabel_->SetLabel(FormatDayHeader(selectedDayEpoch_));
        dayEventList_->Clear();
        agendaEventIds_.clear();

        const auto dayEvents = EventsForDay(selectedDayEpoch_);
        for (const auto& event : dayEvents) {
            std::string row;
            if (event.allDay) {
                row = "All day  ";
            }
            else {
                row = FormatUtcDateTimeInput(event.startDateTime, false).substr(11, 5) + "  ";
            }
            row += event.title;
            dayEventList_->Append(row);
            agendaEventIds_.push_back(event.id);
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
        selectedDayEpoch_ = StartOfUtcDay(event.startDateTime);
        statusLabel_->SetLabel(wxString::Format("Editing event #%lld", event.id));
        RefreshMonthGrid();
        RefreshDayAgenda();
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
        if (event.title.empty()) {
            wxMessageBox("Title is required.", "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        if (event.startDateTime < 0 || event.endDateTime < 0) {
            wxMessageBox("Use YYYY-MM-DD for all-day events or YYYY-MM-DD HH:MM for timed events.",
                "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        if (event.endDateTime < event.startDateTime) {
            wxMessageBox("End must be after start.", "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        return true;
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
        RefreshMonthGrid();
    }

    void OnShowMonthView(wxCommandEvent&) {
        calendarBook_->SetSelection(0);
    }

    void OnShowDayView(wxCommandEvent&) {
        calendarBook_->SetSelection(1);
    }

    void OnToday(wxCommandEvent&) {
        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        const std::tm tm = EpochToUtcTm(now);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        RefreshMonthGrid();
        RefreshDayAgenda();
        ClearForm();
    }

    void OnPreviousPeriod(wxCommandEvent&) {
        if (calendarBook_->GetSelection() == 0) {
            ShiftVisibleMonth(-1);
        }
        else {
            selectedDayEpoch_ -= 86400;
            RefreshMonthGrid();
            RefreshDayAgenda();
        }
    }

    void OnNextPeriod(wxCommandEvent&) {
        if (calendarBook_->GetSelection() == 0) {
            ShiftVisibleMonth(1);
        }
        else {
            selectedDayEpoch_ += 86400;
            RefreshMonthGrid();
            RefreshDayAgenda();
        }
    }

    void OnMonthCellClicked(wxCommandEvent& event) {
        for (int index = 0; index < kMonthCellCount; ++index) {
            if (monthCells_[index] == event.GetEventObject()) {
                selectedDayEpoch_ = monthCellEpochs_[index];
                const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
                visibleYear_ = tm.tm_year + 1900;
                visibleMonth_ = tm.tm_mon + 1;
                RefreshMonthGrid();
                RefreshDayAgenda();
                calendarBook_->SetSelection(1);
                ClearForm();
                break;
            }
        }
    }

    void OnDayEventSelected(wxCommandEvent& event) {
        const int selection = event.GetSelection();
        if (selection == wxNOT_FOUND || static_cast<size_t>(selection) >= agendaEventIds_.size()) {
            return;
        }

        auto selectedEvent = eventRepository_.getById(agendaEventIds_[selection]);
        if (selectedEvent) {
            LoadEventIntoForm(*selectedEvent);
        }
    }

    void OnNew(wxCommandEvent&) {
        ClearForm();
        dayEventList_->SetSelection(wxNOT_FOUND);
    }

    void OnSave(wxCommandEvent&) {
        Event event = BuildEventFromForm();
        if (!ValidateEvent(event)) {
            return;
        }

        try {
            eventRepository_.upsert(event);
            selectedDayEpoch_ = StartOfUtcDay(event.startDateTime);
            const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
            visibleYear_ = tm.tm_year + 1900;
            visibleMonth_ = tm.tm_mon + 1;
            RefreshEvents();
            statusLabel_->SetLabel("Event saved");
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Save failed: %s", ex.what()),
                "Database error", wxOK | wxICON_ERROR, this);
        }
    }

    void OnDelete(wxCommandEvent&) {
        if (selectedEventId_ <= 0) {
            wxMessageBox("Select an event first.", "Delete", wxOK | wxICON_INFORMATION, this);
            return;
        }

        try {
            eventRepository_.deleteEvent(selectedEventId_);
            RefreshEvents();
            ClearForm();
            statusLabel_->SetLabel("Event deleted");
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Delete failed: %s", ex.what()),
                "Database error", wxOK | wxICON_ERROR, this);
        }
    }

    void OnRefresh(wxCommandEvent&) {
        RefreshEvents();
    }

    void OnAllDayChanged(wxCommandEvent&) {
        const bool allDay = allDayCtrl_->GetValue();
        const long long startEpoch = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), false);
        const long long endEpoch = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), false);

        if (startEpoch >= 0) {
            startCtrl_->SetValue(FormatUtcDateTimeInput(startEpoch, allDay));
        }
        if (endEpoch >= 0) {
            endCtrl_->SetValue(FormatUtcDateTimeInput(endEpoch, allDay));
        }
    }

    SQLite::Database db_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
    long long localCalendarId_ = 0;
    long long selectedDayEpoch_ = 0;
    long long selectedEventId_ = -1;
    int visibleYear_ = 1970;
    int visibleMonth_ = 1;
    std::vector<Event> events_;
    std::vector<long long> agendaEventIds_;

    wxButton* monthViewButton_ = nullptr;
    wxButton* dayViewButton_ = nullptr;
    wxButton* todayButton_ = nullptr;
    wxButton* previousButton_ = nullptr;
    wxButton* nextButton_ = nullptr;
    wxStaticText* monthTitleLabel_ = nullptr;
    wxSimplebook* calendarBook_ = nullptr;
    wxStaticText* selectedDayLabel_ = nullptr;
    wxListBox* dayEventList_ = nullptr;
    std::array<wxButton*, kMonthCellCount> monthCells_{};
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
