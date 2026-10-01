// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file parser.h
 * @brief OBJ and MTL text → `obj::Asset` / `obj::MaterialLibrary`.
 *
 * Exception-free and bounded: a malformed statement is skipped with a warning
 * (a face naming a pool entry past the end, a number that does not parse), a
 * file that is not text at all is refused. Negative indices resolve against the
 * pool size at the point they are read, as the format defines.
 */

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "obj.h"

namespace whiteout {
namespace models {
namespace obj {

struct ParseOutcome {
    std::optional<Asset> asset;
    std::string error;
    std::vector<std::string> warnings;

    bool ok() const {
        return asset.has_value();
    }
};

struct MtlParseOutcome {
    MaterialLibrary library;
    std::vector<std::string> warnings;
};

class Parser {
public:
    static ParseOutcome FromText(std::string_view text);
    static MtlParseOutcome MaterialsFromText(std::string_view text);
};

} // namespace obj
} // namespace models
} // namespace whiteout
