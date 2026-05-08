#include "services/microsoft_account_service.h"

#include <utils/http.h>
#include <utils/json.hpp>

std::optional<Account> GetMicrosoftUserInfo(AccessToken& token) {
    HttpRequest req;

    req.url = "https://graph.microsoft.com/v1.0/me";
    const std::string accessToken = token.GetToken();
    if (accessToken.empty()) {
        return std::nullopt;
    }
    req.headers.push_back("Authorization: Bearer " + accessToken);

    const std::string response = PerformHttpRequest(req);
    if (response.empty()) {
        return std::nullopt;
    }

    try {
        const auto json = nlohmann::json::parse(response);

        Account account;
        account.providerUserId = json.value("id", "");
        account.name = json.value("displayName", "");
        account.provider = "MICROSOFT";
        account.refreshToken = token.GetRefreshToken();

        return account;
    }
    catch (...) {
        return std::nullopt;
    }
}
