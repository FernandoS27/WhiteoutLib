// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// The Diablo III install the `[install]` arms read, through CASC.
///
/// The loose corpus these tests used to read is gone; the shipped files are the
/// evidence now. `WHITEOUT_CASC_D3` names the install, else the Battle.net
/// records are asked. No install is not a failure: the caller warns and returns.

#include <whiteout/common_types.h>
#include <whiteout/storages/casc/storage.h>
#include <whiteout/utils/blizzard_game_finder.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::test::d3 {

inline std::optional<storages::casc::Storage> openInstall() {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("WHITEOUT_CASC_D3")) {
        dirs.emplace_back(env);
    }
    for (const utils::BlizzardGameInfo& game : utils::findBlizzardGames()) {
        if (game.game == utils::BlizzardGame::DiabloIII) {
            dirs.push_back(game.path);
        }
    }
    for (const std::string& dir : dirs) {
        if (!std::filesystem::exists(std::filesystem::path(dir) / ".build.info")) {
            continue;
        }
        if (auto storage = storages::casc::Storage::open(dir)) {
            return storage;
        }
    }
    return std::nullopt;
}

/// A file by its CASC path (`Base\Textures\irradiance.tex`); empty when absent.
inline std::vector<u8> readInstallFile(const storages::casc::Storage& storage,
                                       const std::string& path) {
    auto bytes = storage.readFile(path);
    return bytes ? std::move(*bytes) : std::vector<u8>{};
}

} // namespace whiteout::test::d3
