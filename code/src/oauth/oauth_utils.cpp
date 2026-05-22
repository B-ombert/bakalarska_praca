#include "oauth/oauth_utils.h"
#include "http.h"

#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <future>
#include <sstream>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <wx/utils.h>

#include "utils/http.h"

namespace beast = boost::beast;
namespace http = beast::http;

namespace {

thread_local std::string g_lastOAuthErrorMessage;
std::atomic_bool g_oauthCancelRequested = false;

void SetLastOAuthErrorMessage(const std::string& message) {
    g_lastOAuthErrorMessage = message;
}

std::string UrlEncodeFormValue(const std::string& value) {
    std::string encoded;
    encoded.reserve(value.size() * 3);

    for (const unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded.push_back(static_cast<char>(ch));
        }
        else if (ch == ' ') {
            encoded.push_back('+');
        }
        else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return encoded;
}

std::string UrlDecodeValue(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());

    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded.push_back(' ');
        }
        else if (value[index] == '%' && index + 2 < value.size()) {
            const std::string hex = value.substr(index + 1, 2);
            decoded.push_back(static_cast<char>(std::strtoul(hex.c_str(), nullptr, 16)));
            index += 2;
        }
        else {
            decoded.push_back(value[index]);
        }
    }

    return decoded;
}

std::string ExtractQueryParam(const std::string& target, const std::string& key) {
    const auto queryStart = target.find('?');
    if (queryStart == std::string::npos) {
        return "";
    }

    const std::string query = target.substr(queryStart + 1);
    std::stringstream stream(query);
    std::string pair;

    while (std::getline(stream, pair, '&')) {
        const auto separator = pair.find('=');
        const std::string paramKey = pair.substr(0, separator);
        if (paramKey != key) {
            continue;
        }

        const std::string paramValue = (separator == std::string::npos) ? "" : pair.substr(separator + 1);
        return UrlDecodeValue(paramValue);
    }

    return "";
}

} // namespace

OAuthRedirectServer::OAuthRedirectServer()
    : acceptor_(ioContext_),
      socket_(ioContext_) {
}

OAuthRedirectServer::~OAuthRedirectServer() {
    Stop();
}

void OAuthRedirectServer::Start(SuccessHandler onSuccess, ErrorHandler onError) {
    onSuccess_ = std::move(onSuccess);
    onError_ = std::move(onError);
    finished_ = false;
    buffer_.consume(buffer_.size());
    ioContext_.restart();
    port_ = 0;

    boost::system::error_code ec;
    const tcp::endpoint endpoint(net::ip::make_address("127.0.0.1"), 0);

    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
        FinishWithError("Failed to open redirect server socket: " + ec.message());
        return;
    }

    acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
    if (ec) {
        FinishWithError("Failed to configure redirect server socket: " + ec.message());
        return;
    }

    acceptor_.bind(endpoint, ec);
    if (ec) {
        FinishWithError("Failed to bind redirect server to a local dynamic port: " + ec.message());
        return;
    }

    port_ = acceptor_.local_endpoint(ec).port();
    if (ec || port_ == 0) {
        FinishWithError("Failed to read the dynamic OAuth redirect port: " + ec.message());
        return;
    }

    acceptor_.listen(net::socket_base::max_listen_connections, ec);
    if (ec) {
        FinishWithError("Failed to listen for OAuth redirect: " + ec.message());
        return;
    }

    std::cout << "Waiting for redirect on " << GetRedirectUri() << "\n";
    DoAccept();

    worker_ = std::thread([this]() {
        ioContext_.run();
    });
}

std::string OAuthRedirectServer::GetRedirectUri() const {
    return "http://localhost:" + std::to_string(port_);
}

void OAuthRedirectServer::Stop() {
    CloseSockets();
    ioContext_.stop();

    if (worker_.joinable()) {
        worker_.join();
    }

    ioContext_.restart();
}

void OAuthRedirectServer::DoAccept() {
    acceptor_.async_accept(socket_, [this](const boost::system::error_code& ec) {
        OnAccept(ec);
    });
}

