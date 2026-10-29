// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// What the mesh-removal and mesh-merge sources share.

#include <string>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace meshdetail {

inline std::string number(u64 value) {
    return std::to_string(value);
}

} // namespace meshdetail
} // namespace wem
} // namespace models
} // namespace whiteout
