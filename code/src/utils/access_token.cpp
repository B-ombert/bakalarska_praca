#include "utils/access_token.h"
#include "oauth/oauth_utils.h"
#include <iostream>
#include <utils/json.hpp>
#include "utils/http.h"

using json = nlohmann::json;

AccessToken::AccessToken(int platform) : Platform(platform) {
    if (!GetNewToken()) {
        if (LastErrorMessage.empty()) {
            LastErrorMessage = "Error in obtaining new access token";
        }
        std::cerr << LastErrorMessage;
    }
    InitialAcquisitionAttempted = true;
}

AccessToken::AccessToken(AccessToken&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.TokenMutex);
    AccessTokenValue = std::move(other.AccessTokenValue);
    RefreshTokenValue = std::move(other.RefreshTokenValue);
    LastErrorMessage = std::move(other.LastErrorMessage);
    ExpiresAt = other.ExpiresAt;
    Platform = other.Platform;
    AllowInteractiveFallback = other.AllowInteractiveFallback;
    InitialAcquisitionAttempted = other.InitialAcquisitionAttempted;
}

AccessToken& AccessToken::operator=(AccessToken&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(TokenMutex, other.TokenMutex);
    AccessTokenValue = std::move(other.AccessTokenValue);
    RefreshTokenValue = std::move(other.RefreshTokenValue);
    LastErrorMessage = std::move(other.LastErrorMessage);
    ExpiresAt = other.ExpiresAt;
    Platform = other.Platform;
    AllowInteractiveFallback = other.AllowInteractiveFallback;
    InitialAcquisitionAttempted = other.InitialAcquisitionAttempted;
    return *this;
}

AccessToken::AccessToken(int platform, const std::string &refresh_token, const bool allowInteractiveFallback)
    : AccessToken(platform, refresh_token, allowInteractiveFallback, true) {}

AccessToken::AccessToken(int platform,
                         const std::string& refresh_token,
                         const bool allowInteractiveFallback,
                         const bool refreshImmediately) :
Platform(platform), RefreshTokenValue(refresh_token), AllowInteractiveFallback(allowInteractiveFallback) {
    if (refresh_token.empty()) {
        LastErrorMessage = "Invalid refresh token";
        std::cerr << "Invalid refresh token\n";
    }

    if (!refreshImmediately) {
        InitialAcquisitionAttempted = true;
        return;
    }

    if (!RefreshAccessToken()) {
        if (LastErrorMessage.empty()) {
            LastErrorMessage = "Token refresh failed, user needs to log in";
        }
        std::cerr << "Token refresh failed, user needs to log in\n";

        if (AllowInteractiveFallback && !GetNewToken()) {
            if (LastErrorMessage.empty()) {
                LastErrorMessage = "Error in obtaining new access token";
            }
            std::cerr << LastErrorMessage << "\n";
        }
    }
    InitialAcquisitionAttempted = true;
}

const std::string AccessToken::GetToken() {
    std::lock_guard<std::mutex> lock(TokenMutex);
    if (IsAccessTokenValid()) {
        return AccessTokenValue;
    }

    if (!RefreshTokenValue.empty() && RefreshAccessToken()) {
        return AccessTokenValue;
    }

    if (AllowInteractiveFallback && !InitialAcquisitionAttempted) {
        GetNewToken();
        InitialAcquisitionAttempted = true;
    }
    return AccessTokenValue;
}

const std::string AccessToken::GetRefreshToken() {
    std::lock_guard<std::mutex> lock(TokenMutex);
    return this->RefreshTokenValue;
}

const std::string& AccessToken::GetLastError() const {
    return LastErrorMessage;
}

int AccessToken::GetPlatform() const {
    return Platform;
}

bool AccessToken::ParseJsonTokenResponse(const std::string &tokenResponse) {
    try {
        json j = json::parse(tokenResponse);

        if (!j.contains("access_token") || !j.contains("expires_in")) {
            LastErrorMessage = "Invalid token response";
            std::cerr << "Invalid token response\n";
            return false;
        }

        this->AccessTokenValue = j["access_token"].get<std::string>();

        if (j.contains("refresh_token")) {
            this->RefreshTokenValue = j["refresh_token"].get<std::string>();
        }

        int expiresIn = j["expires_in"].get<int>();
        this->ExpiresAt = std::chrono::system_clock::now()
                  + std::chrono::seconds(expiresIn);
        LastErrorMessage.clear();

        return true;
    }
    catch (std::exception& e) {
        LastErrorMessage = std::string("JSON parse error: ") + e.what();
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return false;
    }
}

bool AccessToken::IsAccessTokenValid() const {
    using namespace std::chrono;

    if (this->AccessTokenValue.empty()) {
        return false;
    }

    auto now = system_clock::now();
    return now < this->ExpiresAt - seconds(60);
}

bool AccessToken::GetNewToken() {
    LastErrorMessage.clear();
    std::string token_response;
    switch (Platform) {
        case GOOGLE:
            token_response = GetAccessTokenFromGoogle();
            break;

        case MICROSOFT:
            token_response = GetAccessTokenFromMS();
            break;
    }

    if (token_response.empty() && LastErrorMessage.empty()) {
        LastErrorMessage = GetLastOAuthErrorMessage();
        if (LastErrorMessage.empty()) {
            LastErrorMessage = "Interactive sign-in did not complete successfully.";
        }
    }

    if (token_response.empty()) {
        return false;
    }

    return this->ParseJsonTokenResponse(token_response);
}

bool AccessToken::RefreshAccessToken() {
    if (this->RefreshTokenValue.empty()) {
        LastErrorMessage = "Refresh token is missing.";
        return false;
    }

    LastErrorMessage.clear();

    HttpRequest req;
    req.headers = {
    "Content-Type: application/x-www-form-urlencoded"
    };

    std::ostringstream body;

    switch (Platform) {
        case GOOGLE:
            req.url = GOOGLE_TOKEN_URL;

            body
            << "client_id=" << GOOGLE_CLIENT_ID
            << "&client_secret=" << GOOGLE_CLIENT_SECRET
            << "&grant_type=refresh_token"
            << "&refresh_token=" << this->RefreshTokenValue;
            break;

        case MICROSOFT:
            req.url = MS_TOKEN_URL;

            body
            << "client_id=" << MS_CLIENT_ID
            << "&grant_type=refresh_token"
            << "&refresh_token=" << this->UrlEncode(this->RefreshTokenValue)
            << "&scope=https://graph.microsoft.com/.default";
            break;
    }

    req.postData = body.str();
    req.verb = POST;

    std::string response = PerformHttpRequest(req);
    if (response.empty()) {
        LastErrorMessage = "Empty response while refreshing the access token.";
        std::cerr << "Empty response\n";
        return false;
    }

    return ParseJsonTokenResponse(response);
}

std::string AccessToken::UrlEncode(const std::string& value) {
    std::ostringstream os;
    os.fill('0');
    os << std::hex;

    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            os << c;
        } else {
            os << '%' << std::setw(2) << int((unsigned char)c);
        }
    }

    return os.str();
}
