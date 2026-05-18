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
/// @bind js_name=BlizzardGame, java_package=whiteout.utils
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
/// @bind value_object, java_package=whiteout.utils
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

// ============================================================================
// Bindable wrappers
// ============================================================================
//
// Free functions don't currently bind through the codegen; expose them
// as static methods of a holder class. The vector return is wrapped in
// a `BlizzardGameInfoList` opaque handle (size() / at(index)) rather
// than marshalled as an array so we don't need vector<value_object>
// return support in the codegen.

/// Opaque handle around `std::vector<BlizzardGameInfo>` returned by
/// `BlizzardGameFinder::findAll()`. Iterate via size() + at(index).
/// @bind methods, no_default_ctor, move_only, java_package=whiteout.utils
class BlizzardGameInfoList {
public:
    explicit BlizzardGameInfoList(std::vector<BlizzardGameInfo> games)
        : m_games(std::move(games)) {}

    BlizzardGameInfoList(const BlizzardGameInfoList&) = delete;
    BlizzardGameInfoList& operator=(const BlizzardGameInfoList&) = delete;
    BlizzardGameInfoList(BlizzardGameInfoList&&) = default;
    BlizzardGameInfoList& operator=(BlizzardGameInfoList&&) = default;

    /// Number of game entries in the list.
    size_t size() const noexcept { return m_games.size(); }

    /// Borrowed reference to entry at @p index. Valid until this list
    /// is destroyed.
    const BlizzardGameInfo& at(size_t index) const { return m_games.at(index); }

private:
    std::vector<BlizzardGameInfo> m_games;
};

/// Static-method facade around the free functions above so they bind
/// through the class-method codegen path.
/// @bind methods, no_default_ctor, java_package=whiteout.utils
class BlizzardGameFinder {
public:
    BlizzardGameFinder() = delete;

    /// Discover installed Blizzard games. See `findBlizzardGames()`.
    static BlizzardGameInfoList findAll() {
        return BlizzardGameInfoList(findBlizzardGames());
    }

    /// Map a display name to a BlizzardGame enum. See
    /// `blizzardGameFromName()`.
    static BlizzardGame fromName(const std::string& name) {
        return blizzardGameFromName(name);
    }

    /// Get the canonical display name for a known game. See
    /// `blizzardGameToName()`. Returns empty string for Unknown.
    static std::string toName(BlizzardGame game) {
        const char* s = blizzardGameToName(game);
        return s != nullptr ? std::string(s) : std::string();
    }
};

} // namespace whiteout::utils
