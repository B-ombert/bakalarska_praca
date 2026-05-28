#pragma once

#include <optional>
#include <string>

#include <wx/bmpcbox.h>
#include <wx/dialog.h>
#include <wx/textctrl.h>

#include "models/calendar.h"

class CalendarEditorDialog final : public wxDialog {
public:
    explicit CalendarEditorDialog(wxWindow* parent, const std::optional<Calendar>& calendar = std::nullopt);

    std::string GetCalendarName() const;
    std::string GetCalendarDescription() const;
    std::string GetCalendarColor() const;
    bool HasIcalImport() const;
    Calendar::IcalImportResult GetIcalImportResult() const;

private:
    static std::string ToUtf8String(const wxString& value);
    void OnOk(wxCommandEvent& event);
    void OnImportIcal(wxCommandEvent& event);

    std::optional<Calendar> calendar_;
    std::optional<Calendar::IcalImportResult> importedIcal_;
    std::string savedName_;
    std::string savedDescription_;
    std::string savedColor_;
    wxTextCtrl* nameCtrl_ = nullptr;
    wxTextCtrl* descriptionCtrl_ = nullptr;
    wxBitmapComboBox* colorComboBox_ = nullptr;
};
