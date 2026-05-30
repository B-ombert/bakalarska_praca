#include "ui/event_editor_dialog.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/arrstr.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "models/rrule.h"
#include "utils/datetime_utils.h"
#include "utils/timezone_utils.h"

class TimePickerCtrl final : public wxPanel {
public:
    explicit TimePickerCtrl(wxWindow* parent)
        : wxPanel(parent, wxID_ANY) {
        auto* sizer = new wxBoxSizer(wxHORIZONTAL);
        textCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(82, -1));
        dropdownButton_ = new wxButton(this, wxID_ANY, "v", wxDefaultPosition, wxSize(34, -1));
        sizer->Add(textCtrl_, 1, wxEXPAND);
        sizer->Add(dropdownButton_, 0, wxEXPAND);
        SetSizer(sizer);

        dropdownButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShowPopup(); });
    }

    wxString GetValue() const {
        return textCtrl_ != nullptr ? textCtrl_->GetValue() : wxString{};
    }

    void SetValue(const wxString& value) {
        if (textCtrl_ != nullptr) {
            textCtrl_->SetValue(value);
        }
    }

    void PopulateChoices(const wxArrayString& choices) {
        if (choicesPopulated_) {
            return;
        }
        choices_ = choices;
        choicesPopulated_ = true;
    }

    bool Enable(bool enable = true) override {
        if (textCtrl_ != nullptr) {
            textCtrl_->Enable(enable);
        }
        if (dropdownButton_ != nullptr) {
            dropdownButton_->Enable(enable);
        }
        return wxPanel::Enable(enable);
    }

    void SetEditable(const bool editable) {
        if (textCtrl_ != nullptr) {
            textCtrl_->SetEditable(editable);
        }
        if (dropdownButton_ != nullptr) {
            dropdownButton_->Enable(editable);
        }
    }

private:
    void EnsurePopupCreated() {
        if (popup_ != nullptr) {
            return;
        }

        popup_ = new wxPopupWindow(this, wxBORDER_SIMPLE);
        listBox_ = new wxListBox(popup_, wxID_ANY, wxDefaultPosition, wxSize(120, 220), choices_);
        auto* popupSizer = new wxBoxSizer(wxVERTICAL);
        popupSizer->Add(listBox_, 1, wxEXPAND);
        popup_->SetSizerAndFit(popupSizer);

        listBox_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { AcceptSelection(); });
        listBox_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { AcceptSelection(); });
        listBox_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const int hit = listBox_->HitTest(event.GetPosition());
            if (hit == wxNOT_FOUND) {
                event.Skip();
                return;
            }

            listBox_->SetSelection(hit);
            AcceptSelection();
        });
    }

    void ShowPopup() {
        EnsurePopupCreated();
        if (popup_ == nullptr || listBox_ == nullptr) {
            return;
        }

        const wxString currentValue = GetValue();
        const int selected = listBox_->FindString(currentValue);
        if (selected != wxNOT_FOUND) {
            listBox_->SetSelection(selected);
        }

        popup_->SetSize(wxSize(std::max(GetSize().GetWidth(), 120), 220));
        popup_->Position(ClientToScreen(wxPoint(0, GetSize().GetHeight())), wxSize(0, 0));
        popup_->Show(true);
        popup_->Raise();
        listBox_->SetFocus();
    }

    void AcceptSelection() {
        if (listBox_ == nullptr) {
            return;
        }
        const int selection = listBox_->GetSelection();
        if (selection != wxNOT_FOUND) {
            SetValue(listBox_->GetString(selection));
        }
        if (popup_ != nullptr) {
            popup_->Show(false);
        }
    }

    wxTextCtrl* textCtrl_ = nullptr;
    wxButton* dropdownButton_ = nullptr;
    wxPopupWindow* popup_ = nullptr;
    wxListBox* listBox_ = nullptr;
    wxArrayString choices_;
    bool choicesPopulated_ = false;
};

namespace {

std::string WxStringToUtf8(const wxString& value) {
    const wxScopedCharBuffer utf8 = value.ToUTF8();
    return utf8 ? std::string(utf8.data()) : std::string();
}

EventDraftDefaults BuildDefaultTimedDraft(const long long defaultDayEpoch) {
    const std::string displayTimezone = GetCurrentLocalTimeZoneName();
    const long long displayStartEpoch = defaultDayEpoch + 9 * 3600;
    const long long displayEndEpoch = defaultDayEpoch + 10 * 3600;
    return EventDraftDefaults{
        ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayStartEpoch).value_or(displayStartEpoch),
        ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayEndEpoch).value_or(displayEndEpoch),
        false};
}

long long FormatDisplayEndEpoch(const long long epoch, const bool allDay) {
    if (!allDay) {
        return epoch;
    }
    return epoch > 0 ? epoch - kSecondsPerDay : epoch;
}

