#pragma once
#include <boost/asio.hpp>
#include <json.hpp>
#include <http.h>
#include "crypto.h"
#include "types.h"

using json = nlohmann::json;
using tcp = boost::asio::ip::tcp;
namespace net = boost::asio;

const std::string GOOGLE_CLIENT_ID = "66725031344-fl2rs8kipc55nvmkmnp561a9op8ho3bq.apps.googleusercontent.com";
const std::string GOOGLE_CLIENT_SECRET = "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";
inline const char* GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";

const std::string MS_CLIENT_ID = "de85f5c0-f11f-428b-9bdd-831e43c9ab1a";
inline const char* MS_TOKEN_URL = "https://login.microsoftonline.com/common/oauth2/v2.0/token";

const std::string REDIRECT_URI = "http://localhost:8080";

std::string CatchRedirectedAuthCode();
std::string ParseJsonTokenResponse(const std::string& tokenResponse);
std::string WritePostDataForGoogle(const tokenRequestParameters& params);
std::string WritePostDataForMS(const tokenRequestParameters& params);
std::string ExchangeCodeForToken(Platform platform, const tokenRequestParameters& params);

