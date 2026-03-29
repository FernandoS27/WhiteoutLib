// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file deflate.h
/// @brief Forwarding header — the DEFLATE codec now lives in common/deflate.h.

#pragma once

#include "../../common/deflate.h"

namespace whiteout::textures::png {

using ::whiteout::zlib_compress;
using ::whiteout::zlib_decompress;

} // namespace whiteout::textures::png
