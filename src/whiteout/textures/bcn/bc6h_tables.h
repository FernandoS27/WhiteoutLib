// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc6h_tables.h
/// @brief BC6H partition and anchor-index tables (spec-defined).
///
/// Extracted from bcn_common.h so that only BC6H-specific translation units
/// pay for these static data tables.

#pragma once

#include <array>

#include <whiteout/common_types.h>

namespace whiteout::textures {

// ============================================================================
// BC6H partition table (2-subset, 32 patterns × 16 texels)
// ============================================================================

static constexpr std::array<std::array<u8, 16>, 32> BC6H_PARTITION_TABLE = {{
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1},
    {0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1},
    {0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1},
    {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0},
    {0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0},
    {0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0},
    {0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0},
}};

/// Anchor index for subset 1 in BC6H 2-subset modes.
static constexpr std::array<u8, 32> BC6H_ANCHOR_2 = {{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
}};

// ============================================================================
// BC6H mode table
// ============================================================================
//
// A BC6H block header is a permutation of endpoint-field bits.  For each mode,
// `BC6H_LAYOUT_n[i]` says what block bit `i` carries: which field, and which
// bit of that field.  Fields are ordered so that `field & 3` is the endpoint
// index (0 = first subset low, 1 = first subset high, 2/3 = second subset) and
// `field >> 2` is the colour channel.
//
// Each layout is a bijection: every block bit of the header appears once and
// every field bit is filled exactly once.  `bc6h_layout_test.cpp` enforces
// that, which is what the previous hand-written extraction got wrong.

enum BC6HField : u8 {
    BC6H_RW, BC6H_RX, BC6H_RY, BC6H_RZ,
    BC6H_GW, BC6H_GX, BC6H_GY, BC6H_GZ,
    BC6H_BW, BC6H_BX, BC6H_BY, BC6H_BZ,
    BC6H_D,  BC6H_M,
};

struct BC6HBitRef {
    u8 field; ///< BC6HField
    u8 bit;   ///< bit index within that field
};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_1 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_GY,4}, {BC6H_BY,4}, {BC6H_BZ,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_GZ,4}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_BZ,0}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BZ,1}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_BZ,2}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_BZ,3},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_2 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_GY,5}, {BC6H_GZ,4}, {BC6H_GZ,5}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_BZ,0}, {BC6H_BZ,1},
    {BC6H_BY,4}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_BY,5}, {BC6H_BZ,2}, {BC6H_GY,4}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BZ,3}, {BC6H_BZ,5}, {BC6H_BZ,4},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_RY,5}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_RZ,5},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_3 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RW,10}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GW,10}, {BC6H_BZ,0}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BW,10}, {BC6H_BZ,1}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_BZ,2}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_BZ,3},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_4 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RW,10}, {BC6H_GZ,4}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GW,10}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BW,10}, {BC6H_BZ,1}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_BZ,0},
    {BC6H_BZ,2}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_GY,4}, {BC6H_BZ,3},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_5 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RW,10}, {BC6H_BY,4}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GW,10}, {BC6H_BZ,0}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BW,10}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_BZ,1},
    {BC6H_BZ,2}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_BZ,4}, {BC6H_BZ,3},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_6 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_BY,4}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GY,4}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BZ,4},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_GZ,4}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_BZ,0}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BZ,1}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_BZ,2}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_BZ,3},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_7 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_GZ,4},
    {BC6H_BY,4}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_BZ,2}, {BC6H_GY,4}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BZ,3}, {BC6H_BZ,4},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_BZ,0}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BZ,1}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_RY,5}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_RZ,5},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_8 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_BZ,0},
    {BC6H_BY,4}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GY,5}, {BC6H_GY,4}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_GZ,5}, {BC6H_BZ,1},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_GZ,4}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BZ,2}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_BZ,3}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_BZ,4},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_9 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_BZ,0},
    {BC6H_BZ,1}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_BY,4}, {BC6H_BZ,2}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_GY,4}, {BC6H_BZ,3},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_BZ,5}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GZ,4}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_BZ,4}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_BY,5},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 82> BC6H_LAYOUT_10 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_GZ,4}, {BC6H_BZ,0}, {BC6H_BZ,1},
    {BC6H_BZ,2}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GY,5}, {BC6H_BY,4}, {BC6H_BZ,3}, {BC6H_BZ,5}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_GY,4}, {BC6H_BY,5}, {BC6H_BZ,4}, {BC6H_GZ,5},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_GY,0},
    {BC6H_GY,1}, {BC6H_GY,2}, {BC6H_GY,3}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GZ,0}, {BC6H_GZ,1}, {BC6H_GZ,2}, {BC6H_GZ,3}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5}, {BC6H_BY,0}, {BC6H_BY,1},
    {BC6H_BY,2}, {BC6H_BY,3}, {BC6H_RY,0}, {BC6H_RY,1}, {BC6H_RY,2}, {BC6H_RY,3}, {BC6H_RY,4},
    {BC6H_RY,5}, {BC6H_RZ,0}, {BC6H_RZ,1}, {BC6H_RZ,2}, {BC6H_RZ,3}, {BC6H_RZ,4}, {BC6H_RZ,5},
    {BC6H_D,0}, {BC6H_D,1}, {BC6H_D,2}, {BC6H_D,3}, {BC6H_D,4}
}};

static constexpr std::array<BC6HBitRef, 65> BC6H_LAYOUT_11 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_RX,6},
    {BC6H_RX,7}, {BC6H_RX,8}, {BC6H_RX,9}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GX,6}, {BC6H_GX,7}, {BC6H_GX,8}, {BC6H_GX,9}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5}, {BC6H_BX,6}, {BC6H_BX,7},
    {BC6H_BX,8}, {BC6H_BX,9}
}};