std::string BuildRecurrenceRule(const long long startDateTime,
                                const int selection,
                                const bool hasCount,
                                const unsigned int count,
                                const bool hasUntil,
                                const long long untilEpoch) {
    const long long localDisplayEpoch =
        ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), startDateTime);
    const std::tm startTm = EpochToUtcTm(localDisplayEpoch);

    std::string rule;
    switch (selection) {
        case 1:
            rule = "RRULE:FREQ=DAILY";
            break;
        case 2: {
            static const char* weekdays[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
            rule = std::string("RRULE:FREQ=WEEKLY;BYDAY=") + weekdays[startTm.tm_wday] + ";WKST=MO";
            break;
        }
        case 3:
            rule = "RRULE:FREQ=MONTHLY;BYMONTHDAY=" + std::to_string(startTm.tm_mday);
            break;
        case 4:
            rule = "RRULE:FREQ=YEARLY;BYMONTHDAY=" + std::to_string(startTm.tm_mday);
            break;
        default:
            return "";
    }

    if (hasCount && count > 0) {
        rule += ";COUNT=" + std::to_string(count);
    }
    else if (hasUntil && untilEpoch > 0) {
        const long long untilEndOfDay = StartOfUtcDay(untilEpoch) + kSecondsPerDay - 1;
        rule += ";UNTIL=" + epochToIso(untilEndOfDay);
    }

    return rule;
}

int RecurrenceSelectionFromRule(const std::string& recurrenceRule) {
    if (recurrenceRule.empty()) {
        return 0;
    }

    const RRule rule = RRule().parseRRule(recurrenceRule);
    switch (rule.freq) {
        case Frequency::DAILY:
            return 1;
        case Frequency::WEEKLY:
            return 2;
        case Frequency::MONTHLY:
            return 3;
        case Frequency::YEARLY:
            return 4;
        case Frequency::UNKNOWN:
            return 0;
    }

    return 0;
}

std::optional<unsigned int> ParseUnsignedInteger(const wxString& value) {
    const std::string text = value.ToStdString();
    if (text.empty() ||
        !std::all_of(text.begin(), text.end(), [](const unsigned char ch) { return std::isdigit(ch) != 0; })) {
        return std::nullopt;
    }

    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(text);
    }
    catch (...) {
        return std::nullopt;
    }
    if (parsed == 0 || parsed > std::numeric_limits<unsigned int>::max()) {
        return std::nullopt;
    }
    return static_cast<unsigned int>(parsed);
}

int SelectedYear(const wxSpinCtrl* choice) {
    if (choice == nullptr) {
        return kMinCalendarYear;
    }
    return std::max(kMinCalendarYear, choice->GetValue());
}

int SelectedMonth(const wxComboBox* choice) {
    if (choice == nullptr || choice->GetSelection() == wxNOT_FOUND) {
        return 1;
    }
    return choice->GetSelection() + 1;
}

long long DisplayDayFromChoice(const wxSpinCtrl* yearChoice, const wxComboBox* monthChoice, const wxSpinCtrl* dayChoice) {
    if (dayChoice == nullptr) {
        return -1;
    }
    const int year = SelectedYear(yearChoice);
    const int month = SelectedMonth(monthChoice);
    return MakeUtcEpoch(year, month, dayChoice->GetValue());
}

int MinuteOfDayFromDisplayEpoch(const long long displayEpoch) {
    const long long secondsIntoDay = ((displayEpoch % kSecondsPerDay) + kSecondsPerDay) % kSecondsPerDay;
    return static_cast<int>(secondsIntoDay / 60);
}

std::string FormatTimeInput(const int minuteOfDay) {
    const int normalizedMinute = std::clamp(minuteOfDay, 0, kMinutesPerDay - 1);
    const int hour = normalizedMinute / 60;
    const int minute = normalizedMinute % 60;
    std::ostringstream output;
    output << std::setw(2) << std::setfill('0') << hour
           << ':' << std::setw(2) << std::setfill('0') << minute;
    return output.str();
}

std::optional<int> MinutesFromTimeInput(const TimePickerCtrl* choice) {
    if (choice == nullptr) {
        return 0;
    }

    std::string value = choice->GetValue().ToStdString();
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](const unsigned char ch) { return std::isspace(ch) != 0; }),
                value.end());

    const std::size_t separator = value.find(':');
    if (separator == std::string::npos || value.find(':', separator + 1) != std::string::npos) {
        return std::nullopt;
    }

    const std::string hourText = value.substr(0, separator);
    const std::string minuteText = value.substr(separator + 1);
    const auto allDigits = [](const std::string& text) {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(),
                           [](const unsigned char ch) { return std::isdigit(ch) != 0; });
    };
    if (!allDigits(hourText) || !allDigits(minuteText)) {
        return std::nullopt;
    }

    const int hour = std::stoi(hourText);
    const int minute = std::stoi(minuteText);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return std::nullopt;
    }

    return hour * 60 + minute;
}

