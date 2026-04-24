#pragma once

#include <ctime>
#include <string>

long long MakeUtcEpoch(int year, int month, int day, int hour = 0, int minute = 0, int second = 0);
std::tm EpochToUtcTm(long long epoch);
long long StartOfUtcDay(long long epoch);

long long ParseUtcDateTimeInput(const std::string& value, bool allDay);
std::string FormatUtcDateTimeInput(long long epoch, bool allDay);

long long iso8601ToEpoch(const std::string& iso);
long long iso8601ToEpochDate(const std::string& iso);
std::string epochToIso(long long epoch);
std::string epochToIsoDate(long long epoch);
