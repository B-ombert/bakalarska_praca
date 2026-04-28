#include "ui/local_calendar_app.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wx.h>

#include "models/calendar.h"
#include "models/event.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "utils/datetime_utils.h"

namespace {

constexpr int kMonthCellCount = 42;
constexpr int kMinutesPerDay = 24 * 60;
constexpr int kSecondsPerDay = 86400;
constexpr int kTimelineHourHeight = 80;
constexpr int kTimelineHeaderHeight = 52;
constexpr int kTimelineAllDayLanePadding = 8;
constexpr int kTimelineAllDayRowHeight = 30;
constexpr int kTimelineAllDayMinRows = 2;
constexpr int kTimelineTimeLabelWidth = 74;

enum class CalendarViewMode {
    MONTH = 0,
    WEEK = 1,
    DAY = 2
};

std::string FormatMonthTitle(const int year, const int month) {
    static const std::array<const char*, 12> months = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };

    return std::string(months[month - 1]) + " " + std::to_string(year);
}

std::string FormatDayHeader(const long long dayEpoch) {
    const std::tm tm = EpochToUtcTm(dayEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%A, %d %B %Y");
    return output.str();
}

std::string FormatShortDayHeader(const long long dayEpoch) {
    const std::tm tm = EpochToUtcTm(dayEpoch);
    std::ostringstream output;
    output << std::put_time(&tm, "%a %d %b");
    return output.str();
}

std::string FormatWeekTitle(const long long weekStartEpoch) {
    const long long weekEndEpoch = weekStartEpoch + 6 * kSecondsPerDay;
    const std::tm startTm = EpochToUtcTm(weekStartEpoch);
    const std::tm endTm = EpochToUtcTm(weekEndEpoch);

    std::ostringstream output;
    output << std::put_time(&startTm, "%d %b");
    if (startTm.tm_mon != endTm.tm_mon || startTm.tm_year != endTm.tm_year) {
        output << " - " << std::put_time(&endTm, "%d %b %Y");
    }
    else {
        output << " - " << std::put_time(&endTm, "%d %b %Y");
    }
    return output.str();
}

std::string FormatTimeLabel(const long long epoch) {
    return FormatUtcDateTimeInput(epoch, false).substr(11, 5);
}

std::string BuildTimelineEventLabel(const Event& event, const long long dayEpoch) {
    if (event.allDay) {
        return event.title.empty() ? "All day" : event.title;
    }

    const long long clippedStart = std::max(event.startDateTime, dayEpoch);
    const long long clippedEnd = std::min(event.endDateTime, dayEpoch + kSecondsPerDay);

    std::ostringstream output;
    output << event.title << "\n" << FormatTimeLabel(clippedStart) << " - " << FormatTimeLabel(clippedEnd);
    return output.str();
}

std::string BuildMonthEventLabel(const Event& event) {
    if (event.allDay) {
        return event.title;
    }
    return event.title + " " + FormatTimeLabel(event.startDateTime);
}

void NormalizeAllDayEventRange(Event& event) {
    if (!event.allDay) {
        return;
    }

    event.startDateTime = StartOfUtcDay(event.startDateTime);
    event.endDateTime = StartOfUtcDay(event.endDateTime);

    if (event.endDateTime <= event.startDateTime) {
        event.endDateTime = event.startDateTime + kSecondsPerDay;
    }
}

int DaysInMonth(const int year, const int month) {
    static const std::array<int, 12> daysPerMonth = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month != 2) {
        return daysPerMonth[month - 1];
    }

    const bool leapYear = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leapYear ? 29 : 28;
}

int MonthGridOffset(const int year, const int month) {
    const long long firstDayEpoch = MakeUtcEpoch(year, month, 1);
    const std::tm tm = EpochToUtcTm(firstDayEpoch);
    return (tm.tm_wday + 6) % 7;
}

bool IsSameUtcDay(const long long lhs, const long long rhs) {
    return StartOfUtcDay(lhs) == StartOfUtcDay(rhs);
}

long long StartOfUtcWeek(const long long epoch) {
    const long long dayEpoch = StartOfUtcDay(epoch);
    const std::tm tm = EpochToUtcTm(dayEpoch);
    const int mondayOffset = (tm.tm_wday + 6) % 7;
    return dayEpoch - static_cast<long long>(mondayOffset) * kSecondsPerDay;
}

std::string MakeLocalProviderEventId(const long long startDateTime) {
    const auto now = static_cast<long long>(std::time(nullptr));
    return "local-" + std::to_string(now) + "-" + std::to_string(startDateTime);
}

struct EventDraftDefaults {
    long long startDateTime = 0;
    long long endDateTime = 0;
    bool allDay = false;
};

