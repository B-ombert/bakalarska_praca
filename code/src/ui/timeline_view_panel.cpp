#include "ui/timeline_view_panel.h"

#include <algorithm>
#include <ctime>

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/panel.h>
#include <wx/sizer.h>

namespace {

std::string BuildHeaderSpanLabel(const Event& event, const bool continuesBefore, const bool continuesAfter) {
    std::string label = event.title.empty() ? "Event" : event.title;
    if (continuesBefore) {
        label = "< " + label;
    }
    if (continuesAfter) {
        label += " >";
    }
    return label;
}

bool UsesWeekHeaderSpan(const CalendarViewMode mode, const Event& event) {
    return mode == CalendarViewMode::WEEK && (event.allDay || SpansMultipleDays(event));
}

} // namespace

TimelineViewPanel::TimelineViewPanel(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                       wxVSCROLL | wxHSCROLL | wxBORDER_NONE) {
    SetScrollRate(16, 16);
    SetBackgroundColour(*wxWHITE);

    canvas_ = new wxPanel(this);
    canvas_->SetBackgroundStyle(wxBG_STYLE_PAINT);
    canvas_->Bind(wxEVT_PAINT, &TimelineViewPanel::OnPaint, this);
    canvas_->Bind(wxEVT_LEFT_UP, &TimelineViewPanel::OnCanvasLeftUp, this);
    Bind(wxEVT_SIZE, &TimelineViewPanel::OnHostResized, this);

    SetSizer(new wxBoxSizer(wxVERTICAL));
    GetSizer()->Add(canvas_, 1, wxEXPAND);
}

void TimelineViewPanel::SetMode(const CalendarViewMode mode) {
    mode_ = mode;
    RefreshView();
}

void TimelineViewPanel::SetRangeStart(const long long rangeStartEpoch) {
    rangeStartEpoch_ = rangeStartEpoch;
    RefreshView();
}

void TimelineViewPanel::SetSelectedDay(const long long selectedDayEpoch) {
    selectedDayEpoch_ = selectedDayEpoch;
    RefreshView();
}

void TimelineViewPanel::SetEvents(const std::vector<Event>& events) {
    events_ = events;
    RefreshView();
}

void TimelineViewPanel::SetEventClickHandler(std::function<void(long long)> handler) {
    eventClickHandler_ = std::move(handler);
}

void TimelineViewPanel::SetEmptySlotClickHandler(std::function<void(long long, int)> handler) {
    emptySlotClickHandler_ = std::move(handler);
}

int TimelineViewPanel::DayCount() const {
    return mode_ == CalendarViewMode::WEEK ? 7 : 1;
}

int TimelineViewPanel::CurrentColumnWidth() const {
    const int available = std::max(220, GetClientSize().GetWidth() - 18 - kTimelineTimeLabelWidth);
    return std::max(180, available / std::max(1, DayCount()));
}

int TimelineViewPanel::CurrentAllDayLaneHeight() const {
    int rowCount = kTimelineAllDayMinRows;

    if (mode_ == CalendarViewMode::WEEK) {
        for (const auto& span : BuildHeaderSpans()) {
            rowCount = std::max(rowCount, span.row + 1);
        }
    }
    else {
        for (int dayIndex = 0; dayIndex < DayCount(); ++dayIndex) {
            const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
            int dayAllDayCount = 0;

            for (const auto& event : events_) {
                if (event.deletedAt != 0 || !event.allDay) {
                    continue;
                }
                if (event.startDateTime < dayEpoch + kSecondsPerDay && event.endDateTime > dayEpoch) {
                    ++dayAllDayCount;
                }
            }

            rowCount = std::max(rowCount, dayAllDayCount);
        }
    }

    return kTimelineAllDayLanePadding * 2 + rowCount * kTimelineAllDayRowHeight;
}

int TimelineViewPanel::TotalCanvasHeight() const {
    return kTimelineHeaderHeight + CurrentAllDayLaneHeight() + 24 * kTimelineHourHeight;
}

int TimelineViewPanel::TimelineTop() const {
    return kTimelineHeaderHeight + CurrentAllDayLaneHeight();
}

void TimelineViewPanel::RefreshView() {
    const int dayColumnWidth = CurrentColumnWidth();
    const int canvasWidth = kTimelineTimeLabelWidth + DayCount() * dayColumnWidth + 8;
    const int canvasHeight = TotalCanvasHeight();

    canvas_->SetMinSize(wxSize(canvasWidth, canvasHeight));
    canvas_->SetSize(canvasWidth, canvasHeight);
    SetVirtualSize(canvasWidth, canvasHeight);

    RebuildEventButtons();
    canvas_->Refresh();
    canvas_->Update();
    Layout();
}

