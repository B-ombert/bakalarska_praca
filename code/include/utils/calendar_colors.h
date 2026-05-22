#pragma once

#include <array>
#include <cstddef>
#include <random>
#include <string>
#include <utility>

inline const std::array<std::pair<const char*, const char*>, 8>& CalendarColorPalette() {
    static const std::array<std::pair<const char*, const char*>, 8> colors = {{
        {"Blue", "#1A73E8"},
        {"Teal", "#00897B"},
        {"Green", "#188038"},
        {"Orange", "#F4511E"},
        {"Red", "#D93025"},
        {"Indigo", "#3F51B5"},
        {"Pink", "#D81B60"},
        {"Purple", "#8E24AA"},
    }};
    return colors;
}

inline std::string DefaultCalendarColor() {
    return CalendarColorPalette().front().second;
}

inline int CalendarColorIndex(const std::string& colorHex) {
    const auto& colors = CalendarColorPalette();
    for (size_t i = 0; i < colors.size(); ++i) {
        if (colorHex == colors[i].second) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

inline std::string NormalizeCalendarColor(const std::string& colorHex) {
    const auto& colors = CalendarColorPalette();
    for (const auto& [_, hex] : colors) {
        if (colorHex == hex) {
            return hex;
        }
    }
    return DefaultCalendarColor();
}

inline std::string RandomCalendarColor() {
    static std::random_device device;
    static std::mt19937 generator(device());
    std::uniform_int_distribution<size_t> distribution(0, CalendarColorPalette().size() - 1);
    return CalendarColorPalette()[distribution(generator)].second;
}
