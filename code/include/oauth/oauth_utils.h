#pragma once
#include <functional>
#include <string>
#include <thread>
#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
#include "utils/types.h"

using tcp = boost::asio::ip::tcp;
namespace net = boost::asio;

const std::string GOOGLE_CLIENT_ID = "66725031344-fl2rs8kipc55nvmkmnp561a9op8ho3bq.apps.googleusercontent.com";
const std::string GOOGLE_CLIENT_SECRET = "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";
inline const char* GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";

const std::string MS_CLIENT_ID = "de85f5c0-f11f-428b-9bdd-831e43c9ab1a";
inline const char* MS_TOKEN_URL = "https://login.microsoftonline.com/common/oauth2/v2.0/token";

const std::string REDIRECT_URI = "http://localhost:8080";

class OAuthRedirectServer {
public:
    using SuccessHandler = std::function<void(const std::string&)>;
    using ErrorHandler = std::function<void(const std::string&)>;

    OAuthRedirectServer();
    ~OAuthRedirectServer();

    void Start(SuccessHandler onSuccess, ErrorHandler onError);
    void Stop();

private:
    void DoAccept();
    void OnAccept(const boost::system::error_code& ec);
    void DoRead();
    void OnRead(const boost::system::error_code& ec, std::size_t bytesTransferred);
    void FinishWithCode(const std::string& code);
    void FinishWithError(const std::string& error);
    void SendBrowserResponse();
    void CloseSockets();

    net::io_context ioContext_;
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    boost::asio::streambuf buffer_;
    std::thread worker_;
    SuccessHandler onSuccess_;
    ErrorHandler onError_;
    bool finished_ = false;
};

std::string CatchRedirectedAuthCode();
bool OpenUrlInBrowser(const std::string& url);
void RequestOAuthCancellation();
std::string GetLastOAuthErrorMessage();
void ClearLastOAuthErrorMessage();
std::string WritePostDataForGoogle(const tokenRequestParameters& params);
std::string WritePostDataForMS(const tokenRequestParameters& params);
std::string ExchangeCodeForToken(Platform platform, const tokenRequestParameters& params);

