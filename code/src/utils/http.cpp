#include "utils/http.h"
#include "utils/url_parser.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>
#include <stdexcept>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp = net::ip::tcp;

namespace {

std::string TrimHeaderValue(const std::string& value) {
    const auto start = value.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

template <typename Stream>
HttpResponse ExecuteRequest(Stream& stream, const ParsedUrl& parsed, const HttpRequest& req) {
    http::request<http::string_body> request;
    request.version(11);
    request.target(parsed.target);
    request.set(http::field::host, parsed.host);
    request.set(http::field::user_agent, "CalendarApp/1.0");
    request.set(http::field::connection, "close");

    for (const auto& header : req.headers) {
        const auto separator = header.find(':');
        if (separator == std::string::npos) {
            continue;
        }

        request.set(header.substr(0, separator), TrimHeaderValue(header.substr(separator + 1)));
    }

    switch (req.verb) {
        case POST:
            request.method(http::verb::post);
            break;
        case PATCH:
            request.method(http::verb::patch);
            break;
        case GET:
            request.method(http::verb::get);
            break;
        case DELETE_:
            request.method(http::verb::delete_);
            break;
        default:
            throw std::invalid_argument("Unknown HTTP request verb");
    }

    if (!req.postData.empty()) {
        request.body() = req.postData;
        request.prepare_payload();
    }

    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);

    return HttpResponse{static_cast<int>(response.result_int()), response.body()};
}

} // namespace

HttpResponse PerformHttpRequestWithResponse(const HttpRequest& req) {
    try {
        const ParsedUrl parsed = ParseUrl(req.url);

        net::io_context ioc;
        tcp::resolver resolver(ioc);
        const auto results = resolver.resolve(parsed.host, parsed.port);

        if (parsed.scheme == "https") {
            net::ssl::context ssl(net::ssl::context::tlsv12_client);
            ssl.set_default_verify_paths();

            beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl);
            beast::get_lowest_layer(stream).connect(results);
            stream.handshake(net::ssl::stream_base::client);

            HttpResponse response = ExecuteRequest(stream, parsed, req);

            beast::error_code ec;
            stream.shutdown(ec);
            return response;
        }

        if (parsed.scheme == "http") {
            beast::tcp_stream stream(ioc);
            stream.connect(results);
            return ExecuteRequest(stream, parsed, req);
        }

        std::cerr << "Unsupported URL scheme: " << parsed.scheme << "\n";
        return {};
    }

    catch (const std::exception& e) {
        std::cerr << "HTTP error (boost): " << e.what() << "\n";
        return {};
    }
}

std::string PerformHttpRequest(const HttpRequest& req) {
    return PerformHttpRequestWithResponse(req).body;
}
