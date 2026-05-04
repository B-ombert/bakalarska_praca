#include "services/google_account_service.h"

#include <utils/http.h>
#include <utils/json.hpp>

std::optional<Account> GetGoogleUserInfo(AccessToken& token) {
    HttpRequest req;

    req.url = "https://www.googleapis.com/oauth2/v3/userinfo";
    req.headers.push_back("Authorization: Bearer " + token.AccessTokenValue);

    std::string response = PerformHttpRequest(req);

    if (response.empty()) {
        return std::nullopt;
    }

    try {
        auto json = nlohmann::json::parse(response);

        Account account;

        account.providerUserId = json.value("sub", "");
        account.name = json.value("name", "");
        account.provider = "GOOGLE";
        account.refreshToken = token.GetRefreshToken();

        return account;
    }
    catch (...) {
        return std::nullopt;
    }
}
