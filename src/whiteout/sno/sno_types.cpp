// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/sno_types.h>

#include <cstring>

namespace whiteout {
namespace sno {

namespace {

constexpr i32 kMinSnoGroup = static_cast<i32>(SnoGroup::Unknown);
constexpr i32 kMaxSnoGroupInclusive = static_cast<i32>(SnoGroup::Indicator);

} // namespace

const char* snoGroupName(SnoGroup group) {
    switch (group) {
    case SnoGroup::Unknown:
        return "Unknown";
    case SnoGroup::Code:
        return "Code";
    case SnoGroup::None:
        return "None";
    case SnoGroup::Actor:
        return "Actor";
    case SnoGroup::NpcComponentSet:
        return "NpcComponentSet";
    case SnoGroup::AiBehavior:
        return "AiBehavior";
    case SnoGroup::AiState:
        return "AiState";
    case SnoGroup::AmbientSound:
        return "AmbientSound";
    case SnoGroup::Animation:
        return "Animation";
    case SnoGroup::Animation2D:
        return "Animation2D";
    case SnoGroup::AnimSet:
        return "AnimSet";
    case SnoGroup::Appearance:
        return "Appearance";
    case SnoGroup::Hero:
        return "Hero";
    case SnoGroup::Cloth:
        return "Cloth";
    case SnoGroup::Conversation:
        return "Conversation";
    case SnoGroup::ConversationList:
        return "ConversationList";
    case SnoGroup::EffectGroup:
        return "EffectGroup";
    case SnoGroup::Encounter:
        return "Encounter";
    case SnoGroup::Explosion:
        return "Explosion";
    case SnoGroup::FlagSet:
        return "FlagSet";
    case SnoGroup::Font:
        return "Font";
    case SnoGroup::GameBalance:
        return "GameBalance";
    case SnoGroup::Global:
        return "Global";
    case SnoGroup::LevelArea:
        return "LevelArea";
    case SnoGroup::Light:
        return "Light";
    case SnoGroup::MarkerSet:
        return "MarkerSet";
    case SnoGroup::Observer:
        return "Observer";
    case SnoGroup::Particle:
        return "Particle";
    case SnoGroup::Physics:
        return "Physics";
    case SnoGroup::PhysMesh:
        return "PhysMesh";
    case SnoGroup::Power:
        return "Power";
    case SnoGroup::Quest:
        return "Quest";
    case SnoGroup::Rope:
        return "Rope";
    case SnoGroup::Scene:
        return "Scene";
    case SnoGroup::Script:
        return "Script";
    case SnoGroup::ShaderMap:
        return "ShaderMap";
    case SnoGroup::Shader:
        return "Shader";
    case SnoGroup::Shake:
        return "Shake";
    case SnoGroup::SkillKit:
        return "SkillKit";
    case SnoGroup::Sound:
        return "Sound";
    case SnoGroup::StringList:
        return "StringList";
    case SnoGroup::Surface:
        return "Surface";
    case SnoGroup::Texture:
        return "Texture";
    case SnoGroup::Trail:
        return "Trail";
    case SnoGroup::UI:
        return "UI";
    case SnoGroup::Weather:
        return "Weather";
    case SnoGroup::World:
        return "World";
    case SnoGroup::Recipe:
        return "Recipe";
    case SnoGroup::Condition:
        return "Condition";
    case SnoGroup::TreasureClass:
        return "TreasureClass";
    case SnoGroup::Account:
        return "Account";
    case SnoGroup::Material:
        return "Material";
    case SnoGroup::Lore:
        return "Lore";
    case SnoGroup::Reverb:
        return "Reverb";
    case SnoGroup::Music:
        return "Music";
    case SnoGroup::Tutorial:
        return "Tutorial";
    case SnoGroup::ControlScheme:
        return "ControlScheme";
    case SnoGroup::AnimTree:
        return "AnimTree";
    case SnoGroup::Vibration:
        return "Vibration";
    case SnoGroup::wWiseSoundBank:
        return "wWiseSoundBank";
    case SnoGroup::Speaker:
        return "Speaker";
    case SnoGroup::Item:
        return "Item";
    case SnoGroup::PlayerClass:
        return "PlayerClass";
    case SnoGroup::FogVolume:
        return "FogVolume";
    case SnoGroup::Biome:
        return "Biome";
    case SnoGroup::Wall:
        return "Wall";
    case SnoGroup::SoundTable:
        return "SoundTable";
    case SnoGroup::SubZone:
        return "SubZone";
    case SnoGroup::MaterialValue:
        return "MaterialValue";
    case SnoGroup::MonsterFamily:
        return "MonsterFamily";
    case SnoGroup::TileSet:
        return "TileSet";
    case SnoGroup::Population:
        return "Population";
    case SnoGroup::MaterialValueSet:
        return "MaterialValueSet";
    case SnoGroup::WorldState:
        return "WorldState";
    case SnoGroup::Schedule:
        return "Schedule";
    case SnoGroup::VectorField:
        return "VectorField";
    case SnoGroup::PvPMode:
        return "PvPMode";
    case SnoGroup::StoryBoard:
        return "StoryBoard";
    case SnoGroup::POI:
        return "POI";
    case SnoGroup::Territory:
        return "Territory";
    case SnoGroup::AudioContext:
        return "AudioContext";
    case SnoGroup::VoProcess:
        return "VoProcess";
    case SnoGroup::DemonScroll:
        return "DemonScroll";
    case SnoGroup::QuestChain:
        return "QuestChain";
    case SnoGroup::LoudnessPreset:
        return "LoudnessPreset";
    case SnoGroup::ItemType:
        return "ItemType";
    case SnoGroup::Achievement:
        return "Achievement";
    case SnoGroup::Crafter:
        return "Crafter";
    case SnoGroup::HoudiniParticlesSim:
        return "HoudiniParticlesSim";
    case SnoGroup::Movie:
        return "Movie";
    case SnoGroup::TiledStyle:
        return "TiledStyle";
    case SnoGroup::Affix:
        return "Affix";
    case SnoGroup::Reputation:
        return "Reputation";
    case SnoGroup::ParagonNode:
        return "ParagonNode";
    case SnoGroup::MonsterAffix:
        return "MonsterAffix";
    case SnoGroup::ParagonBoard:
        return "ParagonBoard";
    case SnoGroup::SetItemBonus:
        return "SetItemBonus";
    case SnoGroup::StoreProduct:
        return "StoreProduct";
    case SnoGroup::ParagonGlyph:
        return "ParagonGlyph";
    case SnoGroup::ParagonGlyphAffix:
        return "ParagonGlyphAffix";
    case SnoGroup::Challenge:
        return "Challenge";
    case SnoGroup::MarkingShape:
        return "MarkingShape";
    case SnoGroup::ItemRequirement:
        return "ItemRequirement";
    case SnoGroup::Boost:
        return "Boost";
    case SnoGroup::Emote:
        return "Emote";
    case SnoGroup::Jewelry:
        return "Jewelry";
    case SnoGroup::PlayerTitle:
        return "PlayerTitle";
    case SnoGroup::Emblem:
        return "Emblem";
    case SnoGroup::Dye:
        return "Dye";
    case SnoGroup::FogOfWar:
        return "FogOfWar";
    case SnoGroup::ParagonThreshold:
        return "ParagonThreshold";
    case SnoGroup::AiAwareness:
        return "AiAwareness";
    case SnoGroup::TrackedReward:
        return "TrackedReward";
    case SnoGroup::CollisionSettings:
        return "CollisionSettings";
    case SnoGroup::Aspect:
        return "Aspect";
    case SnoGroup::AbTest:
        return "AbTest";
    case SnoGroup::Stagger:
        return "Stagger";
    case SnoGroup::EyeColor:
        return "EyeColor";
    case SnoGroup::Makeup:
        return "Makeup";
    case SnoGroup::MarkingColor:
        return "MarkingColor";
    case SnoGroup::HairColor:
        return "HairColor";
    case SnoGroup::DungeonAffix:
        return "DungeonAffix";
    case SnoGroup::Activity:
        return "Activity";
    case SnoGroup::Season:
        return "Season";
    case SnoGroup::HairStyle:
        return "HairStyle";
    case SnoGroup::FacialHair:
        return "FacialHair";
    case SnoGroup::Face:
        return "Face";
    case SnoGroup::MercenaryClass:
        return "MercenaryClass";
    case SnoGroup::PassivePowerContainer:
        return "PassivePowerContainer";
    case SnoGroup::MountProfile:
        return "MountProfile";
    case SnoGroup::AICoordinator:
        return "AICoordinator";
    case SnoGroup::CrafterTab:
        return "CrafterTab";
    case SnoGroup::TownPortalCosmetic:
        return "TownPortalCosmetic";
    case SnoGroup::AxeTest:
        return "AxeTest";
    case SnoGroup::Wizard:
        return "Wizard";
    case SnoGroup::FootstepTable:
        return "FootstepTable";
    case SnoGroup::Modal:
        return "Modal";
    case SnoGroup::CollectiblePower:
        return "CollectiblePower";
    case SnoGroup::AppearanceSet:
        return "AppearanceSet";
    case SnoGroup::Preset:
        return "Preset";
    case SnoGroup::PreviewComposition:
        return "PreviewComposition";
    case SnoGroup::SpawnPool:
        return "SpawnPool";
    case SnoGroup::Raid:
        return "Raid";
    case SnoGroup::BattlePassTier:
        return "BattlePassTier";
    case SnoGroup::Zone:
        return "Zone";
    case SnoGroup::DeathKit:
        return "DeathKit";
    case SnoGroup::Snippet:
        return "Snippet";
    case SnoGroup::CommunityModifier:
        return "CommunityModifier";
    case SnoGroup::GenericNodeGraph:
        return "GenericNodeGraph";
    case SnoGroup::UserDefinedData:
        return "UserDefinedData";
    case SnoGroup::DataStore:
        return "DataStore";
    case SnoGroup::BehaviorContainer:
        return "BehaviorContainer";
    case SnoGroup::ActorService:
        return "ActorService";
    case SnoGroup::DamageRemap:
        return "DamageRemap";
    case SnoGroup::Vendor:
        return "Vendor";
    case SnoGroup::GenericSkillTree:
        return "GenericSkillTree";
    case SnoGroup::Crowd:
        return "Crowd";
    case SnoGroup::VisualRemap:
        return "VisualRemap";
    case SnoGroup::PowerModifier:
        return "PowerModifier";
    case SnoGroup::UIDesignerNotification:
        return "UIDesignerNotification";
    case SnoGroup::HoudiniDigitalAsset:
        return "HoudiniDigitalAsset";
    case SnoGroup::HoudiniDigitalAssetPreset:
        return "HoudiniDigitalAssetPreset";
    case SnoGroup::Indicator:
        return "Indicator";
    default:
        return "Unknown";
    }
}

SnoGroup snoGroupFromName(const char* name) {
    if (!name)
        return SnoGroup::None;
    // Iterate all known groups and compare names.
    for (i32 i = kMinSnoGroup; i <= kMaxSnoGroupInclusive; ++i) {
        const char* n = snoGroupName(static_cast<SnoGroup>(i));
        if (n && std::strcmp(n, name) == 0)
            return static_cast<SnoGroup>(i);
    }
    return SnoGroup::None;
}

const char* snoGroupDir(SnoGroup group) {
    return snoGroupName(group);
}

const char* snoGroupDirD3(SnoGroup group) {
    // D3 CASC uses different directory names for 8 groups.
    // See docs/CASC_ROOT_FORMATS.md and CascLib's Diablo3 root handler.
    switch (group) {
    case SnoGroup::NpcComponentSet:
        return "Adventure"; // ID 2 — D3 "Adventure" vs D4 "NpcComponentSet"
    case SnoGroup::Animation:
        return "Anim"; // ID 6
    case SnoGroup::Animation2D:
        return "Anim2D"; // ID 7
    case SnoGroup::Global:
        return "Globals"; // ID 21
    case SnoGroup::Shader:
        return "Shaders"; // ID 37
    case SnoGroup::Shake:
        return "Shakes"; // ID 38
    case SnoGroup::Texture:
        return "Textures"; // ID 44
    case SnoGroup::World:
        return "Worlds"; // ID 48
    default:
        return snoGroupDir(group);
    }
}

const char* snoGroupExtension(SnoGroup group) {
    switch (group) {
    case SnoGroup::Actor:
        return "acr";
    case SnoGroup::NpcComponentSet:
        return "npc";
    case SnoGroup::AiBehavior:
        return "aib";
    case SnoGroup::AiState:
        return "ais";
    case SnoGroup::AmbientSound:
        return "ams";
    case SnoGroup::Animation:
        return "ani";
    case SnoGroup::Animation2D:
        return "an2";
    case SnoGroup::AnimSet:
        return "ans";
    case SnoGroup::Appearance:
        return "app";
    case SnoGroup::Hero:
        return "hro";
    case SnoGroup::Cloth:
        return "clt";
    case SnoGroup::Conversation:
        return "cnv";
    case SnoGroup::ConversationList:
        return "cnl";
    case SnoGroup::EffectGroup:
        return "efg";
    case SnoGroup::Encounter:
        return "enc";
    case SnoGroup::Explosion:
        return "xpl";
    case SnoGroup::FlagSet:
        return "flg";
    case SnoGroup::Font:
        return "fnt";
    case SnoGroup::GameBalance:
        return "gam";
    case SnoGroup::Global:
        return "glo";
    case SnoGroup::LevelArea:
        return "lvl";
    case SnoGroup::Light:
        return "lit";
    case SnoGroup::MarkerSet:
        return "mrk";
    case SnoGroup::Observer:
        return "obs";
    case SnoGroup::Particle:
        return "prt";
    case SnoGroup::Physics:
        return "phy";
    case SnoGroup::PhysMesh:
        return "phm";
    case SnoGroup::Power:
        return "pow";
    case SnoGroup::Quest:
        return "qst";
    case SnoGroup::Rope:
        return "rop";
    case SnoGroup::Scene:
        return "scn";
    case SnoGroup::Script:
        return "scr";
    case SnoGroup::ShaderMap:
        return "shm";
    case SnoGroup::Shader:
        return "shd";
    case SnoGroup::Shake:
        return "shk";
    case SnoGroup::SkillKit:
        return "skl";
    case SnoGroup::Sound:
        return "snd";
    case SnoGroup::StringList:
        return "stl";
    case SnoGroup::Surface:
        return "srf";
    case SnoGroup::Texture:
        return "tex";
    case SnoGroup::Trail:
        return "trl";
    case SnoGroup::UI:
        return "ui";
    case SnoGroup::Weather:
        return "wth";
    case SnoGroup::World:
        return "wrl";
    case SnoGroup::Recipe:
        return "rcp";
    case SnoGroup::Condition:
        return "cnd";
    case SnoGroup::TreasureClass:
        return "trs";
    case SnoGroup::Account:
        return "acc";
    case SnoGroup::Material:
        return "mat";
    case SnoGroup::Lore:
        return "lor";
    case SnoGroup::Reverb:
        return "rev";
    case SnoGroup::Music:
        return "mus";
    case SnoGroup::Tutorial:
        return "tut";
    case SnoGroup::ControlScheme:
        return "ctr";
    case SnoGroup::AnimTree:
        return "ant";
    case SnoGroup::Vibration:
        return "vib";
    case SnoGroup::wWiseSoundBank:
        return "wsb";
    case SnoGroup::Speaker:
        return "spk";
    case SnoGroup::Item:
        return "itm";
    case SnoGroup::PlayerClass:
        return "plc";
    case SnoGroup::FogVolume:
        return "fog";
    case SnoGroup::Biome:
        return "bio";
    case SnoGroup::Wall:
        return "wal";
    case SnoGroup::SoundTable:
        return "sdt";
    case SnoGroup::SubZone:
        return "sbz";
    case SnoGroup::MaterialValue:
        return "mtv";
    case SnoGroup::MonsterFamily:
        return "mfm";
    case SnoGroup::TileSet:
        return "tst";
    case SnoGroup::Population:
        return "pop";
    case SnoGroup::MaterialValueSet:
        return "mvs";
    case SnoGroup::WorldState:
        return "wst";
    case SnoGroup::Schedule:
        return "sch";
    case SnoGroup::VectorField:
        return "vfd";
    case SnoGroup::PvPMode:
        return "pvp";
    case SnoGroup::StoryBoard:
        return "stb";
    case SnoGroup::POI:
        return "poi";
    case SnoGroup::Territory:
        return "ter";
    case SnoGroup::AudioContext:
        return "auc";
    case SnoGroup::VoProcess:
        return "vop";
    case SnoGroup::DemonScroll:
        return "dss";
    case SnoGroup::QuestChain:
        return "qc";
    case SnoGroup::LoudnessPreset:
        return "lou";
    case SnoGroup::ItemType:
        return "itt";
    case SnoGroup::Achievement:
        return "ach";
    case SnoGroup::Crafter:
        return "crf";
    case SnoGroup::HoudiniParticlesSim:
        return "hps";
    case SnoGroup::Movie:
        return "vid";
    case SnoGroup::TiledStyle:
        return "tsl";
    case SnoGroup::Affix:
        return "aff";
    case SnoGroup::Reputation:
        return "rep";
    case SnoGroup::ParagonNode:
        return "pgn";
    case SnoGroup::MonsterAffix:
        return "maf";
    case SnoGroup::ParagonBoard:
        return "pbd";
    case SnoGroup::SetItemBonus:
        return "set";
    case SnoGroup::StoreProduct:
        return "prd";
    case SnoGroup::ParagonGlyph:
        return "gph";
    case SnoGroup::ParagonGlyphAffix:
        return "gaf";
    case SnoGroup::Challenge:
        return "cha";
    case SnoGroup::MarkingShape:
        return "msh";
    case SnoGroup::ItemRequirement:
        return "irq";
    case SnoGroup::Boost:
        return "bst";
    case SnoGroup::Emote:
        return "emo";
    case SnoGroup::Jewelry:
        return "jwl";
    case SnoGroup::PlayerTitle:
        return "pt";
    case SnoGroup::Emblem:
        return "emb";
    case SnoGroup::Dye:
        return "dye";
    case SnoGroup::FogOfWar:
        return "fow";
    case SnoGroup::ParagonThreshold:
        return "pth";
    case SnoGroup::AiAwareness:
        return "aia";
    case SnoGroup::TrackedReward:
        return "trd";
    case SnoGroup::CollisionSettings:
        return "col";
    case SnoGroup::Aspect:
        return "asp";
    case SnoGroup::AbTest:
        return "abt";
    case SnoGroup::Stagger:
        return "stg";
    case SnoGroup::EyeColor:
        return "eye";
    case SnoGroup::Makeup:
        return "mak";
    case SnoGroup::MarkingColor:
        return "mcl";
    case SnoGroup::HairColor:
        return "hcl";
    case SnoGroup::DungeonAffix:
        return "dax";
    case SnoGroup::Activity:
        return "act";
    case SnoGroup::Season:
        return "sea";
    case SnoGroup::HairStyle:
        return "har";
    case SnoGroup::FacialHair:
        return "fhr";
    case SnoGroup::Face:
        return "fac";
    case SnoGroup::MercenaryClass:
        return "mrc";
    case SnoGroup::PassivePowerContainer:
        return "ppc";
    case SnoGroup::MountProfile:
        return "mpp";
    case SnoGroup::AICoordinator:
        return "aic";
    case SnoGroup::CrafterTab:
        return "ctb";
    case SnoGroup::TownPortalCosmetic:
        return "tpc";
    case SnoGroup::AxeTest:
        return "axe";
    case SnoGroup::Wizard:
        return "wiz";
    case SnoGroup::FootstepTable:
        return "fst";
    case SnoGroup::Modal:
        return "mdl";
    case SnoGroup::CollectiblePower:
        return "cpw";
    case SnoGroup::AppearanceSet:
        return "aps";
    case SnoGroup::Preset:
        return "pst";
    case SnoGroup::PreviewComposition:
        return "pvc";
    case SnoGroup::SpawnPool:
        return "spn";
    case SnoGroup::Raid:
        return "rdx";
    case SnoGroup::BattlePassTier:
        return "bpt";
    case SnoGroup::Zone:
        return "zon";
    case SnoGroup::DeathKit:
        return "dtk";
    case SnoGroup::Snippet:
        return "snp";
    case SnoGroup::CommunityModifier:
        return "cmo";
    case SnoGroup::GenericNodeGraph:
        return "gng";
    case SnoGroup::UserDefinedData:
        return "udd";
    case SnoGroup::DataStore:
        return "fds";
    case SnoGroup::BehaviorContainer:
        return "bvr";
    case SnoGroup::ActorService:
        return "asv";
    case SnoGroup::DamageRemap:
        return "dmg";
    case SnoGroup::Vendor:
        return "vnd";
    case SnoGroup::GenericSkillTree:
        return "gst";
    case SnoGroup::Crowd:
        return "crd";
    case SnoGroup::VisualRemap:
        return "vrm";
    case SnoGroup::PowerModifier:
        return "pmd";
    case SnoGroup::UIDesignerNotification:
        return "udn";
    case SnoGroup::HoudiniDigitalAsset:
        return "hds";
    case SnoGroup::HoudiniDigitalAssetPreset:
        return "hdp";
    case SnoGroup::Indicator:
        return "ind";
    default:
        return nullptr;
    }
}

SnoGroup snoGroupFromExtension(const char* ext) {
    if (!ext || !ext[0])
        return SnoGroup::None;

    // Iterate all known group IDs and match by extension.
    static constexpr i32 kGroupIds[] = {
        1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  17,  18,  19,
        20,  21,  22,  23,  24,  26,  27,  28,  29,  30,  31,  32,  33,  35,  36,  37,  38,  39,
        40,  42,  43,  44,  45,  46,  47,  48,  49,  51,  52,  53,  57,  59,  60,  62,  63,  65,
        67,  68,  71,  72,  73,  74,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,
        88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105,
        106, 107, 108, 109, 110, 111, 112, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124,
        125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142,
        143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 160, 161,
        162, 163, 164, 165, 166, 167, 168, 169, 170, 172, 175, 176, 177, 178, 179, 180};

    for (i32 const id : kGroupIds) {
        auto group = static_cast<SnoGroup>(id);
        const char* groupExt = snoGroupExtension(group);
        if (groupExt && std::strcmp(groupExt, ext) == 0)
            return group;
    }
    return SnoGroup::None;
}

} // namespace sno
} // namespace whiteout
