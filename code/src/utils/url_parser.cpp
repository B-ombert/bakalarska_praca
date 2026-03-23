#include "url_parser.h"
#include <string>

ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl p;

    auto schemeEnd = url.find("://");
    p.scheme = url.substr(0, schemeEnd);

    auto hostStart = schemeEnd + 3;
    auto pathStart = url.find("/", hostStart);

    p.host = url.substr(hostStart, pathStart - hostStart);
    p.target = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
    p.port = (p.scheme == "https") ? "443" : "80";

    return p;
}