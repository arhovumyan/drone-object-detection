#pragma once
// OS detection: compile-time constant + runtime host description.
// The build system (CMakeLists.txt) also selects the camera/display backend
// per platform; this header reports what we are running on.

#include <string>
#include <fstream>

namespace dd {

enum class OS { Windows, Linux, Other };

#if defined(_WIN32)
inline constexpr OS kHostOS = OS::Windows;
#elif defined(__linux__)
inline constexpr OS kHostOS = OS::Linux;
#else
inline constexpr OS kHostOS = OS::Other;
#endif

inline const char* OSName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#else
    return "Other";
#endif
}

// True only when actually running on an NVIDIA Jetson board.
inline bool RuntimeIsJetson() {
#if defined(__linux__)
    std::ifstream f("/proc/device-tree/model");
    if (f) {
        std::string s;
        std::getline(f, s);
        return s.find("Jetson") != std::string::npos ||
               s.find("NVIDIA") != std::string::npos;
    }
#endif
    return false;
}

inline std::string HostDescription() {
#if defined(_WIN32)
    return "Windows (x64)";
#elif defined(__linux__)
    std::ifstream f("/proc/device-tree/model");
    if (f) {
        std::string s;
        std::getline(f, s);
        if (!s.empty()) return "Linux / " + s;
    }
    return "Linux (generic)";
#else
    return "Unknown OS";
#endif
}

} // namespace dd
