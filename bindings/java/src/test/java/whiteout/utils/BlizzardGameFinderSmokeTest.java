// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Surface smoke test for BlizzardGameFinder + BlizzardGameInfoList +
// BlizzardGameInfo + BlizzardGame. Doesn't depend on any games actually
// being installed — just verifies the bindings work end-to-end:
//   - findAll() returns a non-null list (possibly empty)
//   - fromName / toName round-trip
//   - BlizzardGameInfo field accessors work on whatever findAll returns

package whiteout.utils;

public class BlizzardGameFinderSmokeTest {

    public static void main(String[] args) {
        testFromAndToNameRoundTrip();
        testFindAllReturnsList();
        System.out.println("OK: all BlizzardGameFinder smoke tests passed");
    }

    static void testFromAndToNameRoundTrip() {
        // toName + fromName should round-trip for a known game.
        String wow = BlizzardGameFinder.toName(BlizzardGame.WorldOfWarcraft);
        require(wow != null && !wow.isEmpty(),
            "toName(WoW) returns non-empty string (got '" + wow + "')");
        BlizzardGame back = BlizzardGameFinder.fromName(wow);
        require(back == BlizzardGame.WorldOfWarcraft,
            "fromName('" + wow + "') round-trips to WorldOfWarcraft, got " + back);
        // Unknown name → Unknown enum.
        BlizzardGame unk = BlizzardGameFinder.fromName("totally not a real game");
        require(unk == BlizzardGame.Unknown,
            "fromName(garbage) == Unknown, got " + unk);
    }

    static void testFindAllReturnsList() {
        try (BlizzardGameInfoList list = BlizzardGameFinder.findAll()) {
            long n = list.size();
            require(n >= 0, "size() non-negative");
            System.out.println("  findAll() returned " + n + " game(s)");
            for (long i = 0; i < n; ++i) {
                try (BlizzardGameInfo info = list.at(i)) {
                    BlizzardGame g = info.getGame();
                    String name = info.getName();
                    String path = info.getPath();
                    require(name != null, "entry[" + i + "].name non-null");
                    require(path != null, "entry[" + i + "].path non-null");
                    System.out.println("    [" + i + "] game=" + g
                        + " name='" + name + "' path='" + path + "'");
                }
            }
        }
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
