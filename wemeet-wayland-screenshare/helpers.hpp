#pragma once


#include <string>
#include <sstream>

// Inline function to colorize text
inline std::string color_text(const std::string& text, const std::string& color_code) {
    return "\033[" + color_code + "m" + text + "\033[0m";
}

// Helper functions for specific colors
inline std::string red_text(const std::string& text) {
    return color_text(text, "31");
}

inline std::string green_text(const std::string& text) {
    return color_text(text, "32");
}

inline std::string yellow_text(const std::string& text) {
    return color_text(text, "33");
}

inline std::string int_to_hexstr(int value) {
    std::stringstream ss;
    ss << std::hex << value;
    return ss.str();
}

inline std::string toLowerString(const std::string& str) {
    std::string lower_str = str;
    for (char& c : lower_str) {
        c = std::tolower(c);
    }
    return lower_str;
}