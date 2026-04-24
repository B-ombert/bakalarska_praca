#pragma once

#include <string>
#include <vector>

enum class Frequency { DAILY, WEEKLY, MONTHLY, YEARLY, UNKNOWN };

struct RRule {
    Frequency freq = Frequency::UNKNOWN;
    int interval = 1;

    int count = 0;
    long long until = 0;

    std::vector<int> byDay;
    std::vector<int> byMonthDay;

    int weekStart = 1;
    bool hasCount = false;
    bool hasUntil = false;

    RRule parseRRule(const std::string& rule);
    std::string toGoogleRRule();
    std::string toOutlookRRule();
};
