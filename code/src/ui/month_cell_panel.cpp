#include "ui/month_cell_panel.h"

#include <sstream>
#include <utility>

#include <wx/button.h>
#include <wx/sizer.h>

#include "ui/calendar_ui_shared.h"

MonthCellPanel::MonthCellPanel(wxWindow* parent,
                               const int index,
                               std::function<void(int)> dayClicked,
                               std::function<void(int)> emptySpaceClicked,
                               std::function<void(long long)> eventClicked)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(150, 132)),
      index_(index),
      dayClicked_(std::move(dayClicked)),
      emptySpaceClicked_(std::move(emptySpaceClicked)),
      eventClicked_(std::move(eventClicked)) {
    SetBackgroundColour(*wxWHITE);

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);
    headerButton_ = new wxButton(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
    headerButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (dayClicked_) {
            dayClicked_(index_);
        }
    });
    rootSizer->Add(headerButton_, 0, wxEXPAND | wxALL, 4);

    bodyPanel_ = new wxPanel(this);
    bodyPanel_->SetBackgroundColour(*wxWHITE);
    bodyPanel_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (emptySpaceClicked_) {
            emptySpaceClicked_(index_);
        }
    });

    eventsSizer_ = new wxBoxSizer(wxVERTICAL);
    bodyPanel_->SetSizer(eventsSizer_);
    rootSizer->Add(bodyPanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    SetSizer(rootSizer);
}

namespace {

std::string BuildMonthSegmentLabel(const MonthCellEventSegment& segment) {
    std::string label = segment.label;
    if (label.empty()) {
        if (segment.continuesBefore && segment.continuesAfter) {
            return "< >";
        }
        if (segment.continuesBefore) {
            return "<";
        }
        if (segment.continuesAfter) {
            return ">";
        }
        return "";
    }
    if (segment.continuesBefore) {
        label = "< " + label;
    }
    if (segment.continuesAfter) {
        label += " >";
    }
    return label;
}

} // namespace

void MonthCellPanel::UpdateCell(const long long dayEpoch,
                                const int dayNumber,
                                const bool inCurrentMonth,
                                const bool isToday,
                                const bool isSelected,
                                const std::vector<std::optional<MonthCellEventSegment>>& eventRows) {
    dayEpoch_ = dayEpoch;

    std::ostringstream header;
    if (isToday) {
        header << "Today ";
    }
    if (!inCurrentMonth) {
        header << "(" << dayNumber << ")";
    }
    else {
        header << dayNumber;
    }

    headerButton_->SetLabel(header.str());
    const wxColour selectionColour = isSelected ? wxColour(221, 235, 255) : wxColour(246, 248, 252);
    headerButton_->SetBackgroundColour(selectionColour);
    headerButton_->SetForegroundColour(inCurrentMonth ? wxColour(34, 34, 34) : wxColour(140, 140, 140));

    eventsSizer_->Clear(true);
    eventWidgets_.clear();

    const wxColour cellColour = isSelected ? wxColour(239, 245, 255) : *wxWHITE;
    SetBackgroundColour(cellColour);
    bodyPanel_->SetBackgroundColour(cellColour);

    for (const auto& row : eventRows) {
        if (row.has_value()) {
            auto* eventButton = new wxButton(bodyPanel_, wxID_ANY, wxString::FromUTF8(BuildMonthSegmentLabel(*row)),
                                             wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
            eventButton->SetMinSize(wxSize(-1, 24));
            eventButton->SetBackgroundColour(wxColour(214, 234, 248));
            eventButton->SetForegroundColour(wxColour(24, 52, 77));
            eventButton->Bind(wxEVT_BUTTON, [handler = eventClicked_, id = row->eventId](wxCommandEvent&) {
                if (handler) {
                    handler(id);
                }
            });
            eventsSizer_->Add(eventButton, 0, wxEXPAND | wxBOTTOM, 3);
            eventWidgets_.push_back(eventButton);
        }
        else {
            auto* spacer = new wxPanel(bodyPanel_, wxID_ANY, wxDefaultPosition, wxSize(-1, 24));
            spacer->SetMinSize(wxSize(-1, 24));
            spacer->SetBackgroundColour(cellColour);
            eventsSizer_->Add(spacer, 0, wxEXPAND | wxBOTTOM, 3);
            eventWidgets_.push_back(spacer);
        }
    }

    bodyPanel_->Layout();
    bodyPanel_->Refresh();
    bodyPanel_->Update();
    Layout();
    Refresh();
}
