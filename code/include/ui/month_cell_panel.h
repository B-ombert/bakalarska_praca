#pragma once

#include <functional>
#include <vector>

#include <wx/panel.h>

#include "models/event.h"

class MonthCellPanel final : public wxPanel {
public:
    MonthCellPanel(wxWindow* parent,
                   int index,
                   std::function<void(int)> dayClicked,
                   std::function<void(int)> emptySpaceClicked,
                   std::function<void(long long)> eventClicked);

    void UpdateCell(long long dayEpoch,
                    int dayNumber,
                    bool inCurrentMonth,
                    bool isToday,
                    bool isSelected,
                    const std::vector<Event>& dayEvents);

private:
    int index_ = -1;
    long long dayEpoch_ = 0;
    std::function<void(int)> dayClicked_;
    std::function<void(int)> emptySpaceClicked_;
    std::function<void(long long)> eventClicked_;
    class wxButton* headerButton_ = nullptr;
    wxPanel* bodyPanel_ = nullptr;
    class wxBoxSizer* eventsSizer_ = nullptr;
    std::vector<class wxButton*> eventButtons_;
};
