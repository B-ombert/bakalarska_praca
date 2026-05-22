#pragma once

#include <optional>

#include <wx/dialog.h>

#include "models/event.h"
#include "ui/calendar_ui_shared.h"

enum class RecurrenceEditScope {
    THIS_INSTANCE,
    THIS_AND_FOLLOWING,
    ENTIRE_SERIES
};

class EventEditorDialog final : public wxDialog {
public:
    EventEditorDialog(wxWindow* parent,
                      const std::optional<Event>& event,
                      long long defaultDayEpoch,
                      const std::optional<EventDraftDefaults>& defaults = std::nullopt);

    bool IsDeleteRequested() const;
    RecurrenceEditScope GetRecurrenceEditScope() const;
    std::optional<Event> BuildEvent(long long calendarId) const;

private:
    void BuildLayout();
    void LoadInitialValues();
    bool ValidateEvent(const Event& event) const;
    long long ReadDateTimeControls(bool start, bool allDay) const;
    void WriteDateTimeControls(bool start, long long epoch, bool allDay);
    void PopulateDateChoices(long long centerDisplayDay);
    void PopulateYearChoices(long long centerDisplayDay);
    void PopulateMonthChoices();
    void RefreshDayChoicesForSelectedDateParts();
    void RefreshDayChoicesForRow(class wxComboBox* yearChoice,
                                 class wxComboBox* monthChoice,
                                 class wxComboBox* dayChoice);
    void PopulateTimeChoices();
    void UpdateTimeControlsEnabled();
    void OnAllDayChanged(wxCommandEvent& event);
    void OnSave(wxCommandEvent& event);
    void OnDelete(wxCommandEvent& event);

    std::optional<Event> originalEvent_;
    long long defaultDayEpoch_ = 0;
    int yearChoiceStart_ = kMinCalendarYear;
    std::optional<EventDraftDefaults> defaults_;
    bool deleteRequested_ = false;

    class wxTextCtrl* titleCtrl_ = nullptr;
    class wxTextCtrl* locationCtrl_ = nullptr;
    class wxComboBox* startYearCtrl_ = nullptr;
    class wxComboBox* startMonthCtrl_ = nullptr;
    class wxComboBox* startDateCtrl_ = nullptr;
    class wxComboBox* startTimeCtrl_ = nullptr;
    class wxComboBox* endYearCtrl_ = nullptr;
    class wxComboBox* endMonthCtrl_ = nullptr;
    class wxComboBox* endDateCtrl_ = nullptr;
    class wxComboBox* endTimeCtrl_ = nullptr;
    class wxTextCtrl* descriptionCtrl_ = nullptr;
    class wxCheckBox* allDayCtrl_ = nullptr;
    class wxComboBox* recurrenceCtrl_ = nullptr;
    class wxComboBox* recurrenceScopeCtrl_ = nullptr;
};