wxArrayString BuildTimeLabels() {
    wxArrayString labels;
    labels.Alloc(kMinutesPerDay / 15);
    for (int minute = 0; minute < kMinutesPerDay; minute += 15) {
        const int hour = minute / 60;
        const int min = minute % 60;
        labels.Add(wxString::Format("%02d:%02d", hour, min));
    }
    return labels;
}

const wxArrayString& TimeLabels() {
    static const wxArrayString labels = BuildTimeLabels();
    return labels;
}

wxArrayString BuildMonthLabels() {
    static const std::array<const char*, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    wxArrayString labels;
    labels.Alloc(months.size());
    for (const char* month : months) {
        labels.Add(month);
    }
    return labels;
}

const wxArrayString& MonthLabels() {
    static const wxArrayString labels = BuildMonthLabels();
    return labels;
}

wxString FormatCalendarChoiceLabel(const Calendar& calendar,
                                   const std::unordered_map<long long, wxString>& accountLabels) {
    wxString label = wxString::FromUTF8(calendar.name);
    if (calendar.isPrimary) {
        label += " (Primary)";
    }
    if (calendar.isShared) {
        label += " (Shared)";
    }
    if (calendar.isReadOnly) {
        label += " (Read-only)";
    }

    const auto accountIt = accountLabels.find(calendar.accountId);
    if (accountIt != accountLabels.end() && !accountIt->second.empty()) {
        label += " - ";
        label += accountIt->second;
    }
    return label;
}

} // namespace

