#include "ui/local_calendar_app.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choicdlg.h>
#include <wx/combobox.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wx.h>

#include "models/account.h"
#include "models/calendar.h"
#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "services/account_service.h"
#include "sync/google_calendar_sync_service.h"
#include "sync/outlook_calendar_sync_service.h"
#include "ui/calendar_ui_shared.h"
#include "ui/event_editor_dialog.h"
#include "ui/month_cell_panel.h"
#include "ui/timeline_view_panel.h"
#include "utils/access_token.h"
#include "utils/datetime_utils.h"

namespace {

struct MonthCellMeta {
    long long dayEpoch = 0;
    int dayNumber = 0;
    bool inCurrentMonth = true;
};

struct VisibleRange {
    long long startEpoch = 0;
    long long endEpoch = 0;
};

constexpr int kYearComboChunkSize = 120;
constexpr int kYearComboBackwardPadding = 20;
constexpr const char* kLocalProvider = "LOCAL";
constexpr const char* kLocalProviderUserId = "local-account";
constexpr const char* kLocalCalendarProviderId = "local-calendar";
constexpr const char* kDefaultAccountCalendarProviderId = "default-calendar";

bool IsLocalProvider(const std::string& provider) {
    return provider == kLocalProvider;
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
    return label;
}

class CalendarEditorDialog final : public wxDialog {
public:
    explicit CalendarEditorDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "New Calendar", wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* rootSizer = new wxBoxSizer(wxVERTICAL);

        auto* formSizer = new wxFlexGridSizer(1, 2, 10, 10);
        formSizer->AddGrowableCol(1, 1);

        formSizer->Add(new wxStaticText(this, wxID_ANY, "Name"), 0, wxALIGN_CENTER_VERTICAL);
        nameCtrl_ = new wxTextCtrl(this, wxID_ANY);
        formSizer->Add(nameCtrl_, 1, wxEXPAND);

        rootSizer->Add(formSizer, 1, wxEXPAND | wxALL, 14);

        auto* buttonSizer = CreateSeparatedButtonSizer(wxOK | wxCANCEL);
        if (buttonSizer != nullptr) {
            rootSizer->Add(buttonSizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 14);
        }

        SetSizerAndFit(rootSizer);
        SetMinSize(wxSize(420, GetSize().GetHeight()));
        CentreOnParent();

        Bind(wxEVT_BUTTON, &CalendarEditorDialog::OnOk, this, wxID_OK);
    }

    std::string GetCalendarName() const {
        return nameCtrl_->GetValue().ToStdString();
    }

private:
    void OnOk(wxCommandEvent& event) {
        if (nameCtrl_->GetValue().Trim(true).Trim(false).IsEmpty()) {
            wxMessageBox("Calendar name is required.", "Validation", wxOK | wxICON_WARNING, this);
            return;
        }

        event.Skip();
    }

    wxTextCtrl* nameCtrl_ = nullptr;
};

class LocalCalendarFrame final : public wxFrame {
public:
    explicit LocalCalendarFrame(const std::string& dbPath)
        : wxFrame(nullptr, wxID_ANY, "Calendar", wxDefaultPosition, wxSize(1440, 860)),
          db_(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE),
          accountRepository_(db_),
          eventRepository_(db_),
          calendarRepository_(db_) {
        localAccountId_ = GetLocalAccountId();

        const long long now = static_cast<long long>(std::time(nullptr));
        selectedDayEpoch_ = StartOfUtcDay(now);
        SyncVisibleMonthToSelectedDay();

        BuildLayout();
        ApplyLook();
        BindEvents();

        ReloadAccounts();
        SetCurrentAccount(localAccountId_, false);
        RefreshEvents();
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

        // Fallback keeps the UI resilient if startup seeding ever gets skipped.
        Account localAccount{};
        localAccount.name = "Local account";
        localAccount.provider = kLocalProvider;
        localAccount.providerUserId = kLocalProviderUserId;
        localAccount.refreshToken.clear();
        return accountRepository_.Upsert(localAccount);
    }

    void PromotePrimaryCalendar(const long long accountId, const long long calendarId) {
        SQLite::Statement query(
            db_,
            "UPDATE calendars SET is_primary = CASE WHEN id = ? THEN 1 ELSE 0 END WHERE account_id = ?");
        query.bind(1, calendarId);
        query.bind(2, accountId);
        query.exec();
    }

