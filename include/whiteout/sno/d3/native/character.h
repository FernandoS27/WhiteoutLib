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
///
/// A second binary joined later: the **retail Windows 2.8.x exe** (imagebase
/// 0x400000 -- addresses spelled `0x7100...` are the Switch build, five- to
/// seven-digit ones are retail).  Retail ships the name tables the Switch
/// pass could only infer around (`g_LookCategoryNames`, `g_LookValueNames`,
/// `g_HairStyleNames`) and is where the whole attachment pipeline was
/// recovered -- see `D3_CHARACTER_DRESSING_RE.md` in the WhiteoutFlakes repo
/// for the full report.

#pragma once

#include <whiteout/sno/d3/native/types.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

/// The inverse: which class and gender a player Appearance stem names, or
/// nullopt for a stem that is not a player rig. Case-insensitive, and it
/// tolerates the suffixed variants the installs ship (`_characterSelect`,
/// `_FrontEnd`) by matching on the prefix -- those are the same character.
std::optional<std::pair<PlayerClass, Gender>> playerFromAppearanceStem(std::string_view stem);

// ---------------------------------------------------------------------------
// What the character is wearing: the item -> look pipeline
// ---------------------------------------------------------------------------

/// FROM THE BINARY (`Str_HashLower33`, retail Windows `0x54BF00`): the GBID
/// hash. `h = h*33 + tolower(c)`, seed 0, over the record's `szName` -- an
/// item is *addressed* by this: the shipped GameBalance records leave their
/// `gbid` field zeroed and the engine stamps `gbidHash(szName)` in at load,
/// and every cross-record reference (`gbidItemType`, dye and affix gbids) is
/// this hash of the referee's name. Lower-casing is ASCII: the names are.
inline u32 gbidHash(std::string_view name) {
    u32 h = 0;
    for (const char c : name)
        h = h * 33 +
            static_cast<u8>((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    return h;
}

/// The text of a StringList serialized string (`StringTableEntry::szKey` /
/// `szValue`): the span up to the first NUL. The raw span is the engine's
/// SerializeData block, whose size INCLUDES the terminator -- returning it
/// whole would hand every consumer a name that fails equality against the
/// spelling it was written from.
inline std::string_view stlText(const std::vector<u8>& s) {
    const auto* p = reinterpret_cast<const char*>(s.data());
    size_t n = 0;
    while (n < s.size() && p[n] != '\0') ++n;
    return {p, n};
}

/// FROM THE BINARY (`Actor_GetTagMapValue`, `0x7100609CD0`). The two tags an
/// item's Actor carries; there is no per-item mesh or material asset for armour.
inline constexpr u32 kTagItemLookValue = 0x10400;  ///< which mesh variant
inline constexpr u32 kTagItemLookName = 0x10401;   ///< which material set

/// FROM THE BINARY. With no item equipped -- or an item whose Actor has neither
/// tag -- `Appearance_GetDefaultLook` supplies these.
inline constexpr u32 kDefaultLookValue = 0;
inline constexpr std::string_view kDefaultLookName = "A";

/// FROM THE BINARY (`g_VisualSlotToLookCategory`, retail `0x140C758`; the
/// Switch build's `g_EquipSlotToLookCategory` `0x7100E3D28C` is the same
/// table). Exactly four equipment slots drive geoset switching; everything
/// else a character wears is an attached model on a hardpoint, not a
/// visibility flip.
///
/// Both sides of each pair are now identified: `visualSlot` is an
/// `EVisualSlot` ordinal and `lookCategory` is an `EEquipmentSlot` ordinal
/// (proved by the missing-component warning printing the category through
/// `g_EnumNames_EquipmentSlot`, retail `0x1443AA8`). The four pairs read
/// Torso->TRS, Feet->BTS, Hands->GLV, Legs->LEG.
struct EquipSlotLook {
    u32 visualSlot;
    u32 lookCategory;
};
std::span<const EquipSlotLook> equipSlotLookCategories();

/// FROM THE BINARY (`ActorModel_SetVisualEquipment`, retail `0x750880`): the
/// index into the nine-slot `VisualEquipment` block at ACD+308. Its own
/// numbering, NOT `EEquipmentSlot`. Slot 8 holds four raw cosmetic GBIDs
/// rather than a VisualItem and is out of the dressing room's scope.
enum class EVisualSlot : i32 {
    Head = 0,
    Torso = 1,
    Feet = 2,
    Hands = 3,
    RightHand = 4,
    LeftHand = 5,
    Shoulders = 6,
    Legs = 7,
    Cosmetics = 8,
};

/// The look category a visual slot geoset-switches through, or nullopt for
/// the attachment slots (0/4/5/6) and cosmetics.
std::optional<u32> lookCategoryForVisualSlot(EVisualSlot slot);

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
/// Values above 9 skip the middle step entirely (the binary guards on `<= 9`)
/// -- that is the whole `CLS_*` family: it has no fallback and degrades
/// straight to the naked mesh.
/// Returns the single retry value; the caller falls back to 0 after that.
u32 fallbackLookValue(u32 lookValue);

/// FROM THE BINARY (`g_LookCategoryNames`, retail `0x14776F0`): the six look
/// categories, by `EEquipmentSlot` ordinal. Returns nullptr for anything else
/// -- which is exactly what `LookCategory_GetName` (`0xC04080`) does, and what
/// makes categories 1 (Head) and 8 (Shoulders) attachment-only: with no name
/// there is no pattern to match, and `ActorModel_ApplyLook` returns before
/// touching a sub-object.
const char* lookCategoryName(u32 lookCategory);

/// FROM THE BINARY (`g_LookValueNames`, retail `0x14777E0`): all nineteen look
/// values. 0 NKD; 1/2/7 LIT_A/B/C; 3/4/8 MED_A/B/C; 5/6/9 HVY_A/B/C; 10..18
/// CLS_LIT/MED/HVY x A/B/C. Two long-standing open questions die here: the
/// SECOND spare value of each weight group is the `C` variant, and the values
/// >= 10 are a whole `CLS_` (class-specific) family. Returns "NKD" for an
/// unknown value -- the engine's own behaviour (`LookValue_GetName`,
/// `0xC04050`), which is why an out-of-range value renders the naked mesh
/// rather than nothing.
const char* lookValueName(u32 lookValue);

/// FROM THE BINARY (`ActorModel_ApplyLook`, retail `0x7544A0`): the engine
/// never parses a geoset name. It builds `sprintf("%s_%s", categoryName,
/// valueName)` and does a case-SENSITIVE substring search: a sub-object
/// matching the bare category name participates in the flip, and one matching
/// the full pattern is the one drawn. `parseGeosetName()` stays the right way
/// to LABEL the corpus; SELECTION must go through these two so a faithful
/// reimplementation can never disagree with the engine on an oddly-spelled
/// name.
bool matchesLookCategory(std::string_view subObjectName, u32 lookCategory);
bool matchesLook(std::string_view subObjectName, u32 lookCategory, u32 lookValue);

/// FROM THE BINARY (`Str_Hash33`): the CASE-SENSITIVE cousin of `gbidHash`,
/// `h = h*33 + c`, seed 0. Tag `0x10401` (the look NAME) stores this hash of
/// the single-letter look name, and `Appearance_FindLookIndexById` (retail
/// `0x575730`) resolves it by hashing each look's name -- returning 0, the
/// first look, on a miss.
inline u32 lookNameHash33(std::string_view name) {
    u32 h = 0;
    for (const char c : name) h = h * 33 + static_cast<u8>(c);
    return h;
}

/// The index of the look whose name hashes to `lookNameHash`, resolved the
/// way retail does (0 on a miss, never -1).
size_t findLookIndexByHash(const Appearances& app, u32 lookNameHash);

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

/// The look value a weight class's plain (`A`) variant uses: Naked -> 0,
/// Light -> 1, Medium -> 3, Heavy -> 5.
///
/// Once an inference from the fallback partition; now read straight off
/// `g_LookValueNames` (retail `0x14777E0`), which also settles the spares --
/// see `lookValueForGeoset` for the full mapping including `B`/`C` and the
/// `CLS_` family.
u32 armourWeightBaseLookValue(ArmourWeight weight);

/// The look value a geoset-name token maps to, now that `g_LookValueNames`
/// settles the whole table: NKD -> 0; LIT/MED/HVY A -> 1/3/5, B -> 2/4/6,
/// C -> 7/8/9; with the CLS_ qualifier -> 10..18. Returns nullopt for a token
/// combination outside the table (e.g. CLS_NKD, which no name exists for).
std::optional<u32> lookValueForGeoset(ArmourWeight weight, char variant, bool clsFamily);

// ---------------------------------------------------------------------------
// Attachments: helms, weapons, shoulders (retail 2.8 pipeline)
// ---------------------------------------------------------------------------

/// FROM THE BINARY (`g_HairStyleNames`, retail `0x144CC50`): 0 NKD, 1 HLM1,
/// 2 HLM2, 3 BALD -- a fourth style the corpus passes never surfaced. A
/// helmet's Actor carries the style in tag `0x10404`; the sub-objects are
/// literally named `Hair_<style>` and `ActorModel_ApplyHairStyle` (retail
/// `0x764070`) draws exactly one of them.
enum class HairStyle : u32 { Naked = 0, Helm1 = 1, Helm2 = 2, Bald = 3 };
const char* hairStyleName(HairStyle style);

/// FROM THE BINARY: the remaining tag ids the attachment pass reads off an
/// item's Actor (`D3_CHARACTER_DRESSING_RE.md` section 6).
inline constexpr u32 kTagItemHoldType = 0x10081;   ///< how a weapon is held
inline constexpr u32 kTagItemHairStyle = 0x10404;  ///< helm -> HairStyle
inline constexpr u32 kTagUsePerClassArt = 0x17020; ///< gates the 0x17xxx pairs

/// FROM THE BINARY (`ActorModel_GetItemAttachModelSno`, retail `0x750BD0`):
/// the per-class, per-gender art tag. A switch, NOT `0x17000 + 2*class` --
/// DemonHunter (class 0) takes `0x17008` and Barbarian is the default at
/// `0x17000`. `+1` selects the FEMALE art: measured on the corpus
/// (`Helm_hell_base_01.acr` tag 0x17000 -> Helm_barbM..., 0x17001 ->
/// Helm_barbF...), which also pins the runtime's gender flag as
/// nonzero-for-female -- the OPPOSITE of this header's `Gender` enum, so the
/// conversion lives here and nowhere else.
u32 perClassArtTag(PlayerClass cls, Gender gender, bool secondary);

/// What an item's TYPE contributes to the attachment path. The engine tests
/// ItemType FLAG BITS (`Item_GetAttachHardpointName`, retail `0xAB9060`); a
/// 2.8 client ships no ItemTypes records, so the flags cannot be read from
/// data and are RECONSTRUCTED per type name instead -- each field names the
/// flag it stands in for. `itemTypeTraits()` maps the known type-name gbids;
/// a type it does not know contributes nothing, which degrades to the
/// by-slot hardpoint defaults, not to a crash.
struct ItemTypeTraits {
    bool neverDraw = false;     ///< flag 47: equips, but has no world model
    bool shield = false;        ///< flag 26: HP_shield / HP_sheath_Shield
    bool offhandOnly = false;   ///< flag 51: never normalised into the right hand
    bool helm = false;          ///< flag 25: slot 0 -> HP_helm
    bool shoulder = false;      ///< flag 48: HP_left_shoulderPad (+ the twin)
    bool sheathAtHip = false;   ///< flag 9 family: sheaths at the hip, not the back
    bool sheathHidden = false;  ///< flags 52/92/134: carried but not drawn sheathed
    bool ignoreSheathe = false; ///< flag 77: drawn in hand even when sheathed
};

/// The traits of a known item type, by its name's `gbidHash`. INFERENCE where
/// it goes beyond the RE report's flag list (hip-sheathing daggers and hand
/// crossbows, vanishing fist weapons -- in-game behaviour, not decompiled
/// bits); the table is deliberately small and every row is a name the shipped
/// `gbidItemType` values crack to.
ItemTypeTraits itemTypeTraits(u32 gbidItemType);

/// Which class an item TYPE is locked to, or nullopt for a type any class can
/// wear. The class lives nowhere else client-side (no Hero SNOs ship, the
/// ItemTypes tree with its flags went server-side): the game's own encoding is
/// the type NAME -- 44 `<Slot>_<Class>` armour spellings in ItemTypeNames.stl
/// (`ChestArmor_Wizard`, `SpiritStone_Monk`, ...) plus the class-locked base
/// weapon/offhand families (mighty weapons, voodoo masks, wands, flails, ...).
/// The suffix match is spelled by `playerClassName()`; the base-type rows are
/// the game's equip rules, curated -- a type in neither bucket is genuinely
/// class-neutral (rings, generic swords), NOT unknown. Case-insensitive, like
/// every gbid-addressed name.
std::optional<PlayerClass> playerClassFromTypeName(std::string_view typeName);

/// The class of a SET the client data cannot class on its own. Exactly six
/// shipped sets are class-locked in game while every member is generic-typed
/// (verified member-by-member on the 2.8 snapshot); everything else derives
/// from its members' types and must not be listed here. Keyed on the
/// ItemSets.stl KEY (`Ninja_Set_x1`), not the display name.
std::optional<PlayerClass> playerClassForSetKey(std::string_view setKey);

/// FROM THE BINARY (`Item_GetAttachHardpointName`, retail `0xAB9060`), the
/// whole decision: nullopt = do not draw at all (flag 47); an empty view =
/// carried but not drawn; otherwise the hardpoint to attach at. `holdType` is
/// tag `0x10081`'s value, defaulted to 0 when the tag is absent (the engine's
/// tag-map default).
std::optional<std::string_view> attachHardpointName(u32 holdType, EVisualSlot slot,
                                                    const ItemTypeTraits& traits,
                                                    bool sheathed);

/// FROM THE BINARY (`Hardpoint_IsSheath`, retail `0xAB9840`): case-insensitive
/// against the five `HP_sheath_*` names. A sheathed weapon zeroes its enchant
/// effect level through exactly this test.
bool isSheathHardpoint(std::string_view hardpointName);

/// Everything one equipped item does to the character, resolved the way the
/// engine resolves it (`D3_CHARACTER_DRESSING_RE.md` sections 5 and 6). Pure:
/// reads only the arguments.
struct EquipVisual {
    // Armour slots (Torso/Feet/Hands/Legs): the geoset flip.
    std::optional<u32> lookValue;     ///< tag 0x10400; empty = tag absent
    std::optional<u32> lookNameHash;  ///< tag 0x10401 (Str_Hash33 of the look name)
    // Attachment slots (Head/RightHand/LeftHand/Shoulders): a child model.
    i32 attachActorSno = -1;          ///< per-class art or the item's own Actor
    i32 secondAttachActorSno = -1;    ///< shoulders: the HP_right_shoulderPad twin
    std::string_view hardpoint;       ///< empty = carried, not drawn
    bool drawn = false;               ///< false for flag-47 types and empty hardpoints
    bool secondAttach = false;        ///< slot 6 attaches twice
    // Head only:
    HairStyle hairStyle = HairStyle::Naked; ///< tag 0x10404 off the helm
};
EquipVisual resolveEquip(const Actor& itemActor, const ItemTypeTraits& traits,
                         EVisualSlot slot, PlayerClass cls, Gender gender, bool sheathed);

// ---------------------------------------------------------------------------
// Dyes
// ---------------------------------------------------------------------------

/// FROM THE BINARY (`Render_DrawRenderRecords`, `0x71000F3F50`), exactly:
///
///     bUseDyeType = (dyeType != 0) ? 1.0 : 0.0
///     tintRampUV  = (dyeType >= 2) ? ((dyeType - 2) + 0.5) / 21.0 : 0.0
///
/// So 0 is undyed and 2..22 are twenty-one dye colours sampled at the ROW
/// CENTRES of the `dye_ramp` texture. The 0.5 is what makes it a 21-row
/// palette rather than a gradient -- interpolating between dyes is wrong.
///
/// **1 is HIDDEN, not reserved** (retail corrects the earlier reading):
/// `ActorModel_ApplyLook` (`0x7544A0`) short-circuits `dyeType == 1` to the
/// default (naked) look, and the attachment path groups it with `gbid == -1`
/// -- the "vanishing dye" players use to hide a helmet. A dye of 1 must never
/// reach the shader.
inline constexpr i32 kDyeNone = 0;
inline constexpr i32 kDyeHidden = 1;
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
