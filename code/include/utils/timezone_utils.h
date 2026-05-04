#pragma once

#include <optional>
#include <string>

std::string GetWindowsZonesXmlPath();
std::string GetCurrentLocalTimeZoneName();
std::string MicrosoftTimeZoneToIana(const std::string& timezone);
std::string IanaTimeZoneToMicrosoft(const std::string& timezone);
std::string NormalizeTimeZoneForProvider(const std::string& provider, const std::string& timezone);
std::optional<int> GetUtcOffsetSeconds(const std::string& timezone, long long utcEpoch);
long long ConvertUtcEpochToTimeZoneDisplayEpoch(const std::string& timezone, long long utcEpoch);
std::optional<long long> ConvertTimeZoneDisplayEpochToUtcEpoch(const std::string& timezone, long long displayEpoch);
