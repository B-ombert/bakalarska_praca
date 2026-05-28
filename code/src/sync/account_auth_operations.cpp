#include "sync/account_auth_operations.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include "models/calendar.h"
#include "oauth/oauth_utils.h"
#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "services/account_service.h"
#include "sync/google_calendar_sync_service.h"
#include "sync/outlook_calendar_sync_service.h"
#include "utils/sqlite_utils.h"

namespace {

std::optional<int> PlatformForProvider(const std::string& provider) {
    if (provider == "GOOGLE") {
        return GOOGLE;
    }
    if (provider == "MICROSOFT") {
        return MICROSOFT;
    }
    return std::nullopt;
}

void ApplyGooglePrimaryCalendarDisplayName(const Account& account, std::vector<Calendar>& calendars) {
    if (account.provider != "GOOGLE" || account.name.empty()) {
        return;
    }

    for (auto& calendar : calendars) {
        if (calendar.isPrimary) {
            calendar.name = account.name;
        }
    }
}

bool FetchAndStoreRemoteCalendarsForAccount(const Account& account,
                                            AccessToken& token,
                                            CalendarRepository& calendarRepository,
                                            EventRepository& eventRepository) {
    if (account.provider == "GOOGLE") {
        GoogleCalendarSyncService service(calendarRepository, eventRepository);
        auto remoteCalendars = service.fetchRemoteCalendars(token.GetToken());
        ApplyGooglePrimaryCalendarDisplayName(account, remoteCalendars);
        for (auto& calendar : remoteCalendars) {
            calendar.accountId = account.id;
        }
        service.syncCalendarsIncremental(
            account.id,
            remoteCalendars,
            [&token]() { return token.GetToken(); },
            true);
    }
    else if (account.provider == "MICROSOFT") {
        OutlookCalendarSyncService service(calendarRepository, eventRepository);
        auto remoteCalendars = service.fetchRemoteCalendars(token.GetToken());
        for (auto& calendar : remoteCalendars) {
            calendar.accountId = account.id;
        }
        service.syncCalendarsIncremental(
            account.id,
            remoteCalendars,
            [&token]() { return token.GetToken(); },
            true);
    }

    return true;
}

} // namespace

AuthOperationResult RunAddAccountAuthOperation(const std::string& dbPath, const int platform) {
    AuthOperationResult result;
    result.kind = AuthOperationKind::ADD_ACCOUNT;
    result.platform = platform;

    auto token = std::make_shared<AccessToken>(platform);
    const std::string accessToken = token->GetToken();
    if (accessToken.empty()) {
        result.error = token->GetLastError().empty()
            ? "Could not obtain an access token for the selected account."
            : token->GetLastError();
        return result;
    }

    auto account = GetAccountUserInfo(*token);
    if (!account.has_value()) {
        result.error = "Login succeeded, but account details could not be loaded from the server.";
        return result;
    }

    account->refreshToken = token->GetRefreshToken();

    SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    ConfigureSqliteConnection(db);
    AccountRepository accountRepository(db);
    EventRepository eventRepository(db);
    CalendarRepository calendarRepository(db);

    const long long accountId = accountRepository.Upsert(*account);
    account->id = accountId;
    result.account = *account;
    result.accountId = accountId;

    try {
        FetchAndStoreRemoteCalendarsForAccount(*account, *token, calendarRepository, eventRepository);
    }
    catch (const std::exception& ex) {
        result.warning = std::string("The account was added, but calendars could not be fetched from the server: ") + ex.what();
    }

    result.token = std::move(token);
    result.success = true;
    return result;
}

AuthOperationResult RunActivateAccountAuthOperation(const std::string& dbPath, Account account) {
    AuthOperationResult result;
    result.kind = AuthOperationKind::ACTIVATE_ACCOUNT;
    result.accountId = account.id;
    result.account = account;

    const auto platform = PlatformForProvider(account.provider);
    if (!platform.has_value()) {
        result.error = "This account provider is not supported for sign-in.";
        return result;
    }

    SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    ConfigureSqliteConnection(db);
    AccountRepository accountRepository(db);
    EventRepository eventRepository(db);
    CalendarRepository calendarRepository(db);

    if (account.refreshToken.empty()) {
        account.refreshToken = accountRepository.GetRefreshToken(account);
    }

    if (account.refreshToken.empty()) {
        result.error = "The saved account session could not be restored because no refresh token is available.";
        return result;
    }

    auto token = std::make_shared<AccessToken>(*platform, account.refreshToken, false);
    const std::string accessToken = token->GetToken();
    if (accessToken.empty()) {
        result.error = token->GetLastError().empty()
            ? "Could not restore the saved account session."
            : token->GetLastError();
        return result;
    }

    const std::string updatedRefreshToken = token->GetRefreshToken();
    if (!updatedRefreshToken.empty() && updatedRefreshToken != account.refreshToken) {
        account.refreshToken = updatedRefreshToken;
        accountRepository.Upsert(account);
    }

    if (auto refreshedAccount = GetAccountUserInfo(*token); refreshedAccount.has_value()) {
        refreshedAccount->id = account.id;
        refreshedAccount->refreshToken = account.refreshToken;
        if (refreshedAccount->providerUserId.empty()) {
            refreshedAccount->providerUserId = account.providerUserId;
        }
        accountRepository.Upsert(*refreshedAccount);
        account = *refreshedAccount;
        account.id = result.accountId;
    }

    try {
        FetchAndStoreRemoteCalendarsForAccount(account, *token, calendarRepository, eventRepository);
    }
    catch (const std::exception& ex) {
        result.warning = std::string("The account session was restored, but calendars could not be fetched from the server: ") + ex.what();
    }

    result.account = account;
    result.token = std::move(token);
    result.success = true;
    return result;
}