class EventEditorDialog final : public wxDialog {
public:
    EventEditorDialog(wxWindow* parent,
                      const std::optional<Event>& event,
                      const long long defaultDayEpoch,
                      const std::optional<EventDraftDefaults>& defaults = std::nullopt)
        : wxDialog(parent, wxID_ANY, event.has_value() ? "Edit Event" : "New Event",
                   wxDefaultPosition, wxSize(460, 470),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          originalEvent_(event),
          defaultDayEpoch_(defaultDayEpoch),
          defaults_(defaults) {
        BuildLayout();
        LoadInitialValues();
    }

    bool IsDeleteRequested() const {
        return deleteRequested_;
    }

    std::optional<Event> BuildEvent(const long long calendarId) const {
        Event event{};

        if (originalEvent_.has_value()) {
            event = *originalEvent_;
        }

        event.calendarId = calendarId;
        event.title = titleCtrl_->GetValue().ToStdString();
        event.location = locationCtrl_->GetValue().ToStdString();
        event.description = descriptionCtrl_->GetValue().ToStdString();
        event.allDay = allDayCtrl_->GetValue();
        event.status = "confirmed";
        event.type = EventType::SINGLE;
        event.deletedAt = 0;
        event.syncStatus = SYNCED;
        event.lastModified = std::time(nullptr);
        event.updatedAt = event.lastModified;
        if (event.createdAt == 0) {
            event.createdAt = event.lastModified;
        }

        event.startDateTime = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), event.allDay);
        event.endDateTime = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), event.allDay);
        NormalizeAllDayEventRange(event);

        if (!originalEvent_.has_value()) {
            event.instanceStart = event.startDateTime;
            event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
        }

        if (!ValidateEvent(event)) {
            return std::nullopt;
        }

        return event;
    }

