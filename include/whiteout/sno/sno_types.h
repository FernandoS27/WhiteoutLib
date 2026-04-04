// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>

namespace whiteout {
namespace sno {

/// Known SNO group identifiers used in Diablo IV.
/// These come from the CoreTOC.dat and are used to categorise assets.
enum class SnoGroup : i32 {
    Unknown = -3,
    Code = -2,
    None = -1,
    Actor = 1,
    NpcComponentSet = 2,
    AiBehavior = 3,
    AiState = 4,
    AmbientSound = 5,
    Animation = 6,
    Animation2D = 7,
    AnimSet = 8,
    Appearance = 9,
    Hero = 10,
    Cloth = 11,
    Conversation = 12,
    ConversationList = 13,
    EffectGroup = 14,
    Encounter = 15,
    Explosion = 17,
    FlagSet = 18,
    Font = 19,
    GameBalance = 20,
    Global = 21,
    LevelArea = 22,
    Light = 23,
    MarkerSet = 24,
    Observer = 26,
    Particle = 27,
    Physics = 28,
    Power = 29,
    PhysMesh = 30,
    Quest = 31,
    Rope = 32,
    Scene = 33,
    Script = 35,
    ShaderMap = 36,
    Shader = 37,
    Shake = 38,
    SkillKit = 39,
    Sound = 40,
    StringList = 42,
    Surface = 43,
    Texture = 44,
    Trail = 45,
    UI = 46,
    Weather = 47,
    World = 48,
    Recipe = 49,
    Condition = 51,
    TreasureClass = 52,
    Account = 53,
    Material = 57,
    Lore = 59,
    Reverb = 60,
    Music = 62,
    Tutorial = 63,
    ControlScheme = 65,
    AnimTree = 67,
    Vibration = 68,
    wWiseSoundBank = 71,
    Speaker = 72,
    Item = 73,
    PlayerClass = 74,
    FogVolume = 76,
    Biome = 77,
    Wall = 78,
    SoundTable = 79,
    SubZone = 80,
    MaterialValue = 81,
    MonsterFamily = 82,
    TileSet = 83,
    Population = 84,
    MaterialValueSet = 85,
    WorldState = 86,
    Schedule = 87,
    VectorField = 88,
    PvPMode = 89,
    StoryBoard = 90,
    POI = 91,
    Territory = 92,
    AudioContext = 93,
    VoProcess = 94,
    DemonScroll = 95,
    QuestChain = 96,
    LoudnessPreset = 97,
    ItemType = 98,
    Achievement = 99,
    Crafter = 100,
    HoudiniParticlesSim = 101,
    Movie = 102,
    TiledStyle = 103,
    Affix = 104,
    Reputation = 105,
    ParagonNode = 106,
    MonsterAffix = 107,
    ParagonBoard = 108,
    SetItemBonus = 109,
    StoreProduct = 110,
    ParagonGlyph = 111,
    ParagonGlyphAffix = 112,
    Challenge = 114,
    MarkingShape = 115,
    ItemRequirement = 116,
    Boost = 117,
    Emote = 118,
    Jewelry = 119,
    PlayerTitle = 120,
    Emblem = 121,
    Dye = 122,
    FogOfWar = 123,
    ParagonThreshold = 124,
    AiAwareness = 125,
    TrackedReward = 126,
    CollisionSettings = 127,
    Aspect = 128,
    AbTest = 129,
    Stagger = 130,
    EyeColor = 131,
    Makeup = 132,
    MarkingColor = 133,
    HairColor = 134,
    DungeonAffix = 135,
    Activity = 136,
    Season = 137,
    HairStyle = 138,
    FacialHair = 139,
    Face = 140,
    MercenaryClass = 141,
    PassivePowerContainer = 142,
    MountProfile = 143,
    AICoordinator = 144,
    CrafterTab = 145,
    TownPortalCosmetic = 146,
    AxeTest = 147,
    Wizard = 148,
    FootstepTable = 149,
    Modal = 150,
    CollectiblePower = 151,
    AppearanceSet = 152,
    Preset = 153,
    PreviewComposition = 154,
    SpawnPool = 155,
    Raid = 156,
    BattlePassTier = 157,
    Zone = 158,
    DeathKit = 160,
    Snippet = 161,
    CommunityModifier = 162,
    GenericNodeGraph = 163,
    UserDefinedData = 164,
    DataStore = 165,
    BehaviorContainer = 166,
    ActorService = 167,
    DamageRemap = 168,
    Vendor = 169,
    GenericSkillTree = 170,
    Crowd = 172,
    VisualRemap = 175,
    PowerModifier = 176,
    UIDesignerNotification = 177,
    HoudiniDigitalAsset = 178,
    HoudiniDigitalAssetPreset = 179,
    Indicator = 180,
};

/// Returns a human-readable name for an SnoGroup value.
const char* snoGroupName(SnoGroup group);

/// Look up an SnoGroup by its human-readable name (case-sensitive).
/// Returns SnoGroup::None if no match is found.
SnoGroup snoGroupFromName(const char* name);

/// Returns the directory name used in D4 CASC paths for this SNO group
/// (e.g. "Texture", "Actor"). Unknown values map to "Unknown".
const char* snoGroupDir(SnoGroup group);

/// Returns the directory name used in D3 CASC paths for this SNO group.
/// D3 uses different names for several groups (e.g. "Textures" instead of
/// "Texture", "Anim" instead of "Animation"). Falls back to snoGroupDir()
/// for groups that share the same directory name in both games.
const char* snoGroupDirD3(SnoGroup group);

/// Returns the file extension for this SNO group (e.g. "tex", "acr").
/// Returns nullptr for unknown groups.
const char* snoGroupExtension(SnoGroup group);

/// Look up an SnoGroup by its file extension (e.g. "app" → SnoGroup::Appearance).
/// Returns SnoGroup::None if no match is found.
SnoGroup snoGroupFromExtension(const char* ext);

/// SNO file header magic number.
constexpr u32 kSnoMagic = 0xDEADBEEF;

/// SNO file header (first 20 bytes of every SNO file).
struct SnoHeader {
    u32 magic;      ///< Always 0xDEADBEEF
    u32 formatHash; ///< Hash identifying the top-level type
    u32 unknown08;  ///< Unknown / version
    u32 unknown0C;  ///< Unknown
    i32 snoId;      ///< The SNO ID of this asset (at offset 0x10)
};

} // namespace sno
} // namespace whiteout
