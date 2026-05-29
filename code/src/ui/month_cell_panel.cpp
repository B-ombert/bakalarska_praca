#include "ui/month_cell_panel.h"

#include <functional>
#include <sstream>
#include <utility>

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "ui/calendar_ui_shared.h"
#include "utils/calendar_colors.h"

MonthCellPanel::MonthCellPanel(wxWindow* parent,
                               const int index,
                               std::function<void(int)> dayClicked,
                               std::function<void(int)> emptySpaceClicked,
                               std::function<void(long long)> eventDoubleClicked,
                               std::function<void(long long)> eventRightClicked)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(150, 132), wxBORDER_NONE),
      index_(index),
      dayClicked_(std::move(dayClicked)),
      emptySpaceClicked_(std::move(emptySpaceClicked)),
      eventDoubleClicked_(std::move(eventDoubleClicked)),
      eventRightClicked_(std::move(eventRightClicked)) {
    SetBackgroundColour(*wxWHITE);

    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    headerPanel_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 34));
    headerPanel_->SetMinSize(wxSize(-1, 34));
    headerPanel_->SetBackgroundColour(*wxWHITE);
    headerPanel_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (emptySpaceClicked_) {
            emptySpaceClicked_(index_);
        }
    });

    auto* headerSizer = new wxBoxSizer(wxHORIZONTAL);
    headerButton_ = new wxButton(headerPanel_, wxID_ANY, "", wxDefaultPosition, wxSize(28, 28), wxBORDER_NONE);
    headerButton_->SetMinSize(wxSize(28, 28));
    headerButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (dayClicked_) {
            dayClicked_(index_);
        }
    });
    headerButton_->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        dayButtonHovered_ = true;
        ApplyDayButtonStyle();
        event.Skip();
    });
    headerButton_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        dayButtonHovered_ = false;
        ApplyDayButtonStyle();
        event.Skip();
    });
    headerSizer->Add(headerButton_, 0, wxLEFT | wxTOP, 3);
    headerSizer->AddStretchSpacer();
    headerPanel_->SetSizer(headerSizer);
    rootSizer->Add(headerPanel_, 0, wxEXPAND);

    bodyPanel_ = new wxPanel(this);
    bodyPanel_->SetBackgroundColour(*wxWHITE);
    bodyPanel_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
        if (emptySpaceClicked_) {
            emptySpaceClicked_(index_);
        }
    });

    eventsSizer_ = new wxBoxSizer(wxVERTICAL);
    bodyPanel_->SetSizer(eventsSizer_);
    rootSizer->Add(bodyPanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 0);
    SetSizer(rootSizer);
}

namespace {

std::string BuildMonthSegmentLabel(const MonthCellEventSegment& segment) {
    return segment.label;
}

void BindEventActionsRecursive(wxWindow* window,
                               std::function<void()> doubleClickHandler,
                               std::function<void()> rightClickHandler) {
    window->Bind(wxEVT_LEFT_DCLICK, [handler = doubleClickHandler](wxMouseEvent&) {
        if (handler) {
            handler();
        }
    });
    window->Bind(wxEVT_RIGHT_UP, [handler = std::move(rightClickHandler)](wxMouseEvent&) {
        if (handler) {
            handler();
        }
    });
}

} // namespace

std::uint64_t MonthCellPanel::ComputeCellFingerprint(
    const long long dayEpoch,
    const int dayNumber,
    const bool inCurrentMonth,
    const bool isToday,
    const bool isSelected,
    const std::vector<std::optional<MonthCellEventSegment>>& eventRows) {
    auto combine = [](std::uint64_t& seed, const std::uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    };

    std::uint64_t seed = static_cast<std::uint64_t>(dayEpoch);
    combine(seed, static_cast<std::uint64_t>(dayNumber));
    combine(seed, inCurrentMonth ? 1 : 0);
    combine(seed, isToday ? 1 : 0);
    combine(seed, isSelected ? 1 : 0);
    combine(seed, eventRows.size());

    for (const auto& row : eventRows) {
        combine(seed, row.has_value() ? 1 : 0);
        if (!row.has_value()) {
            continue;
        }
        combine(seed, static_cast<std::uint64_t>(row->eventId));
        combine(seed, static_cast<std::uint64_t>(std::hash<std::string>{}(row->label)));
        combine(seed, static_cast<std::uint64_t>(std::hash<std::string>{}(row->colorHex)));
        combine(seed, row->continuesBefore ? 1 : 0);
        combine(seed, row->continuesAfter ? 1 : 0);
        combine(seed, row->isSummary ? 1 : 0);
    }

    return seed;
}

