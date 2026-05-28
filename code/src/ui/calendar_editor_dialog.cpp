#include "ui/calendar_editor_dialog.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include <wx/button.h>
#include <wx/brush.h>
#include <wx/dcmemory.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "utils/calendar_colors.h"

namespace {

wxColour CalendarColour(const std::string& colorHex) {
    const std::string normalized = NormalizeCalendarColor(colorHex);
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    if (normalized.size() == 7) {
        std::stringstream stream;
        stream << std::hex << normalized.substr(1, 2);
        stream >> red;
        stream.clear();
        stream.str("");
        stream << std::hex << normalized.substr(3, 2);
        stream >> green;
        stream.clear();
        stream.str("");
        stream << std::hex << normalized.substr(5, 2);
        stream >> blue;
    }
    return wxColour(static_cast<unsigned char>(red),
                    static_cast<unsigned char>(green),
                    static_cast<unsigned char>(blue));
}

wxBitmap MakeColorSwatchBitmap(const wxColour& colour) {
    wxBitmap bitmap(44, 14);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetBrush(wxBrush(colour));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, 44, 14);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

} // namespace

CalendarEditorDialog::CalendarEditorDialog(wxWindow* parent, const std::optional<Calendar>& calendar)
    : wxDialog(parent,
               wxID_ANY,
               calendar.has_value() ? "Edit Calendar" : "New Calendar",
               wxDefaultPosition,
               wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      calendar_(calendar),
      savedColor_(DefaultCalendarColor()) {
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    auto* formSizer = new wxFlexGridSizer(0, 2, 10, 10);
    formSizer->AddGrowableCol(1, 1);

    formSizer->Add(new wxStaticText(this, wxID_ANY, "Name"), 0, wxALIGN_CENTER_VERTICAL);
    nameCtrl_ = new wxTextCtrl(this, wxID_ANY);
    formSizer->Add(nameCtrl_, 1, wxEXPAND);

    formSizer->Add(new wxStaticText(this, wxID_ANY, "Description"), 0, wxALIGN_TOP);
    descriptionCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 90), wxTE_MULTILINE);
    formSizer->Add(descriptionCtrl_, 1, wxEXPAND);

    formSizer->Add(new wxStaticText(this, wxID_ANY, "Color"), 0, wxALIGN_CENTER_VERTICAL);
    colorComboBox_ = new wxBitmapComboBox(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    for (const auto& [name, hex] : CalendarColorPalette()) {
        colorComboBox_->Append(wxString::FromUTF8(name), MakeColorSwatchBitmap(CalendarColour(hex)));
    }
    colorComboBox_->SetSelection(0);
    formSizer->Add(colorComboBox_, 1, wxEXPAND);

    rootSizer->Add(formSizer, 1, wxEXPAND | wxALL, 14);

    if (!calendar_.has_value()) {
        auto* importButton = new wxButton(this, wxID_ANY, "Import .ics file...");
        importButton->Bind(wxEVT_BUTTON, &CalendarEditorDialog::OnImportIcal, this);
        rootSizer->Add(importButton, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    }

    if (calendar_.has_value()) {
        wxString flags;
        if (calendar_->isPrimary) {
            flags += "Primary calendar";
        }
        if (calendar_->isShared) {
            flags += flags.empty() ? "Shared calendar" : ", shared calendar";
        }
        if (calendar_->isReadOnly) {
            flags += flags.empty() ? "Read-only calendar" : ", read-only calendar";
        }
        if (!flags.empty()) {
            rootSizer->Add(new wxStaticText(this, wxID_ANY, flags), 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
        }

        if (calendar_->isReadOnly) {
            nameCtrl_->Enable(false);
            descriptionCtrl_->Enable(false);
        }
    }

    auto* buttonSizer = CreateSeparatedButtonSizer(wxOK | wxCANCEL);
    if (buttonSizer != nullptr) {
        rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
    }

    SetSizerAndFit(rootSizer);
    SetMinSize(wxSize(420, GetSize().GetHeight()));
    CentreOnParent();

    if (calendar_.has_value()) {
        nameCtrl_->SetValue(wxString::FromUTF8(calendar_->name));
        descriptionCtrl_->SetValue(wxString::FromUTF8(calendar_->description));
        colorComboBox_->SetSelection(CalendarColorIndex(calendar_->colorHex));
    }

    Bind(wxEVT_BUTTON, &CalendarEditorDialog::OnOk, this, wxID_OK);
}

std::string CalendarEditorDialog::GetCalendarName() const {
    return savedName_;
}

std::string CalendarEditorDialog::GetCalendarDescription() const {
    return savedDescription_;
}

std::string CalendarEditorDialog::GetCalendarColor() const {
    return savedColor_;
}

bool CalendarEditorDialog::HasIcalImport() const {
    return importedIcal_.has_value();
}

Calendar::IcalImportResult CalendarEditorDialog::GetIcalImportResult() const {
    Calendar::IcalImportResult result = *importedIcal_;
    result.name = savedName_;
    result.description = savedDescription_;
    return result;
}

std::string CalendarEditorDialog::ToUtf8String(const wxString& value) {
    const wxScopedCharBuffer utf8 = value.ToUTF8();
    return utf8 ? std::string(utf8.data()) : std::string();
}

void CalendarEditorDialog::OnOk(wxCommandEvent& event) {
    if (!(calendar_.has_value() && calendar_->isReadOnly) &&
        nameCtrl_->GetValue().Trim(true).Trim(false).IsEmpty()) {
        wxMessageBox("Calendar name is required.", "Validation", wxOK | wxICON_WARNING, this);
        return;
    }

    savedName_ = calendar_.has_value() && calendar_->isReadOnly
        ? calendar_->name
        : ToUtf8String(nameCtrl_->GetValue());
    savedDescription_ = calendar_.has_value() && calendar_->isReadOnly
        ? calendar_->description
        : ToUtf8String(descriptionCtrl_->GetValue());
    const int selection = std::max(0, colorComboBox_->GetSelection());
    savedColor_ = CalendarColorPalette()[static_cast<size_t>(selection)].second;
    event.Skip();
}

void CalendarEditorDialog::OnImportIcal(wxCommandEvent&) {
    wxFileDialog dialog(
        this,
        "Open iCalendar file",
        "",
        "",
        "iCalendar files (*.ics)|*.ics|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    std::ifstream input(dialog.GetPath().ToStdString(), std::ios::binary);
    if (!input) {
        wxMessageBox("The selected file could not be opened.", "Import failed", wxOK | wxICON_ERROR, this);
        return;
    }

    const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::string error;
    auto parsed = Calendar::fromIcal(body, &error);
    if (!parsed.has_value()) {
        wxMessageBox(wxString::FromUTF8(error.empty() ? "Invalid iCalendar file." : error),
                     "Invalid iCalendar file",
                     wxOK | wxICON_WARNING,
                     this);
        return;
    }

    importedIcal_ = std::move(parsed);
    nameCtrl_->SetValue(wxString::FromUTF8(importedIcal_->name));
    descriptionCtrl_->SetValue(wxString::FromUTF8(importedIcal_->description));
}
