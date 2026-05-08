#include "services/account_service.h"

#include "services/google_account_service.h"
#include "services/microsoft_account_service.h"

std::optional<Account> GetAccountUserInfo(AccessToken& token) {
    switch (token.GetPlatform()) {
        case GOOGLE:
            return GetGoogleUserInfo(token);
        case MICROSOFT:
            return GetMicrosoftUserInfo(token);
        default:
            return std::nullopt;
    }
}