EventEditorDialog::EventEditorDialog(wxWindow* parent,
                                     const std::optional<Event>& event,
                                     const long long defaultDayEpoch,
                                     const std::vector<Calendar>& calendars,
                                     const std::unordered_map<long long, wxString>& accountLabels,
                                     const long long defaultCalendarId,
                                     const std::optional<EventDraftDefaults>& defaults,
                                     const bool readOnly)
    : wxDialog(parent, wxID_ANY, event.has_value() ? "Edit Event" : "New Event",
               wxDefaultPosition, wxSize(720, 620),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      originalEvent_(event),
      defaultDayEpoch_(defaultDayEpoch),
      calendars_(calendars),
      accountLabels_(accountLabels),
      defaultCalendarId_(defaultCalendarId),
      defaults_(defaults),
      readOnly_(readOnly) {
    Freeze();
    BuildLayout();
    LoadInitialValues();
    ApplyReadOnlyState();
    Thaw();
}

bool EventEditorDialog::IsDeleteRequested() const {
    return deleteRequested_;
}

std::optional<Event> EventEditorDialog::BuildEvent(const long long calendarId) const {
    Event event{};

    if (originalEvent_.has_value()) {
        event = *originalEvent_;
    }

    event.calendarId = SelectedCalendarId(calendarId);
    event.title = WxStringToUtf8(titleCtrl_->GetValue());
    event.location = WxStringToUtf8(locationCtrl_->GetValue());
    event.description = WxStringToUtf8(descriptionCtrl_->GetValue());
    event.allDay = allDayCtrl_->GetValue();
    event.timezone = GetCurrentLocalTimeZoneName();
    event.status = "confirmed";
    event.type = EventType::SINGLE;
    event.deletedAt = 0;
    event.updatedAt = std::time(nullptr);
    if (event.createdAt == 0) {
        event.createdAt = event.updatedAt;
    }

    event.startDateTime = ReadDateTimeControls(true, event.allDay);
    event.endDateTime = ReadDateTimeControls(false, event.allDay);
    NormalizeAllDayEventRange(event);
    const bool recurrenceEnabled = recurrenceCtrl_->GetSelection() > 0;
    const bool countLimitEnabled = recurrenceEnabled && recurrenceCountCtrl_->GetValue();
    const bool untilLimitEnabled = recurrenceEnabled && recurrenceUntilCtrl_->GetValue();
    unsigned int recurrenceCount = 0;
    if (countLimitEnabled) {
        const auto parsedCount = ParseUnsignedInteger(recurrenceCountValueCtrl_->GetValue());
        if (!parsedCount.has_value()) {
            wxMessageBox("Repeat count must be a positive unsigned integer.",
                         "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
            return std::nullopt;
        }
        recurrenceCount = *parsedCount;
    }
    const long long recurrenceUntil = untilLimitEnabled ? ReadUntilDateControl() : 0;
    event.recurrenceRule = BuildRecurrenceRule(
        event.startDateTime,
        recurrenceCtrl_->GetSelection(),
        countLimitEnabled,
        recurrenceCount,
        untilLimitEnabled,
        recurrenceUntil);
    event.type = event.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
    event.providerMasterId.clear();

    if (!originalEvent_.has_value()) {
        event.instanceStart = event.recurrenceRule.empty() ? event.startDateTime : 0;
    }
    else if (event.recurrenceRule.empty() && event.instanceStart == 0) {
        event.instanceStart = event.startDateTime;
    }
    else if (!event.recurrenceRule.empty()) {
        event.instanceStart = 0;
    }

    if (!ValidateEvent(event)) {
        return std::nullopt;
    }

    return event;
}

void EventEditorDialog::BuildLayout() {
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    if (!originalEvent_.has_value()) {
        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Calendar"), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        calendarCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        const long long preferredCalendarId = defaultCalendarId_;
        int selectedCalendarIndex = wxNOT_FOUND;
        for (const auto& calendar : calendars_) {
            if (!readOnly_ && calendar.isReadOnly) {
                continue;
            }

            calendarChoiceIds_.push_back(calendar.id);
            calendarCtrl_->Append(FormatCalendarChoiceLabel(calendar, accountLabels_));
            if (calendar.id == preferredCalendarId) {
                selectedCalendarIndex = static_cast<int>(calendarChoiceIds_.size()) - 1;
            }
        }
        if (selectedCalendarIndex == wxNOT_FOUND && !calendarChoiceIds_.empty()) {
            selectedCalendarIndex = 0;
        }
        if (selectedCalendarIndex != wxNOT_FOUND) {
            calendarCtrl_->SetSelection(selectedCalendarIndex);
        }
        rootSizer->Add(calendarCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    }

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Title"), 0, wxLEFT | wxRIGHT | wxTOP, 12);
    titleCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(titleCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Location"), 0, wxLEFT | wxRIGHT, 12);
    locationCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(locationCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    allDayCtrl_ = new wxCheckBox(this, wxID_ANY, "All day");
    rootSizer->Add(allDayCtrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Repeat"), 0, wxLEFT | wxRIGHT, 12);
    recurrenceCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    recurrenceCtrl_->Append("Does not repeat");
    recurrenceCtrl_->Append("Daily");
    recurrenceCtrl_->Append("Weekly");
    recurrenceCtrl_->Append("Monthly");
    recurrenceCtrl_->Append("Yearly");
    recurrenceCtrl_->SetSelection(0);
    rootSizer->Add(recurrenceCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* recurrenceLimitSizer = new wxFlexGridSizer(2, 5, 8, 10);
    recurrenceLimitSizer->AddGrowableCol(3, 1);
    recurrenceCountCtrl_ = new wxCheckBox(this, wxID_ANY, "End after");
    recurrenceCountValueCtrl_ = new wxTextCtrl(this, wxID_ANY, "10", wxDefaultPosition, wxSize(90, -1));
    recurrenceLimitSizer->Add(recurrenceCountCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    recurrenceLimitSizer->Add(recurrenceCountValueCtrl_, 0, wxEXPAND);
    recurrenceLimitSizer->Add(new wxStaticText(this, wxID_ANY, "occurrences"), 0, wxALIGN_CENTER_VERTICAL);
    recurrenceLimitSizer->AddSpacer(1);
    recurrenceLimitSizer->AddSpacer(1);

    recurrenceUntilCtrl_ = new wxCheckBox(this, wxID_ANY, "End by");
    recurrenceUntilYearCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(78, -1),
                                              wxSP_ARROW_KEYS, kMinCalendarYear, 9999, kMinCalendarYear);
    recurrenceUntilMonthCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, MonthLabels(), wxCB_READONLY);
    recurrenceUntilDateCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(70, -1),
                                              wxSP_ARROW_KEYS, 1, 31, 1);
    recurrenceLimitSizer->Add(recurrenceUntilCtrl_, 0, wxALIGN_CENTER_VERTICAL);
    recurrenceLimitSizer->Add(recurrenceUntilYearCtrl_, 0, wxEXPAND);
    recurrenceLimitSizer->Add(recurrenceUntilMonthCtrl_, 0, wxEXPAND);
    recurrenceLimitSizer->Add(recurrenceUntilDateCtrl_, 1, wxEXPAND);
    recurrenceLimitSizer->AddSpacer(1);
    rootSizer->Add(recurrenceLimitSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* dateTimeSizer = new wxFlexGridSizer(2, 5, 8, 10);
    dateTimeSizer->AddGrowableCol(3, 1);
    dateTimeSizer->Add(new wxStaticText(this, wxID_ANY, "Start"), 0, wxALIGN_CENTER_VERTICAL);
    startYearCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(78, -1),
                                    wxSP_ARROW_KEYS, kMinCalendarYear, 9999, kMinCalendarYear);
    startMonthCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, MonthLabels(), wxCB_READONLY);
    startDateCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(70, -1),
                                    wxSP_ARROW_KEYS, 1, 31, 1);
    startTimeCtrl_ = new TimePickerCtrl(this);
    dateTimeSizer->Add(startYearCtrl_, 0, wxEXPAND);
    dateTimeSizer->Add(startMonthCtrl_, 0, wxEXPAND);
    dateTimeSizer->Add(startDateCtrl_, 1, wxEXPAND);
    dateTimeSizer->Add(startTimeCtrl_, 0, wxEXPAND);

    dateTimeSizer->Add(new wxStaticText(this, wxID_ANY, "End"), 0, wxALIGN_CENTER_VERTICAL);
    endYearCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(78, -1),
                                  wxSP_ARROW_KEYS, kMinCalendarYear, 9999, kMinCalendarYear);
    endMonthCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, MonthLabels(), wxCB_READONLY);
    endDateCtrl_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(70, -1),
                                  wxSP_ARROW_KEYS, 1, 31, 1);
    endTimeCtrl_ = new TimePickerCtrl(this);
    dateTimeSizer->Add(endYearCtrl_, 0, wxEXPAND);
    dateTimeSizer->Add(endMonthCtrl_, 0, wxEXPAND);
    dateTimeSizer->Add(endDateCtrl_, 1, wxEXPAND);
    dateTimeSizer->Add(endTimeCtrl_, 0, wxEXPAND);
    rootSizer->Add(dateTimeSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Description"), 0, wxLEFT | wxRIGHT, 12);
    descriptionCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 180), wxTE_MULTILINE);
    rootSizer->Add(descriptionCtrl_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    if (originalEvent_.has_value()) {
        deleteButton_ = new wxButton(this, wxID_ANY, "Delete");
        deleteButton_->Bind(wxEVT_BUTTON, &EventEditorDialog::OnDelete, this);
        buttonSizer->Add(deleteButton_, 0, wxRIGHT, 8);
    }

    cancelButton_ = new wxButton(this, wxID_CANCEL, "Cancel");
    saveButton_ = new wxButton(this, wxID_OK, originalEvent_.has_value() ? "Save changes" : "Create event");
    saveButton_->Bind(wxEVT_BUTTON, &EventEditorDialog::OnSave, this);
    saveButton_->SetDefault();

    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(cancelButton_, 0, wxRIGHT, 8);
    buttonSizer->Add(saveButton_, 0);

    rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizer(rootSizer);
    Layout();

    allDayCtrl_->Bind(wxEVT_CHECKBOX, &EventEditorDialog::OnAllDayChanged, this);
    recurrenceCtrl_->Bind(wxEVT_COMBOBOX, &EventEditorDialog::OnRecurrenceChanged, this);
    recurrenceCountCtrl_->Bind(wxEVT_CHECKBOX, &EventEditorDialog::OnCountLimitChanged, this);
    recurrenceUntilCtrl_->Bind(wxEVT_CHECKBOX, &EventEditorDialog::OnUntilLimitChanged, this);
    startYearCtrl_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { RefreshDayChoicesForSelectedDateParts(); });
    endYearCtrl_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { RefreshDayChoicesForSelectedDateParts(); });
    recurrenceUntilYearCtrl_->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
        RefreshDayChoicesForRow(recurrenceUntilYearCtrl_, recurrenceUntilMonthCtrl_, recurrenceUntilDateCtrl_);
    });
    startMonthCtrl_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { RefreshDayChoicesForSelectedDateParts(); });
    endMonthCtrl_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { RefreshDayChoicesForSelectedDateParts(); });
    recurrenceUntilMonthCtrl_->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
        RefreshDayChoicesForRow(recurrenceUntilYearCtrl_, recurrenceUntilMonthCtrl_, recurrenceUntilDateCtrl_);
    });
}

