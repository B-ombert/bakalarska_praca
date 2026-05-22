#include "utils/app_paths.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <limits.h>
#include <unistd.h>
#include <vector>
#endif

namespace {

constexpr const char* kAppDirectoryName = "LocalCalendar";

std::filesystem::path PathFromEnvironment(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr && value[0] != '\0') {
        return std::filesystem::path(value);
    }
    return {};
}

void EnsureDirectoryExists(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
}

} // namespace

std::filesystem::path GetExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (length == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (length > 0) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        std::error_code error;
        return std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), error).parent_path();
    }
#else
    std::vector<char> buffer(PATH_MAX);
    ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data()).parent_path();
    }
#endif

    return std::filesystem::current_path();
}

std::filesystem::path GetApplicationDataDirectory() {
#if defined(_WIN32)
    std::filesystem::path base = PathFromEnvironment("LOCALAPPDATA");
    if (base.empty()) {
        base = PathFromEnvironment("APPDATA");
    }
    if (base.empty()) {
        base = GetExecutableDirectory();
    }
    std::filesystem::path appData = base / kAppDirectoryName;
#elif defined(__APPLE__)
    std::filesystem::path home = PathFromEnvironment("HOME");
    std::filesystem::path appData = home.empty()
        ? GetExecutableDirectory()
        : home / "Library" / "Application Support" / kAppDirectoryName;
#else
    std::filesystem::path base = PathFromEnvironment("XDG_DATA_HOME");
    if (base.empty()) {
        std::filesystem::path home = PathFromEnvironment("HOME");
        base = home.empty() ? GetExecutableDirectory() : home / ".local" / "share";
    }
    std::filesystem::path appData = base / kAppDirectoryName;
#endif

    EnsureDirectoryExists(appData);
    return appData;
}

std::filesystem::path GetApplicationDatabasePath() {
    return GetApplicationDataDirectory() / "calendar_app.db";
}

std::filesystem::path GetBundledResourcePath(const std::string& relativePath) {
    return GetExecutableDirectory() / "resources" / relativePath;
}