std::vector<Event> TimelineViewPanel::EventsForDay(const long long dayEpoch) const {
    std::vector<Event> results;
    const long long dayEnd = dayEpoch + kSecondsPerDay;

    for (const auto& event : events_) {
        if (event.deletedAt != 0) {
            continue;
        }
        if (event.startDateTime < dayEnd && event.endDateTime > dayEpoch) {
            results.push_back(event);
        }
    }

    return results;
}

std::vector<TimelineViewPanel::HeaderSpanSegment> TimelineViewPanel::BuildHeaderSpans() const {
    std::vector<HeaderSpanSegment> spans;
    if (mode_ != CalendarViewMode::WEEK) {
        return spans;
    }

    const long long rangeEndEpoch = rangeStartEpoch_ + DayCount() * kSecondsPerDay;
    std::vector<Event> spanEvents;
    for (const auto& event : events_) {
        if (event.deletedAt != 0 || !UsesWeekHeaderSpan(mode_, event)) {
            continue;
        }
        if (event.startDateTime < rangeEndEpoch && event.endDateTime > rangeStartEpoch_) {
            spanEvents.push_back(event);
        }
    }

    std::sort(spanEvents.begin(), spanEvents.end(), [](const Event& lhs, const Event& rhs) {
        if (lhs.startDateTime != rhs.startDateTime) {
            return lhs.startDateTime < rhs.startDateTime;
        }
        return lhs.endDateTime < rhs.endDateTime;
    });

    std::vector<std::vector<HeaderSpanSegment>> rows;
    for (const auto& event : spanEvents) {
        const bool continuesBefore = event.startDateTime < rangeStartEpoch_;
        const bool continuesAfter = event.endDateTime > rangeEndEpoch;
        const long long clippedStartDay = std::max(StartOfUtcDay(event.startDateTime), rangeStartEpoch_);
        const long long clippedEndDay = std::min(EventDisplayEndDay(event), rangeStartEpoch_ + (DayCount() - 1LL) * kSecondsPerDay);

        HeaderSpanSegment span;
        span.eventId = event.id;
        span.startDayIndex = static_cast<int>((clippedStartDay - rangeStartEpoch_) / kSecondsPerDay);
        span.endDayIndex = static_cast<int>((clippedEndDay - rangeStartEpoch_) / kSecondsPerDay);
        span.continuesBefore = continuesBefore;
        span.continuesAfter = continuesAfter;
        span.label = BuildHeaderSpanLabel(event, continuesBefore, continuesAfter);

        int assignedRow = 0;
        for (; assignedRow < static_cast<int>(rows.size()); ++assignedRow) {
            bool overlapsExisting = false;
            for (const auto& existing : rows[assignedRow]) {
                if (!(span.endDayIndex < existing.startDayIndex || span.startDayIndex > existing.endDayIndex)) {
                    overlapsExisting = true;
                    break;
                }
            }
            if (!overlapsExisting) {
                break;
            }
        }

        if (assignedRow == static_cast<int>(rows.size())) {
            rows.emplace_back();
        }
        span.row = assignedRow;
        rows[assignedRow].push_back(span);
        spans.push_back(span);
    }

    return spans;
}

