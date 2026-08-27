// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file sha1.h
/// @brief Standalone SHA-1 hash implementation (FIPS 180-1).
///
/// Internal header — not part of the public include path.

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// Incremental SHA-1 hasher.
class SHA1 {
public:
    SHA1();

    /// Feed data into the hash.
    void update(const void* data, size_t length);

    /// Feed a span of bytes.
    void update(std::span<const u8> data) {
        update(data.data(), data.size());
    }

    /// Finalize and return the 20-byte digest.
    /// After calling finalize(), the hasher should not be reused.
    std::array<u8, 20> finalize();

private:
    void processBlock(const u8* block);

    u32 m_state[5];
    u64 m_totalLen = 0;
    u8 m_buffer[64]{};
    size_t m_bufferLen = 0;
};

/// Convenience: compute SHA-1 of a byte span in one call.
inline std::array<u8, 20> sha1Hash(std::span<const u8> data) {
    SHA1 hasher;
    hasher.update(data);
    return hasher.finalize();
}

} // namespace whiteout::storages::common
