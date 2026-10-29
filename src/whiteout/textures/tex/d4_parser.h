// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <span>

#include <whiteout/common_types.h>
#include <whiteout/textures/tex/parser.h>
#include <whiteout/textures/texture.h>

#include "../issue_sink.h"

namespace whiteout::textures::tex {

// Defined in d4_parser.cpp
std::optional<Texture> parseD4Impl(std::span<const u8> texData, std::span<const u8> payloadData,
                                   std::span<const u8> lowResPayloadData, D4TexInfo* outInfo,
                                   IssueSink& sink);

} // namespace whiteout::textures::tex