void TimelineViewPanel::RebuildEventButtons() {
    for (auto* button : eventButtons_) {
        if (button != nullptr) {
            button->Destroy();
        }
    }
    eventButtons_.clear();

    const int dayColumnWidth = CurrentColumnWidth();
    const int usableWidth = dayColumnWidth - 8;

    const auto headerSpans = BuildHeaderSpans();
    if (mode_ == CalendarViewMode::WEEK) {
        for (const auto& span : headerSpans) {
            const int x = kTimelineTimeLabelWidth + span.startDayIndex * dayColumnWidth + 4;
            const int width = std::max(44, (span.endDayIndex - span.startDayIndex + 1) * dayColumnWidth - 8);
            const int y = kTimelineHeaderHeight + kTimelineAllDayLanePadding + span.row * kTimelineAllDayRowHeight;
            auto* button = new wxButton(canvas_, wxID_ANY, span.label,
                                        wxPoint(x, y),
                                        wxSize(width, kTimelineAllDayRowHeight - 4), wxBU_LEFT);
            button->SetBackgroundColour(wxColour(214, 234, 248));
            button->SetForegroundColour(wxColour(24, 52, 77));
            button->Bind(wxEVT_BUTTON, [handler = eventClickHandler_, id = span.eventId](wxCommandEvent&) {
                if (handler) {
                    handler(id);
                }
            });
            eventButtons_.push_back(button);
        }
    }

    for (int dayIndex = 0; dayIndex < DayCount(); ++dayIndex) {
        const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
        auto dayEvents = EventsForDay(dayEpoch);

        std::vector<TimelineSegment> timedSegments;
        std::vector<TimelineSegment> allDaySegments;

        for (const auto& event : dayEvents) {
            if (event.allDay && mode_ != CalendarViewMode::WEEK) {
                TimelineSegment segment;
                segment.eventId = event.id;
                segment.dayEpoch = dayEpoch;
                segment.allDay = true;
                segment.label = BuildTimelineEventLabel(event, dayEpoch);
                allDaySegments.push_back(segment);
                continue;
            }
            if (UsesWeekHeaderSpan(mode_, event)) {
                continue;
            }

            TimelineSegment segment;
            segment.eventId = event.id;
            segment.dayEpoch = dayEpoch;
            segment.startMinute = std::max(0, static_cast<int>((std::max(event.startDateTime, dayEpoch) - dayEpoch) / 60));
            segment.endMinute = std::min(kMinutesPerDay, static_cast<int>((std::min(event.endDateTime, dayEpoch + kSecondsPerDay) - dayEpoch + 59) / 60));
            segment.endMinute = std::max(segment.startMinute + 15, segment.endMinute);
            segment.label = BuildTimelineEventLabel(event, dayEpoch);
            timedSegments.push_back(segment);
        }

        std::sort(timedSegments.begin(), timedSegments.end(),
                  [](const TimelineSegment& lhs, const TimelineSegment& rhs) {
                      if (lhs.startMinute != rhs.startMinute) {
                          return lhs.startMinute < rhs.startMinute;
                      }
                      return lhs.endMinute < rhs.endMinute;
                  });

        std::vector<size_t> activeIndices;
        for (size_t index = 0; index < timedSegments.size(); ++index) {
            activeIndices.erase(
                std::remove_if(activeIndices.begin(), activeIndices.end(),
                               [&timedSegments, index](const size_t activeIndex) {
                                   return timedSegments[activeIndex].endMinute <= timedSegments[index].startMinute;
                               }),
                activeIndices.end());

            int column = 0;
            while (true) {
                bool used = false;
                for (const size_t activeIndex : activeIndices) {
                    if (timedSegments[activeIndex].column == column) {
                        used = true;
                        break;
                    }
                }
                if (!used) {
                    break;
                }
                ++column;
            }

            timedSegments[index].column = column;
            activeIndices.push_back(index);
        }

        for (size_t i = 0; i < timedSegments.size(); ++i) {
            int overlapColumns = timedSegments[i].column + 1;
            for (size_t j = 0; j < timedSegments.size(); ++j) {
                if (i == j) {
                    continue;
                }
                const bool overlaps = timedSegments[i].startMinute < timedSegments[j].endMinute &&
                                      timedSegments[i].endMinute > timedSegments[j].startMinute;
                if (overlaps) {
                    overlapColumns = std::max(overlapColumns, timedSegments[j].column + 1);
                }
            }
            timedSegments[i].columnCount = std::max(1, overlapColumns);
        }

        const int dayX = kTimelineTimeLabelWidth + dayIndex * dayColumnWidth;

        for (size_t index = 0; index < allDaySegments.size(); ++index) {
            auto* button = new wxButton(canvas_, wxID_ANY, allDaySegments[index].label,
                                        wxPoint(dayX + 4,
                                                kTimelineHeaderHeight + kTimelineAllDayLanePadding +
                                                static_cast<int>(index) * kTimelineAllDayRowHeight),
                                        wxSize(usableWidth, kTimelineAllDayRowHeight - 4), wxBU_LEFT);
            button->SetBackgroundColour(wxColour(214, 234, 248));
            button->SetForegroundColour(wxColour(24, 52, 77));
            button->Bind(wxEVT_BUTTON, [handler = eventClickHandler_, id = allDaySegments[index].eventId](wxCommandEvent&) {
                if (handler) {
                    handler(id);
                }
            });
            eventButtons_.push_back(button);
        }

        for (const auto& segment : timedSegments) {
            const int columnWidth = std::max(48, usableWidth / segment.columnCount);
            const int x = dayX + 4 + segment.column * columnWidth;
            const int y = TimelineTop() + (segment.startMinute * kTimelineHourHeight) / 60;
            const int height = std::max(24, ((segment.endMinute - segment.startMinute) * kTimelineHourHeight) / 60);
            const int width = std::max(44, columnWidth - 4);

            auto* button = new wxButton(canvas_, wxID_ANY, segment.label,
                                        wxPoint(x, y), wxSize(width, height), wxBU_LEFT);
            button->SetBackgroundColour(wxColour(187, 222, 251));
            button->SetForegroundColour(wxColour(16, 49, 80));
            button->Bind(wxEVT_BUTTON, [handler = eventClickHandler_, id = segment.eventId](wxCommandEvent&) {
                if (handler) {
                    handler(id);
                }
            });
            eventButtons_.push_back(button);
        }
    }
}

