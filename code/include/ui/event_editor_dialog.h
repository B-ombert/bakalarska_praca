#pragma once

#include <optional>

#include <wx/dialog.h>

#include "models/event.h"
#include "ui/calendar_ui_shared.h"

class EventEditorDialog final : public wxDialog {
public:
    EventEditorDialog(wxWindow* parent,
                      const std::optional<Event>& event,
                      long long defaultDayEpoch,
                      const std::optional<EventDraftDefaults>& defaults = std::nullopt);

    bool IsDeleteRequested() const;
    std::optional<Event> BuildEvent(long long calendarId) const;

private:
    void BuildLayout();
    void LoadInitialValues();
    bool ValidateEvent(const Event& event) const;
    void OnAllDayChanged(wxCommandEvent& event);
    void OnSave(wxCommandEvent& event);
    void OnDelete(wxCommandEvent& event);

    std::optional<Event> originalEvent_;
    long long defaultDayEpoch_ = 0;
    std::optional<EventDraftDefaults> defaults_;
    bool deleteRequested_ = false;

    class wxTextCtrl* titleCtrl_ = nullptr;
    class wxTextCtrl* locationCtrl_ = nullptr;
    class wxTextCtrl* startCtrl_ = nullptr;
    class wxTextCtrl* endCtrl_ = nullptr;
    class wxTextCtrl* descriptionCtrl_ = nullptr;
    class wxCheckBox* allDayCtrl_ = nullptr;
};
