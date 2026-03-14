// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/textures/texture.h>
#include "../compatibility.h"

namespace whiteout::textures {

class Writer {
public:
    virtual ~Writer() = default;

    virtual void write(const std::string& filePath, const Texture& texture) = 0;
    virtual std::vector<u8> write(const Texture& texture) = 0;
};

} // namespace whiteout::textures
