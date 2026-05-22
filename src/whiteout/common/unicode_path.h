// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Internal helper: open std::ifstream / std::ofstream from a UTF-8 encoded
// path string.  On POSIX the path is passed through unchanged since the OS
// already expects UTF-8.  On Windows the string is converted to UTF-16 (via
// MultiByteToWideChar(CP_UTF8)) and wrapped in std::filesystem::path, which
// portably accepts wide strings on Windows across MSVC and libstdc++ (MinGW).
//
// This header is INTERNAL — never include it from public (include/) headers.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

namespace whiteout::common {

/// Build a std::filesystem::path from a UTF-8-encoded string without going
/// through the deprecated std::filesystem::u8path (removed in C++26).
inline std::filesystem::path utf8_to_path(const std::string& utf8) {
    const auto* begin = reinterpret_cast<const char8_t*>(utf8.data());
    return std::filesystem::path(begin, begin + utf8.size());
}

} // namespace whiteout::common

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace whiteout::common {

/// Convert a UTF-8 string to a UTF-16 wide string for use with Win32 / MSVC
/// stream constructors.
inline std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty())
        return {};
    const int wlen =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0)
        return {};
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                          wlen);
    return wide;
}

/// Open a std::ifstream from a UTF-8 path. Routes through
/// std::filesystem::path(std::wstring) so non-ASCII paths work under both
/// MSVC and MinGW's libstdc++ (which lacks MSVC's wstring stream overload).
inline std::ifstream open_ifstream(const std::string& utf8Path,
                                   std::ios::openmode mode = std::ios::in) {
    return std::ifstream(std::filesystem::path(utf8_to_wide(utf8Path)), mode);
}

inline std::ofstream open_ofstream(const std::string& utf8Path,
                                   std::ios::openmode mode = std::ios::out) {
    return std::ofstream(std::filesystem::path(utf8_to_wide(utf8Path)), mode);
}

} // namespace whiteout::common

#else // !_WIN32

namespace whiteout::common {

/// On POSIX the path is already UTF-8; open directly.
inline std::ifstream open_ifstream(const std::string& utf8Path,
                                   std::ios::openmode mode = std::ios::in) {
    return std::ifstream(utf8Path, mode);
}

/// On POSIX the path is already UTF-8; open directly.
inline std::ofstream open_ofstream(const std::string& utf8Path,
                                   std::ios::openmode mode = std::ios::out) {
    return std::ofstream(utf8Path, mode);
}

} // namespace whiteout::common

#endif // _WIN32
