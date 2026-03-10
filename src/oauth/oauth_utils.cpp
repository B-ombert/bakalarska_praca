#include "oauth_utils.h"
#include <boost/asio.hpp>

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

        std::istream request_stream(&buf);
        std::string request;
        std::getline(request_stream, request);

        std::string code;
        size_t pos = request.find("code=");
        if(pos != std::string::npos){
            size_t start = pos + 5;
            size_t end = request.find('&', start);
            if(end == std::string::npos) end = request.find(' ', start);
            code = request.substr(start, end - start);
        }

        std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n\r\n"
                "<html><body><h2>Login successful.</h2></body></html>";

        boost::asio::write(socket, boost::asio::buffer(response));

        std::cout << "Auth code captured: " << code << "\n";
        return code;

    }catch(std::exception& e){
        std::cerr << "Error: " << e.what() << "\n";
        return "";
    }
}

std::string ParseJsonTokenResponse(const std::string& tokenResponse){
    try{
        json j = json::parse(tokenResponse);
        if(j.contains("access_token")){
            return j["access_token"].get<std::string>();
        }
        else{
            std::cerr << "No access token in response";
            return "";
        }
    } catch(const std::exception& e){
        std::cerr << "JSON parse error: " << e.what() << "\n";
        return "";
    }
}

std::string WritePostDataForGoogle(const tokenRequestParameters& params){
    std::ostringstream postData;
    postData << "code=" << params.code
             << "&client_id=" << GOOGLE_CLIENT_ID
             << "&client_secret=" << GOOGLE_CLIENT_SECRET
             << "&redirect_uri=" << REDIRECT_URI
             << "&grant_type=authorization_code"
             << "&code_verifier=" << params.code_verifier;

    return postData.str();
}

std::string WritePostDataForMS(const tokenRequestParameters& params){
    std::ostringstream postData;
    postData << "client_id=" << MS_CLIENT_ID
             << "&grant_type=authorization_code"
             << "&code=" << params.code
             << "&redirect_uri=" << REDIRECT_URI
             << "&code_verifier=" << params.code_verifier
             << "&scope=offline_access%20Calendars.ReadWrite";

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

    req.headers = {
        "Content-Type: application/x-www-form-urlencoded"
    };

    return PerformHttpRequest(req);
}