void EventEditorDialog::LoadInitialValues() {
    if (originalEvent_.has_value()) {
        const Event& event = *originalEvent_;
        titleCtrl_->SetValue(wxString::FromUTF8(event.title));
        locationCtrl_->SetValue(wxString::FromUTF8(event.location));
        descriptionCtrl_->SetValue(wxString::FromUTF8(event.description));
        allDayCtrl_->SetValue(event.allDay);
        recurrenceCtrl_->SetSelection(RecurrenceSelectionFromRule(event.recurrenceRule));
        const RRule rule = RRule().parseRRule(event.recurrenceRule);
        recurrenceCountCtrl_->SetValue(rule.hasCount);
        recurrenceCountValueCtrl_->SetValue(rule.hasCount ? wxString::Format("%u", rule.count) : wxString("10"));
        recurrenceUntilCtrl_->SetValue(rule.hasUntil);
        WriteUntilDateControl(rule.hasUntil ? rule.until : event.startDateTime);
        const long long centerEpoch = event.allDay
            ? event.startDateTime
            : ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), event.startDateTime);
        PopulateDateChoices(centerEpoch);
        WriteDateTimeControls(true, event.startDateTime, event.allDay);
        WriteDateTimeControls(false, FormatDisplayEndEpoch(event.endDateTime, event.allDay), event.allDay);
        UpdateTimeControlsEnabled();
        UpdateRecurrenceLimitControlsEnabled();
        return;
    }

    titleCtrl_->SetValue("");
    locationCtrl_->SetValue("");
    descriptionCtrl_->SetValue("");
    const EventDraftDefaults defaults = defaults_.value_or(BuildDefaultTimedDraft(defaultDayEpoch_));
    allDayCtrl_->SetValue(defaults.allDay);
    recurrenceCtrl_->SetSelection(0);
    recurrenceCountCtrl_->SetValue(false);
    recurrenceCountValueCtrl_->SetValue("10");
    recurrenceUntilCtrl_->SetValue(false);
    WriteUntilDateControl(defaults.startDateTime);
    const long long centerEpoch = defaults.allDay
        ? defaults.startDateTime
        : ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), defaults.startDateTime);
    PopulateDateChoices(centerEpoch);
    WriteDateTimeControls(true, defaults.startDateTime, defaults.allDay);
    WriteDateTimeControls(false, FormatDisplayEndEpoch(defaults.endDateTime, defaults.allDay), defaults.allDay);
    UpdateTimeControlsEnabled();
    UpdateRecurrenceLimitControlsEnabled();
}

