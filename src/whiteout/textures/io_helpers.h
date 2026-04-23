// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file io_helpers.h
/// @brief Shared file I/O utilities for texture parsers and writers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

#include "../common/unicode_path.h"
#include "issue_sink.h"

namespace whiteout::textures {

/// Read an entire binary file into a byte vector.
/// Returns std::nullopt in lenient mode on failure, or throws in strict mode.
inline std::optional<std::vector<u8>> read_file_bytes(const std::string& path, IssueSink& sink) {
    auto file = ::whiteout::common::open_ifstream(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        sink.fail("Failed to open file: " + path);
        return std::nullopt;
    }

    const auto end_pos = file.tellg();
    if (end_pos < 0) {
        sink.fail("Failed to determine file size: " + path);
        return std::nullopt;
    }

    const auto size = static_cast<size_t>(end_pos);
    file.seekg(0, std::ios::beg);

    std::vector<u8> buf(size);
    if (size > 0 &&
        !file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        sink.fail("Failed to read file: " + path);
        return std::nullopt;
    }

    return buf;
}

/// Write a byte vector to a binary file.
/// Returns false in lenient mode on failure, or throws in strict mode.
inline bool write_file_bytes(const std::string& path, std::span<const u8> data, IssueSink& sink) {
    auto file = ::whiteout::common::open_ofstream(path, std::ios::binary);
    if (!file.is_open()) {
        sink.fail("Failed to open file for writing: " + path);
        return false;
    }

    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!file) {
        sink.fail("Failed to write file: " + path);
        return false;
    }

    return true;
}

} // namespace whiteout::textures
