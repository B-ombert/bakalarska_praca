#include "utils/provider_utils.h"

bool IsLocalProvider(const std::string& provider) {
    return provider == kProviderLocal;
}

bool IsRemoteProvider(const std::string& provider) {
    return !IsLocalProvider(provider);
}

bool IsCalendarSyncProvider(const std::string& provider) {
    return provider == kProviderGoogle || provider == kProviderMicrosoft;
}

std::optional<Platform> PlatformForProvider(const std::string& provider) {
    if (provider == kProviderGoogle) {
        return GOOGLE;
    }
    if (provider == kProviderMicrosoft) {
        return MICROSOFT;
    }
    return std::nullopt;
}

std::string ProviderForPlatform(const Platform platform) {
    switch (platform) {
        case GOOGLE:
            return kProviderGoogle;
        case MICROSOFT:
            return kProviderMicrosoft;
    }

    return {};
}

std::string ProviderLabel(const std::string& provider) {
    if (provider == kProviderGoogle) {
        return "Google";
    }
    if (provider == kProviderMicrosoft) {
        return "Outlook";
    }
    if (provider == kProviderLocal) {
        return "Local";
    }
    return provider;
}

void ApplyProviderCalendarDisplayDefaults(const Account& account, std::vector<Calendar>& calendars) {
    if (account.provider != kProviderGoogle || account.name.empty()) {
        return;
    }

    for (auto& calendar : calendars) {
        if (calendar.isPrimary) {
            calendar.name = account.name;
        }
    }
}