bool EventEditorDialog::ValidateEvent(const Event& event) const {
    const auto validationMessage = ValidateEventForUi(event);
    if (!validationMessage.has_value()) {
        return true;
    }

    wxMessageBox(*validationMessage, "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
    return false;
}

long long EventEditorDialog::SelectedCalendarId(const long long fallbackCalendarId) const {
    if (calendarCtrl_ == nullptr) {
        return fallbackCalendarId;
    }

    const int selection = calendarCtrl_->GetSelection();
    if (selection >= 0 && selection < static_cast<int>(calendarChoiceIds_.size())) {
        return calendarChoiceIds_[selection];
    }

    return fallbackCalendarId;
}

long long EventEditorDialog::ReadDateTimeControls(const bool start, const bool allDay) const {
    const auto* dateCtrl = start ? startDateCtrl_ : endDateCtrl_;
    const auto* yearCtrl = start ? startYearCtrl_ : endYearCtrl_;
    const auto* monthCtrl = start ? startMonthCtrl_ : endMonthCtrl_;
    const auto* timeCtrl = start ? startTimeCtrl_ : endTimeCtrl_;
    const long long displayDay = DisplayDayFromChoice(yearCtrl, monthCtrl, dateCtrl);
    if (displayDay < 0) {
        return -1;
    }

    if (allDay) {
        return start ? displayDay : displayDay + kSecondsPerDay;
    }

    const std::optional<int> minuteOfDay = MinutesFromTimeInput(timeCtrl);
    if (!minuteOfDay.has_value()) {
        return -1;
    }

    const long long displayEpoch = displayDay + static_cast<long long>(*minuteOfDay) * 60;
    const auto utcEpoch = ConvertTimeZoneDisplayEpochToUtcEpoch(GetCurrentLocalTimeZoneName(), displayEpoch);
    return utcEpoch.value_or(displayEpoch);
}

long long EventEditorDialog::ReadUntilDateControl() const {
    return DisplayDayFromChoice(recurrenceUntilYearCtrl_, recurrenceUntilMonthCtrl_, recurrenceUntilDateCtrl_);
}

void EventEditorDialog::WriteDateTimeControls(const bool start, const long long epoch, const bool allDay) {
    auto* dateCtrl = start ? startDateCtrl_ : endDateCtrl_;
    auto* yearCtrl = start ? startYearCtrl_ : endYearCtrl_;
    auto* monthCtrl = start ? startMonthCtrl_ : endMonthCtrl_;
    auto* timeCtrl = start ? startTimeCtrl_ : endTimeCtrl_;
    const long long displayEpoch = allDay
        ? epoch
        : ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), epoch);
    const long long displayDay = StartOfUtcDay(displayEpoch);
    const std::tm tm = EpochToUtcTm(displayDay);
    const int year = tm.tm_year + 1900;
    yearCtrl->SetValue(std::clamp(year, kMinCalendarYear, 9999));
    monthCtrl->SetSelection(tm.tm_mon);
    RefreshDayChoicesForRow(yearCtrl, monthCtrl, dateCtrl);

    dateCtrl->SetValue(std::clamp(tm.tm_mday, dateCtrl->GetMin(), dateCtrl->GetMax()));
    timeCtrl->SetValue(FormatTimeInput(MinuteOfDayFromDisplayEpoch(displayEpoch)));
}

void EventEditorDialog::WriteUntilDateControl(const long long epoch) {
    if (recurrenceUntilYearCtrl_ == nullptr ||
        recurrenceUntilMonthCtrl_ == nullptr ||
        recurrenceUntilDateCtrl_ == nullptr) {
        return;
    }

    const long long displayEpoch = ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), epoch);
    const long long displayDay = StartOfUtcDay(displayEpoch);
    const std::tm tm = EpochToUtcTm(displayDay);
    recurrenceUntilYearCtrl_->SetValue(std::clamp(tm.tm_year + 1900, kMinCalendarYear, 9999));
    recurrenceUntilMonthCtrl_->SetSelection(tm.tm_mon);
    RefreshDayChoicesForRow(recurrenceUntilYearCtrl_, recurrenceUntilMonthCtrl_, recurrenceUntilDateCtrl_);
    recurrenceUntilDateCtrl_->SetValue(std::clamp(tm.tm_mday,
                                                  recurrenceUntilDateCtrl_->GetMin(),
                                                  recurrenceUntilDateCtrl_->GetMax()));
}

