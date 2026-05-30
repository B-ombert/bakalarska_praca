#pragma once

#include <string>

#include "models/event.h"

Event ParseTextEventInput(const std::string& input);
Event ParseTextEventInput(const std::string& input, long long referenceEpoch, const std::string& timezone);
