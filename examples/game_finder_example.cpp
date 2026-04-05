// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <cstdio>
#include <whiteout/utils/blizzard_game_finder.h>

int main() {
    auto games = whiteout::utils::findBlizzardGames();

    if (games.empty()) {
        std::printf("No Blizzard game installations found.\n");
        return 0;
    }

    std::printf("Found %zu Blizzard game installation(s):\n\n", games.size());
    for (auto& info : games) {
        const char* tag = (info.game == whiteout::utils::BlizzardGame::Unknown) ? " [?]" : "";
        std::printf("  %-35s %s%s\n", info.name.c_str(), info.path.c_str(), tag);
    }

    return 0;
}
