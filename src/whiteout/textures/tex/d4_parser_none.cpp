// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file d4_parser_none.cpp
/// @brief `parseD4Impl` for a build without the Diablo IV reader
/// (`WHITEOUT_TEXTURES_D4=OFF`): every Diablo IV `.tex` fails to parse, saying
/// why, and nothing here needs the SNO tables `d4_parser.cpp` reads.

#include "d4_parser.h"

namespace whiteout::textures::tex {

std::optional<Texture> parseD4Impl(std::span<const u8>, std::span<const u8>, std::span<const u8>, D4TexInfo*,
                                   IssueSink& sink) {
    sink.fail("Diablo IV TEX support is not built (WHITEOUT_TEXTURES_D4=OFF)");
    return std::nullopt;
}

} // namespace whiteout::textures::tex
