// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file jenkins.h
/// @brief Jenkins hashlittle2 hash function.
///
/// Internal header — not part of the public include path.

#pragma once

#include <cstddef>
#include <string>

#include <whiteout/common_types.h>

namespace whiteout::storages::common {

/// Result of a Jenkins hashlittle2 call.
struct JenkinsHash {
    u32 pc; ///< Primary hash.
    u32 pb; ///< Secondary hash.
};

/// Bob Jenkins' lookup3 hashlittle2 — produces two 32-bit hash values.
/// @param key   Pointer to the key data.
/// @param length  Length of the key in bytes.
/// @param pc    Initial value / seed for primary hash (modified in-place).
/// @param pb    Initial value / seed for secondary hash (modified in-place).
void jenkinsHashlittle2(const void* key, size_t length, u32& pc, u32& pb);

/// Normalize a filename for MPQ/CASC hashing: uppercase ASCII + '/' → '\\'.
/// @param name  The filename to normalize.
/// @return The normalized filename.
std::string normalizePath(const std::string& name);

/// Convenience: normalize the filename and compute Jenkins hashlittle2.
/// Seeds are initialized to 0.
/// @param filename  Path to hash (will be normalized internally).
/// @return The two 32-bit hash values.
JenkinsHash jenkinsHash(const std::string& filename);

} // namespace whiteout::storages::common
