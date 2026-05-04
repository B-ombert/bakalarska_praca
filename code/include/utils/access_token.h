#pragma once
#include "oauth/oauth_google.h"
#include "oauth/oauth_ms.h"
#include "utils/types.h"
#include <chrono>

class AccessToken {
public:
    std::string AccessTokenValue;
    std::string RefreshTokenValue;

    AccessToken(int platform);
    AccessToken(int platform, const std::string& refresh_token, bool allowInteractiveFallback = true);
    int Platform;
    const std::string GetToken();
    const std::string GetRefreshToken();
    const std::string& GetLastError() const;

    std::chrono::system_clock::time_point ExpiresAt;

    static std::string UrlEncode(const std::string& value);

private:
    std::string LastErrorMessage;
    bool AllowInteractiveFallback = true;
    bool InitialAcquisitionAttempted = false;

    bool IsAccessTokenValid() const;
    bool RefreshAccessToken();
    bool GetNewToken();
    bool ParseJsonTokenResponse(const std::string& tokenResponse);
};
