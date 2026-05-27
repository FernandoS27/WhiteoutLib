// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Smoke test for the newly-exposed mdx::Parser/Writer + verifies that
// FlagEnum constants now use the actual C++ literal values (the prior
// codegen used ordinal positions, silently mangling every bitmask enum).

package whiteout.mdx;

public class MdxParserSmokeTest {

    public static void main(String[] args) {
        testParserFactoryAndDefaults();
        testWriterFactoryAndDefaults();
        testFlagEnumValuesAreBitwise();
        testBitmaskPackUnpack();
        System.out.println("OK: all MdxParser smoke tests passed");
    }

    static void testParserFactoryAndDefaults() {
        try (Parser p = new Parser()) {
            require(p != null, "default Parser()");
            require(!p.hasIssues(),
                "fresh parser reports no issues before parsing");
        }
        try (Parser p = Parser.createUpgradeMode(UpgradeMode.PreserveOriginal)) {
            require(p != null, "Parser.createUpgradeMode(...)");
        }
    }

    static void testWriterFactoryAndDefaults() {
        try (Writer w = new Writer()) {
            require(w != null, "default Writer()");
        }
    }

    static void testFlagEnumValuesAreBitwise() {
        // mdx::Node::NodeFlag is the textbook bitmask enum that proves
        // the value-correctness fix. Per mdx/structures.h:
        //   DontInheritTranslation = 0x1, DontInheritScaling = 0x2,
        //   DontInheritRotation    = 0x4, Billboarded         = 0x8, …
        // The ordinal-position bug would have produced 1,2,3,4… not 1,2,4,8.
        require(NodeFlag.DontInheritTranslation.value == 1,
            "NodeFlag.DontInheritTranslation == 1, got "
            + NodeFlag.DontInheritTranslation.value);
        require(NodeFlag.DontInheritScaling.value == 2,
            "NodeFlag.DontInheritScaling == 2, got "
            + NodeFlag.DontInheritScaling.value);
        require(NodeFlag.DontInheritRotation.value == 4,
            "NodeFlag.DontInheritRotation == 4 (would have been 3 before the fix), got "
            + NodeFlag.DontInheritRotation.value);
        // Two distinct bitflags must not overlap.
        require((NodeFlag.DontInheritTranslation.value
                & NodeFlag.DontInheritRotation.value) == 0,
            "Distinct NodeFlags don't overlap bitwise");
        // Every NodeFlag value is a power of two (the bitmask contract).
        for (NodeFlag nf : NodeFlag.values()) {
            if (nf.value == 0) continue;
            require(Integer.bitCount(nf.value) == 1,
                "NodeFlag." + nf.name() + " == "
                + Integer.toHexString(nf.value) + " is a power of two");
        }
    }

    static void testBitmaskPackUnpack() {
        // Bitmask enums get auto-generated pack/unpack helpers so users
        // don't have to OR raw .value ints by hand.
        java.util.EnumSet<NodeFlag> set = java.util.EnumSet.of(
            NodeFlag.DontInheritTranslation, NodeFlag.DontInheritRotation);
        int packed = NodeFlag.pack(set);
        require(packed == (1 | 4),
            "pack({DontInheritTranslation, DontInheritRotation}) == 1|4, got "
            + Integer.toHexString(packed));
        java.util.EnumSet<NodeFlag> back = NodeFlag.unpack(packed);
        require(back.equals(set),
            "unpack round-trips ({DontInheritTranslation, DontInheritRotation})");
        require(back.contains(NodeFlag.DontInheritTranslation)
                && back.contains(NodeFlag.DontInheritRotation)
                && !back.contains(NodeFlag.DontInheritScaling),
            "unpack returns exactly the source bits");
        // fromInt on a bitmask now returns null instead of throwing for
        // OR'd combinations.
        require(NodeFlag.fromInt(packed) == null,
            "fromInt(OR-combined) returns null (was: throw before the fix)");
        // Single-bit fromInt still works.
        require(NodeFlag.fromInt(NodeFlag.DontInheritRotation.value)
                == NodeFlag.DontInheritRotation,
            "fromInt(single bit) round-trips");
    }

    private static void require(boolean cond, String msg) {
        if (!cond) throw new AssertionError("FAIL: " + msg);
    }
}