    std::vector<Calendar> EnsureCalendarsForAccount(const Account& account) {
        if (IsLocalProvider(account.provider)) {
            auto legacyCalendar = calendarRepository_.getByProviderId(0, kLocalCalendarProviderId);
            if (legacyCalendar.has_value()) {
                SQLite::Statement migrate(db_, "UPDATE calendars SET account_id = ? WHERE id = ?");
                migrate.bind(1, account.id);
                migrate.bind(2, legacyCalendar->id);
                migrate.exec();
            }

            auto localCalendar = calendarRepository_.getByProviderId(account.id, kLocalCalendarProviderId);
            if (!localCalendar.has_value()) {
                Calendar calendar{};
                calendar.accountId = account.id;
                calendar.providerCalendarId = kLocalCalendarProviderId;
                calendar.name = "Local Calendar";
                calendar.timezone = "UTC";
                calendar.isPrimary = true;
                calendar.isReadOnly = false;
                calendar.syncEnabled = false;
                calendar.lastSyncedAt = 0;
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
        calendar.timezone = "UTC";
        calendar.isPrimary = true;
        calendar.isReadOnly = false;
        calendar.syncEnabled = false;
        calendar.lastSyncedAt = 0;

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

        auto visibleIt = accountVisibleCalendarIds_.find(account.id);
        if (visibleIt == accountVisibleCalendarIds_.end()) {
            visibleCalendarIds_.clear();
            for (const auto& calendar : accountCalendars_) {
                visibleCalendarIds_.insert(calendar.id);
            }
        }
        else {
            visibleCalendarIds_ = visibleIt->second;
            for (auto it = visibleCalendarIds_.begin(); it != visibleCalendarIds_.end();) {
                const bool exists = std::any_of(accountCalendars_.begin(), accountCalendars_.end(), [it](const Calendar& calendar) {
                    return calendar.id == *it;
                });
                if (!exists) {
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

    void RefreshCalendarControls() {
        if (calendarListPanel_ == nullptr || calendarListSizer_ == nullptr) {
            return;
        }

        calendarListPanel_->Freeze();
        calendarListSizer_->Clear(true);

        for (const auto& calendar : accountCalendars_) {
            auto* rowSizer = new wxBoxSizer(wxHORIZONTAL);
            auto* visibility = new wxCheckBox(calendarListPanel_, wxID_ANY, "");
            visibility->SetValue(visibleCalendarIds_.count(calendar.id) > 0);
            visibility->Bind(wxEVT_CHECKBOX, [this, calendarId = calendar.id](wxCommandEvent& event) {
                if (event.IsChecked()) {
                    visibleCalendarIds_.insert(calendarId);
                }
                else {
                    visibleCalendarIds_.erase(calendarId);
                    if (selectedCalendarId_ == calendarId && !accountCalendars_.empty()) {
                        const auto visibleIt = std::find_if(accountCalendars_.begin(), accountCalendars_.end(), [this](const Calendar& candidate) {
                            return visibleCalendarIds_.count(candidate.id) > 0;
                        });
                        if (visibleIt != accountCalendars_.end()) {
                            selectedCalendarId_ = visibleIt->id;
                        }
                    }
                }
                SaveCurrentCalendarSessionState();
                RefreshEvents();
                CallAfter([this]() { RefreshCalendarControls(); });
            });

            auto* selectButton = new wxButton(calendarListPanel_, wxID_ANY, FormatCalendarLabel(calendar), wxDefaultPosition, wxSize(-1, 24), wxBU_LEFT);
            selectButton->SetMinSize(wxSize(160, 24));
            if (selectedCalendarId_ == calendar.id) {
                wxFont font = selectButton->GetFont();
                font.SetWeight(wxFONTWEIGHT_BOLD);
                selectButton->SetFont(font);
            }
            selectButton->Bind(wxEVT_BUTTON, [this, calendarId = calendar.id](wxCommandEvent&) {
                selectedCalendarId_ = calendarId;
                visibleCalendarIds_.insert(calendarId);
                SaveCurrentCalendarSessionState();
                CallAfter([this]() { RefreshCalendarControls(); });
            });

            rowSizer->Add(visibility, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            rowSizer->Add(selectButton, 1, wxEXPAND);
            if (!calendar.isPrimary) {
                auto* deleteButton = new wxButton(calendarListPanel_, wxID_ANY, "Delete", wxDefaultPosition, wxSize(62, 24));
                deleteButton->Bind(wxEVT_BUTTON, [this, calendarId = calendar.id](wxCommandEvent&) {
                    const auto calendarToDelete = FindLoadedCalendarById(calendarId);
                    if (!calendarToDelete.has_value()) {
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
                });
                rowSizer->Add(deleteButton, 0, wxLEFT, 6);
            }
            calendarListSizer_->Add(rowSizer, 0, wxEXPAND | wxBOTTOM, 4);
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

    bool FetchAndStoreRemoteCalendars(const Account& account, const std::string& accessToken) {
        std::vector<Calendar> remoteCalendars;

        if (account.provider == "GOOGLE") {
            GoogleCalendarSyncService service(calendarRepository_, eventRepository_);
            remoteCalendars = service.fetchRemoteCalendars(accessToken);
        }
        else if (account.provider == "MICROSOFT") {
            OutlookCalendarSyncService service(calendarRepository_, eventRepository_);
            remoteCalendars = service.fetchRemoteCalendars(accessToken);
        }
        else {
            return true;
        }

        for (auto& calendar : remoteCalendars) {
            calendar.accountId = account.id;
            calendarRepository_.upsert(calendar);
        }

        return true;
    }

    std::optional<std::string> EnsureSessionAccessToken(Account account) {
        if (IsLocalProvider(account.provider)) {
            return std::string{};
        }

        const auto existing = sessionAccessTokens_.find(account.id);
        if (existing != sessionAccessTokens_.end() && !existing->second.empty()) {
            return existing->second;
        }

        const auto platform = PlatformForProvider(account.provider);
        if (!platform.has_value()) {
            return std::nullopt;
        }

        const std::string refreshToken = account.refreshToken.empty()
            ? accountRepository_.GetRefreshToken(account)
            : account.refreshToken;
        if (refreshToken.empty()) {
            return std::nullopt;
        }

        AccessToken token(*platform, refreshToken);
        const std::string accessToken = token.GetToken();
        if (accessToken.empty()) {
            return std::nullopt;
        }

        sessionAccessTokens_[account.id] = accessToken;

        const std::string updatedRefreshToken = token.GetRefreshToken();
        if (!updatedRefreshToken.empty() && updatedRefreshToken != refreshToken) {
            account.refreshToken = updatedRefreshToken;
            accountRepository_.Upsert(account);
        }

        return accessToken;
    }

    bool CreateCalendarForCurrentAccount(const std::string& name) {
        if (currentAccountId_ == 0) {
            return false;
        }

        Calendar calendar{};
        calendar.accountId = currentAccountId_;
        calendar.providerCalendarId = "calendar-" + std::to_string(currentAccountId_) + "-" +
            std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
            std::to_string(static_cast<long long>(accountCalendars_.size() + 1));
        calendar.name = name;
        calendar.timezone = "UTC";
        calendar.isPrimary = false;
        calendar.isReadOnly = false;
        calendar.syncEnabled = false;
        calendar.lastSyncedAt = 0;

        calendarRepository_.upsert(calendar);
        const auto created = calendarRepository_.getByProviderId(currentAccountId_, calendar.providerCalendarId);
        if (!created.has_value()) {
            return false;
        }

        accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
        selectedCalendarId_ = created->id;
        visibleCalendarIds_.insert(created->id);
        SaveCurrentCalendarSessionState();
        RefreshCalendarControls();
        RefreshEvents();
        statusLabel_->SetLabel("Calendar created");
        return true;
    }

    bool DeleteCalendarById(const long long calendarId) {
        const auto calendar = FindLoadedCalendarById(calendarId);
        if (!calendar.has_value() || calendar->isPrimary) {
            return false;
        }

        db_.exec("BEGIN IMMEDIATE TRANSACTION");
        try {
            SQLite::Statement deleteEvents(db_, "DELETE FROM events WHERE calendar_id = ?");
            deleteEvents.bind(1, calendarId);
            deleteEvents.exec();

            SQLite::Statement deleteCalendar(db_, "DELETE FROM calendars WHERE id = ?");
            deleteCalendar.bind(1, calendarId);
            deleteCalendar.exec();

            db_.exec("COMMIT");
        }
        catch (...) {
            db_.exec("ROLLBACK");
            throw;
        }

        accountCalendars_ = calendarRepository_.getByAccount(currentAccountId_);
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

    void OpenCreateCalendarDialog() {
        const auto currentAccount = FindLoadedAccountById(currentAccountId_);
        if (!currentAccount.has_value()) {
            wxMessageBox("No current account is selected.", "Calendar error", wxOK | wxICON_WARNING, this);
            return;
        }

        CalendarEditorDialog dialog(this);
        if (dialog.ShowModal() != wxID_OK) {
            return;
        }

        if (!CreateCalendarForCurrentAccount(dialog.GetCalendarName())) {
            wxMessageBox("The calendar could not be created.", "Calendar error", wxOK | wxICON_ERROR, this);
        }
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
        accountComboUpdateInProgress_ = true;
        accountComboAccountIds_.clear();

        accountComboBox_->Freeze();
        accountComboBox_->Clear();

        int selectedIndex = wxNOT_FOUND;
        for (const auto& account : availableAccounts_) {
            accountComboAccountIds_.push_back(account.id);
            accountComboBox_->Append(FormatAccountLabel(account));
            if (account.id == currentAccountId_) {
                selectedIndex = static_cast<int>(accountComboAccountIds_.size()) - 1;
            }
        }

        const int newAccountIndex = static_cast<int>(accountComboAccountIds_.size());
        accountComboBox_->Append("New account...");
        accountComboBox_->SetSelection(selectedIndex == wxNOT_FOUND ? newAccountIndex : selectedIndex);
        accountComboBox_->Thaw();

        const auto currentAccount = FindLoadedAccountById(currentAccountId_);
        removeAccountButton_->Enable(currentAccount.has_value() && !IsLocalProvider(currentAccount->provider));
        accountComboUpdateInProgress_ = false;
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
        if (!IsLocalProvider(account->provider)) {
            const bool hadSessionToken = sessionAccessTokens_.count(account->id) > 0 &&
                !sessionAccessTokens_[account->id].empty();
            const auto accessToken = EnsureSessionAccessToken(*account);
            if (!accessToken.has_value()) {
                wxMessageBox("The saved session token is missing and the refresh token could not create a new access token.",
                             "Account session error", wxOK | wxICON_WARNING, this);
            }
            else if (!hadSessionToken) {
                FetchAndStoreRemoteCalendars(*account, *accessToken);
                ReloadAccounts();
                auto refreshedAccount = accountRepository_.GetById(account->id);
                if (refreshedAccount.has_value()) {
                    account = refreshedAccount;
                }
            }
        }

        LoadCalendarsForCurrentAccount(*account);
        RefreshAccountControls();
        RefreshCalendarControls();

        if (refreshEvents) {
            Freeze();
            RefreshEvents();
            Thaw();
        }

        return true;
    }

    bool DeleteAccountCascade(const long long accountId) {
        if (accountId == localAccountId_) {
            return false;
        }

        db_.exec("BEGIN IMMEDIATE TRANSACTION");
        try {
            SQLite::Statement deleteEvents(
                db_,
                "DELETE FROM events WHERE calendar_id IN (SELECT id FROM calendars WHERE account_id = ?)");
            deleteEvents.bind(1, accountId);
            deleteEvents.exec();

            SQLite::Statement deleteCalendars(db_, "DELETE FROM calendars WHERE account_id = ?");
            deleteCalendars.bind(1, accountId);
            deleteCalendars.exec();

            accountRepository_.DeleteById(accountId);
            db_.exec("COMMIT");
            return true;
        }
        catch (...) {
            db_.exec("ROLLBACK");
            throw;
        }
    }

    bool AddRemoteAccount(const int platform) {
        try {
            statusLabel_->SetLabel("Opening sign-in...");
            wxBusyCursor busy;

            AccessToken token(platform);
            const std::string accessToken = token.GetToken();
            if (accessToken.empty()) {
                wxMessageBox("Could not obtain an access token for the selected account.",
                             "Sign-in failed", wxOK | wxICON_ERROR, this);
                return false;
            }

            auto account = GetAccountUserInfo(token);
            if (!account.has_value()) {
                wxMessageBox("Login succeeded, but account details could not be loaded from the server.",
                             "Account load failed", wxOK | wxICON_ERROR, this);
                return false;
            }

            account->refreshToken = token.GetRefreshToken();
            const long long accountId = accountRepository_.Upsert(*account);
            account->id = accountId;
            sessionAccessTokens_[accountId] = accessToken;

            if (!FetchAndStoreRemoteCalendars(*account, accessToken)) {
                wxMessageBox("The account was added, but calendars could not be fetched from the server.",
                             "Calendar fetch warning", wxOK | wxICON_WARNING, this);
            }

            ReloadAccounts();
            if (!SetCurrentAccount(accountId, true)) {
                return false;
            }

            statusLabel_->SetLabel("Account connected and calendars loaded");
            return true;
        }
        catch (const std::exception& ex) {
            wxMessageBox(wxString::Format("Sign-in failed: %s", ex.what()),
                         "Account error", wxOK | wxICON_ERROR, this);
            return false;
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
        if (!AddRemoteAccount(platform)) {
            RefreshAccountControls();
        }
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

        auto* accountSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* accountCaption = new wxStaticText(panel, wxID_ANY, "Account:");
        accountComboBox_ = new wxComboBox(panel, wxID_ANY, "", wxDefaultPosition, wxSize(240, -1), 0, nullptr, wxCB_READONLY);
        removeAccountButton_ = new wxButton(panel, wxID_ANY, "Remove account");
        accountSizer->Add(accountCaption, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        accountSizer->Add(accountComboBox_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        accountSizer->Add(removeAccountButton_, 0, wxALIGN_CENTER_VERTICAL);

        toolbarSizer->Add(appTitle, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 20);
        toolbarSizer->Add(modeComboBox_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
        toolbarSizer->Add(todayButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 12);
        toolbarSizer->Add(previousButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
        toolbarSizer->Add(nextButton_, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 18);
        toolbarSizer->AddStretchSpacer();
        toolbarSizer->Add(periodSizer, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 16);
        toolbarSizer->Add(accountSizer, 0, wxALIGN_CENTER_VERTICAL);

        auto* contentSizer = new wxBoxSizer(wxHORIZONTAL);
        auto* leftPane = new wxBoxSizer(wxVERTICAL);
        calendarBook_ = new wxSimplebook(panel, wxID_ANY);

        auto* monthPage = new wxPanel(calendarBook_);
        auto* monthSizer = new wxBoxSizer(wxVERTICAL);
        auto* weekdaySizer = new wxGridSizer(1, 7, 8, 8);
        const std::array<const char*, 7> weekdays = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        for (const char* weekday : weekdays) {
            weekdaySizer->Add(new wxStaticText(monthPage, wxID_ANY, weekday), 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, 4);
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

        auto* sidePane = new wxBoxSizer(wxVERTICAL);
        sidePane->Add(new wxStaticText(panel, wxID_ANY, "Actions"), 0, wxALL, 10);

        auto* buttonSizer = new wxBoxSizer(wxVERTICAL);
        newButton_ = new wxButton(panel, wxID_ANY, "New Event");
        refreshButton_ = new wxButton(panel, wxID_ANY, "Refresh");
        newButton_->SetMinSize(wxSize(140, 36));
        buttonSizer->Add(newButton_, 0, wxEXPAND | wxBOTTOM, 8);
        buttonSizer->Add(refreshButton_, 0, wxEXPAND);
        sidePane->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        sidePane->Add(new wxStaticText(panel, wxID_ANY, "Calendars"), 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        newCalendarButton_ = new wxButton(panel, wxID_ANY, "New Calendar");
        sidePane->Add(newCalendarButton_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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
        contentSizer->Add(sidePane, 1, wxEXPAND | wxTOP | wxRIGHT | wxBOTTOM, 12);

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
        accountComboBox_->SetBackgroundColour(surfaceBg);
    }

    void BindEvents() {
        modeComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnModeChanged, this);
        todayButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnToday, this);
        previousButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(-1); });
        nextButton_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ShiftCurrentPeriod(1); });
        newButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNew, this);
        refreshButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnRefresh, this);
        newCalendarButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnNewCalendar, this);
        yearComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnYearChanged, this);
        accountComboBox_->Bind(wxEVT_COMBOBOX, &LocalCalendarFrame::OnAccountSelected, this);
        removeAccountButton_->Bind(wxEVT_BUTTON, &LocalCalendarFrame::OnDeleteCurrentAccount, this);
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

    void RefreshEvents() {
        if (accountCalendars_.empty()) {
            events_.clear();
            RefreshViewState();
            statusLabel_->SetLabel("No calendars available for the selected account");
            return;
        }

        const VisibleRange range = ComputeVisibleRange();
        events_.clear();
        for (const auto& calendar : accountCalendars_) {
            if (visibleCalendarIds_.count(calendar.id) == 0) {
                continue;
            }

            auto calendarEvents = eventRepository_.getEventsInRange(calendar.id, range.startEpoch, range.endEpoch);
            events_.insert(events_.end(), calendarEvents.begin(), calendarEvents.end());
        }

        std::sort(events_.begin(), events_.end(), [](const Event& lhs, const Event& rhs) {
            if (lhs.startDateTime != rhs.startDateTime) {
                return lhs.startDateTime < rhs.startDateTime;
            }
            if (lhs.endDateTime != rhs.endDateTime) {
                return lhs.endDateTime < rhs.endDateTime;
            }
            return lhs.id < rhs.id;
        });
        RefreshViewState();
        statusLabel_->SetLabel(wxString::Format("Loaded %zu visible event(s)", events_.size()));
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
        const long long today = StartOfUtcDay(std::time(nullptr));
        std::array<MonthCellMeta, kMonthCellCount> cells{};
        std::array<std::vector<std::optional<MonthCellEventSegment>>, kMonthCellCount> cellRows{};

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

        for (int weekIndex = 0; weekIndex < 6; ++weekIndex) {
            const int weekCellStart = weekIndex * 7;
            const long long weekStartEpoch = cells[weekCellStart].dayEpoch;
            const long long weekEndEpoch = cells[weekCellStart + 6].dayEpoch + kSecondsPerDay;

            std::vector<Event> spanningEvents;
            for (const auto& event : events_) {
                if (event.deletedAt != 0 || !SpansMultipleDays(event)) {
                    continue;
                }
                if (event.startDateTime < weekEndEpoch && event.endDateTime > weekStartEpoch) {
                    spanningEvents.push_back(event);
                }
            }

            std::sort(spanningEvents.begin(), spanningEvents.end(), [](const Event& lhs, const Event& rhs) {
                if (lhs.startDateTime != rhs.startDateTime) {
                    return lhs.startDateTime < rhs.startDateTime;
                }
                return lhs.endDateTime < rhs.endDateTime;
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
                const long long clippedStartDay = std::max(StartOfUtcDay(event.startDateTime), weekStartEpoch);
                const long long clippedEndDay = std::min(EventDisplayEndDay(event), weekEndEpoch - kSecondsPerDay);

                WeekSpan span;
                span.eventId = event.id;
                span.startDayOffset = static_cast<int>((clippedStartDay - weekStartEpoch) / kSecondsPerDay);
                span.endDayOffset = static_cast<int>((clippedEndDay - weekStartEpoch) / kSecondsPerDay);
                span.continuesBefore = event.startDateTime < weekStartEpoch;
                span.continuesAfter = event.endDateTime > weekEndEpoch;
                span.label = BuildMonthEventLabel(event);

                int assignedRow = 0;
                for (; assignedRow < static_cast<int>(weekRows.size()); ++assignedRow) {
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
                    segment.continuesBefore = span.continuesBefore || offset > span.startDayOffset;
                    segment.continuesAfter = span.continuesAfter || offset < span.endDayOffset;
                    cellRows[cellIndex][span.row] = segment;
                }
            }

            for (int offset = 0; offset < 7; ++offset) {
                cellRows[weekCellStart + offset].resize(weekRows.size());
            }

            for (int offset = 0; offset < 7; ++offset) {
                const int cellIndex = weekCellStart + offset;
                auto dayEvents = EventsForDay(cells[cellIndex].dayEpoch);
                for (const auto& event : dayEvents) {
                    if (SpansMultipleDays(event)) {
                        continue;
                    }
                    MonthCellEventSegment segment;
                    segment.eventId = event.id;
                    segment.label = BuildMonthEventLabel(event);
                    cellRows[cellIndex].push_back(segment);
                }
            }
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
            Freeze();
            RefreshEvents();
            Thaw();
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

    bool DeleteEventById(const long long eventId) {
        try {
            eventRepository_.deleteEvent(eventId);
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

        const auto builtEvent = dialog.BuildEvent(selectedCalendarId_);
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

        FocusDay(StartOfUtcDay(selectedEvent->startDateTime), true);
        statusLabel_->SetLabel(wxString::Format("Editing event #%lld", selectedEvent->id));
        OpenEventDialog(*selectedEvent);
    }

    void OpenNewEventDialog(const long long dayEpoch, const EventDraftDefaults& defaults) {
        if (selectedCalendarId_ == 0) {
            wxMessageBox("The current account does not have an active calendar yet.",
                         "No calendar available", wxOK | wxICON_WARNING, this);
            return;
        }

        FocusDay(dayEpoch, true);
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
        defaults.startDateTime = dayEpoch + static_cast<long long>(minuteOfDay) * 60;
        defaults.endDateTime = defaults.startDateTime + 3600;
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
        FocusDay(StartOfUtcDay(std::time(nullptr)), true);
    }

    void OnNew(wxCommandEvent&) {
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            OpenNewAllDayEventDialog(selectedDayEpoch_);
            return;
        }

        const long long now = static_cast<long long>(std::time(nullptr));
        const long long baseDay = currentViewMode_ == CalendarViewMode::DAY
            ? selectedDayEpoch_
            : std::max(selectedDayEpoch_, StartOfUtcDay(now));
        const std::tm nowTm = EpochToUtcTm(std::max(now, kMinCalendarEpoch));
        const int minuteOfDay = nowTm.tm_hour * 60;
        OpenNewTimedEventDialog(baseDay, minuteOfDay);
    }

    void OnRefresh(wxCommandEvent&) {
        RefreshEvents();
    }

    void OnNewCalendar(wxCommandEvent&) {
        OpenCreateCalendarDialog();
    }

    void OnYearChanged(wxCommandEvent& event) {
        long selectedYear = visibleYear_;
        if (!event.GetString().ToLong(&selectedYear)) {
            return;
        }
        visibleYear_ = std::max(kMinCalendarYear, static_cast<int>(selectedYear));
        EnsureYearComboContains(visibleYear_);
        if (currentViewMode_ == CalendarViewMode::MONTH) {
            RefreshViewState();
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
        if (accountComboUpdateInProgress_) {
            return;
        }

        const int selection = accountComboBox_->GetSelection();
        if (selection == wxNOT_FOUND) {
            return;
        }

        if (selection == static_cast<int>(accountComboAccountIds_.size())) {
            PromptForNewAccount();
            return;
        }

        if (selection >= 0 && selection < static_cast<int>(accountComboAccountIds_.size())) {
            SetCurrentAccount(accountComboAccountIds_[selection], true);
        }
    }

    void OnDeleteCurrentAccount(wxCommandEvent&) {
        const auto currentAccount = FindLoadedAccountById(currentAccountId_);
        if (!currentAccount.has_value() || IsLocalProvider(currentAccount->provider)) {
            return;
        }

        const int confirm = wxMessageBox(
            wxString::Format("Remove account '%s' and all calendars/events stored under it?",
                             FormatAccountLabel(*currentAccount)),
            "Remove account",
            wxYES_NO | wxNO_DEFAULT | wxICON_WARNING,
            this);
        if (confirm != wxYES) {
            return;
        }

        try {
            if (!DeleteAccountCascade(currentAccount->id)) {
                return;
            }

            ReloadAccounts();
            SetCurrentAccount(localAccountId_, true);
            statusLabel_->SetLabel("Account removed");
        }
        catch (const SQLite::Exception& ex) {
            wxMessageBox(wxString::Format("Account deletion failed: %s", ex.what()),
                         "Database error", wxOK | wxICON_ERROR, this);
        }
    }

    SQLite::Database db_;
    AccountRepository accountRepository_;
    EventRepository eventRepository_;
    CalendarRepository calendarRepository_;
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
    std::unordered_map<long long, std::set<long long>> accountVisibleCalendarIds_;
    std::unordered_map<long long, long long> accountSelectedCalendarIds_;
    std::unordered_map<long long, std::string> sessionAccessTokens_;
    std::vector<Event> events_;
    bool accountComboUpdateInProgress_ = false;

    wxComboBox* modeComboBox_ = nullptr;
    wxButton* todayButton_ = nullptr;
    wxButton* previousButton_ = nullptr;
    wxButton* nextButton_ = nullptr;
    wxStaticText* monthTitleLabel_ = nullptr;
    wxComboBox* yearComboBox_ = nullptr;
    int yearComboWindowStart_ = kMinCalendarYear;
    int yearComboWindowEnd_ = kMinCalendarYear + kYearComboChunkSize - 1;
    wxComboBox* accountComboBox_ = nullptr;
    wxButton* removeAccountButton_ = nullptr;
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