static constexpr std::array<BC6HBitRef, 65> BC6H_LAYOUT_12 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_RX,6},
    {BC6H_RX,7}, {BC6H_RX,8}, {BC6H_RW,10}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2}, {BC6H_GX,3},
    {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GX,6}, {BC6H_GX,7}, {BC6H_GX,8}, {BC6H_GW,10}, {BC6H_BX,0},
    {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5}, {BC6H_BX,6}, {BC6H_BX,7},
    {BC6H_BX,8}, {BC6H_BW,10}
}};

static constexpr std::array<BC6HBitRef, 65> BC6H_LAYOUT_13 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RX,4}, {BC6H_RX,5}, {BC6H_RX,6},
    {BC6H_RX,7}, {BC6H_RW,11}, {BC6H_RW,10}, {BC6H_GX,0}, {BC6H_GX,1}, {BC6H_GX,2},
    {BC6H_GX,3}, {BC6H_GX,4}, {BC6H_GX,5}, {BC6H_GX,6}, {BC6H_GX,7}, {BC6H_GW,11},
    {BC6H_GW,10}, {BC6H_BX,0}, {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3}, {BC6H_BX,4}, {BC6H_BX,5},
    {BC6H_BX,6}, {BC6H_BX,7}, {BC6H_BW,11}, {BC6H_BW,10}
}};

static constexpr std::array<BC6HBitRef, 65> BC6H_LAYOUT_14 = {{
    {BC6H_M,0}, {BC6H_M,1}, {BC6H_M,2}, {BC6H_M,3}, {BC6H_M,4}, {BC6H_RW,0}, {BC6H_RW,1},
    {BC6H_RW,2}, {BC6H_RW,3}, {BC6H_RW,4}, {BC6H_RW,5}, {BC6H_RW,6}, {BC6H_RW,7}, {BC6H_RW,8},
    {BC6H_RW,9}, {BC6H_GW,0}, {BC6H_GW,1}, {BC6H_GW,2}, {BC6H_GW,3}, {BC6H_GW,4}, {BC6H_GW,5},
    {BC6H_GW,6}, {BC6H_GW,7}, {BC6H_GW,8}, {BC6H_GW,9}, {BC6H_BW,0}, {BC6H_BW,1}, {BC6H_BW,2},
    {BC6H_BW,3}, {BC6H_BW,4}, {BC6H_BW,5}, {BC6H_BW,6}, {BC6H_BW,7}, {BC6H_BW,8}, {BC6H_BW,9},
    {BC6H_RX,0}, {BC6H_RX,1}, {BC6H_RX,2}, {BC6H_RX,3}, {BC6H_RW,15}, {BC6H_RW,14},
    {BC6H_RW,13}, {BC6H_RW,12}, {BC6H_RW,11}, {BC6H_RW,10}, {BC6H_GX,0}, {BC6H_GX,1},
    {BC6H_GX,2}, {BC6H_GX,3}, {BC6H_GW,15}, {BC6H_GW,14}, {BC6H_GW,13}, {BC6H_GW,12},
    {BC6H_GW,11}, {BC6H_GW,10}, {BC6H_BX,0}, {BC6H_BX,1}, {BC6H_BX,2}, {BC6H_BX,3},
    {BC6H_BW,15}, {BC6H_BW,14}, {BC6H_BW,13}, {BC6H_BW,12}, {BC6H_BW,11}, {BC6H_BW,10}
}};

/// Per-mode geometry.  Index by mode id 1..14; index 0 is the invalid entry.
struct BC6HModeDesc {
    u8 subsets;               ///< 1 or 2
    bool transformed;         ///< endpoints after the first are deltas
    u8 endpoint_bits;         ///< precision of the base endpoint
    std::array<u8, 3> delta_bits; ///< per-channel delta precision
    u16 header_bits;          ///< 82 for 2-subset modes, 65 for 1-subset
    const BC6HBitRef* layout;
};

static constexpr std::array<BC6HModeDesc, 15> BC6H_MODES = {{
    {0, false, 0, {0, 0, 0}, 0, nullptr},
    {2, true, 10, {5, 5, 5}, 82, BC6H_LAYOUT_1.data()},
    {2, true, 7, {6, 6, 6}, 82, BC6H_LAYOUT_2.data()},
    {2, true, 11, {5, 4, 4}, 82, BC6H_LAYOUT_3.data()},
    {2, true, 11, {4, 5, 4}, 82, BC6H_LAYOUT_4.data()},
    {2, true, 11, {4, 4, 5}, 82, BC6H_LAYOUT_5.data()},
    {2, true, 9, {5, 5, 5}, 82, BC6H_LAYOUT_6.data()},
    {2, true, 8, {6, 5, 5}, 82, BC6H_LAYOUT_7.data()},
    {2, true, 8, {5, 6, 5}, 82, BC6H_LAYOUT_8.data()},
    {2, true, 8, {5, 5, 6}, 82, BC6H_LAYOUT_9.data()},
    {2, false, 6, {6, 6, 6}, 82, BC6H_LAYOUT_10.data()},
    {1, false, 10, {10, 10, 10}, 65, BC6H_LAYOUT_11.data()},
    {1, true, 11, {9, 9, 9}, 65, BC6H_LAYOUT_12.data()},
    {1, true, 12, {8, 8, 8}, 65, BC6H_LAYOUT_13.data()},
    {1, true, 16, {4, 4, 4}, 65, BC6H_LAYOUT_14.data()},
}};

} // namespace whiteout::textures
