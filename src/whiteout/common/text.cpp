// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "text.h"

#include <cerrno>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace whiteout {
namespace text {

namespace {

/// The locale's radix put back to `.`.
void FixRadix(char* buffer) {
    const char* const radix = std::localeconv()->decimal_point;
    if (radix == nullptr || std::strcmp(radix, ".") == 0) {
        return;
    }
    const std::size_t width = std::strlen(radix);
    char* found = std::strstr(buffer, radix);
    if (found == nullptr) {
        return;
    }
    *found = '.';
    if (width > 1) {
        std::memmove(found + 1, found + width, std::strlen(found + width) + 1);
    }
}

void AppendFormatted(std::string& out, f64 value, const char* format) {
    if (!std::isfinite(value)) {
        out += '0';
        return;
    }
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), format, value);
    FixRadix(buffer);
    out += buffer;
}

} // namespace

bool ParseF64(std::string_view token, f64& out) {
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
    }
    if (token.empty()) {
        return false;
    }
#if defined(__cpp_lib_to_chars)
    const std::from_chars_result result =
        std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc() && result.ptr == token.data() + token.size();
#else
    // libc++ before LLVM 20 has no floating from_chars; strtod honours
    // LC_NUMERIC, so the radix is substituted first.
    std::string scratch(token);
    const char* const radix = std::localeconv()->decimal_point;
    if (radix != nullptr && std::strcmp(radix, ".") != 0) {
        const std::size_t dot = scratch.find('.');
        if (dot != std::string::npos) {
            scratch.replace(dot, 1, radix);
        }
    }
    char* end = nullptr;
    errno = 0;
    out = std::strtod(scratch.c_str(), &end);
    return end == scratch.c_str() + scratch.size() &&
           !(errno == ERANGE && (out == 0.0 || std::isinf(out)));
#endif
}

bool ParseF32(std::string_view token, f32& out) {
    f64 wide = 0.0;
    if (!ParseF64(token, wide)) {
        return false;
    }
    out = static_cast<f32>(wide);
    return true;
}

bool ParseI64(std::string_view token, i64& out) {
    if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
    }
    if (token.empty()) {
        return false;
    }
    const std::from_chars_result result =
        std::from_chars(token.data(), token.data() + token.size(), out);
    return result.ec == std::errc() && result.ptr == token.data() + token.size();
}

void AppendF32(std::string& out, f32 value) {
    AppendFormatted(out, static_cast<f64>(value), "%.9g");
}

void AppendF64(std::string& out, f64 value) {
    AppendFormatted(out, value, "%.17g");
}

} // namespace text
} // namespace whiteout
