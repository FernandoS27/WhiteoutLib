// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file byte_order.h
/// @brief Forwarding shim — the byte-order helpers now live in common/byte_order.h.
///
/// Kept for backwards compatibility with existing storages includes;
/// new code should include `<whiteout/common/byte_order.h>` directly.

#pragma once

#include "../../common/byte_order.h"

namespace whiteout::storages::common {

using ::whiteout::common::readBE16;
using ::whiteout::common::readBE32;
using ::whiteout::common::readBE40;
using ::whiteout::common::readBEVar;
using ::whiteout::common::readLE16;
using ::whiteout::common::readLE32;
using ::whiteout::common::readLE64;
using ::whiteout::common::readLEi32;
using ::whiteout::common::writeBE16;
using ::whiteout::common::writeBE32;
using ::whiteout::common::writeBE40;
using ::whiteout::common::writeLE16;
using ::whiteout::common::writeLE32;
using ::whiteout::common::pushBE16;
using ::whiteout::common::pushBE32;
using ::whiteout::common::pushLE16;
using ::whiteout::common::pushLE32;

} // namespace whiteout::storages::common
