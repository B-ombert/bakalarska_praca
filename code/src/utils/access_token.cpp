#include "access_token.h"

AccessToken::AccessToken(::Platform platform) : Platform(platform) {
    if (!GetNewToken()) {
        std::cerr << "Error in obtaining new access token";
    }
}

AccessToken::AccessToken(::Platform platform, const std::string &refresh_token) :
Platform(platform), RefreshTokenValue(refresh_token) {
    if (refresh_token.empty()) {
        std::cerr << "Invalid refresh token\n";
    }

    if (!RefreshAccessToken()) {
        std::cerr << "Token refresh failed, user needs to log in\n";

        if (!GetNewToken()) {
            std::cerr << "Error in obtaining new access token\n";
        }
    }
}

const std::string AccessToken::GetToken() {
    if (IsAccessTokenValid()) {
        return AccessTokenValue;
    }

    if (RefreshAccessToken()) {
        return AccessTokenValue;
    }

    GetNewToken();
    return AccessTokenValue;
}

const std::string AccessToken::GetRefreshToken() {
    return this->RefreshTokenValue;
}


bool AccessToken::ParseJsonTokenResponse(const std::string &tokenResponse) {
    try {
        json j = json::parse(tokenResponse);

        if (!j.contains("access_token") || !j.contains("expires_in")) {
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

        return true;
    }
    catch (std::exception& e) {
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
    std::string token_response;
    switch (Platform) {
        case GOOGLE:
            token_response = GetAccessTokenFromGoogle();
            break;

        case MICROSOFT:
            token_response = GetAccessTokenFromMS();
            break;
    }

    return this->ParseJsonTokenResponse(token_response);
}

bool AccessToken::RefreshAccessToken() {
    if (this->RefreshTokenValue.empty()) {
        return false;
    }

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

    std::string response = PerformHttpRequest(req);
    if (response.empty()) {
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

void AccessToken::TestingFunction() {
    std::cout << "Current access token duration: " << this->ExpiresAt << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "Refreshing access token \n";
    this->RefreshAccessToken();
    std::cout << "New access token duration: " << this->ExpiresAt << "\n";
}
