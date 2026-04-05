#include <cstdio>
#include <whiteout/utils/blizzard_game_finder.h>
int main() {
    auto g = whiteout::utils::blizzardGameFromName("Warcraft III");
    std::printf("Result: %d\n", static_cast<int>(g));
    auto g2 = whiteout::utils::blizzardGameFromName("Battle.net");
    std::printf("Result2: %d\n", static_cast<int>(g2));
    auto games = whiteout::utils::findBlizzardGames();
    for (auto& info : games) {
        std::printf("  game=%d name='%s' path='%s'\n", static_cast<int>(info.game), info.name.c_str(), info.path.c_str());
    }
}
