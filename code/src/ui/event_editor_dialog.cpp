#include "ui/event_editor_dialog.h"

#include <ctime>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "utils/datetime_utils.h"

namespace {

long long FormatDisplayEndEpoch(const long long epoch, const bool allDay) {
    if (!allDay) {
        return epoch;
    }
    return epoch > 0 ? epoch - kSecondsPerDay : epoch;
}

} // namespace

EventEditorDialog::EventEditorDialog(wxWindow* parent,
                                     const std::optional<Event>& event,
                                     const long long defaultDayEpoch,
                                     const std::optional<EventDraftDefaults>& defaults)
    : wxDialog(parent, wxID_ANY, event.has_value() ? "Edit Event" : "New Event",
               wxDefaultPosition, wxSize(460, 470),
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
    if (event.createdAt == 0) {
        event.createdAt = event.lastModified;
    }

    event.startDateTime = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), event.allDay);
    event.endDateTime = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), event.allDay);
    NormalizeAllDayEventRange(event);

    if (!originalEvent_.has_value()) {
        event.instanceStart = event.startDateTime;
        event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
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

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Start (UTC)"), 0, wxLEFT | wxRIGHT, 12);
    startCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(startCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "End (UTC)"), 0, wxLEFT | wxRIGHT, 12);
    endCtrl_ = new wxTextCtrl(this, wxID_ANY);
    rootSizer->Add(endCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(this, wxID_ANY, "Description"), 0, wxLEFT | wxRIGHT, 12);
    descriptionCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 140), wxTE_MULTILINE);
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
        titleCtrl_->SetValue(event.title);
        locationCtrl_->SetValue(event.location);
        descriptionCtrl_->SetValue(event.description);
        allDayCtrl_->SetValue(event.allDay);
        startCtrl_->SetValue(FormatUtcDateTimeInput(event.startDateTime, event.allDay));
        endCtrl_->SetValue(FormatUtcDateTimeInput(FormatDisplayEndEpoch(event.endDateTime, event.allDay), event.allDay));
        return;
    }

    titleCtrl_->SetValue("");
    locationCtrl_->SetValue("");
    descriptionCtrl_->SetValue("");
    const EventDraftDefaults defaults = defaults_.value_or(
        EventDraftDefaults{defaultDayEpoch_ + 9 * 3600, defaultDayEpoch_ + 10 * 3600, false});
    allDayCtrl_->SetValue(defaults.allDay);
    startCtrl_->SetValue(FormatUtcDateTimeInput(defaults.startDateTime, defaults.allDay));
    endCtrl_->SetValue(FormatUtcDateTimeInput(FormatDisplayEndEpoch(defaults.endDateTime, defaults.allDay), defaults.allDay));
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
    ApplyAllDayToggleToInputs(startCtrl_, endCtrl_, allDayCtrl_->GetValue());
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
