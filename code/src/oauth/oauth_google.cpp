#include "oauth/oauth_google.h"
#include "oauth/oauth_utils.h"
#include "utils/crypto.h"
#include "utils/http.h"
#include <sstream>

std::string GetAccessTokenFromGoogle(){
    std::string code_verifier = GenerateCodeVerifier();
    std::string code_challenge = GenerateCodeChallenge(code_verifier);

    const OAuthAuthorizationResult authorization = RunOAuthAuthorization([&](const std::string& redirectUri) {
        std::ostringstream auth;
        auth << "https://accounts.google.com/o/oauth2/v2/auth?"
             << "client_id=" << GOOGLE_CLIENT_ID
             << "&redirect_uri=" << UrlEncodeOAuthValue(redirectUri)
             << "&response_type=code"
             << "&scope=https://www.googleapis.com/auth/calendar https://www.googleapis.com/auth/userinfo.profile https://www.googleapis.com/auth/userinfo.email"
             << "&access_type=offline"
             << "&prompt=consent"
             << "&code_challenge=" << code_challenge
             << "&code_challenge_method=S256";
        return auth.str();
    });
    std::string code = authorization.code;

    if (code.empty()){
        std::cerr << "Couldn't get auth code";
        return "";
    }

    std::cout << "Auth code: " << code << "\n";

    tokenRequestParameters params {code, code_verifier, authorization.redirectUri};

    std::string tokenResponse = ExchangeCodeForToken(GOOGLE, params);
    std::cout << "\n Token response: \n" << tokenResponse << "\n";

    return tokenResponse;
}