void EventEditorDialog::PopulateDateChoices(const long long centerDisplayDay) {
    PopulateYearChoices(centerDisplayDay);
    const std::tm tm = EpochToUtcTm(StartOfUtcDay(centerDisplayDay));
    if (startMonthCtrl_ != nullptr) {
        startMonthCtrl_->SetSelection(tm.tm_mon);
    }
    if (endMonthCtrl_ != nullptr) {
        endMonthCtrl_->SetSelection(tm.tm_mon);
    }
    RefreshDayChoicesForSelectedDateParts();
}

void EventEditorDialog::PopulateYearChoices(const long long centerDisplayDay) {
    const int centerYear = std::max(kMinCalendarYear, EpochToUtcTm(StartOfUtcDay(centerDisplayDay)).tm_year + 1900);
    auto setYear = [centerYear](wxSpinCtrl* choice) {
        if (choice == nullptr) {
            return;
        }
        choice->SetRange(kMinCalendarYear, 9999);
        choice->SetValue(std::clamp(centerYear, kMinCalendarYear, 9999));
    };

    setYear(startYearCtrl_);
    setYear(endYearCtrl_);
}

void EventEditorDialog::PopulateMonthChoices() {
    auto appendChoices = [](wxComboBox* choice) {
        if (choice == nullptr) {
            return;
        }
        choice->Freeze();
        choice->Clear();
        choice->Append(MonthLabels());
        choice->SetSelection(0);
        choice->Thaw();
    };
    appendChoices(startMonthCtrl_);
    appendChoices(endMonthCtrl_);
}

void EventEditorDialog::RefreshDayChoicesForSelectedDateParts() {
    if (startDateCtrl_ != nullptr) {
        RefreshDayChoicesForRow(startYearCtrl_, startMonthCtrl_, startDateCtrl_);
    }
    if (endDateCtrl_ != nullptr) {
        RefreshDayChoicesForRow(endYearCtrl_, endMonthCtrl_, endDateCtrl_);
    }
}

void EventEditorDialog::RefreshDayChoicesForRow(wxSpinCtrl* yearChoice,
                                                wxComboBox* monthChoice,
                                                wxSpinCtrl* dayChoice) {
    if (yearChoice == nullptr || monthChoice == nullptr || dayChoice == nullptr) {
        return;
    }
    const int previousValue = std::max(1, dayChoice->GetValue());
    const int year = SelectedYear(yearChoice);
    const int month = SelectedMonth(monthChoice);
    const int dayCount = DaysInMonth(year, month);
    dayChoice->SetRange(1, dayCount);
    dayChoice->SetValue(std::min(previousValue, dayCount));
}

void EventEditorDialog::PopulateTimeChoices() {
    auto appendChoices = [](TimePickerCtrl* choice) {
        if (choice == nullptr) {
            return;
        }
        choice->PopulateChoices(TimeLabels());
    };

    appendChoices(startTimeCtrl_);
    appendChoices(endTimeCtrl_);
}

void EventEditorDialog::EnsureTimeChoicesPopulated() {
    if (timeChoicesPopulated_) {
        return;
    }

    PopulateTimeChoices();
    timeChoicesPopulated_ = true;
}

void EventEditorDialog::UpdateTimeControlsEnabled() {
    const bool enabled = !readOnly_ && !allDayCtrl_->GetValue();
    if (enabled) {
        EnsureTimeChoicesPopulated();
    }
    if (startTimeCtrl_ != nullptr) {
        startTimeCtrl_->Enable(enabled);
    }
    if (endTimeCtrl_ != nullptr) {
        endTimeCtrl_->Enable(enabled);
    }
}

void EventEditorDialog::UpdateRecurrenceLimitControlsEnabled() {
    const bool recurrenceEnabled = !readOnly_ && recurrenceCtrl_ != nullptr && recurrenceCtrl_->GetSelection() > 0;
    const bool countEnabled = recurrenceEnabled && recurrenceCountCtrl_ != nullptr && recurrenceCountCtrl_->GetValue();
    const bool untilEnabled = recurrenceEnabled && recurrenceUntilCtrl_ != nullptr && recurrenceUntilCtrl_->GetValue();

    if (recurrenceCountCtrl_ != nullptr) {
        recurrenceCountCtrl_->Enable(recurrenceEnabled);
    }
    if (recurrenceCountValueCtrl_ != nullptr) {
        recurrenceCountValueCtrl_->Enable(countEnabled);
    }
    if (recurrenceUntilCtrl_ != nullptr) {
        recurrenceUntilCtrl_->Enable(recurrenceEnabled);
    }
    if (recurrenceUntilYearCtrl_ != nullptr) {
        recurrenceUntilYearCtrl_->Enable(untilEnabled);
    }
    if (recurrenceUntilMonthCtrl_ != nullptr) {
        recurrenceUntilMonthCtrl_->Enable(untilEnabled);
    }
    if (recurrenceUntilDateCtrl_ != nullptr) {
        recurrenceUntilDateCtrl_->Enable(untilEnabled);
    }
}

