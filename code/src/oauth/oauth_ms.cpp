#include "oauth_ms.h"
#include "http.h"
#include <sstream>
#include <thread>
#include <windows.h>
#include <shellapi.h>
#include <boost/asio.hpp>

using namespace boost::asio;
using tcp = ip::tcp;

std::string GetMSAuthCode(const std::string& code_challenge){
    std::string auth_url =
            "https://login.microsoftonline.com/common/oauth2/v2.0/authorize?"
            "client_id=" + MS_CLIENT_ID +
            "&response_type=code" +
            "&redirect_uri=" + REDIRECT_URI +
            "&response_mode=query" +
            "&scope=offline_access%20Calendars.ReadWrite" +
            "&code_challenge=" + code_challenge +
            "&code_challenge_method=S256";

    std::cout << "\nOpening Microsoft login \n";

    std::string code;

    std::thread serverThread([&code](){
        code = CatchRedirectedAuthCode();
    });

    ShellExecuteA(0, "open", auth_url.c_str(), 0, 0, SW_SHOWNORMAL);

    serverThread.join();

    return code;
}

std::string GetAccessTokenFromMS(){
    std::string code_verifier = GenerateCodeVerifier();
    std::string code_challenge = GenerateCodeChallenge(code_verifier);

    std::string code = GetMSAuthCode(code_challenge);

    if (code.empty()){
        std::cerr << "Couldn't get auth code";
        return "";
    }

    std::cout << "Auth code: " << code << "\n";

    tokenRequestParameters params {code, code_verifier};

    std::string tokenResponse = ExchangeCodeForToken(MICROSOFT, params);
    std::cout << "\n Token response: \n" << tokenResponse << "\n";

    return tokenResponse;
}