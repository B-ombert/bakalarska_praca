#include "ui/event_search_results_dialog.h"

#include <algorithm>
#include <ctime>
#include <sstream>

#include <wx/button.h>
#include <wx/cursor.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

#include "models/rrule.h"
#include "ui/calendar_ui_shared.h"
#include "utils/calendar_colors.h"
#include "utils/datetime_utils.h"
#include "utils/timezone_utils.h"

namespace {

wxString Utf8(const std::string& value) {
    return wxString::FromUTF8(value);
}

wxFont BoldFont(wxWindow* window, const int pointDelta = 0) {
    wxFont font = window->GetFont();
    font.SetWeight(wxFONTWEIGHT_BOLD);
    if (pointDelta != 0) {
        font.SetPointSize(std::max(1, font.GetPointSize() + pointDelta));
    }
    return font;
}

std::string FormatDisplayDate(const long long epoch) {
    const std::tm tm = EpochToUtcTm(StartOfUtcDay(epoch));
    std::ostringstream output;
    output << std::put_time(&tm, "%d %b %Y");
    return output.str();
}

std::string FormatDisplayDateTime(const long long epoch) {
    const std::tm tm = EpochToUtcTm(epoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%d %b %Y %H:%M");
    return output.str();
}

std::string FormatEventDisplayDateTime(const Event& event, const long long epoch) {
    const long long displayEpoch = ConvertUtcEpochToTimeZoneDisplayEpoch(GetCurrentLocalTimeZoneName(), epoch);
    return FormatDisplayDateTime(displayEpoch);
}

std::string FormatEventDisplayDateTime(const Event& event) {
    return event.allDay ? FormatDisplayDate(event.GetDisplayStartEpoch())
                        : FormatDisplayDateTime(event.GetDisplayStartEpoch());
}

std::string FormatStartLabel(const Event& event) {
    const long long start = event.GetDisplayStartEpoch();
    return event.allDay ? FormatDisplayDate(start) : FormatDisplayDateTime(start);
}

std::string FormatEndLabel(const Event& event) {
    const long long start = event.GetDisplayStartEpoch();
    const long long end = event.GetDisplayEndEpoch();
    if (event.allDay) {
        const long long displayEnd = end > 0 ? end - kSecondsPerDay : end;
        return FormatDisplayDate(displayEnd);
    }

    if (StartOfUtcDay(start) == StartOfUtcDay(end)) {
        return FormatTimeLabel(end);
    }
    return FormatDisplayDateTime(end);
}

std::string FrequencyLabel(const RRule& rule) {
    const int interval = std::max(1, rule.interval);
    const auto every = [interval](const char* singular, const char* plural) {
        if (interval == 1) {
            return std::string("every ") + singular;
        }
        return std::string("every ") + std::to_string(interval) + " " + plural;
    };

    switch (rule.freq) {
        case Frequency::DAILY:
            return every("day", "days");
        case Frequency::WEEKLY:
            return every("week", "weeks");
        case Frequency::MONTHLY:
            return every("month", "months");
        case Frequency::YEARLY:
            return every("year", "years");
        case Frequency::UNKNOWN:
            return "";
    }
    return "";
}

std::string RecurrenceLabel(const Event& event) {
    if (event.recurrenceRule.empty()) {
        return "";
    }

    const RRule rule = RRule().parseRRule(event.recurrenceRule);
    std::string label = FrequencyLabel(rule);
    if (label.empty()) {
        return "recurring event";
    }

    if (rule.hasUntil) {
        label += ", until " + FormatDisplayDate(rule.until);
    }
    else if (rule.hasCount) {
        label += ", repeats " + std::to_string(rule.count) + " times";
    }
    return label;
}

std::string DescriptionPreview(const std::string& description) {
    constexpr std::size_t kMaxPreviewLength = 180;
    if (description.size() <= kMaxPreviewLength) {
        return description;
    }
    return description.substr(0, kMaxPreviewLength) + "...";
}

} // namespace

EventSearchResultsDialog::EventSearchResultsDialog(wxWindow* parent,
                                                   const wxString& keyword,
                                                   const std::vector<EventSearchResultItem>& results,
                                                   EditCallback onEdit)
    : wxDialog(parent,
               wxID_ANY,
               "Search results",
               wxDefaultPosition,
               wxSize(820, 640),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      onEdit_(std::move(onEdit)) {
    Build(keyword, results);
}

void EventSearchResultsDialog::Build(const wxString& keyword, const std::vector<EventSearchResultItem>& results) {
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(this, wxID_ANY, wxString::Format("Results for \"%s\"", keyword));
    title->SetFont(BoldFont(title, 2));
    headerSizer->Add(title, 1, wxALIGN_CENTER_VERTICAL);
    headerSizer->Add(new wxStaticText(this, wxID_ANY, wxString::Format("%zu event(s)", results.size())),
                     0,
                     wxALIGN_CENTER_VERTICAL);
    rootSizer->Add(headerSizer, 0, wxEXPAND | wxALL, 14);

    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetScrollRate(0, 10);
    scroll->SetBackgroundColour(wxColour(246, 248, 252));
    auto* listSizer = new wxBoxSizer(wxVERTICAL);

    if (results.empty()) {
        auto* empty = new wxStaticText(scroll, wxID_ANY, "No matching events were found.");
        listSizer->Add(empty, 0, wxALL, 16);
    }
    else {
        for (const auto& item : results) {
            AddEventBlock(scroll, listSizer, item);
        }
    }

    scroll->SetSizer(listSizer);
    rootSizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 14);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* closeButton = new wxButton(this, wxID_CANCEL, "Close");
    buttons->Add(closeButton, 0);
    rootSizer->Add(buttons, 0, wxEXPAND | wxALL, 14);

    SetSizer(rootSizer);
}

void EventSearchResultsDialog::AddEventBlock(wxWindow* parent, wxSizer* sizer, const EventSearchResultItem& item) {
    const Event& event = item.event;
    auto* block = new wxPanel(parent, wxID_ANY);
    block->SetBackgroundColour(wxColour(255, 255, 255));

    auto* blockSizer = new wxBoxSizer(wxVERTICAL);
    auto* topSizer = new wxBoxSizer(wxHORIZONTAL);

    auto* leftSizer = new wxBoxSizer(wxVERTICAL);
    const std::string titleText = event.title.empty() ? std::string("(Untitled)") : event.title;
    auto* titleSizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(block, wxID_ANY, Utf8(titleText));
    title->SetFont(BoldFont(title, 1));
    titleSizer->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    if (!item.calendarName.empty()) {
        auto* calendarLabel = new wxStaticText(block, wxID_ANY, Utf8(" in " + item.calendarName));
        calendarLabel->SetForegroundColour(wxColour(wxString::FromUTF8(NormalizeCalendarColor(item.calendarColorHex))));
        titleSizer->Add(calendarLabel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
    }
    leftSizer->Add(titleSizer, 0, wxBOTTOM, 4);
    leftSizer->Add(new wxStaticText(block, wxID_ANY, Utf8(FormatStartLabel(event))), 0, wxBOTTOM, 4);

    if (!event.location.empty()) {
        leftSizer->Add(new wxStaticText(block, wxID_ANY, wxString("Location: ") + Utf8(event.location)), 0, wxBOTTOM, 4);
    }

    const std::string recurrence = RecurrenceLabel(event);
    if (!recurrence.empty()) {
        leftSizer->Add(new wxStaticText(block, wxID_ANY, Utf8(recurrence)), 0, wxBOTTOM, 4);
    }

    topSizer->Add(leftSizer, 1, wxEXPAND | wxRIGHT, 16);

    auto* rightSizer = new wxBoxSizer(wxVERTICAL);
    const wxString startText = event.allDay
        ? wxString("All day")
        : Utf8(FormatTimeLabel(event.GetDisplayStartEpoch()));
    auto* startLabel = new wxStaticText(block, wxID_ANY, startText);
    startLabel->SetFont(BoldFont(startLabel));
    rightSizer->Add(startLabel, 0, wxALIGN_RIGHT | wxBOTTOM, 4);
    rightSizer->Add(new wxStaticText(block, wxID_ANY, Utf8(std::string("to ") + FormatEndLabel(event))),
                    0,
                    wxALIGN_RIGHT | wxBOTTOM,
                    8);

    if (item.editable) {
        auto* editButton = new wxButton(block, wxID_ANY, "Edit");
        editButton->Bind(wxEVT_BUTTON, [this, id = event.id](wxCommandEvent&) {
            if (onEdit_) {
                onEdit_(id);
            }
        });
        rightSizer->Add(editButton, 0, wxALIGN_RIGHT);
    }

    topSizer->Add(rightSizer, 0, wxEXPAND);
    blockSizer->Add(topSizer, 0, wxEXPAND);

    AddDescriptionPreview(block, blockSizer, event.description);
    AddExceptions(block, blockSizer, item);

    block->SetSizer(blockSizer);
    sizer->Add(block, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
}

void EventSearchResultsDialog::AddDescriptionPreview(wxWindow* parent, wxSizer* sizer, const std::string& description) {
    if (description.empty()) {
        return;
    }

    const bool truncated = description.size() > 180;
    auto* preview = new wxStaticText(parent, wxID_ANY, Utf8(DescriptionPreview(description)));
    preview->Wrap(720);
    preview->SetForegroundColour(wxColour(92, 98, 112));
    if (truncated) {
        preview->SetCursor(wxCursor(wxCURSOR_HAND));
        preview->Bind(wxEVT_LEFT_UP, [this, description](wxMouseEvent&) {
            wxMessageBox(Utf8(description), "Event description", wxOK | wxICON_INFORMATION, this);
        });
    }
    sizer->Add(preview, 0, wxEXPAND | wxTOP, 8);
}

void EventSearchResultsDialog::AddExceptions(wxWindow* parent, wxSizer* sizer, const EventSearchResultItem& item) {
    if (item.exceptions.empty()) {
        return;
    }

    sizer->Add(new wxStaticLine(parent), 0, wxEXPAND | wxTOP | wxBOTTOM, 8);
    auto* heading = new wxStaticText(parent, wxID_ANY, "Exceptions");
    heading->SetFont(BoldFont(heading));
    sizer->Add(heading, 0, wxBOTTOM, 4);

    std::vector<std::string> cancelled;
    std::vector<std::string> modified;
    for (const auto& exception : item.exceptions) {
        if (exception.type == RecurrenceOverrideType::CANCELLED) {
            cancelled.push_back(FormatEventDisplayDateTime(item.event, exception.originalStart));
        }
        else if (exception.replacement.has_value()) {
            modified.push_back(
                FormatEventDisplayDateTime(item.event, exception.originalStart) + " -> " +
                FormatEventDisplayDateTime(*exception.replacement));
        }
    }

    if (!cancelled.empty()) {
        sizer->Add(new wxStaticText(parent, wxID_ANY, Utf8("Cancelled: " + cancelled.front())),
                   0,
                   wxBOTTOM,
                   2);
        for (std::size_t index = 1; index < cancelled.size(); ++index) {
            sizer->Add(new wxStaticText(parent, wxID_ANY, Utf8("           " + cancelled[index])),
                       0,
                       wxBOTTOM,
                       2);
        }
    }

    if (!modified.empty()) {
        sizer->Add(new wxStaticText(parent, wxID_ANY, Utf8("Modified: " + modified.front())),
                   0,
                   wxBOTTOM,
                   2);
        for (std::size_t index = 1; index < modified.size(); ++index) {
            sizer->Add(new wxStaticText(parent, wxID_ANY, Utf8("          " + modified[index])),
                       0,
                       wxBOTTOM,
                       2);
        }
    }
}
