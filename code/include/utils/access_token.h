#pragma once
#include "oauth/oauth_google.h"
#include "oauth/oauth_ms.h"
#include "utils/types.h"
#include <chrono>
#include <mutex>

class AccessToken {
public:
    AccessToken(int platform);
    AccessToken(int platform, const std::string& refresh_token, bool allowInteractiveFallback = true);
    AccessToken(int platform,
                const std::string& refresh_token,
                bool allowInteractiveFallback,
                bool refreshImmediately);
    AccessToken(AccessToken&& other) noexcept;
    AccessToken& operator=(AccessToken&& other) noexcept;
    AccessToken(const AccessToken&) = delete;
    AccessToken& operator=(const AccessToken&) = delete;
    const std::string GetToken();
    const std::string GetRefreshToken();
    const std::string& GetLastError() const;
    int GetPlatform() const;

    static std::string UrlEncode(const std::string& value);

private:
    std::string AccessTokenValue;
    std::string RefreshTokenValue;
    std::string LastErrorMessage;
    std::chrono::system_clock::time_point ExpiresAt{};
    int Platform;
    bool AllowInteractiveFallback = true;
    bool InitialAcquisitionAttempted = false;
    mutable std::mutex TokenMutex;

    bool IsAccessTokenValid() const;
    bool RefreshAccessToken();
    bool GetNewToken();
    bool ParseJsonTokenResponse(const std::string& tokenResponse);
};
