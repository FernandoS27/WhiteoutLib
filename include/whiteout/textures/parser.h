// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include "../compatibility.h"

namespace whiteout::textures {

class Parser {
public:
    virtual ~Parser() = default;

    virtual std::optional<Texture> parse(std::span<const u8> buffer) = 0;
    virtual std::optional<Texture> parse(const std::string& filePath) = 0;
};

} // namespace whiteout::textures