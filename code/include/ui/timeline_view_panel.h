#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/scrolwin.h>

#include "models/event.h"
#include "ui/calendar_ui_shared.h"

class TimelineViewPanel final : public wxScrolledWindow {
public:
    explicit TimelineViewPanel(wxWindow* parent);

    void SetMode(CalendarViewMode mode);
    void SetRangeStart(long long rangeStartEpoch);
    void SetSelectedDay(long long selectedDayEpoch);
    void SetSelectedEventId(long long selectedEventId);
    void SetEvents(const std::vector<Event>& events);
    void SetEventClickHandler(std::function<void(long long)> handler);
    void SetEventDoubleClickHandler(std::function<void(long long)> handler);
    void SetEventRightClickHandler(std::function<void(long long)> handler);
    void SetEmptySlotClickHandler(std::function<void(long long, int)> handler);
    void SetEmptyRangeDragHandler(std::function<void(long long, int, int)> handler);

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
    bool TimelineSlotFromPoint(const wxPoint& point, long long& dayEpoch, int& minuteOfDay) const;
    void RefreshView();
    std::vector<Event> EventsForDay(long long dayEpoch) const;
    std::vector<HeaderSpanSegment> BuildHeaderSpans() const;
    void RebuildEventButtons();
    void OnHostResized(wxSizeEvent& event);
    void OnCanvasLeftDown(wxMouseEvent& event);
    void OnCanvasLeftUp(wxMouseEvent& event);
    void OnCanvasMotion(wxMouseEvent& event);
    void OnPaint(wxPaintEvent& event);

    CalendarViewMode mode_ = CalendarViewMode::DAY;
    long long rangeStartEpoch_ = 0;
    long long selectedDayEpoch_ = 0;
    long long selectedEventId_ = 0;
    std::uint64_t eventsFingerprint_ = 0;
    std::vector<Event> events_;
    std::function<void(long long)> eventClickHandler_;
    std::function<void(long long)> eventDoubleClickHandler_;
    std::function<void(long long)> eventRightClickHandler_;
    std::function<void(long long, int)> emptySlotClickHandler_;
    std::function<void(long long, int, int)> emptyRangeDragHandler_;
    bool dragSelecting_ = false;
    bool dragMoved_ = false;
    long long dragDayEpoch_ = 0;
    int dragStartMinute_ = 0;
    int dragCurrentMinute_ = 0;
    wxPoint dragStartPoint_;
    wxPanel* canvas_ = nullptr;
    std::vector<class wxButton*> eventButtons_;
};
