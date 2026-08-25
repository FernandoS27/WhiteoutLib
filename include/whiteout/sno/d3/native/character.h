// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Setting up a Diablo III player character.
///
/// Everything here sits on top of the generated parsers in types.h; nothing in
/// this header reads bytes.  It exists because knowing the layout of an `.app`
/// is not enough to draw a dressed character -- you also need the rules the
/// engine applies to it, and those live in code, not in the asset.
///
/// THE ONE FACT THAT ORGANISES ALL OF THIS
/// ---------------------------------------
/// A player Appearance already contains every armour variant as sub-objects.
/// Equipping an item loads no geometry and no material asset: it resolves to
/// **two small integers on the item's Actor tag map**, and those flip
/// visibility bits on sub-objects that were there all along.
///
///     item GBID -> GameBalance -> the item's Actor SNO
///                              -> tag 0x10400 = look VALUE  (which mesh)
///                              -> tag 0x10401 = look NAME   (which material)
///
/// The look category (from the equipment slot) plus the look value choose the
/// MESH; the look name, resolved to an index into `Appearances::arLooks`,
/// chooses the MATERIAL.  `ActorModel_UpdateEquipmentVisuals` (`0x7100086CF0`)
/// is that pass, and `ActorModel_ApplyLook` (`0x7100222000`) does the flipping.
///
/// A NOTE ON WHICH BUILD EACH FACT COMES FROM
/// ------------------------------------------
/// The rules below are read out of the Switch 2.6.2 binary.  The `.app` corpus
/// this library parses is an *older* build -- 536-byte Appearance, v260 -- and
/// the two disagree about where a sub-object stores its geoset descriptor:
///
///   * 2.6.2 packs it into a dword at `SubObject+92`: bit 1 = takes part in
///     look switching, bit 2 = keyed to one specific value, byte 2 = look
///     category, byte 3 = look value.
///   * the shipped corpus has `szName[128]` at that offset instead, and carries
///     the same information in `szMaterialName`, as the Maya shape name
///     (`N_TRS_HVY_AShape_Barb_F_HVY_mat_001`).
///
/// So `parseGeosetName()` below is how you recover it *from the corpus*, and it
/// is corpus-derived: no function in this binary parses those strings.  The
/// category/value numbering, the fallback chain and the dye maths are the
/// opposite -- straight from the binary, and not observable in the files.
/// Each declaration says which it is.

#pragma once

