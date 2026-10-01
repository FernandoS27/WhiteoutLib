// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file writer.h
 * @brief `obj::Asset` / `obj::MaterialLibrary` → text.
 *
 * Deterministic and locale-free: floats in nine significant digits (any f32
 * reads back bit-identically) with a `.` radix whatever the host's locale, so
 * two writes of one asset are the same bytes. `o`, `g`, `usemtl` and `s` are
 * written where they change from the previous face.
 */

#include <string>
#include <string_view>

#include "obj.h"

namespace whiteout {
namespace models {
namespace obj {

class Writer {
public:
    /// @p header lines are written as `#` comments first.
    static std::string ToText(const Asset& asset, std::string_view header = {});
    static std::string MaterialsToText(const MaterialLibrary& library, std::string_view header = {});
};

} // namespace obj
} // namespace models
} // namespace whiteout
