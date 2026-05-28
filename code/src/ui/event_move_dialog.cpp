#include "ui/event_move_dialog.h"

#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace {

wxString FormatCalendarLabelForMove(const Calendar& calendar) {
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
    return label;
}

} // namespace

EventMoveDialog::EventMoveDialog(wxWindow* parent,
                                 const Event& event,
                                 const std::vector<Calendar>& calendars,
                                 const std::unordered_map<long long, wxString>& accountLabels)
    : wxDialog(parent,
               wxID_ANY,
               "Move event to calendar",
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);
    rootSizer->Add(new wxStaticText(this,
                                    wxID_ANY,
                                    wxString::Format("Move \"%s\" to:",
                                                     wxString::FromUTF8(event.title))),
                   0,
                   wxALL,
                   14);

    calendarCtrl_ = new wxComboBox(this, wxID_ANY, "", wxDefaultPosition, wxSize(520, -1), 0, nullptr, wxCB_READONLY);
    for (const auto& calendar : calendars) {
        if (calendar.isReadOnly || calendar.id == event.calendarId) {
            continue;
        }

        wxString label = FormatCalendarLabelForMove(calendar);
        const auto accountIt = accountLabels.find(calendar.accountId);
        if (accountIt != accountLabels.end() && !accountIt->second.empty()) {
            label += " - ";
            label += accountIt->second;
        }

        calendarChoiceIds_.push_back(calendar.id);
        calendarCtrl_->Append(label);
    }

    if (!calendarChoiceIds_.empty()) {
        calendarCtrl_->SetSelection(0);
    }
    rootSizer->Add(calendarCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);

    auto* buttonSizer = CreateSeparatedButtonSizer(wxOK | wxCANCEL);
    if (buttonSizer != nullptr) {
        rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    }

    SetSizerAndFit(rootSizer);
    SetMinSize(wxSize(560, GetSize().GetHeight()));
    CentreOnParent();

    Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
        if (SelectedCalendarId() == 0) {
            wxMessageBox("Choose a destination calendar.", "Move event", wxOK | wxICON_WARNING, this);
            return;
        }
        event.Skip();
    }, wxID_OK);
}

bool EventMoveDialog::HasDestination() const {
    return !calendarChoiceIds_.empty();
}

long long EventMoveDialog::SelectedCalendarId() const {
    if (calendarCtrl_ == nullptr) {
        return 0;
    }

    const int selection = calendarCtrl_->GetSelection();
    if (selection >= 0 && selection < static_cast<int>(calendarChoiceIds_.size())) {
        return calendarChoiceIds_[selection];
    }
    return 0;
}