#include <whiteout/sno/d3/native/types.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout {
namespace sno {
namespace d3 {
namespace native {

// ---------------------------------------------------------------------------
// Who the character is
// ---------------------------------------------------------------------------

/// FROM THE BINARY. Pinned by two independent string builders that agree on the
/// ordinals while using different spellings: `Player_FormatClassNameKey`
/// (`0x710048A000`) and `Player_FormatPortraitName` (`0x710019CA70`).
enum class PlayerClass : i32 {
    DemonHunter = 0,
    Barbarian = 1,
    Wizard = 2,
    WitchDoctor = 3,
    Monk = 4,
    Crusader = 5,
    Necromancer = 6,
};
inline constexpr size_t kPlayerClassCount = 7;

/// Gender is a separate flag, not part of the class. Non-zero = male.
enum class Gender : u8 { Female = 0, Male = 1 };

/// The `Player_FormatClassNameKey` spelling -- what builds a StringList key.
const char* playerClassName(PlayerClass cls);

/// The `Player_FormatPortraitName` spelling. Deliberately separate: this build
/// writes "Demonhunter" and "Witchdoctor" here and "DemonHunter"/"WitchDoctor"
/// in the key builder, so one table cannot serve both and a lookup keyed on the
/// wrong spelling silently finds nothing.
const char* playerPortraitClassName(PlayerClass cls);

/// The file stem of a class's player Appearance, e.g. "Barbarian_Male".
///
/// Not derivable from the class name: Crusader assets carry an `X1_` prefix and
/// Necromancer is `P6_Necro`, so a name-keyed lookup built from
/// `playerClassName()` misses exactly those two classes. Verified by loading all
/// 14 (7 classes x 2 genders) out of the corpus.
std::string playerAppearanceStem(PlayerClass cls, Gender gender);

// ---------------------------------------------------------------------------
// What the character is wearing: the item -> look pipeline
// ---------------------------------------------------------------------------

/// FROM THE BINARY (`Actor_GetTagMapValue`, `0x7100609CD0`). The two tags an
/// item's Actor carries; there is no per-item mesh or material asset for armour.
inline constexpr u32 kTagItemLookValue = 0x10400;  ///< which mesh variant
inline constexpr u32 kTagItemLookName = 0x10401;   ///< which material set

/// FROM THE BINARY. With no item equipped -- or an item whose Actor has neither
/// tag -- `Appearance_GetDefaultLook` supplies these.
inline constexpr u32 kDefaultLookValue = 0;
inline constexpr std::string_view kDefaultLookName = "A";

/// FROM THE BINARY (`g_EquipSlotToLookCategory`, `0x7100E3D28C`). Exactly four
/// equipment slots drive geoset switching; everything else a character wears is
/// an attached model on a hardpoint, not a visibility flip.
///
/// OPEN: which body part each pair refers to. The four are torso, legs, boots
/// and gloves (that much is settled), but the visual-slot index is its own
/// numbering, not `EEquipmentSlot`, and nothing has yet pinned slot 1 to a
/// specific one of the four. The table is therefore exposed as the raw pairs the
/// binary holds rather than as named body parts.
struct EquipSlotLook {
    u32 visualSlot;
    u32 lookCategory;
};
std::span<const EquipSlotLook> equipSlotLookCategories();

/// FROM THE BINARY. Category 8 with value 0 is handled separately by
/// `ActorModel_ApplyLook`: it clears the draw bit on every sub-object whose
/// descriptor matches `(desc & 0xFF0002) == 0x80002`. Given that the player
/// Appearances carry Hair / Hair_HLM1 / Hair_HLM2 sub-objects this is the
/// "no helmet equipped, put the plain hair back" path -- a strong inference from
/// the data it touches, not something the binary states.
inline constexpr u32 kLookCategoryHairHelm = 8;

/// FROM THE BINARY, exactly as `ActorModel_ApplyLook` computes it.
///
/// When no sub-object matches (category, value), the engine retries ONCE with a
/// simpler value and then, if that also misses, with 0 -- so a variant a class
/// does not have degrades to a plainer one and finally to the naked mesh, rather
/// than leaving a hole in the character.
///
///     2, 7 -> 1        4, 8 -> 3        6, 9 -> 5        anything else -> 0
///
/// Values above 9 skip the middle step entirely (the binary guards on `<= 9`).
/// Returns the single retry value; the caller falls back to 0 after that.
u32 fallbackLookValue(u32 lookValue);

/// The value of one tag in a tag map, if present.
std::optional<u32> tagMapValue(std::span<const TagMapEntry> tagMap, u32 tagId);

/// The two look tags read off an equipped item's Actor. Absent entries are left
/// empty rather than defaulted, so a caller can tell "the item says nothing"
/// from "the item says 0" -- the engine treats them the same, but a tool
/// inspecting assets should not have to guess.
struct ItemLook {
    std::optional<u32> lookValue;
    std::optional<u32> lookName;
};
ItemLook itemLook(const Actor& itemActor);

// ---------------------------------------------------------------------------
// Choosing the material: looks
// ---------------------------------------------------------------------------

/// The index of a named look, or 0 when the name is not present.
///
/// FROM THE BINARY for the fallback: `Appearance_FindLookIndex` (`0x71001A7C90`)
/// linear-scans the look table and returns 0 -- the first look, not -1 -- when
/// it finds nothing. Matching by NAME is the corpus shape: 2.6.2's
/// `AppearanceLook` is a bare 4-byte id, while the shipped files carry a name
/// string, which is what `arLooks` holds here.
size_t findLookIndex(const Appearances& app, std::string_view lookName);

/// The material variant a sub-object uses under a given look.
///
/// CORPUS-VERIFIED, over all 441 sub-objects of the 14 player Appearances:
///   * every `SubObject::szName` names an `AppearanceMaterial` in the same file
///     (441/441), which is the link between geometry and material; and
///   * every `AppearanceMaterial` holds exactly `arLooks.size()` variants
///     (233/233), so the look index doubles as the variant index.
///
/// Returns nullptr if either relation fails to hold for this file, rather than
/// indexing out of range.
const SubObjectAppearance* subObjectAppearance(const Appearances& app, const SubObject& sub,
                                               size_t lookIndex);

// ---------------------------------------------------------------------------
// Choosing the mesh: geosets
// ---------------------------------------------------------------------------

/// The four armour slots a geoset name can carry, plus hair.
enum class LookSlot : u8 { Unknown, Torso, Legs, Boots, Gloves, Hair };

/// The armour weight class a geoset name can carry.
enum class ArmourWeight : u8 { Unknown, Naked, Light, Medium, Heavy };

/// A sub-object's geoset descriptor, recovered from its Maya shape name.
///
/// CORPUS-DERIVED. `szMaterialName` is `<token>Shape_<szName>_<NNN>`, or
/// `<token>ShapeDeformed_<szName>_<NNN>` in the Necromancer files -- 436 of 441
/// sub-objects across the 14 player Appearances. The five that do not parse are
/// three effect meshes whose material name is the literal "HC".
///
/// The token itself is `N_<SLOT>[_<QUALIFIER>]_<WEIGHT>[_<VARIANT>][_Cloth]`,
/// e.g. `N_TRS_HVY_A`, `N_LEG_NKD_Cloth`, `N_TRS_CLS_MED_B`, or `Hair_HLM1` /
/// `Hair_NKD` / `N_Hair` for hair. Non-armour meshes (`BarbM_decap_geo`,
/// `Epiphany_shell_geo`, `oneBatch_geo`) parse as a token with no slot, which is
/// why `slot == LookSlot::Unknown` is a normal result and not an error.
///
/// The `_Cloth` suffix appears in both spellings in the shipped files, so the
/// match is case-insensitive.
struct GeosetName {
    bool parsed = false;      ///< the shape-name pattern matched at all
    std::string_view token;   ///< the part before "Shape"; empty when !parsed
    LookSlot slot = LookSlot::Unknown;
    ArmourWeight weight = ArmourWeight::Unknown;
    std::string_view qualifier;  ///< e.g. "CLS"; empty when absent
    char variant = 0;            ///< 'A'..'C', or 0 when the token has none
    bool cloth = false;
};

/// Parse a sub-object's geoset descriptor out of its material name.
///
/// The returned views point into `sub.szMaterialName`; they are valid only as
/// long as that `SubObject` is.
GeosetName parseGeosetName(const SubObject& sub);

/// The look value a weight class's plain variant uses.
///
/// INFERENCE, not a binary fact -- the strongest one in this header, but state
/// it as what it is. The engine's fallback partitions the value space into
/// {1,2,7} -> 1, {3,4,8} -> 3, {6,9} -> 5 and 0, and guards on `value <= 9`;
/// that is exactly four groups of "a base plus up to two extras", which lines up
/// with the four weight classes and with what the corpus holds (NKD has no
/// variant, LIT only `A`, MED and HVY up to `A`/`B`/`C`). So Naked -> 0,
/// Light -> 1, Medium -> 3, Heavy -> 5.
///
/// What is NOT determined: which of the two spare values in each group a `B` or
/// `C` variant takes. The fallback is symmetric (2 and 7 both degrade to 1), so
/// the corpus cannot distinguish them, and no mapping is offered here.
u32 armourWeightBaseLookValue(ArmourWeight weight);

// ---------------------------------------------------------------------------
// Dyes
// ---------------------------------------------------------------------------

/// FROM THE BINARY (`Render_DrawRenderRecords`, `0x71000F3F50`), exactly:
///
///     bUseDyeType = (dyeType != 0) ? 1.0 : 0.0
///     tintRampUV  = (dyeType >= 2) ? ((dyeType - 2) + 0.5) / 21.0 : 0.0
///
/// So 0 is undyed, 1 is reserved, and 2..22 are twenty-one dye colours sampled
/// at the ROW CENTRES of the `dye_ramp` texture. The 0.5 is what makes it a
/// 21-row palette rather than a gradient -- interpolating between dyes is wrong.
inline constexpr i32 kDyeNone = 0;
inline constexpr i32 kDyeFirst = 2;
inline constexpr i32 kDyeLast = 22;
inline constexpr i32 kDyeRampRows = 21;
inline constexpr size_t kDyeCount = 21;

bool usesDyeType(i32 dyeType);
f32 dyeRampU(i32 dyeType);

/// FROM THE BINARY (`ActorModel_SetTintFromAppearance`, `0x710022F090`). The dye
/// is not stored on the item -- it comes from one of five ints in the Appearance
/// selected by a "tint kind" of 9..13, and is then propagated to every attached
/// child model, which is why a weapon inherits its owner's tint.
///
/// Those five ints are `Appearance+428..+444`. The parsed `Appearances` struct
/// exposes them under their placeholder names, so this returns the index (0..4)
/// rather than reaching for a field whose name is still provisional.
/// Returns nullopt for a tint kind outside 9..13, which the binary maps to -1.
std::optional<size_t> tintSlotForKind(u32 tintKind);
inline constexpr u32 kTintKindFirst = 9;
inline constexpr u32 kTintKindLast = 13;

} // namespace native
} // namespace d3
} // namespace sno
} // namespace whiteout
