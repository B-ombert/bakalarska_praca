#include "oauth/oauth_ms.h"
#include "oauth/oauth_utils.h"
#include "utils/crypto.h"
#include "utils/http.h"
#include <sstream>

std::string GetAccessTokenFromMS(){
    std::string code_verifier = GenerateCodeVerifier();
    std::string code_challenge = GenerateCodeChallenge(code_verifier);

    const OAuthAuthorizationResult authorization = RunOAuthAuthorization([&](const std::string& redirectUri) {
        return "https://login.microsoftonline.com/common/oauth2/v2.0/authorize?"
               "client_id=" + MS_CLIENT_ID +
               "&response_type=code" +
               "&redirect_uri=" + UrlEncodeOAuthValue(redirectUri) +
               "&response_mode=query" +
               "&scope=offline_access%20Calendars.ReadWrite%20User.Read" +
               "&code_challenge=" + code_challenge +
               "&code_challenge_method=S256";
    });
    std::string code = authorization.code;

    if (code.empty()){
        return "";
    }

    tokenRequestParameters params {code, code_verifier, authorization.redirectUri};

    return ExchangeCodeForToken(MICROSOFT, params);
}
