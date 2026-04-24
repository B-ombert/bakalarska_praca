#include "oauth/oauth_utils.h"
#include "http.h"
#include <cstdlib>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <sstream>
#include <utils/json.hpp>
#include "utils/http.h"

namespace beast = boost::beast;
namespace http = beast::http;

namespace {

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
        if (value[index] == '+' ) {
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

std::string CatchRedirectedAuthCode(){
    try{
        net::io_context io_context;

        tcp::acceptor acceptor(
                io_context, tcp::endpoint(tcp::v4(), 8080));
        std::cout << "Waiting for redirect on http://localhost:8080\n";

        tcp::socket socket(io_context);
        acceptor.accept(socket);

        boost::asio::streambuf buf;
        boost::asio::read_until(socket, buf, "\r\n\r\n");
        std::string rawRequest(
            boost::asio::buffers_begin(buf.data()),
            boost::asio::buffers_end(buf.data())
        );

        http::request_parser<http::empty_body> parser;
        parser.eager(true);
        beast::error_code ec;
        parser.put(boost::asio::buffer(rawRequest), ec);
        if (ec) {
            throw std::runtime_error("Failed to parse OAuth redirect request");
        }

        parser.put_eof(ec);
        if (ec) {
            throw std::runtime_error("OAuth redirect request ended unexpectedly");
        }

        const auto request = parser.get();

        std::string code;
        std::string errorMessage;
        const std::string target = std::string(request.target());
        code = ExtractQueryParam(target, "code");
        errorMessage = ExtractQueryParam(target, "error");

        std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n\r\n"
                "<html><body><h2>Login successful.</h2></body></html>";

        boost::asio::write(socket, boost::asio::buffer(response));

        if (!errorMessage.empty()) {
            std::cerr << "OAuth redirect returned error: " << errorMessage << "\n";
            return "";
        }

        return code;

    }catch(std::exception& e){
        std::cerr << "Error: " << e.what() << "\n";
        return "";
    }
}

std::string WritePostDataForGoogle(const tokenRequestParameters& params){
    std::ostringstream postData;
    postData << "code=" << UrlEncodeFormValue(params.code)
             << "&client_id=" << UrlEncodeFormValue(GOOGLE_CLIENT_ID)
             << "&client_secret=" << UrlEncodeFormValue(GOOGLE_CLIENT_SECRET)
             << "&redirect_uri=" << UrlEncodeFormValue(REDIRECT_URI)
             << "&grant_type=authorization_code"
             << "&code_verifier=" << UrlEncodeFormValue(params.code_verifier);

    return postData.str();
}

std::string WritePostDataForMS(const tokenRequestParameters& params){
    std::ostringstream postData;
    postData << "client_id=" << UrlEncodeFormValue(MS_CLIENT_ID)
             << "&grant_type=authorization_code"
             << "&code=" << UrlEncodeFormValue(params.code)
             << "&redirect_uri=" << UrlEncodeFormValue(REDIRECT_URI)
             << "&code_verifier=" << UrlEncodeFormValue(params.code_verifier)
             << "&scope=" << UrlEncodeFormValue("offline_access Calendars.ReadWrite");

    return postData.str();
}

std::string ExchangeCodeForToken(Platform platform, const tokenRequestParameters& params){
    HttpRequest req;

    if (platform==GOOGLE) {
        req.url = GOOGLE_TOKEN_URL;
        req.postData = WritePostDataForGoogle(params);
    }
    else if (platform==MICROSOFT) {
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
