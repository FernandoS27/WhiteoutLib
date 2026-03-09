// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief M2 file writer
 *
 * This file provides the Writer class for writing M2 model files back to disk.
 * The writer converts in-memory FileSystem structures to binary M2 format,
 * producing the base .m2, .skin, and .skel files as needed.
 *
 * Chunks are emitted in a fixed deterministic order (MD21 -> LDV1 -> PFID -> SFID -> ...),
 * back-to-back with no inter-chunk alignment padding. The writer pre-allocates a 2 MB
 * buffer and shrinks to fit after serialization.
 *
 * @note The writer currently does not emit .bone or .anim files; only the base .m2,
 *       skin (.skin), and skeleton (.skel) files are written.
 *
 * @example Basic writing
 * @code
 * whiteout::m2::FileSystem model;
 * // ... populate model data ...
 *
 * whiteout::m2::Writer writer;
 * writer.write("output/model.m2", model);
 * @endcode
 */

#include <cstdint>
#include <string>
#include "structures.h"

namespace whiteout {
namespace common {
class BinaryWriter;
}

namespace m2 {

// Use BinaryWriter from Common namespace
using common::BinaryWriter;

// ============================================================================
// M2 Writer
// ============================================================================

/**
 * @brief Writer for M2 model files
 *
 * The Writer takes a FileSystem structure and writes it to disk in binary M2 format.
 * It automatically handles chunk serialization, size patching, and multi-file output
 * (base .m2, .skin profiles, .skel skeleton).
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
class Writer {
public:
    /// @brief Construct a new Writer
    explicit Writer();

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Writer();

    /**
     * @brief Write an M2 file bundle to disk
     *
     * Writes the base .m2 file and all associated .skin files (and .skel if present)
     * to the specified output path. File naming follows M2 bundle conventions:
     * {stem}.m2, {stem}00.skin, {stem}_lod01.skin, {stem}.skel, etc.
     *
     * @param filePath Path where the main .m2 file should be written
     * @param model Complete FileSystem containing all model data
     * @throws std::runtime_error If file cannot be created or written
     */
    void write(const std::string& filePath, const FileSystem& model);

    /**
     * @brief Write a base M2 file to a byte buffer
     * @param model BaseFile data to serialize
     * @return Vector containing the binary M2 data (MD21 chunked format)
     */
    std::vector<uint8_t> write(const BaseFile& model);

    /**
     * @brief Write a skin file to a byte buffer
     * @param model SkinFile data to serialize
     * @return Vector containing the binary .skin data
     */
    std::vector<uint8_t> write(const SkinFile& model);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace m2
} // namespace whiteout
