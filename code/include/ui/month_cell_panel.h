#pragma once

#include <cstdint>
#include <optional>
#include <functional>
#include <string>
#include <vector>

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
                   std::function<void(long long)> eventDoubleClicked,
                   std::function<void(long long)> eventRightClicked);

    void UpdateCell(long long dayEpoch,
                    int dayNumber,
                    bool inCurrentMonth,
                    bool isToday,
                    bool isSelected,
                    const std::vector<std::optional<MonthCellEventSegment>>& eventRows);

private:
    static std::uint64_t ComputeCellFingerprint(long long dayEpoch,
                                                int dayNumber,
                                                bool inCurrentMonth,
                                                bool isToday,
                                                bool isSelected,
                                                const std::vector<std::optional<MonthCellEventSegment>>& eventRows);
    void ApplyDayButtonStyle();
    void OnPaint(wxPaintEvent& event);

    int index_ = -1;
    long long dayEpoch_ = 0;
    std::uint64_t fingerprint_ = 0;
    bool isToday_ = false;
    bool isSelected_ = false;
    bool inCurrentMonth_ = true;
    bool dayButtonHovered_ = false;
    std::function<void(int)> dayClicked_;
    std::function<void(int)> emptySpaceClicked_;
    std::function<void(long long)> eventDoubleClicked_;
    std::function<void(long long)> eventRightClicked_;
    wxPanel* headerPanel_ = nullptr;
    class wxButton* headerButton_ = nullptr;
    wxPanel* bodyPanel_ = nullptr;
    class wxBoxSizer* eventsSizer_ = nullptr;
    std::vector<class wxWindow*> eventWidgets_;
};
