#include "oauth/oauth_google.h"
#include "http.h"
#include <sstream>
#include <windows.h>
#include <shellapi.h>

std::string GetGoogleAuthCode(const std::string& code_challenge){
    std::ostringstream auth;
    auth << "https://accounts.google.com/o/oauth2/v2/auth?"
         << "client_id=" << GOOGLE_CLIENT_ID
         << "&redirect_uri=" << REDIRECT_URI
         << "&response_type=code"
         << "&scope=https://www.googleapis.com/auth/calendar https://www.googleapis.com/auth/userinfo.profile"
         << "&access_type=offline"
         << "&prompt=consent"
         << "&code_challenge=" << code_challenge
         << "&code_challenge_method=S256";

    std::string auth_url = auth.str();

    std::cout << "Opening google login";

    ShellExecuteA(0, "open", auth_url.c_str(), 0, 0, SW_SHOWNORMAL);
    return CatchRedirectedAuthCode();
}

std::string GetAccessTokenFromGoogle(){
    std::string code_verifier = GenerateCodeVerifier();
    std::string code_challenge = GenerateCodeChallenge(code_verifier);

    std::string code = GetGoogleAuthCode(code_challenge);

    if (code.empty()){
        std::cerr << "Couldn't get auth code";
        return "";
    }

    std::cout << "Auth code: " << code << "\n";

    tokenRequestParameters params {code, code_verifier};

    std::string tokenResponse = ExchangeCodeForToken(GOOGLE, params);
    std::cout << "\n Token response: \n" << tokenResponse << "\n";

    return tokenResponse;
}
