#include "ui/local_calendar_app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <set>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choicdlg.h>
#include <wx/combobox.h>
#include <wx/activityindicator.h>
#include <wx/filedlg.h>
#include <wx/frame.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/weakref.h>
#include <wx/wx.h>

#include "models/account.h"
#include "models/calendar.h"
#include "events/event_load_operations.h"
#include "events/event_occurrence_utils.h"
#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/calendar_sync_range_repository.h"
#include "repositories/event_repository.h"
#include "oauth/oauth_utils.h"
#include "sync/account_auth_operations.h"
#include "sync/event_upload_scheduler.h"
#include "sync/google_calendar_sync_service.h"
#include "sync/outlook_calendar_sync_service.h"
#include "ui/calendar_ui_shared.h"
#include "ui/account_manager_dialog.h"
#include "ui/calendar_editor_dialog.h"
#include "ui/event_editor_dialog.h"
#include "ui/event_move_dialog.h"
#include "ui/month_cell_panel.h"
#include "ui/timeline_view_panel.h"
#include "utils/access_token.h"
#include "utils/calendar_colors.h"
#include "utils/datetime_utils.h"
#include "utils/sqlite_utils.h"
#include "utils/timezone_utils.h"

namespace {

struct MonthCellMeta {
    long long dayEpoch = 0;
    int dayNumber = 0;
    bool inCurrentMonth = true;
};

constexpr int kYearComboChunkSize = 120;
constexpr int kYearComboBackwardPadding = 20;
constexpr size_t kMaxMonthCellRenderedRows = 4;
constexpr const char* kLocalProvider = "LOCAL";
constexpr const char* kLocalProviderUserId = "local-account";
constexpr const char* kLocalCalendarProviderId = "local-calendar";
constexpr const char* kDefaultAccountCalendarProviderId = "default-calendar";

