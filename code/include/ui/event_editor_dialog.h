#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include <wx/dialog.h>

#include "models/calendar.h"
#include "models/event.h"
#include "ui/calendar_ui_shared.h"

enum class RecurrenceEditScope {
    THIS_INSTANCE,
    THIS_AND_FOLLOWING,
    ENTIRE_SERIES
};

class TimePickerCtrl;

class EventEditorDialog final : public wxDialog {
public:
    EventEditorDialog(wxWindow* parent,
                      const std::optional<Event>& event,
                      long long defaultDayEpoch,
                      const std::vector<Calendar>& calendars,
                      const std::unordered_map<long long, wxString>& accountLabels,
                      long long defaultCalendarId,
                      const std::optional<EventDraftDefaults>& defaults = std::nullopt,
                      bool readOnly = false);

    bool IsDeleteRequested() const;
    std::optional<Event> BuildEvent(long long calendarId) const;

private:
    void BuildLayout();
    void LoadInitialValues();
    bool ValidateEvent(const Event& event) const;
    long long SelectedCalendarId(long long fallbackCalendarId) const;
    long long ReadDateTimeControls(bool start, bool allDay) const;
    long long ReadUntilDateControl() const;
    void WriteDateTimeControls(bool start, long long epoch, bool allDay);
    void WriteUntilDateControl(long long epoch);
    void PopulateDateChoices(long long centerDisplayDay);
    void PopulateYearChoices(long long centerDisplayDay);
    void PopulateMonthChoices();
    void RefreshDayChoicesForSelectedDateParts();
    void RefreshDayChoicesForRow(class wxSpinCtrl* yearChoice,
                                 class wxComboBox* monthChoice,
                                 class wxSpinCtrl* dayChoice);
    void PopulateTimeChoices();
    void EnsureTimeChoicesPopulated();
    void UpdateTimeControlsEnabled();
    void UpdateRecurrenceLimitControlsEnabled();
    void ApplyReadOnlyState();
    void OnAllDayChanged(wxCommandEvent& event);
    void OnRecurrenceChanged(wxCommandEvent& event);
    void OnCountLimitChanged(wxCommandEvent& event);
    void OnUntilLimitChanged(wxCommandEvent& event);
    void OnSave(wxCommandEvent& event);
    void OnDelete(wxCommandEvent& event);

    std::optional<Event> originalEvent_;
    long long defaultDayEpoch_ = 0;
    std::optional<EventDraftDefaults> defaults_;
    std::vector<Calendar> calendars_;
    std::unordered_map<long long, wxString> accountLabels_;
    long long defaultCalendarId_ = 0;
    std::vector<long long> calendarChoiceIds_;
    bool deleteRequested_ = false;
    bool readOnly_ = false;
    bool timeChoicesPopulated_ = false;

    class wxComboBox* calendarCtrl_ = nullptr;
    class wxTextCtrl* titleCtrl_ = nullptr;
    class wxTextCtrl* locationCtrl_ = nullptr;
    class wxSpinCtrl* startYearCtrl_ = nullptr;
    class wxComboBox* startMonthCtrl_ = nullptr;
    class wxSpinCtrl* startDateCtrl_ = nullptr;
    TimePickerCtrl* startTimeCtrl_ = nullptr;
    class wxSpinCtrl* endYearCtrl_ = nullptr;
    class wxComboBox* endMonthCtrl_ = nullptr;
    class wxSpinCtrl* endDateCtrl_ = nullptr;
    TimePickerCtrl* endTimeCtrl_ = nullptr;
    class wxTextCtrl* descriptionCtrl_ = nullptr;
    class wxCheckBox* allDayCtrl_ = nullptr;
    class wxComboBox* recurrenceCtrl_ = nullptr;
    class wxCheckBox* recurrenceCountCtrl_ = nullptr;
    class wxTextCtrl* recurrenceCountValueCtrl_ = nullptr;
    class wxCheckBox* recurrenceUntilCtrl_ = nullptr;
    class wxSpinCtrl* recurrenceUntilYearCtrl_ = nullptr;
    class wxComboBox* recurrenceUntilMonthCtrl_ = nullptr;
    class wxSpinCtrl* recurrenceUntilDateCtrl_ = nullptr;
    class wxButton* deleteButton_ = nullptr;
    class wxButton* cancelButton_ = nullptr;
    class wxButton* saveButton_ = nullptr;
};
