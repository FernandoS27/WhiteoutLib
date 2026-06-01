// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bit_reader.h
/// @brief Forwarding shim — `extractBits` now lives in common/bit_reader.h.

#pragma once

#include "../../common/bit_reader.h"

namespace whiteout::storages::common {

using ::whiteout::common::extractBits;

} // namespace whiteout::storages::common
