// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/rigging/engine_names.h>

#include <algorithm>
#include <cctype>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Whether a token of @p name is one of @p tokens: `LookupBoneId`'s tokenizer
/// separators, matched without case as `TknzFindTokenId` does.
template <std::size_t N>
bool HasBoneToken(const std::string& name, const char* const (&tokens)[N]) {
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find_first_of(" \t\r\n\",;", start);
        if (end == std::string::npos) {
            end = name.size();
        }
        if (end > start) {
            std::string token = name.substr(start, end - start);
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* known : tokens) {
                if (token == known) {
                    return true;
                }
            }
        }
        start = end + 1;
    }
    return false;
}

} // namespace

bool IsWarcraftEngineBoneName(const std::string& name) {
    static constexpr const char* kTokens[] = {"bone_head", "bone_chest", "bone_foot", "bone_hand",
                                              "bone_turret"};
    return HasBoneToken(name, kTokens);
}

bool IsStarCraftEngineBoneName(const std::string& name) {
    static constexpr const char* kTokens[] = {"bone_chest", "bone_face", "bone_leg", "bone_turret"};
    return HasBoneToken(name, kTokens);
}

bool IsWarcraftLookAtBoneName(const std::string& name) {
    static constexpr const char* kTokens[] = {"bone_head", "bone_chest", "bone_turret"};
    return HasBoneToken(name, kTokens);
}

} // namespace wem
} // namespace models
} // namespace whiteout
