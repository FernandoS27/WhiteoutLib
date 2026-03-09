// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file io_helpers.h
/// @brief Shared file I/O utilities for texture parsers and writers.
///
/// Internal header — not part of the public include path.

#pragma once

#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::textures {

/// Read an entire binary file into a byte vector.
inline std::vector<u8> read_file_bytes(const std::string& path) {
    std::ifstream file;
    file.open(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    const auto size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    std::vector<u8> buf(size);
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Failed to read file: " + path);
    }
    return buf;
}

/// Write a byte vector to a binary file.
inline void write_file_bytes(const std::string& path, std::span<const u8> data) {
    std::ofstream file;
    file.open(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + path);
    }
    file.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
    if (!file) {
        throw std::runtime_error("Failed to write file: " + path);
    }
}

} // namespace whiteout::textures