void TimelineViewPanel::OnHostResized(wxSizeEvent& event) {
    RefreshView();
    event.Skip();
}

void TimelineViewPanel::OnCanvasLeftUp(wxMouseEvent& event) {
    if (!emptySlotClickHandler_) {
        event.Skip();
        return;
    }

    const wxPoint point = event.GetPosition();
    if (point.x < kTimelineTimeLabelWidth || point.y < TimelineTop()) {
        event.Skip();
        return;
    }

    canvas_->Layout();
    canvas_->Refresh();
    const int dayColumnWidth = CurrentColumnWidth();
    const int dayIndex = std::clamp((point.x - kTimelineTimeLabelWidth) / dayColumnWidth, 0, DayCount() - 1);
    const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
    const int minutesFromTop = std::clamp(((point.y - TimelineTop()) * 60) / kTimelineHourHeight, 0, kMinutesPerDay - 1);
    const int hour = minutesFromTop / 60;
    const int minute = (minutesFromTop % 60) < 30 ? 0 : 30;
    emptySlotClickHandler_(dayEpoch, hour * 60 + minute);
}

void TimelineViewPanel::OnPaint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(canvas_);
    dc.SetBackground(wxBrush(*wxWHITE));
    dc.Clear();

    const int dayColumnWidth = CurrentColumnWidth();
    const int canvasWidth = canvas_->GetSize().GetWidth();
    const int canvasHeight = canvas_->GetSize().GetHeight();
    const int timelineTop = TimelineTop();
    const long long today = StartOfUtcDay(std::time(nullptr));

    for (int dayIndex = 0; dayIndex < DayCount(); ++dayIndex) {
        const int x = kTimelineTimeLabelWidth + dayIndex * dayColumnWidth;
        const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
        const bool isToday = IsSameUtcDay(dayEpoch, today);
        const bool isSelected = mode_ == CalendarViewMode::DAY && IsSameUtcDay(dayEpoch, selectedDayEpoch_);

        if (isToday || isSelected) {
            dc.SetBrush(wxBrush(isSelected ? wxColour(232, 240, 254) : wxColour(244, 248, 255)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x + 1, 1, dayColumnWidth - 2, canvasHeight - 2);
        }
    }

    dc.SetPen(wxPen(wxColour(225, 230, 236)));
    dc.SetTextForeground(wxColour(80, 80, 80));

    for (int hour = 0; hour <= 24; ++hour) {
        const int y = timelineTop + hour * kTimelineHourHeight;
        dc.DrawLine(kTimelineTimeLabelWidth, y, canvasWidth, y);
        if (hour < 24) {
            dc.DrawText(wxString::Format("%02d:00", hour), 10, y - 8);
        }
    }

    dc.DrawLine(kTimelineTimeLabelWidth, 0, kTimelineTimeLabelWidth, canvasHeight);
    dc.DrawLine(0, kTimelineHeaderHeight, canvasWidth, kTimelineHeaderHeight);
    dc.DrawLine(0, timelineTop, canvasWidth, timelineTop);

    for (int dayIndex = 0; dayIndex < DayCount(); ++dayIndex) {
        const int x = kTimelineTimeLabelWidth + dayIndex * dayColumnWidth;
        const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
        dc.DrawLine(x, 0, x, canvasHeight);
        dc.DrawText(FormatShortDayHeader(dayEpoch), x + 8, 16);
    }

    dc.DrawLine(kTimelineTimeLabelWidth + DayCount() * dayColumnWidth, 0,
                kTimelineTimeLabelWidth + DayCount() * dayColumnWidth, canvasHeight);
}
