#include "utils/url_parser.h"
#include <string>

ParsedUrl ParseUrl(const std::string& url) {
    ParsedUrl p;

    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        throw std::invalid_argument("Invalid URL: missing scheme");
    }

    p.scheme = url.substr(0, schemeEnd);

    const auto authorityStart = schemeEnd + 3;
    const auto pathStart = url.find_first_of("/?", authorityStart);
    const std::string authority = (pathStart == std::string::npos)
        ? url.substr(authorityStart)
        : url.substr(authorityStart, pathStart - authorityStart);

    const auto portSeparator = authority.rfind(':');
    if (portSeparator != std::string::npos && authority.find(']') == std::string::npos) {
        p.host = authority.substr(0, portSeparator);
        p.port = authority.substr(portSeparator + 1);
    }
    else {
        p.host = authority;
        p.port = (p.scheme == "https") ? "443" : "80";
    }

    if (pathStart == std::string::npos) {
        p.target = "/";
    }
    else {
        p.target = url.substr(pathStart);
    }

    return p;
}
