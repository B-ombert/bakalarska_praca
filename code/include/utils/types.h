#pragma once
#include <iostream>
#include <vector>

enum Platform { GOOGLE, MICROSOFT};

struct simpleEvent {
    std::string summary;
    std::string start;
    std::string end;
};

struct tokenRequestParameters{
    std::string code;
    std::string code_verifier;
};

struct HttpRequest {
    std::string url;
    std::vector<std::string> headers;
    std::string postData;
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};