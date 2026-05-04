#include "ui/event_editor_dialog.h"

#include <ctime>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "events/rrule.h"
#include "utils/datetime_utils.h"
#include "utils/timezone_utils.h"

namespace {

std::string WxStringToUtf8(const wxString& value) {
    const wxScopedCharBuffer utf8 = value.ToUTF8();
    return utf8 ? std::string(utf8.data()) : std::string();
}

long long ParseDisplayDateTimeInput(const std::string& value, const bool allDay) {
    const long long displayEpoch = ParseUtcDateTimeInput(value, allDay);
    if (displayEpoch < 0 || allDay) {
        return displayEpoch;
    }

    const auto utcEpoch = ConvertTimeZoneDisplayEpochToUtcEpoch(GetCurrentLocalTimeZoneName(), displayEpoch);
    return utcEpoch.has_value() ? *utcEpoch : displayEpoch;
}

std::string FormatDisplayDateTimeInput(const long long epoch, const bool allDay) {
    if (allDay) {
        return FormatUtcDateTimeInput(epoch, true);
    }

    return FormatUtcDateTimeInput(ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), epoch), false);
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

void ApplyDisplayAllDayToggleToInputs(wxTextCtrl* startCtrl, wxTextCtrl* endCtrl, const bool allDay) {
    const long long startEpoch = ParseDisplayDateTimeInput(startCtrl->GetValue().ToStdString(), false);
    const long long endEpoch = ParseDisplayDateTimeInput(endCtrl->GetValue().ToStdString(), false);

    if (startEpoch >= 0) {
        startCtrl->SetValue(FormatDisplayDateTimeInput(startEpoch, allDay));
    }
    if (endEpoch >= 0) {
        const long long displayEndEpoch = allDay && endEpoch > 0 ? endEpoch - kSecondsPerDay : endEpoch;
        endCtrl->SetValue(FormatDisplayDateTimeInput(displayEndEpoch, allDay));
    }
}

long long FormatDisplayEndEpoch(const long long epoch, const bool allDay) {
    if (!allDay) {
        return epoch;
    }
    return epoch > 0 ? epoch - kSecondsPerDay : epoch;
}

std::string BuildRecurrenceRule(const long long startDateTime, const int selection) {
    const long long localDisplayEpoch =
        ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), startDateTime);
    const std::tm startTm = EpochToUtcTm(localDisplayEpoch);

    switch (selection) {
        case 1:
            return "RRULE:FREQ=DAILY";
        case 2: {
            static const char* weekdays[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
            return std::string("RRULE:FREQ=WEEKLY;BYDAY=") + weekdays[startTm.tm_wday] + ";WKST=MO";
        }
        case 3:
            return "RRULE:FREQ=MONTHLY;BYMONTHDAY=" + std::to_string(startTm.tm_mday);
        case 4:
            return "RRULE:FREQ=YEARLY;BYMONTHDAY=" + std::to_string(startTm.tm_mday);
        default:
            return "";
    }
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

} // namespace

