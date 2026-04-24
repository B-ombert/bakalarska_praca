#pragma once
#include "oauth/oauth_google.h"
#include "oauth/oauth_ms.h"
#include <chrono>

class AccessToken {
public:
    std::string AccessTokenValue;
    std::string RefreshTokenValue;

    AccessToken(int platform);
    AccessToken(int platform, const std::string& refresh_token);
    int Platform;
    const std::string GetToken();
    const std::string GetRefreshToken();

    std::chrono::system_clock::time_point ExpiresAt;

    static std::string UrlEncode(const std::string& value);

private:


    bool IsAccessTokenValid() const;
    bool RefreshAccessToken();
    bool GetNewToken();
    bool ParseJsonTokenResponse(const std::string& tokenResponse);
};