void OAuthRedirectServer::OnAccept(const boost::system::error_code& ec) {
    if (finished_) {
        return;
    }

    if (ec) {
        FinishWithError("OAuth redirect accept failed: " + ec.message());
        return;
    }

    DoRead();
}

void OAuthRedirectServer::DoRead() {
    net::async_read_until(socket_, buffer_, "\r\n\r\n",
        [this](const boost::system::error_code& ec, const std::size_t bytesTransferred) {
            OnRead(ec, bytesTransferred);
        });
}

void OAuthRedirectServer::OnRead(const boost::system::error_code& ec, const std::size_t bytesTransferred) {
    if (finished_) {
        return;
    }

    if (ec) {
        FinishWithError("OAuth redirect read failed: " + ec.message());
        return;
    }

    const std::string rawRequest(
        boost::asio::buffers_begin(buffer_.data()),
        boost::asio::buffers_begin(buffer_.data()) + static_cast<std::ptrdiff_t>(bytesTransferred));

    http::request_parser<http::empty_body> parser;
    parser.eager(true);
    beast::error_code parseEc;
    parser.put(boost::asio::buffer(rawRequest), parseEc);
    if (parseEc) {
        FinishWithError("Failed to parse OAuth redirect request");
        return;
    }

    parser.put_eof(parseEc);
    if (parseEc) {
        FinishWithError("OAuth redirect request ended unexpectedly");
        return;
    }

    const auto request = parser.get();
    const std::string target = std::string(request.target());
    const std::string code = ExtractQueryParam(target, "code");
    const std::string errorMessage = ExtractQueryParam(target, "error");

    SendBrowserResponse();

    if (!errorMessage.empty()) {
        FinishWithError("OAuth redirect returned error: " + errorMessage);
        return;
    }

    if (code.empty()) {
        FinishWithError("OAuth redirect did not contain an authorization code");
        return;
    }

    FinishWithCode(code);
}

void OAuthRedirectServer::FinishWithCode(const std::string& code) {
    if (finished_) {
        return;
    }

    finished_ = true;
    CloseSockets();
    ioContext_.stop();

    if (onSuccess_) {
        onSuccess_(code);
    }
}

void OAuthRedirectServer::FinishWithError(const std::string& error) {
    if (finished_) {
        return;
    }

    finished_ = true;
    CloseSockets();
    ioContext_.stop();

    if (onError_) {
        onError_(error);
    }
}

void OAuthRedirectServer::SendBrowserResponse() {
    static const char* kResponse =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<title>Sign-in complete</title>"
        "<style>"
        "body{margin:0;min-height:100vh;display:grid;place-items:center;"
        "font-family:Segoe UI,Roboto,Arial,sans-serif;background:#eef5f1;color:#1f2a24;}"
        ".card{max-width:420px;margin:24px;padding:34px 38px;border-radius:22px;"
        "background:white;box-shadow:0 22px 60px rgba(28,64,45,.16);text-align:center;}"
        ".mark{width:58px;height:58px;margin:0 auto 18px;border-radius:50%;"
        "display:grid;place-items:center;background:#dff5e8;color:#137a3d;font-size:34px;font-weight:700;}"
        "h1{font-size:24px;margin:0 0 10px;}p{margin:0;color:#58655e;line-height:1.5;}"
        "</style>"
        "</head>"
        "<body><main class=\"card\"><div class=\"mark\">✓</div>"
        "<h1>Sign-in complete</h1>"
        "<p>You can close this browser tab and return to the calendar app.</p>"
        "</main></body></html>";

    boost::system::error_code ignoredEc;
    boost::asio::write(socket_, boost::asio::buffer(kResponse, std::strlen(kResponse)), ignoredEc);
}

void OAuthRedirectServer::CloseSockets() {
    boost::system::error_code ignoredEc;

    if (socket_.is_open()) {
        socket_.shutdown(tcp::socket::shutdown_both, ignoredEc);
        socket_.close(ignoredEc);
    }

    if (acceptor_.is_open()) {
        acceptor_.cancel(ignoredEc);
        acceptor_.close(ignoredEc);
    }
}

