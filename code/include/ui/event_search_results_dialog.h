#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "models/event.h"
#include "models/recurrence_override.h"

struct EventSearchExceptionDisplay {
    RecurrenceOverrideType type = RecurrenceOverrideType::CANCELLED;
    long long originalStart = 0;
    std::optional<Event> replacement;
};

struct EventSearchResultItem {
    Event event;
    std::string calendarName;
    std::string calendarColorHex;
    bool editable = false;
    std::vector<EventSearchExceptionDisplay> exceptions;
};

class EventSearchResultsDialog final : public wxDialog {
public:
    using EditCallback = std::function<void(long long)>;

    EventSearchResultsDialog(wxWindow* parent,
                             const wxString& keyword,
                             const std::vector<EventSearchResultItem>& results,
                             EditCallback onEdit);

private:
    void Build(const wxString& keyword, const std::vector<EventSearchResultItem>& results);
    void AddEventBlock(class wxWindow* parent, class wxSizer* sizer, const EventSearchResultItem& item);
    void AddDescriptionPreview(class wxWindow* parent, class wxSizer* sizer, const std::string& description);
    void AddExceptions(class wxWindow* parent, class wxSizer* sizer, const EventSearchResultItem& item);

    EditCallback onEdit_;
};
