#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/scrolwin.h>

#include "models/event.h"
#include "ui/calendar_ui_shared.h"

class TimelineViewPanel final : public wxScrolledWindow {
public:
    explicit TimelineViewPanel(wxWindow* parent);

    void SetMode(CalendarViewMode mode);
    void SetRangeStart(long long rangeStartEpoch);
    void SetSelectedDay(long long selectedDayEpoch);
    void SetEvents(const std::vector<Event>& events);
    void SetEventDoubleClickHandler(std::function<void(long long)> handler);
    void SetEventRightClickHandler(std::function<void(long long)> handler);
    void SetEmptySlotClickHandler(std::function<void(long long, int)> handler);

private:
    struct TimelineSegment {
        long long eventId = -1;
        long long dayEpoch = 0;
        int startMinute = 0;
        int endMinute = 0;
        int column = 0;
        int columnCount = 1;
        bool allDay = false;
        std::string label;
        std::string colorHex;
    };

    struct HeaderSpanSegment {
        long long eventId = -1;
        int row = 0;
        int startDayIndex = 0;
        int endDayIndex = 0;
        std::string label;
        std::string colorHex;
        bool continuesBefore = false;
        bool continuesAfter = false;
    };

    int DayCount() const;
    int CurrentColumnWidth() const;
    int CurrentAllDayLaneHeight() const;
    int TotalCanvasHeight() const;
    int TimelineTop() const;
    void RefreshView();
    std::vector<Event> EventsForDay(long long dayEpoch) const;
    std::vector<HeaderSpanSegment> BuildHeaderSpans() const;
    void RebuildEventButtons();
    void OnHostResized(wxSizeEvent& event);
    void OnCanvasLeftUp(wxMouseEvent& event);
    void OnPaint(wxPaintEvent& event);

    CalendarViewMode mode_ = CalendarViewMode::DAY;
    long long rangeStartEpoch_ = 0;
    long long selectedDayEpoch_ = 0;
    std::uint64_t eventsFingerprint_ = 0;
    std::vector<Event> events_;
    std::function<void(long long)> eventDoubleClickHandler_;
    std::function<void(long long)> eventRightClickHandler_;
    std::function<void(long long, int)> emptySlotClickHandler_;
    wxPanel* canvas_ = nullptr;
    std::vector<class wxButton*> eventButtons_;
};
