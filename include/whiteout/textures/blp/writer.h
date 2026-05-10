// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief BLP file writer
 *
 * This file provides the Writer class for encoding Texture objects into
 * BLP1 or BLP2 binary format.
 *
 * @example Basic writing
 * @code
 * blp::Writer writer(blp::Writer::WriteMode::Lenient);
 * auto bytes = writer.write(texture, opts);
 *
 * if (writer.hasIssues()) {
 *     for (const auto& issue : writer.getIssues()) {
 *         std::cout << "Warning: " << issue << std::endl;
 *     }
 * }
 * @endcode
 */

#include <memory>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/textures/blp/types.h>
#include <whiteout/textures/texture.h>
#include <whiteout/textures/writer.h>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::textures::blp {

// ============================================================================
// BLP Writer
// ============================================================================

/**
 * @brief Writer for BLP texture files
 *
 * The Writer takes a Texture and encodes it into BLP1 or BLP2 binary format.
 * It supports palettized, JPEG, DXT, and BGRA encodings.
 *
 * Uses the PImpl (Pointer to Implementation) idiom to hide implementation details.
 */
/// @bind methods=buffer_only, js_name=BlpWriter
class Writer : public textures::Writer {
public:
    /**
     * @brief Writing strictness mode
     */
    /// @bind js_name=BlpWriteMode
    enum class WriteMode {
        Strict, ///< Throw exceptions on encoding errors
        Lenient ///< Log issues and return empty result on errors (recommended)
    };

    /**
     * @brief Construct a new Writer
     * @param writeMode Strictness mode for writing
     * @param pool Optional WorkerPool for parallel quantization (nullptr = serial)
     */
    explicit Writer(WriteMode writeMode = WriteMode::Lenient,
                    interfaces::WorkerPool* pool = nullptr);

    /// @brief Destructor (defined in .cpp for incomplete type)
    ~Writer();

    void write(const std::string& filePath, const Texture& texture) override;

    /**
     * @brief Write a BLP file to a byte buffer with default options
     */
    std::vector<u8> write(const Texture& texture) override;

    /**
     * @brief Write a BLP file to disk
     * @param filePath Path where the BLP file should be written
     * @param texture Texture data to encode
     * @param opts Encoding options (version, encoding, alpha, quality)
     * @throws std::runtime_error If file cannot be created, or encoding fails in strict mode
     */
    void write(const std::string& filePath, const Texture& texture, const SaveOptions& opts);

    /**
     * @brief Write a BLP file to a byte buffer
     * @param texture Texture data to encode
     * @param opts Encoding options (version, encoding, alpha, quality)
     * @return Vector containing the binary BLP data, or empty on failure (in Lenient mode)
     * @throws std::runtime_error If encoding fails in strict mode
     */
    std::vector<u8> write(const Texture& texture, const SaveOptions& opts);

    /**
     * @brief Check if writing encountered any issues
     * @return True if there were warnings or recoverable errors
     */
    bool hasIssues() const;

    /**
     * @brief Get list of issues encountered during writing
     * @return Vector of issue description strings
     */
    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace whiteout::textures::blp
