// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Shared test utilities for WhiteoutLib tests (Catch2-based).

#pragma once

#include <whiteout/common_types.h>

#include <cstring>
#include <filesystem>
#include <span>
#include <string>

namespace whiteout::test {

/// Read a typed value from a byte buffer at the given offset.
template <typename T>
inline T readAt(const u8* data, size_t offset) {
    T val;
    std::memcpy(&val, data + offset, sizeof(T));
    return val;
}

/// Compare two byte spans for equality.
inline bool spanEqual(std::span<const u8> a, std::span<const u8> b) {
    if (a.size() != b.size())
        return false;
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

/// Search common candidate paths for a corpus directory.
inline std::string findCorpusBase(const char* subdir = "Corpus") {
    namespace fs = std::filesystem;
    for (auto candidate : {subdir,
                           (std::string("../") + subdir).c_str(),
                           (std::string("../../") + subdir).c_str(),
                           (std::string("C:/Projects/WhiteoutLib/") + subdir).c_str()}) {
        if (fs::exists(candidate))
            return candidate;
    }
    return {};
}

} // namespace whiteout::test
