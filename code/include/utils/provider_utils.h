#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models/account.h"
#include "models/calendar.h"
#include "utils/types.h"

inline constexpr const char* kProviderLocal = "LOCAL";
inline constexpr const char* kProviderGoogle = "GOOGLE";
inline constexpr const char* kProviderMicrosoft = "MICROSOFT";

bool IsLocalProvider(const std::string& provider);
bool IsRemoteProvider(const std::string& provider);
bool IsCalendarSyncProvider(const std::string& provider);

std::optional<Platform> PlatformForProvider(const std::string& provider);
std::string ProviderForPlatform(Platform platform);
std::string ProviderLabel(const std::string& provider);

void ApplyProviderCalendarDisplayDefaults(const Account& account, std::vector<Calendar>& calendars);