private:
    void BuildLayout() {
        auto* rootSizer = new wxBoxSizer(wxVERTICAL);

        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Title"), 0, wxLEFT | wxRIGHT | wxTOP, 12);
        titleCtrl_ = new wxTextCtrl(this, wxID_ANY);
        rootSizer->Add(titleCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Location"), 0, wxLEFT | wxRIGHT, 12);
        locationCtrl_ = new wxTextCtrl(this, wxID_ANY);
        rootSizer->Add(locationCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        allDayCtrl_ = new wxCheckBox(this, wxID_ANY, "All day");
        rootSizer->Add(allDayCtrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Start (UTC)"), 0, wxLEFT | wxRIGHT, 12);
        startCtrl_ = new wxTextCtrl(this, wxID_ANY);
        rootSizer->Add(startCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(new wxStaticText(this, wxID_ANY, "End (UTC)"), 0, wxLEFT | wxRIGHT, 12);
        endCtrl_ = new wxTextCtrl(this, wxID_ANY);
        rootSizer->Add(endCtrl_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(new wxStaticText(this, wxID_ANY, "Description"), 0, wxLEFT | wxRIGHT, 12);
        descriptionCtrl_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 140), wxTE_MULTILINE);
        rootSizer->Add(descriptionCtrl_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

        auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
        if (originalEvent_.has_value()) {
            auto* deleteButton = new wxButton(this, wxID_ANY, "Delete");
            deleteButton->Bind(wxEVT_BUTTON, &EventEditorDialog::OnDelete, this);
            buttonSizer->Add(deleteButton, 0, wxRIGHT, 8);
        }

        auto* cancelButton = new wxButton(this, wxID_CANCEL, "Cancel");
        auto* saveButton = new wxButton(this, wxID_OK, originalEvent_.has_value() ? "Save changes" : "Create event");
        saveButton->Bind(wxEVT_BUTTON, &EventEditorDialog::OnSave, this);

        buttonSizer->AddStretchSpacer();
        buttonSizer->Add(cancelButton, 0, wxRIGHT, 8);
        buttonSizer->Add(saveButton, 0);

        rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(rootSizer);

        allDayCtrl_->Bind(wxEVT_CHECKBOX, &EventEditorDialog::OnAllDayChanged, this);
    }

    void LoadInitialValues() {
        if (originalEvent_.has_value()) {
            const Event& event = *originalEvent_;
            titleCtrl_->SetValue(event.title);
            locationCtrl_->SetValue(event.location);
            descriptionCtrl_->SetValue(event.description);
            allDayCtrl_->SetValue(event.allDay);
            startCtrl_->SetValue(FormatUtcDateTimeInput(event.startDateTime, event.allDay));
            endCtrl_->SetValue(FormatUtcDateTimeInput(event.endDateTime, event.allDay));
            return;
        }

        titleCtrl_->SetValue("");
        locationCtrl_->SetValue("");
        descriptionCtrl_->SetValue("");
        const EventDraftDefaults defaults = defaults_.value_or(
            EventDraftDefaults{defaultDayEpoch_ + 9 * 3600, defaultDayEpoch_ + 10 * 3600, false});
        allDayCtrl_->SetValue(defaults.allDay);
        startCtrl_->SetValue(FormatUtcDateTimeInput(defaults.startDateTime, defaults.allDay));
        endCtrl_->SetValue(FormatUtcDateTimeInput(defaults.endDateTime, defaults.allDay));
    }

    bool ValidateEvent(const Event& event) const {
        if (event.title.empty()) {
            wxMessageBox("Title is required.", "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
            return false;
        }

        if (event.startDateTime < 0 || event.endDateTime < 0) {
            wxMessageBox("Use YYYY-MM-DD for all-day events or YYYY-MM-DD HH:MM for timed events.",
                         "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
            return false;
        }

        if (event.endDateTime < event.startDateTime) {
            wxMessageBox("End must be after start.", "Validation", wxOK | wxICON_WARNING, const_cast<EventEditorDialog*>(this));
            return false;
        }

        return true;
    }

    void OnAllDayChanged(wxCommandEvent&) {
        const bool allDay = allDayCtrl_->GetValue();
        const long long startEpoch = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), false);
        const long long endEpoch = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), false);

        if (startEpoch >= 0) {
            startCtrl_->SetValue(FormatUtcDateTimeInput(startEpoch, allDay));
        }
        if (endEpoch >= 0) {
            endCtrl_->SetValue(FormatUtcDateTimeInput(endEpoch, allDay));
        }
    }

    void OnSave(wxCommandEvent&) {
        if (!BuildEvent(0).has_value()) {
            return;
        }

        deleteRequested_ = false;
        EndModal(wxID_OK);
    }

    void OnDelete(wxCommandEvent&) {
        if (wxMessageBox("Delete this event?", "Confirm delete", wxYES_NO | wxICON_WARNING, this) != wxYES) {
            return;
        }

        deleteRequested_ = true;
        EndModal(wxID_OK);
    }

    std::optional<Event> originalEvent_;
    long long defaultDayEpoch_ = 0;
    std::optional<EventDraftDefaults> defaults_;
    bool deleteRequested_ = false;

    wxTextCtrl* titleCtrl_ = nullptr;
    wxTextCtrl* locationCtrl_ = nullptr;
    wxTextCtrl* startCtrl_ = nullptr;
    wxTextCtrl* endCtrl_ = nullptr;
    wxTextCtrl* descriptionCtrl_ = nullptr;
    wxCheckBox* allDayCtrl_ = nullptr;
};

class MonthCellPanel final : public wxPanel {
public:
    MonthCellPanel(wxWindow* parent,
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

    void UpdateCell(const long long dayEpoch,
                    const int dayNumber,
                    const bool inCurrentMonth,
                    const bool isToday,
                    const bool isSelected,
                    const std::vector<Event>& dayEvents) {
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
        eventButtons_.clear();

        const wxColour cellColour = isSelected ? wxColour(239, 245, 255) : *wxWHITE;
        SetBackgroundColour(cellColour);
        bodyPanel_->SetBackgroundColour(cellColour);

        for (const auto& event : dayEvents) {
            auto* eventButton = new wxButton(bodyPanel_, wxID_ANY, BuildMonthEventLabel(event),
                                             wxDefaultPosition, wxDefaultSize, wxBU_LEFT);
            eventButton->SetMinSize(wxSize(-1, 24));
            eventButton->SetBackgroundColour(wxColour(214, 234, 248));
            eventButton->SetForegroundColour(wxColour(24, 52, 77));
            eventButton->Bind(wxEVT_BUTTON, [handler = eventClicked_, id = event.id](wxCommandEvent&) {
                if (handler) {
                    handler(id);
                }
            });
            eventsSizer_->Add(eventButton, 0, wxEXPAND | wxBOTTOM, 3);
            eventButtons_.push_back(eventButton);
        }

        Layout();
    }

private:
    int index_ = -1;
    long long dayEpoch_ = 0;
    std::function<void(int)> dayClicked_;
    std::function<void(int)> emptySpaceClicked_;
    std::function<void(long long)> eventClicked_;
    wxButton* headerButton_ = nullptr;
    wxPanel* bodyPanel_ = nullptr;
    wxBoxSizer* eventsSizer_ = nullptr;
    std::vector<wxButton*> eventButtons_;
};

class TimelineViewPanel final : public wxScrolledWindow {
public:
    explicit TimelineViewPanel(wxWindow* parent)
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

    void SetMode(const CalendarViewMode mode) {
        mode_ = mode;
        RefreshView();
    }

    void SetRangeStart(const long long rangeStartEpoch) {
        rangeStartEpoch_ = rangeStartEpoch;
        RefreshView();
    }

    void SetSelectedDay(const long long selectedDayEpoch) {
        selectedDayEpoch_ = selectedDayEpoch;
        RefreshView();
    }

    void SetEvents(const std::vector<Event>& events) {
        events_ = events;
        RefreshView();
    }

    void SetEventClickHandler(std::function<void(long long)> handler) {
        eventClickHandler_ = std::move(handler);
    }

    void SetEmptySlotClickHandler(std::function<void(long long, int)> handler) {
        emptySlotClickHandler_ = std::move(handler);
    }

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
    };

    int DayCount() const {
        return mode_ == CalendarViewMode::WEEK ? 7 : 1;
    }

    int CurrentColumnWidth() const {
        const int available = std::max(220, GetClientSize().GetWidth() - 18 - kTimelineTimeLabelWidth);
        return std::max(180, available / std::max(1, DayCount()));
    }

    int TotalCanvasHeight() const {
        return kTimelineHeaderHeight + CurrentAllDayLaneHeight() + 24 * kTimelineHourHeight;
    }

    int TimelineTop() const {
        return kTimelineHeaderHeight + CurrentAllDayLaneHeight();
    }

    int CurrentAllDayLaneHeight() const {
        int maxAllDayEvents = 0;
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
            maxAllDayEvents = std::max(maxAllDayEvents, dayAllDayCount);
        }

        const int rowCount = std::max(kTimelineAllDayMinRows, maxAllDayEvents);
        return kTimelineAllDayLanePadding * 2 + rowCount * kTimelineAllDayRowHeight;
    }

    void RefreshView() {
        const int dayColumnWidth = CurrentColumnWidth();
        const int canvasWidth = kTimelineTimeLabelWidth + DayCount() * dayColumnWidth + 8;
        const int canvasHeight = TotalCanvasHeight();

        canvas_->SetMinSize(wxSize(canvasWidth, canvasHeight));
        canvas_->SetSize(canvasWidth, canvasHeight);
        SetVirtualSize(canvasWidth, canvasHeight);

        RebuildEventButtons();
        canvas_->Refresh();
        Layout();
    }

    std::vector<Event> EventsForDay(const long long dayEpoch) const {
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

    void RebuildEventButtons() {
        for (auto* button : eventButtons_) {
            if (button != nullptr) {
                button->Destroy();
            }
        }
        eventButtons_.clear();

        const int dayColumnWidth = CurrentColumnWidth();
        const int usableWidth = dayColumnWidth - 8;

        for (int dayIndex = 0; dayIndex < DayCount(); ++dayIndex) {
            const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
            auto dayEvents = EventsForDay(dayEpoch);

            std::vector<TimelineSegment> timedSegments;
            std::vector<TimelineSegment> allDaySegments;

            for (const auto& event : dayEvents) {
                if (event.allDay) {
                    TimelineSegment segment;
                    segment.eventId = event.id;
                    segment.dayEpoch = dayEpoch;
                    segment.startMinute = 0;
                    segment.endMinute = 0;
                    segment.allDay = true;
                    segment.label = BuildTimelineEventLabel(event, dayEpoch);
                    allDaySegments.push_back(segment);
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

    void OnHostResized(wxSizeEvent& event) {
        RefreshView();
        event.Skip();
    }

    void OnCanvasLeftUp(wxMouseEvent& event) {
        if (!emptySlotClickHandler_) {
            event.Skip();
            return;
        }

        const wxPoint point = event.GetPosition();
        if (point.x < kTimelineTimeLabelWidth || point.y < TimelineTop()) {
            event.Skip();
            return;
        }

        const int dayColumnWidth = CurrentColumnWidth();
        const int dayIndex = std::clamp((point.x - kTimelineTimeLabelWidth) / dayColumnWidth, 0, DayCount() - 1);
        const long long dayEpoch = rangeStartEpoch_ + static_cast<long long>(dayIndex) * kSecondsPerDay;
        const int minutesFromTop = std::clamp(((point.y - TimelineTop()) * 60) / kTimelineHourHeight, 0, kMinutesPerDay - 1);
        const int hour = minutesFromTop / 60;
        const int minute = (minutesFromTop % 60) < 30 ? 0 : 30;
        emptySlotClickHandler_(dayEpoch, hour * 60 + minute);
    }

    void OnPaint(wxPaintEvent&) {
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
                dc.SetPen(wxPen(wxColour(225, 230, 236)));
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

    CalendarViewMode mode_ = CalendarViewMode::DAY;
    long long rangeStartEpoch_ = 0;
    long long selectedDayEpoch_ = 0;
    std::vector<Event> events_;
    std::function<void(long long)> eventClickHandler_;
    std::function<void(long long, int)> emptySlotClickHandler_;
    wxPanel* canvas_ = nullptr;
    std::vector<wxButton*> eventButtons_;
};

class LocalCalendarFrame final : public wxFrame {
public:
    explicit LocalCalendarFrame(const std::string& dbPath)
        : wxFrame(nullptr, wxID_ANY, "Calendar", wxDefaultPosition, wxSize(1440, 860)),
          db_(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
          eventRepository_(db_),
          calendarRepository_(db_) {
        localCalendarId_ = EnsureLocalCalendar();
        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        const std::tm today = EpochToUtcTm(now);
        visibleYear_ = today.tm_year + 1900;
        visibleMonth_ = today.tm_mon + 1;
        currentViewMode_ = CalendarViewMode::MONTH;

        BuildLayout();
        ApplyLook();
        BindEvents();
        RefreshEvents();
        ClearForm();
    }

private:
    long long EnsureLocalCalendar() {
        auto existing = calendarRepository_.getByProviderId(0, "local-calendar");
        if (existing) {
            return existing->id;
        }

        Calendar localCalendar{};
        localCalendar.accountId = 0;
        localCalendar.providerCalendarId = "local-calendar";
        localCalendar.name = "Local Calendar";
        localCalendar.timezone = "UTC";
        localCalendar.syncEnabled = false;
        localCalendar.createdAt = std::time(nullptr);
        localCalendar.updatedAt = std::time(nullptr);

        calendarRepository_.upsert(localCalendar);
        existing = calendarRepository_.getByProviderId(0, "local-calendar");
        return existing ? existing->id : 0;
    }

    void BuildLayout() {
        auto* panel = new wxPanel(this);
        auto* rootSizer = new wxBoxSizer(wxVERTICAL);

        auto* toolbarSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* titleBlock = new wxBoxSizer(wxVERTICAL);
        auto* appTitle = new wxStaticText(panel, wxID_ANY, "Local Calendar");
        auto* appSubtitle = new wxStaticText(panel, wxID_ANY, "Month, week and day views with editable event cards");
        titleBlock->Add(appTitle, 0, wxBOTTOM, 2);
        titleBlock->Add(appSubtitle, 0);

        monthViewButton_ = new wxButton(panel, wxID_ANY, "Month");
        weekViewButton_ = new wxButton(panel, wxID_ANY, "Week");
        dayViewButton_ = new wxButton(panel, wxID_ANY, "Day");
        todayButton_ = new wxButton(panel, wxID_ANY, "Today");
        previousButton_ = new wxButton(panel, wxID_ANY, "<");
        nextButton_ = new wxButton(panel, wxID_ANY, ">");
        monthTitleLabel_ = new wxStaticText(panel, wxID_ANY, "");

        toolbarSizer->Add(titleBlock, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 24);
        toolbarSizer->Add(monthViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(weekViewButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(dayViewButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(todayButton_, 0, wxRIGHT, 12);
        toolbarSizer->Add(previousButton_, 0, wxRIGHT, 6);
        toolbarSizer->Add(nextButton_, 0, wxRIGHT, 18);
        toolbarSizer->Add(monthTitleLabel_, 0, wxALIGN_CENTER_VERTICAL);

        auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);

        auto* leftPane = new wxBoxSizer(wxVERTICAL);
        calendarBook_ = new wxSimplebook(panel, wxID_ANY);

        auto* monthPage = new wxPanel(calendarBook_);
        auto* monthSizer = new wxBoxSizer(wxVERTICAL);
        auto* weekdaySizer = new wxGridSizer(1, 7, 8, 8);
        const std::array<const char*, 7> weekdays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        for (const char* weekday : weekdays) {
            auto* label = new wxStaticText(monthPage, wxID_ANY, weekday);
            weekdaySizer->Add(label, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
        }
        monthSizer->Add(weekdaySizer, 0, wxEXPAND | wxALL, 10);

        auto* gridSizer = new wxGridSizer(6, 7, 8, 8);
        for (int index = 0; index < kMonthCellCount; ++index) {
            monthCells_[index] = new MonthCellPanel(
                monthPage,
                index,
                [this](const int cellIndex) { HandleMonthCellClicked(cellIndex); },
                [this](const int cellIndex) { HandleMonthCellCreateEvent(cellIndex); },
                [this](const long long eventId) { OpenEventById(eventId); });
            gridSizer->Add(monthCells_[index], 1, wxEXPAND);
        }
        monthSizer->Add(gridSizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        monthPage->SetSizer(monthSizer);

        auto* weekPage = new wxPanel(calendarBook_);
        auto* weekSizer = new wxBoxSizer(wxVERTICAL);
        weekTimeline_ = new TimelineViewPanel(weekPage);
        weekSizer->Add(weekTimeline_, 1, wxEXPAND | wxALL, 10);
        weekPage->SetSizer(weekSizer);

        auto* dayPage = new wxPanel(calendarBook_);
        auto* daySizer = new wxBoxSizer(wxVERTICAL);
        dayTimeline_ = new TimelineViewPanel(dayPage);
        daySizer->Add(dayTimeline_, 1, wxEXPAND | wxALL, 10);
        dayPage->SetSizer(daySizer);

        calendarBook_->AddPage(monthPage, "Month");
        calendarBook_->AddPage(weekPage, "Week");
        calendarBook_->AddPage(dayPage, "Day");

        leftPane->Add(calendarBook_, 1, wxEXPAND);

        auto* editorPane = new wxBoxSizer(wxVERTICAL);
        auto* editorTitle = new wxStaticText(panel, wxID_ANY, "Event Details");
        editorPane->Add(editorTitle, 0, wxALL, 10);

        titleCtrl_ = AddLabeledTextField(panel, editorPane, "Title");
        locationCtrl_ = AddLabeledTextField(panel, editorPane, "Location");
        allDayCtrl_ = new wxCheckBox(panel, wxID_ANY, "All day");
        editorPane->Add(allDayCtrl_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        startCtrl_ = AddLabeledTextField(panel, editorPane, "Start (UTC)");
        endCtrl_ = AddLabeledTextField(panel, editorPane, "End (UTC)");
        descriptionCtrl_ = AddLabeledTextField(panel, editorPane, "Description", wxTE_MULTILINE, 140);

        auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
        newButton_ = new wxButton(panel, wxID_ANY, "New Event");
        saveButton_ = new wxButton(panel, wxID_ANY, "Save");
        deleteButton_ = new wxButton(panel, wxID_ANY, "Delete");
        refreshButton_ = new wxButton(panel, wxID_ANY, "Refresh");
        buttonSizer->Add(newButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(saveButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(deleteButton_, 0, wxRIGHT, 8);
        buttonSizer->Add(refreshButton_, 0);
        editorPane->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        statusLabel_ = new wxStaticText(panel, wxID_ANY, "Ready");
        editorPane->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        contentSizer->Add(leftPane, 5, wxEXPAND | wxALL, 12);
        contentSizer->Add(editorPane, 2, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

        rootSizer->Add(toolbarSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        rootSizer->Add(contentSizer, 1, wxEXPAND);

        panel->SetSizer(rootSizer);

        weekTimeline_->SetMode(CalendarViewMode::WEEK);
        weekTimeline_->SetEventClickHandler([this](const long long eventId) { OpenEventById(eventId); });
        weekTimeline_->SetEmptySlotClickHandler([this](const long long dayEpoch, const int minuteOfDay) {
            OpenNewTimedEventDialog(dayEpoch, minuteOfDay);
        });
        dayTimeline_->SetMode(CalendarViewMode::DAY);
        dayTimeline_->SetEventClickHandler([this](const long long eventId) { OpenEventById(eventId); });
        dayTimeline_->SetEmptySlotClickHandler([this](const long long dayEpoch, const int minuteOfDay) {
            OpenNewTimedEventDialog(dayEpoch, minuteOfDay);
        });
    }

    void ApplyLook() {
        SetBackgroundColour(wxColour(246, 248, 252));
        if (GetParent() == nullptr) {
            SetMinSize(wxSize(1240, 760));
        }

        const wxColour surfaceBg(255, 255, 255);
        const wxColour accent(26, 115, 232);

        monthViewButton_->SetBackgroundColour(surfaceBg);
        weekViewButton_->SetBackgroundColour(surfaceBg);
        dayViewButton_->SetBackgroundColour(surfaceBg);
        todayButton_->SetBackgroundColour(surfaceBg);
        previousButton_->SetBackgroundColour(surfaceBg);
        nextButton_->SetBackgroundColour(surfaceBg);
        saveButton_->SetBackgroundColour(accent);
        saveButton_->SetForegroundColour(*wxWHITE);
    }

    wxTextCtrl* AddLabeledTextField(wxWindow* parent,
                                    wxBoxSizer* sizer,
                                    const wxString& label,
                                    const long style = 0,
                                    const int minHeight = -1) {
        sizer->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);
        auto* control = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(-1, minHeight), style);
        sizer->Add(control, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        return control;
    }

    void BindEvents() {
        monthViewButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnShowMonthView, this);
        weekViewButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnShowWeekView, this);
        dayViewButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnShowDayView, this);
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnPreviousPeriod, this);
        nextButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNextPeriod, this);
        newButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNew, this);
        saveButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnSave, this);
        deleteButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnDelete, this);
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        allDayCtrl_->Bind(wxEVT_CHECKBOX, &LocalCalendarFrame::OnAllDayChanged, this);
    }

    std::vector<Event> EventsForDay(const long long dayEpoch) const {
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

    void RefreshEvents() {
        events_ = eventRepository_.getByCalendar(localCalendarId_);
        RefreshViewState();
        statusLabel_->SetLabel(wxString::Format("Loaded %zu event(s)", events_.size()));
    }

    void RefreshViewState() {
        RefreshHeaderTitle();
        RefreshMonthGrid();
        RefreshTimelineViews();
    }

    void RefreshHeaderTitle() {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            monthTitleLabel_->SetLabel(FormatMonthTitle(visibleYear_, visibleMonth_));
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            monthTitleLabel_->SetLabel(FormatWeekTitle(StartOfUtcWeek(selectedDayEpoch_)));
        }
        else {
            monthTitleLabel_->SetLabel(FormatDayHeader(selectedDayEpoch_));
        }
    }

    void RefreshMonthGrid() {
        const int firstOffset = MonthGridOffset(visibleYear_, visibleMonth_);
        const int daysCurrentMonth = DaysInMonth(visibleYear_, visibleMonth_);
        const int previousMonth = visibleMonth_ == 1 ? 12 : visibleMonth_ - 1;
        const int previousYear = visibleMonth_ == 1 ? visibleYear_ - 1 : visibleYear_;
        const int nextMonth = visibleMonth_ == 12 ? 1 : visibleMonth_ + 1;
        const int nextYear = visibleMonth_ == 12 ? visibleYear_ + 1 : visibleYear_;
        const int daysPreviousMonth = DaysInMonth(previousYear, previousMonth);
        const long long today = StartOfUtcDay(std::time(nullptr));

        for (int cellIndex = 0; cellIndex < kMonthCellCount; ++cellIndex) {
            int dayNumber = 0;
            bool inCurrentMonth = true;
            int cellMonth = visibleMonth_;
            int cellYear = visibleYear_;

            if (cellIndex < firstOffset) {
                dayNumber = daysPreviousMonth - firstOffset + cellIndex + 1;
                cellMonth = previousMonth;
                cellYear = previousYear;
                inCurrentMonth = false;
            }
            else if (cellIndex >= firstOffset + daysCurrentMonth) {
                dayNumber = cellIndex - (firstOffset + daysCurrentMonth) + 1;
                cellMonth = nextMonth;
                cellYear = nextYear;
                inCurrentMonth = false;
            }
            else {
                dayNumber = cellIndex - firstOffset + 1;
            }

            const long long dayEpoch = MakeUtcEpoch(cellYear, cellMonth, dayNumber);
            monthCellEpochs_[cellIndex] = dayEpoch;
            monthCells_[cellIndex]->UpdateCell(
                dayEpoch,
                dayNumber,
                inCurrentMonth,
                IsSameUtcDay(dayEpoch, today),
                IsSameUtcDay(dayEpoch, selectedDayEpoch_),
                EventsForDay(dayEpoch));
        }
    }

    void RefreshTimelineViews() {
        const long long weekStart = StartOfUtcWeek(selectedDayEpoch_);
        weekTimeline_->SetSelectedDay(selectedDayEpoch_);
        weekTimeline_->SetRangeStart(weekStart);
        weekTimeline_->SetEvents(events_);

        dayTimeline_->SetSelectedDay(selectedDayEpoch_);
        dayTimeline_->SetRangeStart(selectedDayEpoch_);
        dayTimeline_->SetEvents(events_);
    }

    void ClearForm() {
        selectedEventId_ = -1;
        titleCtrl_->Clear();
        locationCtrl_->Clear();
        descriptionCtrl_->Clear();
        allDayCtrl_->SetValue(false);
        startCtrl_->SetValue(FormatUtcDateTimeInput(selectedDayEpoch_ + 9 * 3600, false));
        endCtrl_->SetValue(FormatUtcDateTimeInput(selectedDayEpoch_ + 10 * 3600, false));
        statusLabel_->SetLabel("Creating new event");
    }

    void LoadEventIntoForm(const Event& event) {
        selectedEventId_ = event.id;
        titleCtrl_->SetValue(event.title);
        locationCtrl_->SetValue(event.location);
        descriptionCtrl_->SetValue(event.description);
        allDayCtrl_->SetValue(event.allDay);
        startCtrl_->SetValue(FormatUtcDateTimeInput(event.startDateTime, event.allDay));
        endCtrl_->SetValue(FormatUtcDateTimeInput(event.endDateTime, event.allDay));
        selectedDayEpoch_ = StartOfUtcDay(event.startDateTime);
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        RefreshViewState();
        statusLabel_->SetLabel(wxString::Format("Editing event #%lld", event.id));
    }

    Event BuildEventFromForm() {
        Event event{};
        event.id = selectedEventId_;
        event.calendarId = localCalendarId_;
        event.title = titleCtrl_->GetValue().ToStdString();
        event.location = locationCtrl_->GetValue().ToStdString();
        event.description = descriptionCtrl_->GetValue().ToStdString();
        event.allDay = allDayCtrl_->GetValue();
        event.status = "confirmed";
        event.type = EventType::SINGLE;
        event.deletedAt = 0;
        event.syncStatus = SYNCED;
        event.lastModified = std::time(nullptr);
        event.updatedAt = event.lastModified;
        event.createdAt = event.lastModified;
        event.startDateTime = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), event.allDay);
        event.endDateTime = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), event.allDay);
        NormalizeAllDayEventRange(event);

        if (selectedEventId_ > 0) {
            auto existing = eventRepository_.getById(selectedEventId_);
            if (existing) {
                event.createdAt = existing->createdAt;
                event.providerEventId = existing->providerEventId;
                event.providerMasterId = existing->providerMasterId;
                event.instanceStart = existing->instanceStart;
                event.recurrenceRule = existing->recurrenceRule;
            }
        }
        else {
            event.instanceStart = event.startDateTime;
            event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
        }

        return event;
    }

    bool ValidateEvent(const Event& event) {
        if (event.title.empty()) {
            wxMessageBox("Title is required.", "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        if (event.startDateTime < 0 || event.endDateTime < 0) {
            wxMessageBox("Use YYYY-MM-DD for all-day events or YYYY-MM-DD HH:MM for timed events.",
                         "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        if (event.endDateTime < event.startDateTime) {
            wxMessageBox("End must be after start.", "Validation", wxOK | wxICON_WARNING, this);
            return false;
        }

        return true;
    }

    void ShiftVisibleMonth(const int delta) {
        visibleMonth_ += delta;
        while (visibleMonth_ < 1) {
            visibleMonth_ += 12;
            --visibleYear_;
        }
        while (visibleMonth_ > 12) {
            visibleMonth_ -= 12;
            ++visibleYear_;
        }
    }

    bool PersistEvent(const Event& event) {
        try {
            eventRepository_.upsert(event);
            selectedDayEpoch_ = StartOfUtcDay(event.startDateTime);
            const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
            visibleYear_ = tm.tm_year + 1900;
            visibleMonth_ = tm.tm_mon + 1;
            RefreshEvents();
            LoadEventIntoForm(eventRepository_.getByProviderInstance(event.providerEventId, event.instanceStart).value_or(event));
            statusLabel_->SetLabel("Event saved");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Save failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    bool DeleteEventById(const long long eventId) {
        try {
            eventRepository_.deleteEvent(eventId);
            RefreshEvents();
            ClearForm();
            statusLabel_->SetLabel("Event deleted");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Delete failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    void OpenEventDialog(const std::optional<Event>& event = std::nullopt,
                         const std::optional<EventDraftDefaults>& defaults = std::nullopt) {
        EventEditorDialog dialog(this, event, selectedDayEpoch_, defaults);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (dialog.IsDeleteRequested()) {
            if (event.has_value()) {
                DeleteEventById(event->id);
            }
            return;
        }

        const auto builtEvent = dialog.BuildEvent(localCalendarId_);
        if (!builtEvent.has_value()) {
            return;
        }

        PersistEvent(*builtEvent);
    }

    void OpenEventById(const long long eventId) {
        auto selectedEvent = eventRepository_.getById(eventId);
        if (!selectedEvent.has_value()) {
            return;
        }

        LoadEventIntoForm(*selectedEvent);
        OpenEventDialog(*selectedEvent);
    }

    void OpenNewAllDayEventDialog(const long long dayEpoch) {
        selectedDayEpoch_ = dayEpoch;
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        RefreshViewState();
        ClearForm();

        EventDraftDefaults defaults;
        defaults.startDateTime = dayEpoch;
        defaults.endDateTime = dayEpoch + kSecondsPerDay;
        defaults.allDay = true;
        OpenEventDialog(std::nullopt, defaults);
    }

    void OpenNewTimedEventDialog(const long long dayEpoch, const int minuteOfDay) {
        selectedDayEpoch_ = dayEpoch;
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        RefreshViewState();
        ClearForm();

        EventDraftDefaults defaults;
        defaults.startDateTime = dayEpoch + static_cast<long long>(minuteOfDay) * 60;
        defaults.endDateTime = defaults.startDateTime + 3600;
        defaults.allDay = false;
        OpenEventDialog(std::nullopt, defaults);
    }

    void HandleMonthCellClicked(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        selectedDayEpoch_ = monthCellEpochs_[index];
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        currentViewMode_ = CalendarViewMode::DAY;
        calendarBook_->SetSelection(2);
        RefreshViewState();
        ClearForm();
    }

    void HandleMonthCellCreateEvent(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        OpenNewAllDayEventDialog(monthCellEpochs_[index]);
    }

    void OnShowMonthView(wxCommandEvent&) {
        currentViewMode_ = CalendarViewMode::MONTH;
        calendarBook_->SetSelection(0);
        RefreshHeaderTitle();
    }

    void OnShowWeekView(wxCommandEvent&) {
        currentViewMode_ = CalendarViewMode::WEEK;
        calendarBook_->SetSelection(1);
        RefreshHeaderTitle();
    }

    void OnShowDayView(wxCommandEvent&) {
        currentViewMode_ = CalendarViewMode::DAY;
        calendarBook_->SetSelection(2);
        RefreshHeaderTitle();
    }

    void OnToday(wxCommandEvent&) {
        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        const std::tm tm = EpochToUtcTm(now);
        visibleYear_ = tm.tm_year + 1900;
        visibleMonth_ = tm.tm_mon + 1;
        RefreshViewState();
        ClearForm();
    }

    void OnPreviousPeriod(wxCommandEvent&) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            ShiftVisibleMonth(-1);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            selectedDayEpoch_ -= 7 * kSecondsPerDay;
        }
        else {
            selectedDayEpoch_ -= kSecondsPerDay;
        }

        if (currentViewMode_ != CalendarViewMode::MONTH) {
            const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
            visibleYear_ = tm.tm_year + 1900;
            visibleMonth_ = tm.tm_mon + 1;
        }

        RefreshViewState();
        ClearForm();
    }

    void OnNextPeriod(wxCommandEvent&) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            ShiftVisibleMonth(1);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            selectedDayEpoch_ += 7 * kSecondsPerDay;
        }
        else {
            selectedDayEpoch_ += kSecondsPerDay;
        }

        if (currentViewMode_ != CalendarViewMode::MONTH) {
            const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
            visibleYear_ = tm.tm_year + 1900;
            visibleMonth_ = tm.tm_mon + 1;
        }

        RefreshViewState();
        ClearForm();
    }

    void OnNew(wxCommandEvent&) {
        ClearForm();
        OpenEventDialog();
    }

    void OnSave(wxCommandEvent&) {
        Event event = BuildEventFromForm();
        if (!ValidateEvent(event)) {
            return;
        }
        PersistEvent(event);
    }

    void OnDelete(wxCommandEvent&) {
        if (selectedEventId_ <= 0) {
            wxMessageBox("Select an event first.", "Delete", wxOK | wxICON_INFORMATION, this);
            return;
        }
        DeleteEventById(selectedEventId_);
    }

    void OnRefresh(wxCommandEvent&) {
        RefreshEvents();
    }

    void OnAllDayChanged(wxCommandEvent&) {
        const bool allDay = allDayCtrl_->GetValue();
        const long long startEpoch = ParseUtcDateTimeInput(startCtrl_->GetValue().ToStdString(), false);
        const long long endEpoch = ParseUtcDateTimeInput(endCtrl_->GetValue().ToStdString(), false);

        if (startEpoch >= 0) {
            startCtrl_->SetValue(FormatUtcDateTimeInput(startEpoch, allDay));
        }
        if (endEpoch >= 0) {
            endCtrl_->SetValue(FormatUtcDateTimeInput(endEpoch, allDay));
        }
    }

    SQLite::Database db_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
    long long localCalendarId_ = 0;
    long long selectedDayEpoch_ = 0;
    long long selectedEventId_ = -1;
    int visibleYear_ = 1970;
    int visibleMonth_ = 1;
    CalendarViewMode currentViewMode_ = CalendarViewMode::MONTH;
    std::vector<Event> events_;

    wxButton* monthViewButton_ = nullptr;
    wxButton* weekViewButton_ = nullptr;
    wxButton* dayViewButton_ = nullptr;
    wxButton* todayButton_ = nullptr;
    wxButton* previousButton_ = nullptr;
    wxButton* nextButton_ = nullptr;
    wxStaticText* monthTitleLabel_ = nullptr;
    wxSimplebook* calendarBook_ = nullptr;
    TimelineViewPanel* weekTimeline_ = nullptr;
    TimelineViewPanel* dayTimeline_ = nullptr;
    std::array<MonthCellPanel*, kMonthCellCount> monthCells_{};
    std::array<long long, kMonthCellCount> monthCellEpochs_{};

    wxTextCtrl* titleCtrl_ = nullptr;
    wxTextCtrl* locationCtrl_ = nullptr;
    wxTextCtrl* startCtrl_ = nullptr;
    wxTextCtrl* endCtrl_ = nullptr;
    wxTextCtrl* descriptionCtrl_ = nullptr;
    wxCheckBox* allDayCtrl_ = nullptr;
    wxButton* newButton_ = nullptr;
    wxButton* saveButton_ = nullptr;
    wxButton* deleteButton_ = nullptr;
    wxButton* refreshButton_ = nullptr;
    wxStaticText* statusLabel_ = nullptr;
};

std::string g_dbPath;

class LocalCalendarApp final : public wxApp {
public:
    bool OnInit() override {
        auto* frame = new LocalCalendarFrame(g_dbPath);
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP_NO_MAIN(LocalCalendarApp);

} // namespace

int RunLocalCalendarUi(const std::string& dbPath) {
    g_dbPath = dbPath;

    int argc = 0;
    char** argv = nullptr;

    if (!wxEntryStart(argc, argv)) {
        return 1;
    }

    wxTheApp->CallOnInit();
    const int exitCode = wxTheApp->OnRun();
    wxTheApp->OnExit();
    wxEntryCleanup();

    return exitCode;
}
