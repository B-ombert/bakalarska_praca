#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "utils/access_token.h"

struct PendingEventUploadSession {
    long long accountId = 0;
    std::string provider;
    std::shared_ptr<AccessToken> token;
};

struct PendingEventUploadResult {
    long long accountId = 0;
    bool success = false;
    std::string message;
    std::string refreshedRefreshToken;
    int pendingEventCount = 0;
    int acceptedEventCount = 0;
};

class EventUploadScheduler {
public:
    using ResultCallback = std::function<void(const PendingEventUploadResult&)>;

    EventUploadScheduler(std::string dbPath, ResultCallback onResult);
    ~EventUploadScheduler();

    EventUploadScheduler(const EventUploadScheduler&) = delete;
    EventUploadScheduler& operator=(const EventUploadScheduler&) = delete;

    void Start();
    void Stop();
    void UpsertSession(PendingEventUploadSession session);
    void RemoveSession(long long accountId);
    void QueueAccount(long long accountId);
    void QueueAllSignedInAccounts();
    int CountPendingUploadEvents(long long accountId) const;

private:
    struct Job {
        long long accountId = 0;
    };

    PendingEventUploadResult RunJob(const Job& job);
    void WorkerLoop();
    void QueueAccountLocked(long long accountId);
    void QueueAccountsWithPendingEvents();

    std::string dbPath_;
    ResultCallback onResult_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::unordered_map<long long, PendingEventUploadSession> sessions_;
    std::set<long long> queuedAccountIds_;
    bool running_ = false;
    bool stopping_ = false;
};
