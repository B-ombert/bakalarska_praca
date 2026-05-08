#pragma once
#include "types.h"

HttpResponse PerformHttpRequestWithResponse(const HttpRequest& req);
std::string PerformHttpRequest(const HttpRequest& req);
