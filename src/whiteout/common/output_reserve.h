// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file output_reserve.h
/// @brief How much output a decoder sets aside before it decodes.

#pragma once

#include <algorithm>
#include <cstddef>

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

} // namespace whiteout::common
