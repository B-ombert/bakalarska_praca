#pragma once
#include <iostream>
#include <vector>

enum Platform { GOOGLE, MICROSOFT};

enum Verb {POST, PATCH, GET, DELETE_};

enum SyncStatus {SYNCED, PENDING_INSERT, PENDING_UPDATE, PENDING_DELETE};

enum class EventType {SINGLE, MASTER, OCCURRENCE, EXCEPTION, CANCELLED_INSTANCE};

enum class RecurrenceOverrideType {CANCELLED = 0, MODIFIED = 1};

struct tokenRequestParameters{
    std::string code;
    std::string code_verifier;
    std::string redirect_uri;
};

struct HttpRequest {
    std::string url;
    std::vector<std::string> headers;
    std::string postData;
    int verb = GET;
};

struct HttpResponse {
    int statusCode = 0;
    std::string body;
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};