void MonthCellPanel::UpdateCell(const long long dayEpoch,
                                const int dayNumber,
                                const bool inCurrentMonth,
                                const bool isToday,
                                const bool isSelected,
                                const std::vector<std::optional<MonthCellEventSegment>>& eventRows) {
    const std::uint64_t newFingerprint =
        ComputeCellFingerprint(dayEpoch, dayNumber, inCurrentMonth, isToday, isSelected, eventRows);
    if (newFingerprint == fingerprint_) {
        return;
    }
    fingerprint_ = newFingerprint;
    dayEpoch_ = dayEpoch;

    std::ostringstream header;
    header << dayNumber;

    headerButton_->SetLabel(header.str());
    isToday_ = isToday;
    isSelected_ = isSelected;
    inCurrentMonth_ = inCurrentMonth;
    const wxColour cellColour = isSelected ? wxColour(239, 245, 255) : *wxWHITE;
    const wxColour headerColour = isSelected ? wxColour(239, 245, 255) : *wxWHITE;
    headerPanel_->SetBackgroundColour(headerColour);
    ApplyDayButtonStyle();

    eventsSizer_->Clear(true);
    eventWidgets_.clear();

    SetBackgroundColour(cellColour);
    bodyPanel_->SetBackgroundColour(cellColour);

    for (const auto& row : eventRows) {
        if (row.has_value()) {
            auto* eventPanel = new wxPanel(bodyPanel_, wxID_ANY, wxDefaultPosition, wxSize(-1, 22), wxBORDER_NONE);
            eventPanel->SetMinSize(wxSize(-1, 22));
            eventPanel->SetBackgroundColour(row->isSummary
                ? wxColour(245, 247, 250)
                : wxColour(wxString::FromUTF8(NormalizeCalendarColor(row->colorHex))));

            auto* eventSizer = new wxBoxSizer(wxHORIZONTAL);
            auto* eventLabel = new wxStaticText(
                eventPanel,
                wxID_ANY,
                wxString::FromUTF8(BuildMonthSegmentLabel(*row)),
                wxDefaultPosition,
                wxDefaultSize,
                wxST_ELLIPSIZE_END);
            eventLabel->SetForegroundColour(row->isSummary ? wxColour(95, 110, 124) : *wxWHITE);
            eventSizer->Add(eventLabel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 6);
            eventPanel->SetSizer(eventSizer);

            if (!row->isSummary) {
                auto doubleClickHandler = [handler = eventDoubleClicked_, id = row->eventId]() {
                    if (handler) {
                        handler(id);
                    }
                };
                auto rightClickHandler = [handler = eventRightClicked_, id = row->eventId]() {
                    if (handler) {
                        handler(id);
                    }
                };
                BindEventActionsRecursive(eventPanel, doubleClickHandler, rightClickHandler);
                BindEventActionsRecursive(eventLabel, doubleClickHandler, rightClickHandler);
            }

            auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);
            if (!row->continuesBefore) {
                rowSizer->AddSpacer(4);
            }
            rowSizer->Add(eventPanel, 1, wxEXPAND);
            if (!row->continuesAfter) {
                rowSizer->AddSpacer(4);
            }
            eventsSizer_->Add(rowSizer, 0, wxEXPAND | wxBOTTOM, 2);
            eventWidgets_.push_back(eventPanel);
        }
        else {
            auto* spacer = new wxPanel(bodyPanel_, wxID_ANY, wxDefaultPosition, wxSize(-1, 24));
            spacer->SetMinSize(wxSize(-1, 24));
            spacer->SetBackgroundColour(cellColour);
            eventsSizer_->Add(spacer, 0, wxEXPAND);
            eventWidgets_.push_back(spacer);
        }
    }

    Layout();
    Refresh();
}

void MonthCellPanel::ApplyDayButtonStyle() {
    const wxColour headerColour = isSelected_ ? wxColour(239, 245, 255) : *wxWHITE;
    wxColour background = headerColour;
    wxColour foreground = inCurrentMonth_ ? wxColour(32, 33, 36) : wxColour(128, 134, 139);

    if (isToday_) {
        background = wxColour(26, 115, 232);
        foreground = *wxWHITE;
    }
    else if (dayButtonHovered_) {
        background = wxColour(232, 240, 254);
        foreground = wxColour(32, 33, 36);
    }

    headerButton_->SetBackgroundColour(background);
    headerButton_->SetForegroundColour(foreground);
    headerButton_->Refresh();
}

void MonthCellPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    const wxSize size = GetClientSize();
    dc.SetBrush(wxBrush(GetBackgroundColour()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

    dc.SetPen(wxPen(wxColour(185, 194, 205), 1));
    dc.DrawLine(0, 0, size.GetWidth(), 0);
    dc.DrawLine(0, 0, 0, size.GetHeight());
    dc.DrawLine(size.GetWidth() - 1, 0, size.GetWidth() - 1, size.GetHeight());
    dc.DrawLine(0, size.GetHeight() - 1, size.GetWidth(), size.GetHeight() - 1);
}