OAuthAuthorizationResult RunOAuthAuthorization(const std::function<std::string(const std::string&)>& buildAuthorizationUrl) {
    try {
        ClearLastOAuthErrorMessage();
        g_oauthCancelRequested = false;
        std::promise<std::string> resultPromise;
        auto resultFuture = resultPromise.get_future();
        OAuthRedirectServer server;

        server.Start(
            [&resultPromise](const std::string& code) mutable {
                resultPromise.set_value(code);
            },
            [&resultPromise](const std::string& error) mutable {
                std::cerr << error << "\n";
                SetLastOAuthErrorMessage(error);
                resultPromise.set_value("");
            });

        const std::string redirectUri = server.GetRedirectUri();
        if (redirectUri == "http://localhost:0") {
            SetLastOAuthErrorMessage("OAuth redirect server did not start correctly.");
            server.Stop();
            return {};
        }

        const std::string authUrl = buildAuthorizationUrl(redirectUri);
        if (authUrl.empty() || !OpenUrlInBrowser(authUrl)) {
            server.Stop();
            return {};
        }

        const auto timeout = std::chrono::steady_clock::now() + std::chrono::minutes(3);
        while (resultFuture.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready) {
            if (g_oauthCancelRequested.load()) {
                SetLastOAuthErrorMessage("Sign-in was canceled.");
                server.Stop();
                g_oauthCancelRequested = false;
                return {};
            }

            if (std::chrono::steady_clock::now() >= timeout) {
                SetLastOAuthErrorMessage("Sign-in was not completed in time.");
                server.Stop();
                return {};
            }
        }

        g_oauthCancelRequested = false;
        return OAuthAuthorizationResult{resultFuture.get(), redirectUri};
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        SetLastOAuthErrorMessage(e.what());
        g_oauthCancelRequested = false;
        return {};
    }
}

void RequestOAuthCancellation() {
    g_oauthCancelRequested = true;
}

bool OpenUrlInBrowser(const std::string& url) {
    const bool opened = wxLaunchDefaultBrowser(wxString::FromUTF8(url));
    if (!opened) {
        SetLastOAuthErrorMessage("Failed to open the default browser.");
    }
    return opened;
}

std::string UrlEncodeOAuthValue(const std::string& value) {
    return UrlEncodeFormValue(value);
}

std::string GetLastOAuthErrorMessage() {
    return g_lastOAuthErrorMessage;
}

void ClearLastOAuthErrorMessage() {
    g_lastOAuthErrorMessage.clear();
}

std::string WritePostDataForGoogle(const tokenRequestParameters& params) {
    std::ostringstream postData;
    postData << "code=" << UrlEncodeFormValue(params.code)
             << "&client_id=" << UrlEncodeFormValue(GOOGLE_CLIENT_ID)
             << "&client_secret=" << UrlEncodeFormValue(GOOGLE_CLIENT_SECRET)
             << "&redirect_uri=" << UrlEncodeFormValue(params.redirect_uri)
             << "&grant_type=authorization_code"
             << "&code_verifier=" << UrlEncodeFormValue(params.code_verifier);

    return postData.str();
}

std::string WritePostDataForMS(const tokenRequestParameters& params) {
    std::ostringstream postData;
    postData << "client_id=" << UrlEncodeFormValue(MS_CLIENT_ID)
             << "&grant_type=authorization_code"
             << "&code=" << UrlEncodeFormValue(params.code)
             << "&redirect_uri=" << UrlEncodeFormValue(params.redirect_uri)
             << "&code_verifier=" << UrlEncodeFormValue(params.code_verifier)
             << "&scope=" << UrlEncodeFormValue("offline_access Calendars.ReadWrite");

    return postData.str();
}

std::string ExchangeCodeForToken(Platform platform, const tokenRequestParameters& params) {
    HttpRequest req;

    if (platform == GOOGLE) {
        req.url = GOOGLE_TOKEN_URL;
        req.postData = WritePostDataForGoogle(params);
    }
    else if (platform == MICROSOFT) {
        req.url = MS_TOKEN_URL;
        req.postData = WritePostDataForMS(params);
    }
    else {
        return "";
    }

    req.verb = POST;
    req.headers = {
        "Content-Type: application/x-www-form-urlencoded"
    };

    return PerformHttpRequest(req);
}
