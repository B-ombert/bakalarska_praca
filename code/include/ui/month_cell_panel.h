#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <utility>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/panel.h>

#include "models/event.h"

struct MonthCellEventSegment {
    long long eventId = -1;
    std::string label;
    std::string colorHex;
    bool continuesBefore = false;
    bool continuesAfter = false;
    bool isSummary = false;
};

class MonthCellPanel final : public wxPanel {
public:
    MonthCellPanel(wxWindow* parent,
                   int index,
                   std::function<void(int)> dayClicked,
                   std::function<void(int)> emptySpaceClicked,
                   std::function<void(long long)> eventClicked,
                   std::function<void(long long)> eventDoubleClicked,
                   std::function<void(long long)> eventRightClicked);

    void UpdateCell(long long dayEpoch,
                    int dayNumber,
                    bool inCurrentMonth,
                    bool isToday,
                    bool isSelected,
                    long long selectedEventId,
                    const std::vector<std::optional<MonthCellEventSegment>>& eventRows);
    void SetSelectedEventId(long long selectedEventId);

private:
    struct EventWidgetState {
        class wxWindow* window = nullptr;
        long long eventId = 0;
        wxColour baseColour;
    };

    static std::uint64_t ComputeCellFingerprint(long long dayEpoch,
                                                int dayNumber,
                                                bool inCurrentMonth,
                                                bool isToday,
                                                bool isSelected,
                                                const std::vector<std::optional<MonthCellEventSegment>>& eventRows);
    void ApplyDayButtonStyle();
    void ApplyEventSelection(long long selectedEventId);
    void OnPaint(wxPaintEvent& event);

    int index_ = -1;
    long long dayEpoch_ = 0;
    std::uint64_t fingerprint_ = 0;
    bool isToday_ = false;
    bool isSelected_ = false;
    bool inCurrentMonth_ = true;
    bool dayButtonHovered_ = false;
    long long selectedEventId_ = 0;
    std::function<void(int)> dayClicked_;
    std::function<void(int)> emptySpaceClicked_;
    std::function<void(long long)> eventClicked_;
    std::function<void(long long)> eventDoubleClicked_;
    std::function<void(long long)> eventRightClicked_;
    wxPanel* headerPanel_ = nullptr;
    class wxButton* headerButton_ = nullptr;
    wxPanel* bodyPanel_ = nullptr;
    class wxBoxSizer* eventsSizer_ = nullptr;
    std::vector<EventWidgetState> eventWidgets_;
};