void EventEditorDialog::ApplyReadOnlyState() {
    if (!readOnly_) {
        return;
    }

    if (titleCtrl_ != nullptr) {
        titleCtrl_->SetEditable(false);
    }
    if (locationCtrl_ != nullptr) {
        locationCtrl_->SetEditable(false);
    }
    if (descriptionCtrl_ != nullptr) {
        descriptionCtrl_->SetEditable(false);
    }

    if (allDayCtrl_ != nullptr) {
        allDayCtrl_->Enable(false);
    }
    if (recurrenceCtrl_ != nullptr) {
        recurrenceCtrl_->Enable(false);
    }
    if (recurrenceCountCtrl_ != nullptr) {
        recurrenceCountCtrl_->Enable(false);
    }
    if (recurrenceCountValueCtrl_ != nullptr) {
        recurrenceCountValueCtrl_->Enable(false);
    }
    if (recurrenceUntilCtrl_ != nullptr) {
        recurrenceUntilCtrl_->Enable(false);
    }
    if (recurrenceUntilYearCtrl_ != nullptr) {
        recurrenceUntilYearCtrl_->Enable(false);
    }
    if (recurrenceUntilMonthCtrl_ != nullptr) {
        recurrenceUntilMonthCtrl_->Enable(false);
    }
    if (recurrenceUntilDateCtrl_ != nullptr) {
        recurrenceUntilDateCtrl_->Enable(false);
    }
    if (calendarCtrl_ != nullptr) {
        calendarCtrl_->Enable(false);
    }
    if (startYearCtrl_ != nullptr) {
        startYearCtrl_->Enable(false);
    }
    if (startMonthCtrl_ != nullptr) {
        startMonthCtrl_->Enable(false);
    }
    if (startDateCtrl_ != nullptr) {
        startDateCtrl_->Enable(false);
    }
    if (startTimeCtrl_ != nullptr) {
        startTimeCtrl_->Enable(false);
    }
    if (endYearCtrl_ != nullptr) {
        endYearCtrl_->Enable(false);
    }
    if (endMonthCtrl_ != nullptr) {
        endMonthCtrl_->Enable(false);
    }
    if (endDateCtrl_ != nullptr) {
        endDateCtrl_->Enable(false);
    }
    if (endTimeCtrl_ != nullptr) {
        endTimeCtrl_->Enable(false);
    }
    if (deleteButton_ != nullptr) {
        deleteButton_->Enable(false);
    }
    if (saveButton_ != nullptr) {
        saveButton_->Enable(false);
    }
    if (cancelButton_ != nullptr) {
        cancelButton_->SetLabel("Close");
        cancelButton_->SetDefault();
    }
}

void EventEditorDialog::OnAllDayChanged(wxCommandEvent&) {
    UpdateTimeControlsEnabled();
}

void EventEditorDialog::OnRecurrenceChanged(wxCommandEvent&) {
    UpdateRecurrenceLimitControlsEnabled();
}

void EventEditorDialog::OnCountLimitChanged(wxCommandEvent&) {
    if (recurrenceCountCtrl_ != nullptr &&
        recurrenceCountCtrl_->GetValue() &&
        recurrenceUntilCtrl_ != nullptr) {
        recurrenceUntilCtrl_->SetValue(false);
    }
    UpdateRecurrenceLimitControlsEnabled();
}

void EventEditorDialog::OnUntilLimitChanged(wxCommandEvent&) {
    if (recurrenceUntilCtrl_ != nullptr &&
        recurrenceUntilCtrl_->GetValue() &&
        recurrenceCountCtrl_ != nullptr) {
        recurrenceCountCtrl_->SetValue(false);
    }
    UpdateRecurrenceLimitControlsEnabled();
}

void EventEditorDialog::OnSave(wxCommandEvent&) {
    if (!BuildEvent(0).has_value()) {
        return;
    }

    deleteRequested_ = false;
    EndModal(wxID_OK);
}

void EventEditorDialog::OnDelete(wxCommandEvent&) {
    if (wxMessageBox("Delete this event?", "Confirm delete", wxYES_NO | wxICON_WARNING, this) != wxYES) {
        return;
    }

    deleteRequested_ = true;
    EndModal(wxID_OK);
}
