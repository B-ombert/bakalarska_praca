#include "sync/event_upload_scheduler.h"

#include <chrono>
#include <exception>
#include <vector>
#include <utility>

#include "SQLiteCpp/Backup.h"
#include "repositories/account_repository.h"
#include "repositories/calendar_repository.h"
#include "repositories/event_repository.h"
#include "sync/google_calendar_sync_service.h"
#include "sync/outlook_calendar_sync_service.h"
#include "utils/sqlite_utils.h"

namespace {

constexpr auto kUploadInterval = std::chrono::minutes(5);

bool SupportsEventUploadProvider(const std::string& provider) {
    return provider == "GOOGLE" || provider == "MICROSOFT";
}

int CountPendingUploadEvents(const std::string& dbPath, const long long accountId) {
    SQLite::Database db(dbPath, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    ConfigureSqliteConnection(db);
    CalendarRepository calendarRepository(db);
    EventRepository eventRepository(db);

    int pendingCount = 0;
    const std::vector<Calendar> calendars = calendarRepository.getByAccount(accountId);
    for (const auto& calendar : calendars) {
        if (calendar.isReadOnly || !calendar.syncEnabled || calendar.providerCalendarId.empty()) {
            continue;
        }

        pendingCount += static_cast<int>(eventRepository.getPendingRemoteEvents(calendar.id).size());
    }

    return pendingCount;
}

bool AccountHasPendingUploadCalendars(const std::string& dbPath, const long long accountId) {
    return CountPendingUploadEvents(dbPath, accountId) > 0;
}

} // namespace

EventUploadScheduler::EventUploadScheduler(std::string dbPath, ResultCallback onResult)
    : dbPath_(std::move(dbPath)), onResult_(std::move(onResult)) {}

EventUploadScheduler::~EventUploadScheduler() {
    Stop();
}

void EventUploadScheduler::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return;
    }

    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this]() { WorkerLoop(); });
}

void EventUploadScheduler::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }

        stopping_ = true;
        jobs_.clear();
        queuedAccountIds_.clear();
    }

    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

void EventUploadScheduler::UpsertSession(PendingEventUploadSession session) {
    if (!SupportsEventUploadProvider(session.provider) || session.token == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.find(session.accountId) == sessions_.end()) {
        sessions_[session.accountId] = std::move(session);
        return;
    }

    sessions_[session.accountId].provider = std::move(session.provider);
}

void EventUploadScheduler::RemoveSession(const long long accountId) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(accountId);
    queuedAccountIds_.erase(accountId);
}

void EventUploadScheduler::QueueAccount(const long long accountId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        QueueAccountLocked(accountId);
    }

    cv_.notify_one();
}

void EventUploadScheduler::QueueAllSignedInAccounts() {
    QueueAccountsWithPendingEvents();
    cv_.notify_one();
}

int EventUploadScheduler::CountPendingUploadEvents(const long long accountId) const {
    try {
        return ::CountPendingUploadEvents(dbPath_, accountId);
    }
    catch (...) {
        return 0;
    }
}

PendingEventUploadResult EventUploadScheduler::RunJob(const Job& job) {
    PendingEventUploadResult result;
    result.accountId = job.accountId;

    try {
        SQLite::Database db(dbPath_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        ConfigureSqliteConnection(db);
        AccountRepository accountRepository(db);
        EventRepository eventRepository(db);
        CalendarRepository calendarRepository(db);
        std::string provider;
        std::shared_ptr<AccessToken> token;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto sessionIt = sessions_.find(job.accountId);
            if (sessionIt == sessions_.end() || sessionIt->second.token == nullptr) {
                result.message = "The account is not signed in.";
                return result;
            }

            provider = sessionIt->second.provider;
            token = sessionIt->second.token;
        }

        const std::string oldRefreshToken = token->GetRefreshToken();
        const std::string accessToken = token->GetToken();
        if (accessToken.empty()) {
            result.message = token->GetLastError().empty()
                ? "Could not refresh the access token before uploading events."
                : token->GetLastError();
            return result;
        }

        if (provider == "GOOGLE") {
            GoogleCalendarSyncService service(calendarRepository, eventRepository);
            const SyncPendingEventsResult syncResult =
                service.syncPendingEventsForAccount([token]() { return token->GetToken(); }, job.accountId);
            result.pendingEventCount = syncResult.pendingEventCount;
            result.acceptedEventCount = syncResult.acceptedEventCount;
        }
        else if (provider == "MICROSOFT") {
            OutlookCalendarSyncService service(calendarRepository, eventRepository);
            const SyncPendingEventsResult syncResult =
                service.syncPendingEventsForAccount([token]() { return token->GetToken(); }, job.accountId);
            result.pendingEventCount = syncResult.pendingEventCount;
            result.acceptedEventCount = syncResult.acceptedEventCount;
        }
        else {
            result.message = "This account provider does not support event upload.";
            return result;
        }

        const std::string refreshedRefreshToken = token->GetRefreshToken();
        if (!refreshedRefreshToken.empty() && refreshedRefreshToken != oldRefreshToken) {
            auto account = accountRepository.GetById(job.accountId);
            if (account.has_value()) {
                account->refreshToken = refreshedRefreshToken;
                accountRepository.Upsert(*account);
            }
            result.refreshedRefreshToken = refreshedRefreshToken;
        }

        result.success = true;
        result.message = "Pending event changes uploaded";
    }
    catch (const std::exception& ex) {
        result.message = ex.what();
    }

    return result;
}

void EventUploadScheduler::WorkerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    auto nextPeriodicUpload = std::chrono::steady_clock::now() + kUploadInterval;

    while (!stopping_) {
        cv_.wait_until(lock, nextPeriodicUpload, [this]() {
            return stopping_ || !jobs_.empty();
        });

        if (stopping_) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPeriodicUpload) {
            lock.unlock();
            QueueAccountsWithPendingEvents();
            lock.lock();
            nextPeriodicUpload = now + kUploadInterval;
        }

        if (jobs_.empty()) {
            continue;
        }

        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        queuedAccountIds_.erase(job.accountId);

        lock.unlock();
        PendingEventUploadResult result = RunJob(job);
        if (onResult_) {
            onResult_(result);
        }
        lock.lock();
    }
}

void EventUploadScheduler::QueueAccountLocked(const long long accountId) {
    const auto sessionIt = sessions_.find(accountId);
    if (sessionIt == sessions_.end() || queuedAccountIds_.count(accountId) > 0) {
        return;
    }

    jobs_.push_back(Job{accountId});
    queuedAccountIds_.insert(accountId);
}

void EventUploadScheduler::QueueAccountsWithPendingEvents() {
    std::vector<long long> accountIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accountIds.reserve(sessions_.size());
        for (const auto& [accountId, _] : sessions_) {
            accountIds.push_back(accountId);
        }
    }

    std::vector<long long> accountsWithPendingEvents;
    for (const long long accountId : accountIds) {
        try {
            if (AccountHasPendingUploadCalendars(dbPath_, accountId)) {
                accountsWithPendingEvents.push_back(accountId);
            }
        }
        catch (...) {
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const long long accountId : accountsWithPendingEvents) {
            QueueAccountLocked(accountId);
        }
    }
}
