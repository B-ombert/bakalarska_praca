#pragma once
#include "oauth_google.h"
#include "oauth_ms.h"

class AccessToken {
public:
    AccessToken(Platform platform);
    AccessToken(Platform platform, const std::string& refresh_token);
    Platform Platform;
    const std::string GetToken();
    const std::string GetRefreshToken();

    std::chrono::system_clock::time_point ExpiresAt;

    static std::string UrlEncode(const std::string& value);
    void TestingFunction();

private:
    std::string AccessTokenValue;

    std::string RefreshTokenValue;
    bool IsAccessTokenValid() const;
    bool RefreshAccessToken();
    bool GetNewToken();
    bool ParseJsonTokenResponse(const std::string& tokenResponse);
};