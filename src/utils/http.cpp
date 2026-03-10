#include "http.h"
#include "url_parser.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <iostream>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp = net::ip::tcp;

std::string PerformHttpRequest(const HttpRequest& req) {
    try {
        auto p = ParseUrl(req.url);

        net::io_context ioc;
        net::ssl::context ssl(net::ssl::context::tlsv12_client);
        ssl.set_default_verify_paths();

        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl);

        auto results = resolver.resolve(tcp::v4(),p.host, p.port);

        beast::get_lowest_layer(stream).connect(results);

        stream.handshake(net::ssl::stream_base::client);

        http::request<http::string_body> request;
        request.version(11);
        request.target(p.target);
        request.set(http::field::host, p.host);
        request.set(http::field::user_agent, "CalendarApp/1.0");
        request.set(http::field::connection, "close");

        for (const auto& h : req.headers) {
            auto pos = h.find(':');
            if (pos != std::string::npos) {
                request.set(
                    h.substr(0, pos),
                    h.substr(pos + 1)
                    );
            }
        }

        if (!req.postData.empty()) {
            request.method(http::verb::post);
            request.body() = req.postData;
            request.prepare_payload();
        }
        else {
            request.method(http::verb::get);
        }

        http::write(stream, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;

        http::read(stream, buffer, response);

        beast::error_code ec;
        stream.shutdown(ec);

        return response.body();
    }

    catch (const std::exception& e) {
        std::cerr << "HTTP error (boost): " << e.what() << "\n";
        return "";
    }
}