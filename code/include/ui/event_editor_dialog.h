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
    RecurrenceEditScope GetRecurrenceEditScope() const;
    std::optional<Event> BuildEvent(long long calendarId) const;

private:
    void BuildLayout();
    void LoadInitialValues();
    bool ValidateEvent(const Event& event) const;
    long long SelectedCalendarId(long long fallbackCalendarId) const;
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
    void ApplyReadOnlyState();
    void OnAllDayChanged(wxCommandEvent& event);
    void OnSave(wxCommandEvent& event);
    void OnDelete(wxCommandEvent& event);

    std::optional<Event> originalEvent_;
    long long defaultDayEpoch_ = 0;
    int yearChoiceStart_ = kMinCalendarYear;
    std::optional<EventDraftDefaults> defaults_;
    std::vector<Calendar> calendars_;
    std::unordered_map<long long, wxString> accountLabels_;
    long long defaultCalendarId_ = 0;
    std::vector<long long> calendarChoiceIds_;
    bool deleteRequested_ = false;
    bool readOnly_ = false;

    class wxComboBox* calendarCtrl_ = nullptr;
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
    class wxButton* deleteButton_ = nullptr;
    class wxButton* cancelButton_ = nullptr;
    class wxButton* saveButton_ = nullptr;
};
