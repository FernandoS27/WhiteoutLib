// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file flag_ops.h
 * @brief Bitwise operators for the generated mirrors' flag enums.
 *
 * A mirrored flag word keeps its `enum class` type rather than decaying to
 * `u32`, which is what lets a converter say `hasFlag(layer.shadingFlags,
 * MdxShadingFlag::TwoSided)` instead of remembering a literal. The fixed
 * underlying type is doing real work here too: an `enum class : u32` holds
 * *any* u32, so an undocumented bit in shipped content survives the mirror
 * instead of being masked away.
 *
 * Authored, not generated: which enums get these is a decision the generator
 * makes (a bit vocabulary rather than a list), but the operators themselves
 * are the same eight lines every time.
 */

#include <whiteout/common_types.h>

// clang-format off
#define WHITEOUT_WEM_DEFINE_FLAG_OPERATORS(EnumType)                                               \
    inline constexpr EnumType operator|(EnumType lhs, EnumType rhs) {                              \
        return static_cast<EnumType>(static_cast<u32>(lhs) | static_cast<u32>(rhs));               \
    }                                                                                              \
    inline constexpr EnumType operator&(EnumType lhs, EnumType rhs) {                              \
        return static_cast<EnumType>(static_cast<u32>(lhs) & static_cast<u32>(rhs));               \
    }                                                                                              \
    inline constexpr EnumType operator~(EnumType v) {                                              \
        return static_cast<EnumType>(~static_cast<u32>(v));                                        \
    }                                                                                              \
    inline EnumType& operator|=(EnumType& lhs, EnumType rhs) {                                     \
        lhs = lhs | rhs;                                                                           \
        return lhs;                                                                                \
    }                                                                                              \
    inline EnumType& operator&=(EnumType& lhs, EnumType rhs) {                                     \
        lhs = lhs & rhs;                                                                           \
        return lhs;                                                                                \
    }                                                                                              \
    inline constexpr bool hasFlag(EnumType flags, EnumType flag) {                                 \
        return (static_cast<u32>(flags) & static_cast<u32>(flag)) != 0;                            \
    }
// clang-format on
