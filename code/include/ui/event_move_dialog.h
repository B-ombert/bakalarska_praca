#pragma once

#include <unordered_map>
#include <vector>

#include <wx/combobox.h>
#include <wx/dialog.h>

#include "models/calendar.h"
#include "models/event.h"

class EventMoveDialog final : public wxDialog {
public:
    EventMoveDialog(wxWindow* parent,
                    const Event& event,
                    const std::vector<Calendar>& calendars,
                    const std::unordered_map<long long, wxString>& accountLabels);

    bool HasDestination() const;
    long long SelectedCalendarId() const;

private:
    wxComboBox* calendarCtrl_ = nullptr;
    std::vector<long long> calendarChoiceIds_;
};
