#ifndef APP_PATHS_H
#define APP_PATHS_H

#include <filesystem>
#include <string>

std::filesystem::path GetExecutableDirectory();
std::filesystem::path GetApplicationDataDirectory();
std::filesystem::path GetApplicationDatabasePath();
std::filesystem::path GetBundledResourcePath(const std::string& relativePath);

#endif // APP_PATHS_H
