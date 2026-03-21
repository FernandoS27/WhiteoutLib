// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief M3 file writer
 *
 * This file provides the Writer class for serializing Model structures into
 * binary M3 (MD34) files. The writer produces correctly aligned, version-aware
 * output compatible with StarCraft II and Heroes of the Storm.
 *
 * @example Basic writing
 * @code
 * m3::Writer writer;
 * writer.write("output.m3", model);
 *
 * // Or write to a memory buffer
 * std::vector<u8> buffer = writer.write(model);
 * @endcode
 */

#include <cstdint>
#include <string>
#include <vector>
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace m3 {

/**
 * @brief Writer for M3 model files
 *
 * Writes Model structures to disk in binary M3 format.
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Writer {
public:
    /// @brief Construct a new Writer
    explicit Writer();

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Writer();

    /**
     * @brief Write an M3 model to a file on disk
     * @param filePath Output file path
     * @param model Model data to serialize
     * @throws std::runtime_error If file cannot be created or writing fails
     */
    void write(const std::string& filePath, const Model& model);

    /**
     * @brief Write an M3 model to a byte buffer
     * @param model Model data to serialize
     * @return Byte buffer containing the M3 file data
     */
    std::vector<u8> write(const Model& model);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace m3
} // namespace whiteout