EventEditorDialog::EventEditorDialog(wxWindow* parent,
                                     const std::optional<Event>& event,
                                     const long long defaultDayEpoch,
                                     const std::optional<EventDraftDefaults>& defaults)
    : wxDialog(parent, wxID_ANY, event.has_value() ? "Edit Event" : "New Event",
               wxDefaultPosition, wxSize(580, 620),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      originalEvent_(event),
      defaultDayEpoch_(defaultDayEpoch),
      defaults_(defaults) {
    BuildLayout();
    LoadInitialValues();
}

bool EventEditorDialog::IsDeleteRequested() const {
    return deleteRequested_;
}

std::optional<Event> EventEditorDialog::BuildEvent(const long long calendarId) const {
    Event event{};

    if (originalEvent_.has_value()) {
        event = *originalEvent_;
    }

    event.calendarId = calendarId;
    event.title = WxStringToUtf8(titleCtrl_->GetValue());
    event.location = WxStringToUtf8(locationCtrl_->GetValue());
    event.description = WxStringToUtf8(descriptionCtrl_->GetValue());
    event.allDay = allDayCtrl_->GetValue();
    event.timezone = GetCurrentLocalTimeZoneName();
    event.status = "confirmed";
    event.type = EventType::SINGLE;
    event.deletedAt = 0;
    event.syncStatus = SYNCED;
    event.lastModified = std::time(nullptr);
    event.updatedAt = event.lastModified;
    if (event.createdAt == 0) {
        event.createdAt = event.lastModified;
    }

    event.startDateTime = ParseDisplayDateTimeInput(startCtrl_->GetValue().ToStdString(), event.allDay);
    event.endDateTime = ParseDisplayDateTimeInput(endCtrl_->GetValue().ToStdString(), event.allDay);
    NormalizeAllDayEventRange(event);
    event.recurrenceRule = BuildRecurrenceRule(event.startDateTime, recurrenceCtrl_->GetSelection());
    event.type = event.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
    event.providerMasterId.clear();

    if (!originalEvent_.has_value()) {
        event.instanceStart = event.recurrenceRule.empty() ? event.startDateTime : 0;
        event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
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

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Start"), 0, wxLEFT | wxRIGHT, 12);
    startCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(startCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "End"), 0, wxLEFT | wxRIGHT, 12);
    endCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(endCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Description"), 0, wxLEFT | wxRIGHT, 12);
    descriptionCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 180), wxTE_MULTILINE);
    rootSizer->Add(descriptionCtrl_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    if (originalEvent_.has_value()) {
        auto* deleteButton = new wxButton(this, wxID_ANY, "Delete");
        deleteButton->Bind(wxEVT_BUTTON, &EventEditorDialog::OnDelete, this);
        buttonSizer->Add(deleteButton, 0, wxRIGHT, 8);
    }

    auto* cancelButton = new wxButton(this, wxID_CANCEL, "Cancel");
    auto* saveButton = new wxButton(this, wxID_OK, originalEvent_.has_value() ? "Save changes" : "Create event");
    saveButton->Bind(wxEVT_BUTTON, &EventEditorDialog::OnSave, this);

    buttonSizer->AddStretchSpacer();
    buttonSizer->Add(cancelButton, 0, wxRIGHT, 8);
    buttonSizer->Add(saveButton, 0);

    rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizerAndFit(rootSizer);

    allDayCtrl_->Bind(wxEVT_CHECKBOX, &EventEditorDialog::OnAllDayChanged, this);
}

void EventEditorDialog::LoadInitialValues() {
    if (originalEvent_.has_value()) {
        const Event& event = *originalEvent_;
        titleCtrl_->SetValue(wxString::FromUTF8(event.title));
        locationCtrl_->SetValue(wxString::FromUTF8(event.location));
        descriptionCtrl_->SetValue(wxString::FromUTF8(event.description));
        allDayCtrl_->SetValue(event.allDay);
        recurrenceCtrl_->SetSelection(RecurrenceSelectionFromRule(event.recurrenceRule));
        startCtrl_->SetValue(FormatDisplayDateTimeInput(event.startDateTime, event.allDay));
        endCtrl_->SetValue(FormatDisplayDateTimeInput(FormatDisplayEndEpoch(event.endDateTime, event.allDay), event.allDay));
        return;
    }

    titleCtrl_->SetValue("");
    locationCtrl_->SetValue("");
    descriptionCtrl_->SetValue("");
    const EventDraftDefaults defaults = defaults_.value_or(BuildDefaultTimedDraft(defaultDayEpoch_));
    allDayCtrl_->SetValue(defaults.allDay);
    recurrenceCtrl_->SetSelection(0);
    startCtrl_->SetValue(FormatDisplayDateTimeInput(defaults.startDateTime, defaults.allDay));
    endCtrl_->SetValue(FormatDisplayDateTimeInput(FormatDisplayEndEpoch(defaults.endDateTime, defaults.allDay), defaults.allDay));
}

bool EventEditorDialog::ValidateEvent(const Event& event) const {
    const auto validationMessage = ValidateEventForUi(event);
    if (!validationMessage.has_value()) {
        return true;
    }

    wxMessageBox(*validationMessage, "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
    return false;
}

void EventEditorDialog::OnAllDayChanged(wxCommandEvent&) {
    ApplyDisplayAllDayToggleToInputs(startCtrl_, endCtrl_, allDayCtrl_->GetValue());
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