    bool IsLocalProvider(const std::string& provider) {
        return provider == kLocalProvider;
    }

bool IsSignedInProvider(const Account& account) {
    return !IsLocalProvider(account.provider);
}

std::optional<int> PlatformForProvider(const std::string& provider) {
    if (provider == "GOOGLE") {
        return GOOGLE;
    }
    if (provider == "MICROSOFT") {
        return MICROSOFT;
    }
    return std::nullopt;
}

wxString ProviderDisplayName(const std::string& provider) {
    if (provider == "GOOGLE") {
        return "Google";
    }
    if (provider == "MICROSOFT") {
        return "Outlook";
    }
    if (provider == kLocalProvider) {
        return "Local";
    }
    return wxString::FromUTF8(provider);
}

wxString FormatAccountLabel(const Account& account) {
    if (IsLocalProvider(account.provider)) {
        return "Local account";
    }

    return wxString::Format("%s (%s)",
                            wxString::FromUTF8(account.name),
                            ProviderDisplayName(account.provider));
}

wxString FormatCalendarLabel(const Calendar& calendar) {
    wxString label = wxString::FromUTF8(calendar.name);
    if (calendar.isPrimary) {
        label += " (Primary)";
    }
    if (calendar.isShared) {
        label += " (Shared)";
    }
    if (calendar.isReadOnly) {
        label += " (Read-only)";
    }
    return label;
}

wxColour CalendarColour(const std::string& colorHex) {
    const std::string normalized = NormalizeCalendarColor(colorHex);
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    if (normalized.size() == 7) {
        std::stringstream stream;
        stream << std::hex << normalized.substr(1, 2);
        stream >> red;
        stream.clear();
        stream.str("");
        stream << std::hex << normalized.substr(3, 2);
        stream >> green;
        stream.clear();
        stream.str("");
        stream << std::hex << normalized.substr(5, 2);
        stream >> blue;
    }
    return wxColour(static_cast<unsigned char>(red),
                    static_cast<unsigned char>(green),
                    static_cast<unsigned char>(blue));
}

wxBitmap MakeColorSwatchBitmap(const wxColour& colour) {
    wxBitmap bitmap(44, 14);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(wxBrush(wxColour(255, 255, 255)));
    dc.Clear();
    dc.SetBrush(wxBrush(colour));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, 44, 14);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

std::string SafeIcsFileName(std::string value) {
    if (value.empty()) {
        return "calendar";
    }

    for (char& ch : value) {
        const bool invalid = ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
            ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*';
        if (invalid || std::iscntrl(static_cast<unsigned char>(ch))) {
            ch = '_';
        }
    }

    return value + ".ics";
}

class LocalCalendarFrame final : public wxFrame {
public:
    explicit LocalCalendarFrame(const std::string& dbPath)
        : wxFrame(nullptr, wxID_ANY, "Calendar", wxDefaultPosition, wxSize(1520, 860)),
          dbPath_(dbPath),
          db_(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
          accountRepository_(db_),
          eventRepository_(db_),
          calendarRepository_(db_) {
        ConfigureSqliteConnection(db_);
        localAccountId_ = GetLocalAccountId();
        SyncLocalCalendarTimeZone();

        const long long now = CurrentLocalDisplayEpoch();
        selectedDayEpoch_ = StartOfUtcDay(now);
        SyncVisibleMonthToSelectedDay();

        BuildLayout();
        ApplyLook();
        BindEvents();
        StartEventUploadScheduler();

        ReloadAccounts();
        SetCurrentAccount(localAccountId_, false);
        RefreshAllAccountCalendarCache();
        RefreshEvents();
        StartSilentAccountActivationForStoredAccounts();
    }

    ~LocalCalendarFrame() override {
        if (eventUploadScheduler_) {
            eventUploadScheduler_->Stop();
        }
    }

private:
    long long GetLocalAccountId() {
        const auto accounts = accountRepository_.GetAllAccounts();
        const auto it = std::find_if(accounts.begin(), accounts.end(), [](const Account& account) {
            return account.provider == kLocalProvider && account.providerUserId == kLocalProviderUserId;
        });

        if (it != accounts.end()) {
            return it->id;
        }

        throw std::runtime_error("Local account seed data is missing.");
    }

    void PromotePrimaryCalendar(const long long accountId, const long long calendarId) {
        SQLite::Statement query(
            db_,
            "UPDATE calendars SET is_primary = CASE WHEN id = ? THEN 1 ELSE 0 END WHERE account_id = ?");
        BindInt64(query, 1, calendarId);
        BindInt64(query, 2, accountId);
        query.exec();
    }

    void SyncLocalCalendarTimeZone() {
        auto localCalendar = calendarRepository_.getByProviderId(localAccountId_, kLocalCalendarProviderId);
        if (!localCalendar.has_value()) {
            return;
        }

        const std::string localTimezone = GetCurrentLocalTimeZoneName();
        if (localCalendar->timezone == localTimezone) {
            return;
        }

        localCalendar->timezone = localTimezone;
        calendarRepository_.upsert(*localCalendar);
    }

    std::vector<Calendar> EnsureCalendarsForAccount(const Account& account) {
        if (IsLocalProvider(account.provider)) {
            auto localCalendar = calendarRepository_.getByProviderId(account.id, kLocalCalendarProviderId);
            if (!localCalendar.has_value()) {
                Calendar calendar{};
                calendar.accountId = account.id;
                calendar.providerCalendarId = kLocalCalendarProviderId;
                calendar.name = "Local Calendar";
                calendar.description = "Local-only calendar";
                calendar.timezone = "UTC";
                calendar.colorHex = "#1A73E8";
                calendar.isPrimary = true;
                calendar.isReadOnly = false;
                calendar.syncEnabled = false;
                calendar.syncStatus = SYNCED;
                calendarRepository_.upsert(calendar);
            }
        }

        auto calendars = calendarRepository_.getByAccount(account.id);
        if (!calendars.empty()) {
            const auto primaryIt = std::find_if(calendars.begin(), calendars.end(), [](const Calendar& calendar) {
                return calendar.isPrimary;
            });
            const long long primaryId = (primaryIt != calendars.end() ? *primaryIt : calendars.front()).id;
            if (primaryIt == calendars.end() || (IsLocalProvider(account.provider) &&
                std::none_of(calendars.begin(), calendars.end(), [](const Calendar& calendar) {
                    return calendar.isPrimary && calendar.providerCalendarId == kLocalCalendarProviderId;
                }))) {
                const long long localPrimaryId = IsLocalProvider(account.provider)
                    ? std::find_if(calendars.begin(), calendars.end(), [](const Calendar& calendar) {
                          return calendar.providerCalendarId == kLocalCalendarProviderId;
                      })->id
                    : primaryId;
                PromotePrimaryCalendar(account.id, localPrimaryId);
                calendars = calendarRepository_.getByAccount(account.id);
            }
            return calendars;
        }

        Calendar calendar{};
        calendar.accountId = account.id;
        calendar.providerCalendarId = IsLocalProvider(account.provider)
            ? kLocalCalendarProviderId
            : kDefaultAccountCalendarProviderId;
        calendar.name = IsLocalProvider(account.provider)
            ? "Local Calendar"
            : account.name + " Calendar";
        calendar.description = IsLocalProvider(account.provider)
            ? "Local-only calendar"
            : "";
        calendar.timezone = "UTC";
        calendar.colorHex = IsLocalProvider(account.provider) ? "#1A73E8" : RandomCalendarColor();
        calendar.isPrimary = true;
        calendar.isReadOnly = false;
        calendar.syncEnabled = false;
        calendar.syncStatus = SYNCED;

        calendarRepository_.upsert(calendar);
        return calendarRepository_.getByAccount(account.id);
    }

    void SaveCurrentCalendarSessionState() {
        if (currentAccountId_ == 0) {
            return;
        }

        accountVisibleCalendarIds_[currentAccountId_] = visibleCalendarIds_;
        accountSelectedCalendarIds_[currentAccountId_] = selectedCalendarId_;
    }

    void LoadCalendarsForCurrentAccount(const Account& account) {
        accountCalendars_ = EnsureCalendarsForAccount(account);
        accountCalendarCache_[account.id] = accountCalendars_;

        auto visibleIt = accountVisibleCalendarIds_.find(account.id);
        if (visibleIt == accountVisibleCalendarIds_.end()) {
            for (const auto& calendar : accountCalendars_) {
                visibleCalendarIds_.insert(calendar.id);
            }
        }
        else {
            for (const auto calendarId : visibleIt->second) {
                visibleCalendarIds_.insert(calendarId);
            }
            for (auto it = visibleCalendarIds_.begin(); it != visibleCalendarIds_.end();) {
                const bool exists = std::any_of(accountCalendars_.begin(), accountCalendars_.end(), [it](const Calendar& calendar) {
                    return calendar.id == *it;
                });
                const bool belongsToCurrentAccount = std::any_of(accountCalendars_.begin(), accountCalendars_.end(), [it](const Calendar& calendar) {
                    return calendar.id == *it;
                });
                if (belongsToCurrentAccount && !exists) {
                    it = visibleCalendarIds_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        auto selectedIt = accountSelectedCalendarIds_.find(account.id);
        const bool selectedExists = selectedIt != accountSelectedCalendarIds_.end() &&
            std::any_of(accountCalendars_.begin(), accountCalendars_.end(), [selectedIt](const Calendar& calendar) {
                return calendar.id == selectedIt->second;
            });
        if (selectedExists) {
            selectedCalendarId_ = selectedIt->second;
        }
        else {
            const auto primaryIt = std::find_if(accountCalendars_.begin(), accountCalendars_.end(), [](const Calendar& calendar) {
                return calendar.isPrimary;
            });
            selectedCalendarId_ = primaryIt != accountCalendars_.end()
                ? primaryIt->id
                : (accountCalendars_.empty() ? 0 : accountCalendars_.front().id);
        }

        if (selectedCalendarId_ != 0) {
            visibleCalendarIds_.insert(selectedCalendarId_);
        }
    }

    void RefreshAllAccountCalendarCache() {
        accountCalendarCache_.clear();
        for (const auto& account : availableAccounts_) {
            auto calendars = EnsureCalendarsForAccount(account);
            accountCalendarCache_[account.id] = calendars;
            if (accountVisibleCalendarIds_.count(account.id) == 0) {
                for (const auto& calendar : calendars) {
                    visibleCalendarIds_.insert(calendar.id);
                }
            }
        }
        if (const auto current = FindLoadedAccountById(currentAccountId_); current.has_value()) {
            accountCalendars_ = accountCalendarCache_[current->id];
        }
    }

    std::vector<Calendar> AllLoadedCalendars() const {
        std::vector<Calendar> calendars;
        for (const auto& [_, accountCalendars] : accountCalendarCache_) {
            calendars.insert(calendars.end(), accountCalendars.begin(), accountCalendars.end());
        }
        if (calendars.empty()) {
            calendars = accountCalendars_;
        }
        return calendars;
    }

    void RefreshCalendarControls() {
        if (calendarListPanel_ == nullptr || calendarListSizer_ == nullptr) {
            return;
        }

        calendarListPanel_->Freeze();
        calendarListSizer_->Clear(true);

        RefreshAllAccountCalendarCache();

        const auto makeSidebarActionButton = [this](const wxString& label) {
            auto* button = new wxButton(calendarListPanel_, wxID_ANY, label, wxDefaultPosition, wxSize(42, 28));
            button->SetMinSize(wxSize(42, 28));
            return button;
        };

        for (const auto& account : availableAccounts_) {
            const auto calendarsIt = accountCalendarCache_.find(account.id);
            const std::vector<Calendar> calendars = calendarsIt != accountCalendarCache_.end()
                ? calendarsIt->second
                : std::vector<Calendar>{};

            const bool collapsed = collapsedAccountIds_.count(account.id) > 0;
            const bool hasCalendars = !calendars.empty();
            const bool allVisible = hasCalendars && std::all_of(calendars.begin(), calendars.end(), [this](const Calendar& calendar) {
                return visibleCalendarIds_.count(calendar.id) > 0;
            });

            auto* accountRow = new wxBoxSizer(wxHORIZONTAL);
            auto* accountVisibility = new wxCheckBox(calendarListPanel_, wxID_ANY, "");
            accountVisibility->SetValue(allVisible);
            accountVisibility->Bind(wxEVT_CHECKBOX, [this, accountId = account.id](wxCommandEvent& event) {
                const auto calendarsIt = accountCalendarCache_.find(accountId);
                if (calendarsIt != accountCalendarCache_.end()) {
                    for (const auto& calendar : calendarsIt->second) {
                        if (event.IsChecked()) {
                            visibleCalendarIds_.insert(calendar.id);
                        }
                        else {
                            visibleCalendarIds_.erase(calendar.id);
                        }
                    }
                }
                accountVisibleCalendarIds_[accountId] = visibleCalendarIds_;
                RefreshEvents();
                CallAfter([this]() { RefreshCalendarControls(); });
            });
            auto* accountName = new wxStaticText(calendarListPanel_, wxID_ANY, FormatAccountLabel(account));
            wxFont accountFont = accountName->GetFont();
            accountFont.SetWeight(wxFONTWEIGHT_BOLD);
            accountName->SetFont(accountFont);
            auto* addCalendarButton = makeSidebarActionButton("+");
            addCalendarButton->Bind(wxEVT_BUTTON, [this, accountId = account.id](wxCommandEvent&) {
                OpenCreateCalendarDialog(accountId);
            });
            auto* collapseButton = makeSidebarActionButton(collapsed ? "v" : "^");
            collapseButton->Bind(wxEVT_BUTTON, [this, accountId = account.id](wxCommandEvent&) {
                if (collapsedAccountIds_.count(accountId) > 0) {
                    collapsedAccountIds_.erase(accountId);
                }
                else {
                    collapsedAccountIds_.insert(accountId);
                }
                RefreshCalendarControls();
            });
            accountRow->Add(accountVisibility, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            accountRow->Add(accountName, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            accountRow->Add(addCalendarButton, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            accountRow->Add(collapseButton, 0, wxALIGN_CENTER_VERTICAL);
            calendarListSizer_->Add(accountRow, 0, wxEXPAND | wxBOTTOM, 6);

            if (collapsed) {
                continue;
            }

            for (const auto& calendar : calendars) {
            auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);
            auto* visibility = new wxCheckBox(calendarListPanel_, wxID_ANY, "");
            visibility->SetValue(visibleCalendarIds_.count(calendar.id) > 0);
            visibility->Bind(wxEVT_CHECKBOX, [this, calendarId = calendar.id](wxCommandEvent& event) {
                if (event.IsChecked()) {
                    visibleCalendarIds_.insert(calendarId);
                }
                else {
                    visibleCalendarIds_.erase(calendarId);
                    if (selectedCalendarId_ == calendarId) {
                        const auto allCalendars = AllLoadedCalendars();
                        const auto visibleIt = std::find_if(allCalendars.begin(), allCalendars.end(), [this](const Calendar& candidate) {
                            return !candidate.isReadOnly && visibleCalendarIds_.count(candidate.id) > 0;
                        });
                        if (visibleIt != allCalendars.end()) {
                            selectedCalendarId_ = visibleIt->id;
                        }
                    }
                }
                SaveCurrentCalendarSessionState();
                RefreshEvents();
                CallAfter([this]() { RefreshCalendarControls(); });
            });

            auto* selectButton = new wxButton(calendarListPanel_, wxID_ANY, FormatCalendarLabel(calendar), wxDefaultPosition, wxSize(-1, 24), wxBU_LEFT);
            selectButton->SetMinSize(wxSize(150, 24));
            selectButton->SetBackgroundColour(CalendarColour(calendar.colorHex));
            selectButton->SetForegroundColour(*wxWHITE);
            if (selectedCalendarId_ == calendar.id) {
                wxFont font = selectButton->GetFont();
                font.SetWeight(wxFONTWEIGHT_BOLD);
                selectButton->SetFont(font);
            }
            selectButton->Bind(wxEVT_BUTTON, [this, calendarId = calendar.id](wxCommandEvent&) {
                if (const auto calendar = FindLoadedCalendarById(calendarId); calendar.has_value()) {
                    if (calendar->isReadOnly) {
                        statusLabel_->SetLabel("Read-only calendars cannot be selected for new events");
                        return;
                    }
                    currentAccountId_ = calendar->accountId;
                    accountCalendars_ = accountCalendarCache_[calendar->accountId];
                }
                selectedCalendarId_ = calendarId;
                visibleCalendarIds_.insert(calendarId);
                SaveCurrentCalendarSessionState();
                CallAfter([this]() { RefreshCalendarControls(); });
            });

            rowSizer->AddSpacer(18);
            rowSizer->Add(visibility, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            rowSizer->Add(selectButton, 1, wxEXPAND);
            auto* addEventButton = makeSidebarActionButton("+");
            addEventButton->Enable(!calendar.isReadOnly);
            addEventButton->Bind(wxEVT_BUTTON, [this, calendarId = calendar.id](wxCommandEvent&) {
                if (const auto calendar = FindLoadedCalendarById(calendarId); calendar.has_value()) {
                    if (calendar->isReadOnly) {
                        statusLabel_->SetLabel("Read-only calendars cannot be used for new events");
                        return;
                    }
                    currentAccountId_ = calendar->accountId;
                    accountCalendars_ = accountCalendarCache_[calendar->accountId];
                    selectedCalendarId_ = calendarId;
                    visibleCalendarIds_.insert(calendarId);
                    OpenNewAllDayEventDialog(selectedDayEpoch_);
                }
            });
            rowSizer->Add(addEventButton, 0, wxLEFT, 4);
            auto* actionsButton = makeSidebarActionButton("v");
            actionsButton->Bind(wxEVT_BUTTON, [this, calendarId = calendar.id, actionsButton](wxCommandEvent&) {
                ShowCalendarActionMenu(calendarId, actionsButton);
            });
            rowSizer->Add(actionsButton, 0, wxLEFT, 6);
            calendarListSizer_->Add(rowSizer, 0, wxEXPAND | wxBOTTOM, 4);
            }
        }

        const auto selectedCalendar = FindLoadedCalendarById(selectedCalendarId_);
        const wxString selectedLabel = selectedCalendar.has_value()
            ? "Selected calendar: " + FormatCalendarLabel(*selectedCalendar)
            : wxString("Selected calendar: none");
        selectedCalendarLabel_->SetLabel(selectedLabel);

        calendarListPanel_->Layout();
        calendarListPanel_->FitInside();
        calendarListPanel_->Thaw();
    }

    void ShowCalendarActionMenu(const long long calendarId, wxWindow* anchor) {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (!calendar.has_value() || anchor == nullptr) {
            return;
        }

        wxMenu menu;
        const int editId = wxWindow::NewControlId();
        const int deleteId = wxWindow::NewControlId();
        const int exportId = wxWindow::NewControlId();

        menu.Append(editId, "Edit");
        menu.Append(deleteId, "Delete");
        menu.Enable(deleteId, !calendar->isPrimary && !calendar->isReadOnly);
        menu.AppendSeparator();
        menu.Append(exportId, "Export to .ics...");

        menu.Bind(wxEVT_MENU, [this, calendarId](wxCommandEvent&) {
            OpenEditCalendarDialog(calendarId);
        }, editId);
        menu.Bind(wxEVT_MENU, [this, calendarId](wxCommandEvent&) {
            DeleteCalendarWithConfirmation(calendarId);
        }, deleteId);
        menu.Bind(wxEVT_MENU, [this, calendarId](wxCommandEvent&) {
            ExportCalendarToIcal(calendarId);
        }, exportId);

        anchor->PopupMenu(&menu);
    }

    void DeleteCalendarWithConfirmation(const long long calendarId) {
        const auto calendarToDelete = FindLoadedCalendarById(calendarId);
        if (!calendarToDelete.has_value()) {
            return;
        }

        if (calendarToDelete->isPrimary || calendarToDelete->isReadOnly) {
            wxMessageBox("This calendar cannot be deleted.",
                         "Delete calendar",
                         wxOK | wxICON_INFORMATION,
                         this);
            return;
        }

        const int confirm = wxMessageBox(
            "Delete this calendar and all events stored in it?",
            "Delete calendar",
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this);
        if (confirm != wxYES) {
            return;
        }

        try {
            DeleteCalendarById(calendarId);
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Calendar deletion failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
        }
    }

    void ExportCalendarToIcal(const long long calendarId) {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (!calendar.has_value()) {
            return;
        }

        wxFileDialog dialog(
            this,
            "Export calendar to iCalendar file",
            "",
            wxString::FromUTF8(SafeIcsFileName(calendar->name)),
            "iCalendar files (*.ics)|*.ics|All files (*.*)|*.*",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        const auto events = eventRepository_.getByCalendar(calendarId);
        const std::string body = calendar->ExportToIcal(events);

        std::ofstream output(dialog.GetPath().ToStdString(), std::ios::binary | std::ios::trunc);
        if (!output) {
            wxMessageBox("The selected file could not be opened for writing.",
                         "Export failed",
                         wxOK | wxICON_ERROR,
                         this);
            return;
        }

        output << body;
        if (!output.good()) {
            wxMessageBox("The calendar could not be written to the selected file.",
                         "Export failed",
                         wxOK | wxICON_ERROR,
                         this);
            return;
        }

        statusLabel_->SetLabel(wxString::Format(
            "Exported %zu event(s) to iCalendar",
            events.size()));
    }

    void UpdateAuthUiState() {
        if (manageAccountsButton_ != nullptr) {
            manageAccountsButton_->Enable(!authInProgress_);
        }
    }

    bool BeginAuthOperation(const wxString& statusMessage) {
        if (authInProgress_) {
            wxMessageBox("A sign-in is already in progress. Use 'Cancel sign-in' if you want to stop it first.",
                         "Sign-in in progress", wxOK | wxICON_INFORMATION, this);
            return false;
        }

        authInProgress_ = true;
        ClearLastOAuthErrorMessage();
        UpdateAuthUiState();
        statusLabel_->SetLabel(statusMessage);
        ShowAuthProgressDialog(statusMessage);
        Layout();
        return true;
    }

    void FinishAuthOperation() {
        authInProgress_ = false;
        CloseAuthProgressDialog();
        UpdateAuthUiState();
        Layout();
    }

    void ShowAuthProgressDialog(const wxString& statusMessage) {
        if (authProgressDialog_ == nullptr) {
            authProgressDialog_ = new wxDialog(this,
                                               wxID_ANY,
                                               "Signing in",
                                               wxDefaultPosition,
                                               wxDefaultSize,
                                               wxDEFAULT_DIALOG_STYLE & ~wxCLOSE_BOX);

            auto* sizer = new wxBoxSizer(wxVERTICAL);
            authActivityIndicator_ = new wxActivityIndicator(authProgressDialog_, wxID_ANY);
            authProgressLabel_ = new wxStaticText(authProgressDialog_, wxID_ANY, statusMessage);
            auto* cancelButton = new wxButton(authProgressDialog_, wxID_ANY, "Cancel sign-in");

            sizer->Add(authActivityIndicator_, 0, wxALIGN_CENTER | wxTOP | wxLEFT | wxRIGHT, 18);
            sizer->Add(authProgressLabel_, 0, wxALIGN_CENTER | wxALL, 14);
            sizer->Add(cancelButton, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT | wxBOTTOM, 18);
            authProgressDialog_->SetSizerAndFit(sizer);
            authProgressDialog_->CentreOnParent();

            cancelButton->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnCancelAuth, this);
            authProgressDialog_->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
                if (authInProgress_) {
                    statusLabel_->SetLabel("Canceling sign-in...");
                    UpdateAuthProgressMessage("Canceling sign-in...");
                    RequestOAuthCancellation();
                    event.Veto();
                    return;
                }

                event.Skip();
            });
        }

        UpdateAuthProgressMessage(statusMessage);
        authActivityIndicator_->Start();
        authProgressDialog_->Show();
        authProgressDialog_->Raise();
    }

    void UpdateAuthProgressMessage(const wxString& statusMessage) {
        if (authProgressLabel_ != nullptr) {
            authProgressLabel_->SetLabel(statusMessage);
            authProgressDialog_->Layout();
            authProgressDialog_->Fit();
            authProgressDialog_->CentreOnParent();
        }
    }

    void CloseAuthProgressDialog() {
        if (authActivityIndicator_ != nullptr) {
            authActivityIndicator_->Stop();
        }

        if (authProgressDialog_ != nullptr) {
            authProgressDialog_->Hide();
        }
    }

    void HandleAuthOperationResult(const AuthOperationResult& result) {
        FinishAuthOperation();

        if (!result.success) {
            RefreshAccountControls();
            if (result.accountId != 0) {
                accountLoginInProgressIds_.erase(result.accountId);
                signedInAccountIds_.erase(result.accountId);
                accountLoginFailedIds_.insert(result.accountId);
            }
            if (result.error == "Sign-in was canceled.") {
                statusLabel_->SetLabel("Sign-in was canceled");
                return;
            }

            if (result.kind == AuthOperationKind::ACTIVATE_ACCOUNT) {
                statusLabel_->SetLabel(wxString::FromUTF8(result.error));
                const auto platform = PlatformForProvider(result.account.provider);
                if (platform.has_value()) {
                    const int answer = wxMessageBox(
                        wxString::Format("The saved session for '%s' could not be restored.\n\nDo you want to sign in again?",
                                         FormatAccountLabel(result.account)),
                        "Session restore failed",
                        wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION,
                        this);
                    if (answer == wxYES) {
                        StartAddRemoteAccountAsync(*platform);
                    }
                }
                else if (!result.error.empty()) {
                    wxMessageBox(wxString::FromUTF8(result.error), "Account sign-in", wxOK | wxICON_WARNING, this);
                }
                return;
            }

            if (!result.error.empty()) {
                wxMessageBox(wxString::FromUTF8(result.error), "Account sign-in", wxOK | wxICON_WARNING, this);
                statusLabel_->SetLabel(wxString::FromUTF8(result.error));
            }
            else {
                statusLabel_->SetLabel("Account sign-in did not complete");
            }
            return;
        }

        if (result.accountId != 0) {
            accountLoginInProgressIds_.erase(result.accountId);
        }

        if (result.token != nullptr) {
            sessionTokens_[result.accountId] = result.token;
            signedInAccountIds_.insert(result.accountId);
            accountLoginFailedIds_.erase(result.accountId);
        }
        ReloadAccounts();
        RefreshAllAccountCalendarCache();

        if (result.kind == AuthOperationKind::ADD_ACCOUNT) {
            SetCurrentAccount(result.accountId, false);
            events_.clear();
            eventListFingerprint_ = 0;
            RefreshViewState();
            statusLabel_->SetLabel("Account connected and calendars loaded");
        }
        else {
            if (currentAccountId_ == result.accountId) {
                SetCurrentAccount(result.accountId, false);
                events_.clear();
                eventListFingerprint_ = 0;
                RefreshViewState();
            }
            statusLabel_->SetLabel("Account session restored");
        }

        RegisterEventUploadSessionForAccount(result.accountId);

        wxWeakRef<LocalCalendarFrame> weakThis(this);
        wxTheApp->CallAfter([weakThis]() {
            if (!weakThis) {
                return;
            }
            weakThis->RefreshEvents();
        });

        if (!result.warning.empty()) {
            wxMessageBox(wxString::FromUTF8(result.warning), "Calendar sync warning", wxOK | wxICON_WARNING, this);
        }
    }

    void HandleSilentAuthOperationResult(const AuthOperationResult& result) {
        if (result.accountId != 0) {
            accountLoginInProgressIds_.erase(result.accountId);
        }

        if (!result.success) {
            if (result.accountId != 0) {
                signedInAccountIds_.erase(result.accountId);
                accountLoginFailedIds_.insert(result.accountId);
            }
            RefreshAccountControls();
            RefreshCalendarControls();
            return;
        }

        if (result.token != nullptr) {
            sessionTokens_[result.accountId] = result.token;
            signedInAccountIds_.insert(result.accountId);
            accountLoginFailedIds_.erase(result.accountId);
            RegisterEventUploadSessionForAccount(result.accountId);
        }
        ReloadAccounts();
        RefreshAllAccountCalendarCache();
        RefreshAccountControls();
        RefreshCalendarControls();
        RefreshEvents();
        statusLabel_->SetLabel("Stored account sessions restored");
    }

    void StartAddRemoteAccountAsync(const int platform) {
        if (!BeginAuthOperation("Opening sign-in...")) {
            RefreshAccountControls();
            return;
        }

        wxWeakRef<LocalCalendarFrame> weakThis(this);
        const std::string dbPath = dbPath_;
        std::thread([weakThis, dbPath, platform]() {
            const AuthOperationResult result = RunAddAccountAuthOperation(dbPath, platform);
            wxTheApp->CallAfter([weakThis, result]() {
                if (!weakThis) {
                    return;
                }

                weakThis->HandleAuthOperationResult(result);
            });
        }).detach();
    }

    void StartAccountActivationAsync(const Account& account) {
        if (!BeginAuthOperation(wxString::Format("Restoring session for %s...", FormatAccountLabel(account)))) {
            return;
        }

        accountLoginInProgressIds_.insert(account.id);
        accountLoginFailedIds_.erase(account.id);
        RefreshAccountControls();
        RefreshCalendarControls();

        wxWeakRef<LocalCalendarFrame> weakThis(this);
        const std::string dbPath = dbPath_;
        std::thread([weakThis, dbPath, account]() {
            const AuthOperationResult result = RunActivateAccountAuthOperation(dbPath, account);
            wxTheApp->CallAfter([weakThis, result]() {
                if (!weakThis) {
                    return;
                }

                weakThis->HandleAuthOperationResult(result);
            });
        }).detach();
    }

    void StartSilentAccountActivationForStoredAccounts() {
        std::vector<Account> accountsToRestore;
        for (const auto& account : availableAccounts_) {
            if (IsLocalProvider(account.provider) || account.refreshToken.empty()) {
                continue;
            }

            accountLoginInProgressIds_.insert(account.id);
            accountLoginFailedIds_.erase(account.id);
            accountsToRestore.push_back(account);
        }

        if (!accountsToRestore.empty()) {
            wxWeakRef<LocalCalendarFrame> weakThis(this);
            const std::string dbPath = dbPath_;
            std::thread([weakThis, dbPath, accounts = std::move(accountsToRestore)]() {
                for (const auto& account : accounts) {
                const AuthOperationResult result = RunActivateAccountAuthOperation(dbPath, account);
                wxTheApp->CallAfter([weakThis, result]() {
                    if (!weakThis) {
                        return;
                    }

                    weakThis->HandleSilentAuthOperationResult(result);
                });
                }
            }).detach();
        }
        RefreshAccountControls();
        RefreshCalendarControls();
    }

    bool CreateCalendarForCurrentAccount(const std::string& name,
                                         const std::string& description,
                                         const std::string& colorHex) {
        if (currentAccountId_ == 0) {
            return false;
        }

        Calendar calendar{};
        calendar.accountId = currentAccountId_;
        const auto currentAccount = FindLoadedAccountById(currentAccountId_);
        const bool localAccount = currentAccount.has_value() && IsLocalProvider(currentAccount->provider);
        calendar.providerCalendarId = localAccount
            ? "calendar-" + std::to_string(currentAccountId_) + "-" +
                  std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
                  std::to_string(static_cast<long long>(accountCalendars_.size() + 1))
            : "";
        calendar.name = name;
        calendar.description = description;
        calendar.colorHex = NormalizeCalendarColor(colorHex.empty() ? RandomCalendarColor() : colorHex);
        calendar.timezone = GetCurrentLocalTimeZoneName();
        calendar.isPrimary = false;
        calendar.isReadOnly = false;
        calendar.syncEnabled = !localAccount;
        calendar.syncStatus = localAccount ? SYNCED : PENDING_INSERT;

        const long long createdId = calendarRepository_.upsert(calendar);
        const auto created = calendarRepository_.getById(createdId);
        if (!created.has_value()) {
            return false;
        }

        accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
        accountCalendarCache_[currentAccountId_] = accountCalendars_;
        selectedCalendarId_ = created->id;
        visibleCalendarIds_.insert(created->id);
        SaveCurrentCalendarSessionState();
        RefreshCalendarControls();
        RefreshEvents();
        statusLabel_->SetLabel("Calendar created");
        return true;
    }

    bool ImportIcalCalendarForCurrentAccount(Calendar::IcalImportResult imported, const std::string& colorHex) {
        if (currentAccountId_ == 0 || imported.events.empty()) {
            return false;
        }

        const auto currentAccount = FindLoadedAccountById(currentAccountId_);
        const bool localAccount = currentAccount.has_value() && IsLocalProvider(currentAccount->provider);
        Calendar calendar{};
        calendar.accountId = currentAccountId_;
        calendar.providerCalendarId = localAccount
            ? "ical-calendar-" + std::to_string(currentAccountId_) + "-" +
                  std::to_string(static_cast<long long>(std::time(nullptr)))
            : "";
        calendar.name = imported.name;
        calendar.description = imported.description;
        calendar.colorHex = NormalizeCalendarColor(colorHex.empty() ? RandomCalendarColor() : colorHex);
        calendar.timezone = imported.timezone.empty() ? GetCurrentLocalTimeZoneName() : imported.timezone;
        calendar.isPrimary = false;
        calendar.isReadOnly = false;
        calendar.syncEnabled = !localAccount;
        calendar.syncStatus = localAccount ? SYNCED : PENDING_INSERT;
        calendar.deletedAt = 0;

        try {
            long long createdCalendarId = 0;
            RunInSavepoint(db_, "ical_calendar_import", [&]() {
                createdCalendarId = calendarRepository_.upsert(calendar);

                size_t index = 0;
                for (auto event : imported.events) {
                    event.calendarId = createdCalendarId;
                    event.timezone = event.timezone.empty() ? calendar.timezone : event.timezone;
                    NormalizeAllDayEventRange(event);
                    event.deletedAt = 0;
                    event.createdAt = event.createdAt == 0 ? std::time(nullptr) : event.createdAt;
                    event.updatedAt = std::time(nullptr);

                    if (localAccount) {
                        if (event.providerEventId.empty()) {
                            event.providerEventId = MakeLocalProviderEventId(event.startDateTime) + "-" + std::to_string(index);
                        }
                        event.syncStatus = SYNCED;
                    }
                    else {
                        event.providerEventId.clear();
                        event.providerMasterId.clear();
                        event.syncStatus = PENDING_INSERT;
                    }

                    eventRepository_.upsert(event);
                    ++index;
                }
            });

            const auto created = calendarRepository_.getById(createdCalendarId);
            if (!created.has_value()) {
                return false;
            }

            accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
            accountCalendarCache_[currentAccountId_] = accountCalendars_;
            selectedCalendarId_ = created->id;
            visibleCalendarIds_.insert(created->id);
            SaveCurrentCalendarSessionState();
            RefreshCalendarControls();
            RefreshEvents();
            statusLabel_->SetLabel(wxString::Format(
                "Imported %zu event(s) from iCalendar",
                imported.events.size()));
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Import failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    bool UpdateCalendar(Calendar calendar,
                        const std::string& name,
                        const std::string& description,
                        const std::string& colorHex) {
        const bool remoteFieldsChanged = !calendar.isReadOnly &&
            (calendar.name != name || calendar.description != description);
        if (!calendar.isReadOnly) {
            calendar.name = name;
            calendar.description = description;
        }
        calendar.colorHex = NormalizeCalendarColor(colorHex);
        if (remoteFieldsChanged && calendar.syncEnabled && !calendar.providerCalendarId.empty()) {
            calendar.syncStatus = PENDING_UPDATE;
        }
        calendarRepository_.upsert(calendar);
        accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
        accountCalendarCache_[currentAccountId_] = accountCalendars_;
        RefreshCalendarControls();
        RefreshEvents();
        statusLabel_->SetLabel("Calendar updated");
        return true;
    }

    bool DeleteCalendarById(const long long calendarId) {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (!calendar.has_value() || calendar->isPrimary || calendar->isReadOnly) {
            return false;
        }

        if (calendar->syncEnabled && !calendar->providerCalendarId.empty() && calendar->syncStatus != PENDING_INSERT) {
            Calendar deleted = *calendar;
            deleted.deletedAt = std::time(nullptr);
            deleted.syncStatus = PENDING_DELETE;
            calendarRepository_.upsert(deleted);
        }
        else {
            calendarRepository_.deleteById(calendarId);
        }

        accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
        accountCalendarCache_[currentAccountId_] = accountCalendars_;
        visibleCalendarIds_.erase(calendarId);

        const bool selectedStillExists = std::any_of(accountCalendars_.begin(), accountCalendars_.end(), [this](const Calendar& existing) {
            return existing.id == selectedCalendarId_;
        });
        if (!selectedStillExists) {
            const auto primaryIt = std::find_if(accountCalendars_.begin(), accountCalendars_.end(), [](const Calendar& existing) {
                return existing.isPrimary;
            });
            selectedCalendarId_ = primaryIt != accountCalendars_.end()
                ? primaryIt->id
                : (accountCalendars_.empty() ? 0 : accountCalendars_.front().id);
        }

        if (selectedCalendarId_ != 0) {
            visibleCalendarIds_.insert(selectedCalendarId_);
        }

        SaveCurrentCalendarSessionState();
        RefreshCalendarControls();
        RefreshEvents();
        statusLabel_->SetLabel("Calendar deleted");
        return true;
    }

    void OpenCreateCalendarDialog(const long long accountId = 0) {
        const long long targetAccountId = accountId == 0 ? currentAccountId_ : accountId;
        const auto currentAccount = FindLoadedAccountById(targetAccountId);
        if (!currentAccount.has_value()) {
            wxMessageBox("No current account is selected.", "Calendar error", wxOK | wxICON_WARNING, this);
            return;
        }
        currentAccountId_ = targetAccountId;
        accountCalendars_ = accountCalendarCache_[targetAccountId];

        CalendarEditorDialog dialog(this);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (dialog.HasIcalImport()) {
            if (!ImportIcalCalendarForCurrentAccount(dialog.GetIcalImportResult(), dialog.GetCalendarColor())) {
                wxMessageBox("The iCalendar calendar could not be imported.", "Calendar error", wxOK | wxICON_ERROR, this);
            }
            return;
        }

        if (!CreateCalendarForCurrentAccount(dialog.GetCalendarName(), dialog.GetCalendarDescription(), dialog.GetCalendarColor())) {
            wxMessageBox("The calendar could not be created.", "Calendar error", wxOK | wxICON_ERROR, this);
        }
    }

    void OpenEditCalendarDialog(const long long calendarId) {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (!calendar.has_value()) {
            return;
        }

        CalendarEditorDialog dialog(this, calendar);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        UpdateCalendar(*calendar, dialog.GetCalendarName(), dialog.GetCalendarDescription(), dialog.GetCalendarColor());
    }

    std::optional<Account> FindLoadedAccountById(const long long accountId) const {
        const auto it = std::find_if(availableAccounts_.begin(), availableAccounts_.end(), [accountId](const Account& account) {
            return account.id == accountId;
        });

        if (it == availableAccounts_.end()) {
            return std::nullopt;
        }

        return *it;
    }

    std::optional<Calendar> FindLoadedCalendarById(const long long calendarId) const {
        for (const auto& [_, calendars] : accountCalendarCache_) {
            const auto cachedIt = std::find_if(calendars.begin(), calendars.end(), [calendarId](const Calendar& calendar) {
                return calendar.id == calendarId;
            });
            if (cachedIt != calendars.end()) {
                return *cachedIt;
            }
        }

        const auto it = std::find_if(accountCalendars_.begin(), accountCalendars_.end(), [calendarId](const Calendar& calendar) {
            return calendar.id == calendarId;
        });

        if (it == accountCalendars_.end()) {
            return std::nullopt;
        }

        return *it;
    }

    void ReloadAccounts() {
        availableAccounts_ = accountRepository_.GetAllAccounts();
    }

    void RefreshAccountControls() {
        UpdateAuthUiState();
        Layout();
    }

    bool SetCurrentAccount(const long long accountId, const bool refreshEvents) {
        SaveCurrentCalendarSessionState();

        auto account = FindLoadedAccountById(accountId);
        if (!account.has_value()) {
            account = accountRepository_.GetById(accountId);
            if (!account.has_value()) {
                return false;
            }
        }

        currentAccountId_ = account->id;

        LoadCalendarsForCurrentAccount(*account);
        RefreshAccountControls();
        RefreshCalendarControls();

        if (refreshEvents) {
            RefreshEvents();
        }

        const bool hasSessionToken = sessionTokens_.count(account->id) > 0 &&
            sessionTokens_[account->id] != nullptr;
        if (!IsLocalProvider(account->provider) && !hasSessionToken) {
            StartAccountActivationAsync(*account);
        }

        return true;
    }

    bool DeleteAccountCascade(const long long accountId) {
        if (accountId == localAccountId_) {
            return false;
        }

        return accountRepository_.DeleteById(accountId);
    }

    void DeleteAccountWithConfirmation(const long long accountId) {
        const auto account = FindLoadedAccountById(accountId);
        if (!account.has_value() || IsLocalProvider(account->provider)) {
            return;
        }

        const int confirm = wxMessageBox(
            wxString::Format("Remove account '%s' and all calendars/events stored under it?",
                             FormatAccountLabel(*account)),
            "Remove account",
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this);
        if (confirm != wxYES) {
            return;
        }

        try {
            if (!DeleteAccountCascade(account->id)) {
                return;
            }

            if (eventUploadScheduler_ != nullptr) {
                eventUploadScheduler_->RemoveSession(account->id);
            }
            sessionTokens_.erase(account->id);
            signedInAccountIds_.erase(account->id);
            accountLoginFailedIds_.erase(account->id);
            accountLoginInProgressIds_.erase(account->id);
            accountCalendarCache_.erase(account->id);
            ReloadAccounts();
            RefreshAllAccountCalendarCache();
            SetCurrentAccount(localAccountId_, true);
            statusLabel_->SetLabel("Account removed");
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Account deletion failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
        }
    }

    void PromptForNewAccount() {
        wxArrayString providers;
        providers.Add("Google");
        providers.Add("Outlook");

        wxSingleChoiceDialog dialog(this,
                                    "Select the provider for the new account.",
                                    "New account",
                                    providers);
        if (dialog.ShowModal() != wxID_OK) {
            RefreshAccountControls();
            return;
        }

        const int platform = dialog.GetSelection() == 0 ? GOOGLE : MICROSOFT;
        StartAddRemoteAccountAsync(platform);
    }

    void ShowAccountManagerDialog() {
        std::vector<AccountManagerDialog::AccountState> accountStates;
        for (const auto& account : availableAccounts_) {
            accountStates.push_back(AccountManagerDialog::AccountState{
                account,
                IsLocalProvider(account.provider) || signedInAccountIds_.count(account.id) > 0,
                accountLoginFailedIds_.count(account.id) > 0,
                accountLoginInProgressIds_.count(account.id) > 0});
        }

        AccountManagerDialog dialog(
            this,
            accountStates,
            [this]() { PromptForNewAccount(); },
            [this](const long long accountId) {
                if (const auto account = FindLoadedAccountById(accountId); account.has_value()) {
                    StartAccountActivationAsync(*account);
                }
            },
            [this](const long long accountId) { DeleteAccountWithConfirmation(accountId); });
        dialog.ShowModal();
    }

    void StartEventUploadScheduler() {
        wxWeakRef<LocalCalendarFrame> weakThis(this);
        eventUploadScheduler_ = std::make_unique<EventUploadScheduler>(
            dbPath_,
            [weakThis](const PendingEventUploadResult& result) {
                wxTheApp->CallAfter([weakThis, result]() {
                    if (!weakThis) {
                        return;
                    }

                    weakThis->HandleEventUploadResult(result);
                });
            }
        );
        eventUploadScheduler_->Start();
    }

    bool RegisterEventUploadSessionForAccount(const long long accountId) {
        const auto account = FindLoadedAccountById(accountId).has_value()
            ? FindLoadedAccountById(accountId)
            : accountRepository_.GetById(accountId);
        if (!account.has_value() || IsLocalProvider(account->provider)) {
            return false;
        }

        if (eventUploadScheduler_ == nullptr) {
            return false;
        }

        const auto sessionIt = sessionTokens_.find(account->id);
        if (sessionIt == sessionTokens_.end() || sessionIt->second == nullptr) {
            return false;
        }

        const VisibleRange syncRange = ConvertDisplayRangeToUtc(
            ExpandVisibleRangeForRecurrence(ComputeVisibleRange(), currentViewMode_));
        eventUploadScheduler_->UpsertSession(PendingEventUploadSession{
            account->id,
            account->provider,
            sessionIt->second,
            syncRange.startEpoch,
            syncRange.endEpoch});
        return true;
    }

    void UpdateEventUploadSessionsForVisibleRange() {
        if (eventUploadScheduler_ == nullptr) {
            return;
        }

        for (const auto& [accountId, token] : sessionTokens_) {
            if (token != nullptr) {
                RegisterEventUploadSessionForAccount(accountId);
            }
        }
    }

    void StartManualSyncForAllAccounts() {
        if (eventUploadScheduler_ == nullptr) {
            return;
        }

        UpdateEventUploadSessionsForVisibleRange();
        if (sessionTokens_.empty()) {
            statusLabel_->SetLabel("Sync requires at least one signed-in remote account");
            return;
        }

        eventUploadScheduler_->QueueAllSignedInAccounts();
        statusLabel_->SetLabel("Sync for all signed-in accounts queued");
    }

    void HandleEventUploadResult(const PendingEventUploadResult& result) {
        if (!result.success) {
            if (!result.message.empty()) {
                statusLabel_->SetLabel(wxString::Format("Event upload failed: %s", wxString::FromUTF8(result.message)));
            }
            return;
        }

        if (const auto account = FindLoadedAccountById(result.accountId); account.has_value()) {
            accountCalendarCache_[result.accountId] = calendarRepository_.getByAccount(result.accountId);
            if (result.accountId == currentAccountId_) {
                accountCalendars_ = accountCalendarCache_[result.accountId];
            }
            RefreshCalendarControls();
            RefreshEvents();
        }
        statusLabel_->SetLabel(wxString::FromUTF8(result.message));
    }

    void BuildLayout() {
        auto* panel = new wxPanel(this);
        auto* rootSizer = new wxBoxSizer(wxVERTICAL);

        auto* toolbarSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* appTitle = new wxStaticText(panel, wxID_ANY, "Local Calendar");

        modeComboBox_ = new wxComboBox(panel, wxID_ANY, "", wxDefaultPosition, wxSize(110, -1), 0, nullptr, wxCB_READONLY);
        modeComboBox_->Append("Month");
        modeComboBox_->Append("Week");
        modeComboBox_->Append("Day");
        modeComboBox_->SetSelection(0);
        todayButton_ = new wxButton(panel, wxID_ANY, "Today");
        previousButton_ = new wxButton(panel, wxID_ANY, "<");
        nextButton_ = new wxButton(panel, wxID_ANY, ">");
        monthTitleLabel_ = new wxStaticText(panel, wxID_ANY, "");
        monthTitleLabel_->SetMinSize(wxSize(90, -1));
        yearComboBox_ = new wxComboBox(panel, wxID_ANY, "", wxDefaultPosition, wxSize(110, -1), 0, nullptr, wxCB_READONLY);
        PopulateYearComboWindow(visibleYear_);
        auto* periodSizer = new wxBoxSizer(wxHORIZONTAL);
        periodSizer->Add(monthTitleLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
        periodSizer->Add(yearComboBox_, 0, wxALIGN_CENTER_VERTICAL);

        manageAccountsButton_ = new wxButton(panel, wxID_ANY, "Manage accounts");

        toolbarSizer->Add(appTitle, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 20);
        toolbarSizer->Add(modeComboBox_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
        toolbarSizer->Add(todayButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
        toolbarSizer->Add(previousButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
        toolbarSizer->Add(nextButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 18);
        toolbarSizer->AddStretchSpacer(1);
        toolbarSizer->Add(periodSizer, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 16);
        toolbarSizer->AddStretchSpacer(1);
        toolbarSizer->Add(manageAccountsButton_, 0, wxALIGN_CENTER_VERTICAL);

        auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* leftPane = new wxBoxSizer(wxVERTICAL);
        calendarBook_ = new wxSimplebook(panel, wxID_ANY);

        auto* monthPage = new wxPanel(calendarBook_);
        monthPage->SetBackgroundColour(wxColour(246, 248, 252));
        auto* monthSizer = new wxBoxSizer(wxVERTICAL);
        auto* weekdaySizer = new wxGridSizer(1, 7, 0, 0);
        const std::array<const char*, 7> weekdays = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
        for (const char* weekday : weekdays) {
            auto* weekdayLabel = new wxStaticText(monthPage, wxID_ANY, weekday);
            weekdayLabel->SetBackgroundColour(wxColour(246, 248, 252));
            weekdaySizer->Add(weekdayLabel, 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
        }
        monthSizer->Add(weekdaySizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* gridPanel = new wxPanel(monthPage);
        gridPanel->SetBackgroundColour(wxColour(213, 219, 227));
        auto* gridSizer = new wxGridSizer(5, 7, 1, 1);
        for (int index = 0; index < kMonthCellCount; ++index) {
            monthCells_[index] = new MonthCellPanel(
                gridPanel,
                index,
                [this](const int cellIndex) { HandleMonthCellClicked(cellIndex); },
                [this](const int cellIndex) { HandleMonthCellCreateEvent(cellIndex); },
                [this](const long long eventId) { OpenEventById(eventId); });
            gridSizer->Add(monthCells_[index], 1, wxEXPAND);
        }
        gridPanel->SetSizer(gridSizer);
        monthSizer->Add(gridPanel, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
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

        auto* sidePane = new wxBoxSizer(wxVERTICAL);
        sidePane->Add(new wxStaticText(panel, wxID_ANY, "Accounts and calendars"), 0, wxALL, 10);

        refreshButton_ = new wxButton(panel, wxID_ANY, "Refresh");
        sidePane->Add(refreshButton_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        selectedCalendarLabel_ = new wxStaticText(panel, wxID_ANY, "Selected calendar: none");
        sidePane->Add(selectedCalendarLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

        calendarListPanel_ = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxSize(-1, 150), wxVSCROLL | wxBORDER_SIMPLE);
        calendarListPanel_->SetScrollRate(0, 6);
        calendarListSizer_ = new wxBoxSizer(wxVERTICAL);
        calendarListPanel_->SetSizer(calendarListSizer_);
        sidePane->Add(calendarListPanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        statusLabel_ = new wxStaticText(panel, wxID_ANY, "Ready");
        sidePane->Add(statusLabel_, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        contentSizer->Add(leftPane, 5, wxEXPAND | wxALL, 12);
        contentSizer->Add(sidePane, 0, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

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
        modeComboBox_->SetBackgroundColour(surfaceBg);
        todayButton_->SetBackgroundColour(surfaceBg);
        previousButton_->SetBackgroundColour(surfaceBg);
        nextButton_->SetBackgroundColour(surfaceBg);
        yearComboBox_->SetBackgroundColour(surfaceBg);
        manageAccountsButton_->SetBackgroundColour(surfaceBg);
    }

    void BindEvents() {
        modeComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnModeChanged, this);
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(-1); });
        nextButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(1); });
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        yearComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnYearChanged, this);
        manageAccountsButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnManageAccounts, this);
    }

    std::vector<Event> EventsForDay(const long long dayEpoch) const {
        std::vector<Event> results;
        const long long dayEnd = dayEpoch + kSecondsPerDay;

        for (const auto& event : events_) {
            if (event.deletedAt != 0) {
                continue;
            }
            if (event.GetDisplayStartEpoch() < dayEnd && event.GetDisplayEndEpoch() > dayEpoch) {
                results.push_back(event);
            }
        }

        return results;
    }

    void RebuildEventIndex() {
        eventIndexById_.clear();
        eventIndexById_.reserve(events_.size());
        for (size_t index = 0; index < events_.size(); ++index) {
            if (events_[index].id > 0) {
                eventIndexById_[events_[index].id] = index;
            }
        }
    }

    std::optional<Event> FindLoadedEventById(const long long eventId) const {
        if (eventId < 0) {
            const auto projectedIt = projectedOccurrences_.find(eventId);
            if (projectedIt != projectedOccurrences_.end()) {
                return projectedIt->second;
            }
            return std::nullopt;
        }

        const auto indexIt = eventIndexById_.find(eventId);
        if (indexIt != eventIndexById_.end() && indexIt->second < events_.size()) {
            return events_[indexIt->second];
        }

        return std::nullopt;
    }

    VisibleRange ComputeVisibleRange() const {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            const int firstOffset = MonthGridOffset(visibleYear_, visibleMonth_);
            const long long firstDayOfMonth = MakeUtcEpoch(visibleYear_, visibleMonth_, 1);
            const long long gridStart = firstDayOfMonth - static_cast<long long>(firstOffset) * kSecondsPerDay;
            return VisibleRange{gridStart, gridStart + static_cast<long long>(kMonthCellCount) * kSecondsPerDay};
        }

        if (currentViewMode_ == CalendarViewMode::WEEK) {
            const long long weekStart = StartOfUtcWeek(selectedDayEpoch_);
            return VisibleRange{weekStart, weekStart + 7LL * kSecondsPerDay};
        }

        return VisibleRange{selectedDayEpoch_, selectedDayEpoch_ + kSecondsPerDay};
    }

    void EnsureVisibleRangeCoverageAsync(const VisibleRange& utcRange, const bool forceRemoteReload = false) {
        if (utcRange.startEpoch >= utcRange.endEpoch) {
            return;
        }

        const auto account = FindLoadedAccountById(currentAccountId_);
        if (!account.has_value() || IsLocalProvider(account->provider)) {
            return;
        }

        const auto platform = PlatformForProvider(account->provider);
        if (!platform.has_value()) {
            return;
        }

        const auto sessionIt = sessionTokens_.find(account->id);
        if (sessionIt == sessionTokens_.end() || sessionIt->second == nullptr) {
            return;
        }
        auto sessionToken = sessionIt->second;

        std::vector<Calendar> remoteVisibleCalendars;
        for (const auto& calendar : accountCalendars_) {
            if (visibleCalendarIds_.count(calendar.id) == 0 ||
                !calendar.syncEnabled ||
                calendar.providerCalendarId.empty()) {
                continue;
            }

            remoteVisibleCalendars.push_back(calendar);
        }

        if (remoteVisibleCalendars.empty()) {
            return;
        }

        std::string coverageKey = std::to_string(account->id) + ":" +
            std::to_string(utcRange.startEpoch) + ":" +
            std::to_string(utcRange.endEpoch);
        for (const auto& calendar : remoteVisibleCalendars) {
            coverageKey += ":" + std::to_string(calendar.id);
        }
        if (!forceRemoteReload && coveredRangeKeys_.count(coverageKey) > 0) {
            return;
        }
        if (coverageFetchKeys_.count(coverageKey) > 0) {
            return;
        }
        coverageFetchKeys_.insert(coverageKey);

        const std::string dbPath = dbPath_;
        const long long accountId = account->id;
        const std::string provider = account->provider;
        wxWeakRef<LocalCalendarFrame> weakThis(this);

        std::thread([
            weakThis,
            dbPath,
            accountId,
            provider,
            sessionToken = std::move(sessionToken),
            calendars = std::move(remoteVisibleCalendars),
            utcRange,
            coverageKey,
            forceRemoteReload]() mutable {
            bool fetchedAnyRange = false;
            bool allRangesCovered = true;
            std::string error;

            try {
                SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
                ConfigureSqliteConnection(db);
                AccountRepository accountRepository(db);
                CalendarRepository calendarRepository(db);
                EventRepository eventRepository(db);
                CalendarSyncRangeRepository rangeRepository(db);
                RepositoryHolder repository{calendarRepository, eventRepository};
                auto accessTokenProvider = [&sessionToken]() {
                    return sessionToken != nullptr ? sessionToken->GetToken() : std::string{};
                };

                std::unique_ptr<CalendarSyncService> service;
                if (provider == "GOOGLE") {
                    service = std::make_unique<GoogleCalendarSyncService>(calendarRepository, eventRepository);
                }
                else if (provider == "MICROSOFT") {
                    service = std::make_unique<OutlookCalendarSyncService>(calendarRepository, eventRepository);
                }
                else {
                    return;
                }

                for (const auto& calendar : calendars) {
                    const auto persistedCalendar = calendarRepository.getById(calendar.id);
                    if (!persistedCalendar.has_value() ||
                        persistedCalendar->accountId != accountId ||
                        !persistedCalendar->syncEnabled ||
                        persistedCalendar->providerCalendarId.empty()) {
                        continue;
                    }

                    if (provider != "GOOGLE" &&
                        !forceRemoteReload &&
                        rangeRepository.isRangeCovered(
                            persistedCalendar->id,
                            utcRange.startEpoch,
                            utcRange.endEpoch)) {
                        continue;
                    }

                    const bool success = service->fetchAndStoreRemoteEventsInRange(
                        *persistedCalendar,
                        utcRange.startEpoch,
                        utcRange.endEpoch,
                        accessTokenProvider,
                        repository);
                    if (success) {
                        if (provider != "MICROSOFT" && provider != "GOOGLE") {
                            rangeRepository.markRangeCovered(
                                persistedCalendar->id,
                                utcRange.startEpoch,
                                utcRange.endEpoch);
                        }
                        fetchedAnyRange = true;
                    }
                    else {
                        allRangesCovered = false;
                    }
                }

                const std::string updatedRefreshToken = sessionToken != nullptr ? sessionToken->GetRefreshToken() : "";
                if (!updatedRefreshToken.empty()) {
                    auto persistedAccount = accountRepository.GetById(accountId);
                    if (persistedAccount.has_value() && persistedAccount->refreshToken != updatedRefreshToken) {
                        persistedAccount->refreshToken = updatedRefreshToken;
                        accountRepository.Upsert(*persistedAccount);
                    }
                }
            }
            catch (const std::exception& ex) {
                error = ex.what();
                allRangesCovered = false;
            }

            wxTheApp->CallAfter([weakThis, coverageKey, fetchedAnyRange, allRangesCovered, error = std::move(error)]() {
                if (!weakThis) {
                    return;
                }

                weakThis->coverageFetchKeys_.erase(coverageKey);
                if (allRangesCovered) {
                    weakThis->coveredRangeKeys_.insert(coverageKey);
                }
                if (!error.empty()) {
                    weakThis->statusLabel_->SetLabel(wxString::Format(
                        "Remote range load failed: %s",
                        wxString::FromUTF8(error)));
                    return;
                }

                if (fetchedAnyRange) {
                    weakThis->RefreshEvents();
                }
                else if (!allRangesCovered) {
                    weakThis->statusLabel_->SetLabel("Remote range was not loaded; it will be retried later");
                }
            });
        }).detach();
    }

    void RefreshEvents(const bool forceRemoteRangeReload = false) {
        const long long loadGeneration = ++eventLoadGeneration_;

        const std::vector<Calendar> allCalendars = AllLoadedCalendars();
        if (allCalendars.empty()) {
            events_.clear();
            eventIndexById_.clear();
            projectedOccurrences_.clear();
            projectedOccurrenceMasterIds_.clear();
            eventListFingerprint_ = 0;
            RefreshViewState();
            statusLabel_->SetLabel("No calendars available for the selected account");
            return;
        }

        const VisibleRange visibleRange = ComputeVisibleRange();
        const VisibleRange bufferedRange = ExpandVisibleRangeForRecurrence(visibleRange, currentViewMode_);
        const VisibleRange bufferedUtcRange = ConvertDisplayRangeToUtc(bufferedRange);
        UpdateEventUploadSessionsForVisibleRange();
        const std::vector<Calendar> calendarSnapshot = allCalendars;
        const std::set<long long> visibleCalendarIds = visibleCalendarIds_;
        std::unordered_map<long long, std::string> calendarProviders;
        for (const auto& calendar : calendarSnapshot) {
            const auto account = FindLoadedAccountById(calendar.accountId);
            if (account.has_value()) {
                calendarProviders[calendar.id] = account->provider;
            }
        }
        const std::string dbPath = dbPath_;
        statusLabel_->SetLabel("Loading events...");

        wxWeakRef<LocalCalendarFrame> weakThis(this);
        std::thread([
            weakThis,
            dbPath,
            calendarSnapshot,
            visibleCalendarIds,
            calendarProviders,
            visibleRange,
            bufferedRange,
            bufferedUtcRange,
            forceRemoteRangeReload,
            loadGeneration]() {
            const EventLoadResult result = LoadEventsForVisibleRange(EventLoadRequest{
                dbPath,
                calendarSnapshot,
                visibleCalendarIds,
                calendarProviders,
                visibleRange,
                bufferedRange,
                bufferedUtcRange});

            wxTheApp->CallAfter([weakThis, loadGeneration, bufferedUtcRange, forceRemoteRangeReload, result = std::move(result)]() mutable {
                if (!weakThis || weakThis->eventLoadGeneration_ != loadGeneration) {
                    return;
                }

                if (!result.error.empty()) {
                    weakThis->statusLabel_->SetLabel(wxString::Format(
                        "Event load failed: %s",
                        wxString::FromUTF8(result.error)));
                    return;
                }

                weakThis->events_ = std::move(result.events);
                weakThis->projectedOccurrences_ = std::move(result.projectedOccurrences);
                weakThis->projectedOccurrenceMasterIds_ = std::move(result.projectedOccurrenceMasterIds);
                weakThis->RebuildEventIndex();
                weakThis->eventListFingerprint_ = result.fingerprint;
                weakThis->RefreshViewState();
                weakThis->statusLabel_->SetLabel(wxString::Format(
                    "Loaded %zu visible event(s)",
                    result.visibleCount));
                weakThis->EnsureVisibleRangeCoverageAsync(bufferedUtcRange, forceRemoteRangeReload);
            });
        }).detach();
    }

    void RefreshViewState() {
        RefreshHeaderTitle();
        RefreshMonthGrid();
        RefreshTimelineViews();
    }

    void RefreshHeaderTitle() {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            monthTitleLabel_->SetLabel(FormatMonthName(visibleMonth_));
            modeComboBox_->SetSelection(0);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            monthTitleLabel_->SetLabel(FormatWeekTitle(StartOfUtcWeek(selectedDayEpoch_)));
            modeComboBox_->SetSelection(1);
        }
        else {
            monthTitleLabel_->SetLabel(FormatDayHeader(selectedDayEpoch_));
            modeComboBox_->SetSelection(2);
        }
        EnsureYearComboContains(visibleYear_);
        yearComboBox_->Enable(currentViewMode_ == CalendarViewMode::MONTH);
    }

    void RefreshMonthGrid() {
        const int firstOffset = MonthGridOffset(visibleYear_, visibleMonth_);
        const int daysCurrentMonth = DaysInMonth(visibleYear_, visibleMonth_);
        const int previousMonth = visibleMonth_ == 1 ? 12 : visibleMonth_ - 1;
        const int previousYear = visibleMonth_ == 1 ? visibleYear_ - 1 : visibleYear_;
        const int nextMonth = visibleMonth_ == 12 ? 1 : visibleMonth_ + 1;
        const int nextYear = visibleMonth_ == 12 ? visibleYear_ + 1 : visibleYear_;
        const int daysPreviousMonth = DaysInMonth(previousYear, previousMonth);
        const long long today = StartOfUtcDay(CurrentLocalDisplayEpoch());
        std::array<MonthCellMeta, kMonthCellCount> cells{};
        std::array<std::vector<std::optional<MonthCellEventSegment>>, kMonthCellCount> cellRows{};
        std::array<size_t, kMonthCellCount> hiddenRowCounts{};

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
            cells[cellIndex] = MonthCellMeta{dayEpoch, dayNumber, inCurrentMonth};
        }

        std::array<std::vector<const Event*>, kMonthCellCount> singleDayEventsByCell{};
        const long long gridStartEpoch = cells.front().dayEpoch;
        const long long gridEndEpoch = cells.back().dayEpoch + kSecondsPerDay;
        for (const auto& event : events_) {
            if (event.deletedAt != 0 || SpansMultipleDays(event)) {
                continue;
            }
            if (event.GetDisplayStartEpoch() >= gridEndEpoch || event.GetDisplayEndEpoch() <= gridStartEpoch) {
                continue;
            }

            const long long clippedStart = std::max(event.GetDisplayStartEpoch(), gridStartEpoch);
            const int cellIndex = static_cast<int>((StartOfUtcDay(clippedStart) - gridStartEpoch) / kSecondsPerDay);
            if (cellIndex >= 0 && cellIndex < kMonthCellCount) {
                singleDayEventsByCell[cellIndex].push_back(&event);
            }
        }

        for (int weekIndex = 0; weekIndex < 5; ++weekIndex) {
            const int weekCellStart = weekIndex * 7;
            const long long weekStartEpoch = cells[weekCellStart].dayEpoch;
            const long long weekEndEpoch = cells[weekCellStart + 6].dayEpoch + kSecondsPerDay;

            std::vector<Event> spanningEvents;
            for (const auto& event : events_) {
                if (event.deletedAt != 0 || !SpansMultipleDays(event)) {
                    continue;
                }
                if (event.GetDisplayStartEpoch() < weekEndEpoch && event.GetDisplayEndEpoch() > weekStartEpoch) {
                    spanningEvents.push_back(event);
                }
            }

            std::sort(spanningEvents.begin(), spanningEvents.end(), [](const Event& lhs, const Event& rhs) {
                if (lhs.GetDisplayStartEpoch() != rhs.GetDisplayStartEpoch()) {
                    return lhs.GetDisplayStartEpoch() < rhs.GetDisplayStartEpoch();
                }
                return lhs.GetDisplayEndEpoch() < rhs.GetDisplayEndEpoch();
            });

            struct WeekSpan {
                long long eventId = -1;
                int row = 0;
                int startDayOffset = 0;
                int endDayOffset = 0;
                std::string label;
                bool continuesBefore = false;
                bool continuesAfter = false;
            };

            std::vector<std::vector<WeekSpan>> weekRows;
            for (const auto& event : spanningEvents) {
                const long long eventStart = event.GetDisplayStartEpoch();
                const long long eventEnd = event.GetDisplayEndEpoch();
                const long long clippedStartDay = std::max(StartOfUtcDay(eventStart), weekStartEpoch);
                const long long clippedEndDay = std::min(StartOfUtcDay(std::max(eventStart, eventEnd - 1)), weekEndEpoch - kSecondsPerDay);

                WeekSpan span;
                span.eventId = event.id;
                span.startDayOffset = static_cast<int>((clippedStartDay - weekStartEpoch) / kSecondsPerDay);
                span.endDayOffset = static_cast<int>((clippedEndDay - weekStartEpoch) / kSecondsPerDay);
                span.continuesBefore = eventStart < weekStartEpoch;
                span.continuesAfter = eventEnd > weekEndEpoch;
                span.label = BuildMonthEventLabel(event);

                int assignedRow = 0;
                const int rowSearchLimit = std::min(static_cast<int>(weekRows.size()),
                                                    static_cast<int>(kMaxMonthCellRenderedRows));
                for (; assignedRow < rowSearchLimit; ++assignedRow) {
                    bool overlapsExisting = false;
                    for (const auto& existing : weekRows[assignedRow]) {
                        if (!(span.endDayOffset < existing.startDayOffset || span.startDayOffset > existing.endDayOffset)) {
                            overlapsExisting = true;
                            break;
                        }
                    }
                    if (!overlapsExisting) {
                        break;
                    }
                }

                if (assignedRow >= static_cast<int>(kMaxMonthCellRenderedRows)) {
                    for (int offset = span.startDayOffset; offset <= span.endDayOffset; ++offset) {
                        ++hiddenRowCounts[weekCellStart + offset];
                    }
                    continue;
                }

                if (assignedRow == static_cast<int>(weekRows.size())) {
                    weekRows.emplace_back();
                }

                span.row = assignedRow;
                weekRows[assignedRow].push_back(span);

                for (int offset = span.startDayOffset; offset <= span.endDayOffset; ++offset) {
                    const int cellIndex = weekCellStart + offset;
                    if (static_cast<int>(cellRows[cellIndex].size()) <= span.row) {
                        cellRows[cellIndex].resize(span.row + 1);
                    }

                    MonthCellEventSegment segment;
                    segment.eventId = event.id;
                    segment.label = offset == span.startDayOffset ? span.label : "";
                    segment.colorHex = event.colorHex;
                    segment.continuesBefore = span.continuesBefore || offset > span.startDayOffset;
                    segment.continuesAfter = span.continuesAfter || offset < span.endDayOffset;
                    cellRows[cellIndex][span.row] = segment;
                }
            }

            for (int offset = 0; offset < 7; ++offset) {
                cellRows[weekCellStart + offset].resize(std::min(weekRows.size(), kMaxMonthCellRenderedRows));
            }

            for (int offset = 0; offset < 7; ++offset) {
                const int cellIndex = weekCellStart + offset;
                for (const auto* event : singleDayEventsByCell[cellIndex]) {
                    MonthCellEventSegment segment;
                    segment.eventId = event->id;
                    segment.label = BuildMonthEventLabel(*event);
                    segment.colorHex = event->colorHex;
                    if (cellRows[cellIndex].size() < kMaxMonthCellRenderedRows) {
                        cellRows[cellIndex].push_back(segment);
                    }
                    else {
                        ++hiddenRowCounts[cellIndex];
                    }
                }
            }
        }

        for (int cellIndex = 0; cellIndex < kMonthCellCount; ++cellIndex) {
            if (hiddenRowCounts[cellIndex] == 0) {
                continue;
            }

            if (cellRows[cellIndex].size() >= kMaxMonthCellRenderedRows) {
                const size_t removedRows = cellRows[cellIndex].size() - kMaxMonthCellRenderedRows + 1;
                hiddenRowCounts[cellIndex] += removedRows;
                cellRows[cellIndex].resize(kMaxMonthCellRenderedRows - 1);
            }

            MonthCellEventSegment summary;
            summary.label = "+ " + std::to_string(hiddenRowCounts[cellIndex]) + " more";
            summary.isSummary = true;
            cellRows[cellIndex].push_back(summary);
        }

        for (int cellIndex = 0; cellIndex < kMonthCellCount; ++cellIndex) {
            monthCells_[cellIndex]->UpdateCell(
                cells[cellIndex].dayEpoch,
                cells[cellIndex].dayNumber,
                cells[cellIndex].inCurrentMonth,
                IsSameUtcDay(cells[cellIndex].dayEpoch, today),
                IsSameUtcDay(cells[cellIndex].dayEpoch, selectedDayEpoch_),
                cellRows[cellIndex]);
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

    void SyncVisibleMonthToSelectedDay() {
        if (selectedDayEpoch_ < kMinCalendarEpoch) {
            selectedDayEpoch_ = kMinCalendarEpoch;
        }
        const std::tm tm = EpochToUtcTm(selectedDayEpoch_);
        visibleYear_ = std::max(kMinCalendarYear, tm.tm_year + 1900);
        visibleMonth_ = tm.tm_mon + 1;
    }

    void FocusDay(const long long dayEpoch, const bool refreshView = true) {
        selectedDayEpoch_ = std::max(kMinCalendarEpoch, dayEpoch);
        SyncVisibleMonthToSelectedDay();

        if (refreshView) {
            RefreshEvents();
        }
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
        if (visibleYear_ < kMinCalendarYear) {
            visibleYear_ = kMinCalendarYear;
            visibleMonth_ = 1;
        }
    }

    void PopulateYearComboWindow(const int targetYear) {
        const int clampedYear = std::max(kMinCalendarYear, targetYear);
        yearComboWindowStart_ = std::max(kMinCalendarYear, clampedYear - kYearComboBackwardPadding);
        yearComboWindowEnd_ = yearComboWindowStart_ + kYearComboChunkSize - 1;

        yearComboBox_->Freeze();
        yearComboBox_->Clear();
        for (int year = yearComboWindowStart_; year <= yearComboWindowEnd_; ++year) {
            yearComboBox_->Append(std::to_string(year));
        }
        yearComboBox_->SetValue(std::to_string(clampedYear));
        yearComboBox_->Thaw();
    }

    void EnsureYearComboContains(const int year) {
        const int clampedYear = std::max(kMinCalendarYear, year);
        const bool outsideWindow = clampedYear < yearComboWindowStart_ || clampedYear > yearComboWindowEnd_;
        const bool nearTop = clampedYear - yearComboWindowStart_ < 5 && yearComboWindowStart_ > kMinCalendarYear;
        const bool nearBottom = yearComboWindowEnd_ - clampedYear < 5;

        if (outsideWindow || nearTop || nearBottom) {
            PopulateYearComboWindow(clampedYear);
            return;
        }

        yearComboBox_->SetValue(std::to_string(clampedYear));
    }

    void SwitchView(const CalendarViewMode mode, const int pageIndex) {
        currentViewMode_ = mode;
        calendarBook_->SetSelection(pageIndex);
        RefreshEvents();
    }

    void ShiftCurrentPeriod(const int direction) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            ShiftVisibleMonth(direction);
        }
        else if (currentViewMode_ == CalendarViewMode::WEEK) {
            FocusDay(selectedDayEpoch_ + direction * 7LL * kSecondsPerDay, true);
            return;
        }
        else {
            FocusDay(selectedDayEpoch_ + direction * 1LL * kSecondsPerDay, true);
            return;
        }

        RefreshEvents();
    }

    bool PersistEvent(const Event& event) {
        try {
            const auto calendar = FindLoadedCalendarById(event.calendarId);
            if (calendar.has_value() && calendar->isReadOnly) {
                wxMessageBox("This calendar is read-only and its events cannot be changed.",
                             "Read-only calendar", wxOK | wxICON_WARNING, this);
                return false;
            }

            eventRepository_.upsert(event);
            RefreshEvents();
            statusLabel_->SetLabel("Event saved");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Save failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    void PrepareEventForSelectedCalendarSync(Event& event, const bool isNewEvent) const {
        const auto selectedCalendar = std::find_if(
            accountCalendars_.begin(),
            accountCalendars_.end(),
            [&](const Calendar& calendar) {
                return calendar.id == selectedCalendarId_;
            });
        const bool localCalendar = selectedCalendar != accountCalendars_.end() &&
            selectedCalendar->providerCalendarId == kLocalCalendarProviderId;

        if (localCalendar) {
            if (isNewEvent && event.providerEventId.empty()) {
                event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
            }
            event.syncStatus = SYNCED;
            return;
        }

        if (isNewEvent) {
            event.providerEventId.clear();
            event.syncStatus = PENDING_INSERT;
            return;
        }

        event.syncStatus = event.providerEventId.empty() ? PENDING_INSERT : PENDING_UPDATE;
    }

    bool IsLocalCalendar(const long long calendarId) const {
        const auto calendar = FindLoadedCalendarById(calendarId);
        return calendar.has_value() && calendar->providerCalendarId == kLocalCalendarProviderId;
    }

    void PrepareEventForCalendarSync(Event& event, const bool isNewEvent, const long long calendarId) const {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (calendar.has_value() && calendar->providerCalendarId == kLocalCalendarProviderId) {
            PrepareLocalEvent(event, isNewEvent);
            return;
        }

        if (isNewEvent) {
            event.providerEventId.clear();
            event.syncStatus = PENDING_INSERT;
            return;
        }

        event.syncStatus = event.providerEventId.empty() ? PENDING_INSERT : PENDING_UPDATE;
        event.deletedAt = 0;
    }

    void PrepareLocalEvent(Event& event, const bool isNewEvent) const {
        if (isNewEvent && event.providerEventId.empty()) {
            event.providerEventId = MakeLocalProviderEventId(event.startDateTime);
        }
        if (!event.recurrenceRule.empty() && event.providerMasterId.empty() && event.recurrenceGroupId.empty()) {
            event.recurrenceGroupId = event.providerEventId.empty()
                ? MakeLocalProviderEventId(event.startDateTime)
                : event.providerEventId;
        }
        event.syncStatus = SYNCED;
        event.deletedAt = 0;
    }

    static std::string LocalSeriesGroupKey(const Event& event) {
        if (!event.recurrenceGroupId.empty()) {
            return event.recurrenceGroupId;
        }
        return event.providerEventId;
    }

    void DeleteFutureLocalSeriesSegments(const Event& master, const long long fromStartEpoch) {
        const std::string groupKey = LocalSeriesGroupKey(master);
        if (groupKey.empty()) {
            return;
        }

        for (const auto& candidate : eventRepository_.getRecurringMasters(master.calendarId)) {
            if (candidate.id == master.id || LocalSeriesGroupKey(candidate) != groupKey) {
                continue;
            }
            if (candidate.startDateTime >= fromStartEpoch) {
                eventRepository_.deleteEvent(candidate.id);
            }
        }
    }

    std::optional<Event> FindMasterForProjectedOccurrence(const Event& event) {
        if (event.id < 0) {
            const auto masterIt = projectedOccurrenceMasterIds_.find(event.id);
            if (masterIt != projectedOccurrenceMasterIds_.end()) {
                return eventRepository_.getById(masterIt->second);
            }
        }

        if (!event.providerMasterId.empty()) {
            return eventRepository_.getByProviderId(event.calendarId, event.providerMasterId);
        }

        if (!event.recurrenceRule.empty() || event.type == EventType::MASTER) {
            return eventRepository_.getById(event.id);
        }

        return std::nullopt;
    }

    RecurrenceOverride MakeOverride(
        const Event& master,
        const Event& occurrence,
        const RecurrenceOverrideType type,
        const int syncStatus,
        const long long replacementEventId = 0) const {
        RecurrenceOverride overrideEntry;
        overrideEntry.masterEventId = master.id;
        overrideEntry.originalStart = occurrence.instanceStart != 0 ? occurrence.instanceStart : occurrence.startDateTime;
        overrideEntry.type = type;
        overrideEntry.replacementEventId = replacementEventId;
        overrideEntry.syncStatus = syncStatus;
        overrideEntry.deletedAt = 0;
        overrideEntry.createdAt = std::time(nullptr);
        overrideEntry.updatedAt = overrideEntry.createdAt;
        return overrideEntry;
    }

    static bool IsStoredRecurrenceInstance(const Event& event) {
        return event.id > 0 &&
               (event.type == EventType::OCCURRENCE ||
                event.type == EventType::EXCEPTION ||
                !event.providerMasterId.empty());
    }

    void ApplyEntireSeriesEdit(const Event& originalEvent,
                               const Event& editedEvent,
                               const Event& master) {
        const bool editingProjectedOrStoredOccurrence =
            originalEvent.id != master.id ||
            originalEvent.type == EventType::OCCURRENCE ||
            originalEvent.type == EventType::EXCEPTION ||
            !originalEvent.providerMasterId.empty();
        Event updatedMaster = editingProjectedOrStoredOccurrence
            ? PreserveSeriesAnchorDate(master, editedEvent)
            : editedEvent;
        updatedMaster.id = master.id;
        updatedMaster.calendarId = master.calendarId;
        updatedMaster.providerEventId = master.providerEventId;
        updatedMaster.providerMasterId.clear();
        updatedMaster.recurrenceGroupId = LocalSeriesGroupKey(master);
        updatedMaster.instanceStart = 0;
        updatedMaster.type = updatedMaster.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
        PrepareEventForCalendarSync(updatedMaster, false, updatedMaster.calendarId);
        eventRepository_.upsert(updatedMaster);
        eventRepository_.deleteStoredInstancesForMaster(
            updatedMaster.calendarId,
            updatedMaster.id,
            updatedMaster.providerEventId,
            updatedMaster.recurrenceGroupId);
    }

    void ApplySingleInstanceEdit(const Event& originalEvent,
                                 const Event& editedEvent,
                                 const Event& master,
                                 const int overrideSyncStatus,
                                 const bool deleteRequested) {
        if (deleteRequested) {
            if (IsStoredRecurrenceInstance(originalEvent)) {
                eventRepository_.deleteEvent(originalEvent.id);
            }
            eventRepository_.upsertRecurrenceOverride(
                MakeOverride(master, originalEvent, RecurrenceOverrideType::CANCELLED, overrideSyncStatus));
            return;
        }

        Event replacement = editedEvent;
        const bool reuseStoredInstance = IsStoredRecurrenceInstance(originalEvent);
        replacement.id = reuseStoredInstance ? originalEvent.id : -1;
        replacement.calendarId = master.calendarId;
        replacement.providerEventId = reuseStoredInstance ? originalEvent.providerEventId : "";
        replacement.providerMasterId = reuseStoredInstance ? originalEvent.providerMasterId : "";
        replacement.recurrenceGroupId = reuseStoredInstance ? originalEvent.recurrenceGroupId : "";
        replacement.instanceStart = originalEvent.instanceStart != 0
            ? originalEvent.instanceStart
            : originalEvent.startDateTime;
        replacement.recurrenceRule.clear();
        replacement.type = reuseStoredInstance && !replacement.providerMasterId.empty()
            ? EventType::EXCEPTION
            : EventType::SINGLE;
        if (IsLocalCalendar(master.calendarId)) {
            PrepareLocalEvent(replacement, !reuseStoredInstance);
        }
        else {
            replacement.syncStatus = SYNCED;
            replacement.deletedAt = 0;
        }
        const long long replacementId = eventRepository_.upsert(replacement);
        eventRepository_.upsertRecurrenceOverride(
            MakeOverride(master, originalEvent, RecurrenceOverrideType::MODIFIED, overrideSyncStatus, replacementId));
    }

    void ApplyThisAndFollowingEdit(const Event& originalEvent,
                                   const Event& editedEvent,
                                   const Event& master,
                                   const bool deleteRequested) {
        const long long splitStart = originalEvent.instanceStart != 0
            ? originalEvent.instanceStart
            : originalEvent.startDateTime;
        Event truncatedMaster = master;
        truncatedMaster.recurrenceRule = LimitedRecurrenceRule(
            master.recurrenceRule,
            std::max(0LL, splitStart - 1));
        truncatedMaster.type = truncatedMaster.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
        truncatedMaster.recurrenceGroupId = LocalSeriesGroupKey(master);
        PrepareEventForCalendarSync(truncatedMaster, false, truncatedMaster.calendarId);
        eventRepository_.upsert(truncatedMaster);
        DeleteFutureLocalSeriesSegments(master, splitStart);

        if (deleteRequested) {
            return;
        }

        Event followingMaster = editedEvent;
        followingMaster.id = -1;
        followingMaster.calendarId = master.calendarId;
        followingMaster.providerEventId.clear();
        followingMaster.providerMasterId.clear();
        followingMaster.recurrenceGroupId = LocalSeriesGroupKey(master);
        followingMaster.instanceStart = 0;
        followingMaster.recurrenceRule = PreserveRecurrenceEndBoundary(
            followingMaster.recurrenceRule,
            master.recurrenceRule);
        followingMaster.type = followingMaster.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
        PrepareEventForCalendarSync(followingMaster, true, followingMaster.calendarId);
        eventRepository_.upsert(followingMaster);
    }

    bool ApplyRecurringEdit(
        const Event& originalEvent,
        const Event& editedEvent,
        const RecurrenceEditScope scope,
        const bool deleteRequested) {
        auto master = FindMasterForProjectedOccurrence(originalEvent);
        if (!master.has_value()) {
            return false;
        }
        const int overrideSyncStatus = IsLocalCalendar(master->calendarId) ? SYNCED : PENDING_INSERT;

        try {
            if (scope == RecurrenceEditScope::ENTIRE_SERIES) {
                if (deleteRequested) {
                    return DeleteEventById(master->id);
                }
                ApplyEntireSeriesEdit(originalEvent, editedEvent, *master);
            }
            else if (scope == RecurrenceEditScope::THIS_INSTANCE) {
                ApplySingleInstanceEdit(originalEvent, editedEvent, *master, overrideSyncStatus, deleteRequested);
            }
            else {
                ApplyThisAndFollowingEdit(originalEvent, editedEvent, *master, deleteRequested);
            }

            RefreshEvents();
            statusLabel_->SetLabel(deleteRequested ? "Recurring event deleted" : "Recurring event saved");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Recurring event update failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    bool DeleteEventById(const long long eventId) {
        try {
            const auto event = eventRepository_.getById(eventId);
            if (!event.has_value()) {
                return false;
            }

            if (event->syncStatus == PENDING_INSERT || event->providerEventId.empty()) {
                eventRepository_.deleteEvent(eventId);
            }
            else {
                eventRepository_.softDelete(eventId);
            }
            RefreshEvents();
            statusLabel_->SetLabel("Event deleted");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Delete failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    std::unordered_map<long long, wxString> BuildAccountLabelsById() const {
        std::unordered_map<long long, wxString> accountLabels;
        for (const auto& account : availableAccounts_) {
            accountLabels[account.id] = FormatAccountLabel(account);
        }
        return accountLabels;
    }

    bool MoveEventToCalendarFromUi(const Event& event, const long long targetCalendarId) {
        if (targetCalendarId == 0 || targetCalendarId == event.calendarId) {
            return false;
        }

        const auto targetCalendar = FindLoadedCalendarById(targetCalendarId);
        if (!targetCalendar.has_value()) {
            wxMessageBox("The destination calendar could not be found.",
                         "Move event", wxOK | wxICON_WARNING, this);
            return false;
        }
        if (targetCalendar->isReadOnly) {
            wxMessageBox("The destination calendar is read-only.",
                         "Move event", wxOK | wxICON_WARNING, this);
            return false;
        }

        const bool recurringContext =
            event.type == EventType::MASTER ||
            event.type == EventType::OCCURRENCE ||
            !event.recurrenceRule.empty() ||
            !event.providerMasterId.empty();

        try {
            if (recurringContext) {
                auto master = FindMasterForProjectedOccurrence(event);
                if (!master.has_value()) {
                    wxMessageBox("The recurring event master could not be found.",
                                 "Move recurring event", wxOK | wxICON_WARNING, this);
                    return false;
                }

                Event movedSeries = *master;
                movedSeries.calendarId = targetCalendarId;
                movedSeries.providerMasterId.clear();
                movedSeries.recurrenceGroupId = LocalSeriesGroupKey(*master);
                movedSeries.instanceStart = 0;
                movedSeries.type = movedSeries.recurrenceRule.empty() ? EventType::SINGLE : EventType::MASTER;
                if (!eventRepository_.moveEventToCalendar(movedSeries, targetCalendarId).has_value()) {
                    wxMessageBox("The event could not be moved.",
                                 "Move event", wxOK | wxICON_WARNING, this);
                    return false;
                }
            }
            else {
                if (event.id <= 0) {
                    wxMessageBox("The selected event cannot be moved because it is not stored as a database event.",
                                 "Move event", wxOK | wxICON_WARNING, this);
                    return false;
                }
                if (!eventRepository_.moveEventToCalendar(event, targetCalendarId).has_value()) {
                    wxMessageBox("The event could not be moved.",
                                 "Move event", wxOK | wxICON_WARNING, this);
                    return false;
                }
            }

            currentAccountId_ = targetCalendar->accountId;
            accountCalendars_ = accountCalendarCache_[targetCalendar->accountId];
            selectedCalendarId_ = targetCalendarId;
            visibleCalendarIds_.insert(targetCalendarId);
            SaveCurrentCalendarSessionState();
            RefreshEvents();
            RefreshCalendarControls();
            statusLabel_->SetLabel(recurringContext ? "Recurring event moved" : "Event moved");
            return true;
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Move failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    void OpenMoveEventDialog(const Event& event) {
        EventMoveDialog dialog(this, event, AllLoadedCalendars(), BuildAccountLabelsById());
        if (!dialog.HasDestination()) {
            wxMessageBox("There is no writable destination calendar available.",
                         "Move event", wxOK | wxICON_INFORMATION, this);
            return;
        }
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        MoveEventToCalendarFromUi(event, dialog.SelectedCalendarId());
    }

    void OpenEventActionDialog(const Event& event, const bool readOnly) {
        if (readOnly) {
            OpenEventDialog(event, std::nullopt, true);
            return;
        }

        wxArrayString actions;
        actions.Add("Edit");
        actions.Add("Move to calendar");
        wxSingleChoiceDialog dialog(this,
                                    "Choose what you want to do with this event.",
                                    "Event",
                                    actions);
        dialog.SetSelection(0);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (dialog.GetSelection() == 1) {
            OpenMoveEventDialog(event);
            return;
        }

        OpenEventDialog(event, std::nullopt, false);
    }

    void OpenEventDialog(const std::optional<Event>& event = std::nullopt,
                         const std::optional<EventDraftDefaults>& defaults = std::nullopt,
                         const bool readOnly = false) {
        const long long defaultCalendarId = event.has_value() ? event->calendarId : selectedCalendarId_;
        const auto accountLabels = BuildAccountLabelsById();
        EventEditorDialog dialog(this,
                                 event,
                                 selectedDayEpoch_,
                                 AllLoadedCalendars(),
                                 accountLabels,
                                 defaultCalendarId,
                                 defaults,
                                 readOnly);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (dialog.IsDeleteRequested()) {
            if (event.has_value()) {
                const bool recurringContext =
                    event->type == EventType::MASTER ||
                    event->type == EventType::OCCURRENCE ||
                    !event->recurrenceRule.empty() ||
                    !event->providerMasterId.empty();
                if (recurringContext &&
                    ApplyRecurringEdit(*event, *event, dialog.GetRecurrenceEditScope(), true)) {
                    return;
                }
                DeleteEventById(event->id);
            }
            return;
        }

        const long long fallbackCalendarId = event.has_value() ? event->calendarId : selectedCalendarId_;
        auto builtEvent = dialog.BuildEvent(fallbackCalendarId);
        if (!builtEvent.has_value()) {
            return;
        }
        const long long targetCalendarId = builtEvent->calendarId;

        if (builtEvent->timezone.empty()) {
            const auto selectedCalendar = FindLoadedCalendarById(targetCalendarId);
            builtEvent->timezone = (selectedCalendar.has_value() && !selectedCalendar->timezone.empty())
                ? selectedCalendar->timezone
                : "UTC";
        }

        if (event.has_value()) {
            const bool recurringContext =
                event->type == EventType::MASTER ||
                event->type == EventType::OCCURRENCE ||
                !event->recurrenceRule.empty() ||
                !event->providerMasterId.empty();
            if (recurringContext &&
                ApplyRecurringEdit(*event, *builtEvent, dialog.GetRecurrenceEditScope(), false)) {
                return;
            }
        }

        PrepareEventForCalendarSync(*builtEvent, !event.has_value(), targetCalendarId);
        PersistEvent(*builtEvent);
    }

    void OpenEventById(const long long eventId) {
        std::optional<Event> selectedEvent = FindLoadedEventById(eventId);
        if (!selectedEvent.has_value() && eventId > 0) {
            selectedEvent = eventRepository_.getById(eventId);
        }
        if (!selectedEvent.has_value()) {
            return;
        }

        const auto eventCalendar = FindLoadedCalendarById(selectedEvent->calendarId);
        const bool readOnly = eventCalendar.has_value() && eventCalendar->isReadOnly;

        if (!selectedEvent->providerMasterId.empty()) {
            std::unordered_map<std::string, RemoteMasterMetadata> remoteMasterMetadata;
            auto masterEvent = eventRepository_.getByProviderId(
                selectedEvent->calendarId,
                selectedEvent->providerMasterId);
            if (masterEvent.has_value()) {
                remoteMasterMetadata.emplace(
                    masterEvent->providerEventId,
                    RemoteMasterMetadata{
                        masterEvent->title,
                        masterEvent->description,
                        masterEvent->location,
                        masterEvent->timezone,
                        masterEvent->recurrenceRule,
                        masterEvent->allDay});
                *selectedEvent = BuildEffectiveRemoteEvent(*selectedEvent, remoteMasterMetadata);
            }
        }

        FocusDay(StartOfUtcDay(selectedEvent->GetDisplayStartEpoch()), false);
        statusLabel_->SetLabel(readOnly
            ? wxString::Format("Viewing event #%lld", selectedEvent->id)
            : wxString::Format("Editing event #%lld", selectedEvent->id));
        OpenEventActionDialog(*selectedEvent, readOnly);
    }

    void OpenNewEventDialog(const long long dayEpoch, const EventDraftDefaults& defaults) {
        if (selectedCalendarId_ == 0) {
            wxMessageBox("The current account does not have an active calendar yet.",
                         "No calendar available", wxOK | wxICON_WARNING, this);
            return;
        }
        const auto selectedCalendar = FindLoadedCalendarById(selectedCalendarId_);
        if (selectedCalendar.has_value() && selectedCalendar->isReadOnly) {
            wxMessageBox("The selected calendar is read-only. Choose a writable calendar first.",
                         "Read-only calendar", wxOK | wxICON_INFORMATION, this);
            return;
        }

        FocusDay(dayEpoch, false);
        statusLabel_->SetLabel("Creating new event");
        OpenEventDialog(std::nullopt, defaults);
    }

    void OpenNewAllDayEventDialog(const long long dayEpoch) {
        EventDraftDefaults defaults;
        defaults.startDateTime = dayEpoch;
        defaults.endDateTime = dayEpoch + kSecondsPerDay;
        defaults.allDay = true;
        OpenNewEventDialog(dayEpoch, defaults);
    }

    void OpenNewTimedEventDialog(const long long dayEpoch, const int minuteOfDay) {
        EventDraftDefaults defaults;
        const std::string displayTimezone = GetCurrentLocalTimeZoneName();
        const long long displayStartEpoch = dayEpoch + static_cast<long long>(minuteOfDay) * 60;
        const long long displayEndEpoch = displayStartEpoch + 3600;
        defaults.startDateTime = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayStartEpoch)
            .value_or(displayStartEpoch);
        defaults.endDateTime = ConvertTimeZoneDisplayEpochToUtcEpoch(displayTimezone, displayEndEpoch)
            .value_or(displayEndEpoch);
        defaults.allDay = false;
        OpenNewEventDialog(dayEpoch, defaults);
    }

    void HandleMonthCellClicked(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        currentViewMode_ = CalendarViewMode::DAY;
        calendarBook_->SetSelection(2);
        FocusDay(monthCellEpochs_[index], true);
    }

    void HandleMonthCellCreateEvent(const int index) {
        if (index < 0 || index >= kMonthCellCount) {
            return;
        }

        OpenNewAllDayEventDialog(monthCellEpochs_[index]);
    }

    void OnToday(wxCommandEvent&) {
        FocusDay(StartOfUtcDay(CurrentLocalDisplayEpoch()), false);
        RefreshEvents(true);
    }

    void OnNew(wxCommandEvent&) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            OpenNewAllDayEventDialog(selectedDayEpoch_);
            return;
        }

        const long long now = CurrentLocalDisplayEpoch();
        const long long baseDay = currentViewMode_ == CalendarViewMode::DAY
            ? selectedDayEpoch_
            : std::max(selectedDayEpoch_, StartOfUtcDay(now));
        const std::tm nowTm = EpochToUtcTm(std::max(now, kMinCalendarEpoch));
        const int minuteOfDay = nowTm.tm_hour * 60;
        OpenNewTimedEventDialog(baseDay, minuteOfDay);
    }

    void OnRefresh(wxCommandEvent&) {
        StartManualSyncForAllAccounts();
    }

    void OnNewCalendar(wxCommandEvent&) {
        OpenCreateCalendarDialog();
    }

    void OnManageAccounts(wxCommandEvent&) {
        ShowAccountManagerDialog();
    }

    void OnYearChanged(wxCommandEvent& event) {
        long selectedYear = visibleYear_;
        if (!event.GetString().ToLong(&selectedYear)) {
            return;
        }
        visibleYear_ = std::max(kMinCalendarYear, static_cast<int>(selectedYear));
        const std::tm selectedTm = EpochToUtcTm(selectedDayEpoch_);
        const int selectedDay = std::min(selectedTm.tm_mday, DaysInMonth(visibleYear_, visibleMonth_));
        selectedDayEpoch_ = MakeUtcEpoch(visibleYear_, visibleMonth_, selectedDay);
        EnsureYearComboContains(visibleYear_);
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            RefreshEvents();
        }
    }

    void OnModeChanged(wxCommandEvent&) {
        const int selection = modeComboBox_->GetSelection();
        if (selection == 0) {
            SwitchView(CalendarViewMode::MONTH, 0);
        }
        else if (selection == 1) {
            SwitchView(CalendarViewMode::WEEK, 1);
        }
        else if (selection == 2) {
            SwitchView(CalendarViewMode::DAY, 2);
        }
    }

    void OnAccountSelected(wxCommandEvent&) {
        ShowAccountManagerDialog();
    }

    void OnDeleteCurrentAccount(wxCommandEvent&) {
        DeleteAccountWithConfirmation(currentAccountId_);
    }

    void OnCancelAuth(wxCommandEvent&) {
        if (!authInProgress_) {
            return;
        }

        statusLabel_->SetLabel("Canceling sign-in...");
        UpdateAuthProgressMessage("Canceling sign-in...");
        RequestOAuthCancellation();
    }

    std::string dbPath_;
    SQLite::Database db_;
    AccountRepository accountRepository_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
    std::unique_ptr<EventUploadScheduler> eventUploadScheduler_;
    long long localAccountId_ = 0;
    long long currentAccountId_ = 0;
    long long selectedCalendarId_ = 0;
    long long selectedDayEpoch_ = 0;
    int visibleYear_ = 1970;
    int visibleMonth_ = 1;
    CalendarViewMode currentViewMode_ = CalendarViewMode::MONTH;
    std::vector<Account> availableAccounts_;
    std::vector<Calendar> accountCalendars_;
    std::vector<long long> accountComboAccountIds_;
    std::set<long long> visibleCalendarIds_;
    std::unordered_map<long long, std::vector<Calendar>> accountCalendarCache_;
    std::unordered_map<long long, std::set<long long>> accountVisibleCalendarIds_;
    std::unordered_map<long long, long long> accountSelectedCalendarIds_;
    std::unordered_map<long long, std::shared_ptr<AccessToken>> sessionTokens_;
    std::set<long long> signedInAccountIds_;
    std::set<long long> accountLoginFailedIds_;
    std::set<long long> accountLoginInProgressIds_;
    std::set<long long> collapsedAccountIds_;
    std::set<std::string> coverageFetchKeys_;
    std::set<std::string> coveredRangeKeys_;
    std::vector<Event> events_;
    std::unordered_map<long long, size_t> eventIndexById_;
    std::unordered_map<long long, Event> projectedOccurrences_;
    std::unordered_map<long long, long long> projectedOccurrenceMasterIds_;
    long long nextProjectedOccurrenceId_ = -1;
    long long eventLoadGeneration_ = 0;
    std::uint64_t eventListFingerprint_ = 0;
    bool accountComboUpdateInProgress_ = false;
    bool authInProgress_ = false;

    wxComboBox* modeComboBox_ = nullptr;
    wxButton* todayButton_ = nullptr;
    wxButton* previousButton_ = nullptr;
    wxButton* nextButton_ = nullptr;
    wxStaticText* monthTitleLabel_ = nullptr;
    wxComboBox* yearComboBox_ = nullptr;
    int yearComboWindowStart_ = kMinCalendarYear;
    int yearComboWindowEnd_ = kMinCalendarYear + kYearComboChunkSize - 1;
    wxButton* manageAccountsButton_ = nullptr;
    wxDialog* authProgressDialog_ = nullptr;
    wxActivityIndicator* authActivityIndicator_ = nullptr;
    wxStaticText* authProgressLabel_ = nullptr;
    wxSimplebook* calendarBook_ = nullptr;
    TimelineViewPanel* weekTimeline_ = nullptr;
    TimelineViewPanel* dayTimeline_ = nullptr;
    std::array<MonthCellPanel*, kMonthCellCount> monthCells_{};
    std::array<long long, kMonthCellCount> monthCellEpochs_{};

    wxButton* newButton_ = nullptr;
    wxButton* refreshButton_ = nullptr;
    wxButton* newCalendarButton_ = nullptr;
    wxStaticText* selectedCalendarLabel_ = nullptr;
    wxScrolledWindow* calendarListPanel_ = nullptr;
    wxBoxSizer* calendarListSizer_ = nullptr;
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
