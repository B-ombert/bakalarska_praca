#pragma once

#include <memory>
#include <string>

#include "models/account.h"
#include "utils/access_token.h"

enum class AuthOperationKind {
    ADD_ACCOUNT,
    ACTIVATE_ACCOUNT
};

struct AuthOperationResult {
    AuthOperationKind kind = AuthOperationKind::ADD_ACCOUNT;
    bool success = false;
    long long accountId = 0;
    int platform = GOOGLE;
    std::shared_ptr<AccessToken> token;
    std::string error;
    std::string warning;
    Account account{};
};

AuthOperationResult RunAddAccountAuthOperation(const std::string& dbPath, int platform);
AuthOperationResult RunActivateAccountAuthOperation(const std::string& dbPath, Account account);
