// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file output_reserve.h
/// @brief How much output a decoder sets aside before it decodes.

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout::common {

/// The output to set aside for @p inputSize bytes that claim to decode to
/// @p claimed: the claim, unless it is beyond what the input plausibly
/// holds. An archive's tables can claim gigabytes for a few bytes; past the
/// cap the output grows only as fast as it really decodes.
[[nodiscard]] inline size_t outputReserve(size_t claimed, size_t inputSize) {
    constexpr size_t kPlausibleRatio = 64;
    constexpr size_t kAlwaysFine = size_t{64} << 10;
    return std::min(claimed, std::max(kAlwaysFine, inputSize * kPlausibleRatio));
}

/// Whether @p inputSize bytes may be given all of @p claimed up front.
[[nodiscard]] inline bool plausibleOutput(size_t claimed, size_t inputSize) {
    return outputReserve(claimed, inputSize) == claimed;
}

/// Runs a decoder that writes into a caller's buffer for a caller that wants
/// a vector. @p into(dst) returns the bytes it wrote, stopping where @p dst
/// ends; 0 means the stream is bad. The buffer starts at outputReserve() and
/// doubles while the decoder fills it, so a claim the input does not carry
/// sets aside only about twice what really decodes.
template <class Into>
[[nodiscard]] std::vector<u8> decodeGrowing(size_t expectedSize, size_t inputSize, Into into) {
    std::vector<u8> out;
    size_t capacity = outputReserve(expectedSize, inputSize);
    for (;;) {
        out.resize(capacity);
        size_t const written = into(std::span<u8>(out));
        if (written < capacity || capacity >= expectedSize) {
            out.resize(written);
            return out;
        }
        capacity = std::min(expectedSize, capacity * 2);
    }
}

} // namespace whiteout::common
