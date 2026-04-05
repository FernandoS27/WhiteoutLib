// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace whiteout::utils {

/// Known Blizzard game identifiers.
///
/// The `Unknown` value is used for games discovered via generic heuristics
/// (e.g. the Windows Uninstall registry scan) that don't match a known title.
/// In that case, inspect the `name` field of the result for the display name.
enum class BlizzardGame {
    Unknown,

    // Warcraft
    WorldOfWarcraft,
    WorldOfWarcraftClassic,
    WorldOfWarcraftClassicEra,
    WarcraftIIBattleNetEdition,
    WarcraftIIRemastered,
    WarcraftIII,
    WarcraftIIIReforged,

    // StarCraft
    StarCraft,
    StarCraftRemastered,
    StarCraftII,

    // Diablo
    Diablo,
    DiabloII,
    DiabloIIResurrected,
    DiabloIII,
    DiabloIV,
    DiabloImmortal,

    // Other
    HeroesOfTheStorm,
    Overwatch2,
    Hearthstone,
    BlizzardArcadeCollection,
    BattleNet,
};

/// Result entry from findBlizzardGames().
struct BlizzardGameInfo {
    BlizzardGame game;      ///< Identified game. `Unknown` if not recognized.
    std::string  name;      ///< Human-readable display name.
    std::string  path;      ///< Install directory path.
};

/// Map a display name string to a BlizzardGame enum value.
/// Returns `BlizzardGame::Unknown` if the name is not recognized.
BlizzardGame blizzardGameFromName(const std::string& name);

/// Returns the canonical display name for a known game enum value.
/// Returns an empty string for `BlizzardGame::Unknown`.
const char* blizzardGameToName(BlizzardGame game);

/// Discovers installed Blizzard game directories by scanning platform-specific
/// sources: Windows registry, Battle.net product database, and Steam library
/// folders. On macOS, also scans /Applications for known game bundles. On
/// Linux, scans Wine prefixes and Steam Proton compatdata for Battle.net
/// installations.
///
/// Each result contains the identified game enum, display name, and install
/// path. Duplicate installations found through multiple sources are
/// deduplicated by install path.
///
/// Supported on Windows, macOS, and Linux. On other platforms, returns an
/// empty vector.
///
/// Example:
///   auto games = utils::findBlizzardGames();
///   for (auto& info : games) {
///       if (info.game == BlizzardGame::DiabloIV)
///           std::printf("Found D4 at: %s\n", info.path.c_str());
///   }
std::vector<BlizzardGameInfo> findBlizzardGames();

} // namespace whiteout::utils
