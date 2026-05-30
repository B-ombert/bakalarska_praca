#include "utils/timezone_utils.h"

#include <chrono>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

#include "utils/app_paths.h"
#include "utils/datetime_utils.h"
#include "utils/provider_utils.h"

namespace {

std::string FirstZoneToken(const std::string& zones) {
    const auto firstSpace = zones.find(' ');
    return firstSpace == std::string::npos ? zones : zones.substr(0, firstSpace);
}

struct TimeZoneMaps {
    std::unordered_map<std::string, std::string> windowsToIana;
    std::unordered_map<std::string, std::string> ianaToWindows;
};

TimeZoneMaps BuildMapsFromWindowsZonesXml() {
    TimeZoneMaps maps;

    std::ifstream input(GetWindowsZonesXmlPath());
    if (!input) {
        return maps;
    }

    std::ostringstream xmlBuffer;
    xmlBuffer << input.rdbuf();
    const std::string xml = xmlBuffer.str();

    const std::regex mapZonePattern(
        R"xml(<mapZone\s+other="([^"]+)"\s+territory="([^"]+)"\s+type="([^"]+)")xml");

    std::unordered_map<std::string, std::string> fallbackWindowsToIana;

    for (std::sregex_iterator it(xml.begin(), xml.end(), mapZonePattern), end; it != end; ++it) {
        const std::string windowsName = (*it)[1].str();
        const std::string territory = (*it)[2].str();
        const std::string typeList = (*it)[3].str();
        const std::string canonicalIana = FirstZoneToken(typeList);

        if (canonicalIana.empty()) {
            continue;
        }

        if (!fallbackWindowsToIana.contains(windowsName)) {
            fallbackWindowsToIana.emplace(windowsName, canonicalIana);
        }

        if (territory == "001") {
            maps.windowsToIana[windowsName] = canonicalIana;
        }
        else if (territory == "ZZ" && !maps.windowsToIana.contains(windowsName)) {
            maps.windowsToIana[windowsName] = canonicalIana;
        }

        size_t start = 0;
        while (start < typeList.size()) {
            const size_t endPos = typeList.find(' ', start);
            const std::string ianaName = typeList.substr(start, endPos == std::string::npos ? std::string::npos : endPos - start);
            if (!ianaName.empty() && !maps.ianaToWindows.contains(ianaName)) {
                maps.ianaToWindows.emplace(ianaName, windowsName);
            }
            if (endPos == std::string::npos) {
                break;
            }
            start = endPos + 1;
        }
    }

    for (const auto& [windowsName, ianaName] : fallbackWindowsToIana) {
        if (!maps.windowsToIana.contains(windowsName)) {
            maps.windowsToIana.emplace(windowsName, ianaName);
        }
    }

    return maps;
}

const TimeZoneMaps& GetTimeZoneMaps() {
    static const TimeZoneMaps maps = [] {
        return BuildMapsFromWindowsZonesXml();
    }();
    return maps;
}

const std::unordered_map<std::string, std::string>& WindowsToIanaMap() {
    return GetTimeZoneMaps().windowsToIana;
}

const std::unordered_map<std::string, std::string>& IanaToWindowsMap() {
    return GetTimeZoneMaps().ianaToWindows;
}

const std::chrono::time_zone* LocateTimeZoneCached(const std::string& timezone) {
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, const std::chrono::time_zone*> cache;

    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        if (const auto it = cache.find(timezone); it != cache.end()) {
            return it->second;
        }
    }

    const std::chrono::time_zone* zone = nullptr;
    try {
        zone = std::chrono::get_tzdb().locate_zone(timezone);
    }
    catch (const std::runtime_error&) {
        zone = nullptr;
    }

    const std::lock_guard<std::mutex> lock(cacheMutex);
    cache.emplace(timezone, zone);
    return zone;
}

} // namespace

std::string GetWindowsZonesXmlPath() {
    return GetBundledResourcePath("windowsZones.xml").string();
}

std::string GetCurrentLocalTimeZoneName() {
    static const std::string zoneName = [] {
        try {
            if (const auto* zone = std::chrono::current_zone(); zone != nullptr) {
                return std::string(zone->name());
            }
        }
        catch (const std::runtime_error&) {
        }

        return std::string("Etc/UTC");
    }();
    return zoneName;
}

std::string MicrosoftTimeZoneToIana(const std::string& timezone) {
    if (timezone.empty()) {
        return "";
    }

    if (const auto it = WindowsToIanaMap().find(timezone); it != WindowsToIanaMap().end()) {
        return it->second;
    }

    return timezone;
}

std::string IanaTimeZoneToMicrosoft(const std::string& timezone) {
    if (timezone.empty()) {
        return "";
    }

    if (const auto it = IanaToWindowsMap().find(timezone); it != IanaToWindowsMap().end()) {
        return it->second;
    }

    return timezone;
}

std::string NormalizeTimeZoneForProvider(const std::string& provider, const std::string& timezone) {
    if (provider == kProviderMicrosoft) {
        return MicrosoftTimeZoneToIana(timezone);
    }

    return timezone;
}

std::optional<int> GetUtcOffsetSeconds(const std::string& timezone, const long long utcEpoch) {
    if (timezone.empty()) {
        return std::nullopt;
    }

    try {
        using namespace std::chrono;

        const time_zone* zone = LocateTimeZoneCached(timezone);
        if (zone == nullptr) {
            return std::nullopt;
        }

        const sys_seconds timePoint{seconds{utcEpoch}};
        const sys_info info = zone->get_info(timePoint);
        return static_cast<int>(info.offset.count());
    }
    catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

long long ConvertUtcEpochToTimeZoneDisplayEpoch(const std::string& timezone, const long long utcEpoch) {
    const auto offset = GetUtcOffsetSeconds(timezone, utcEpoch);
    return offset.has_value() ? utcEpoch + offset.value() : utcEpoch;
}

std::optional<long long> ConvertTimeZoneDisplayEpochToUtcEpoch(const std::string& timezone, const long long displayEpoch) {
    if (timezone.empty()) {
        return std::nullopt;
    }

    try {
        using namespace std::chrono;

        const time_zone* zone = LocateTimeZoneCached(timezone);
        if (zone == nullptr) {
            return std::nullopt;
        }

        const std::tm tm = EpochToUtcTm(displayEpoch);
        const local_seconds localTime =
            local_days{year{tm.tm_year + 1900}/month{static_cast<unsigned>(tm.tm_mon + 1)}/day{static_cast<unsigned>(tm.tm_mday)}} +
            hours{tm.tm_hour} + minutes{tm.tm_min} + seconds{tm.tm_sec};

        const sys_seconds sysTime = floor<seconds>(zone->to_sys(localTime, choose::latest));
        return sysTime.time_since_epoch().count();
    }
    catch (const std::runtime_error&) {
        return std::nullopt;
    }
}
