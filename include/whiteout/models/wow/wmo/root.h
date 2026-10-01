// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "types.h"

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

/// @brief `MOHD.flags`, named for what the 12.1 client does with each bit
///        (`CMapObj_ReadRootChunks` 0x141A7EC80 and its callers).
enum class RootFlag : u16 {
    None = 0,
    /// Skip AttenTransVerts: transition vertices keep their file colour.
    NoTransitionAttenuation = 0x0001,
    /// FixColorVertexAlpha subtracts no ambient (the "unified" path).
    NoAmbientInVertexColor = 0x0002,
    /// `MOGP.groupLiquid` is a real LiquidType id, not a legacy basic type.
    LiquidTypeFromDb = 0x0004,
    /// FixColorVertexAlpha only rewrites alpha.
    VertexColorAlphaOnly = 0x0008,
    /// The file has LOD group files; `lodCount` says how many levels.
    Lod = 0x0010,
    /// Keep the first exterior group loaded at the lowest LOD levels.
    PinFirstExterior = 0x0020,
    /// Name-addressed textures load with the client's other texture flags.
    TextureNameFlags = 0x0080,
    /// Read by the render LOD code; its effect is not pinned.
    RenderLod100 = 0x0100,
    /// An extra last LOD level the client always strips at load.
    ExtraLodLevel = 0x0200,
};

inline constexpr bool hasFlag(u16 flags, RootFlag flag) {
    return (flags & static_cast<u16>(flag)) != 0;
}

/// `MOHD`, 64 bytes.
struct Header {
    u32 textureCount = 0;
    /// Used by 12.1 only as the stride of GFID's LOD rows. The group count is
    /// MOGI's (or MGI2's) length.
    u32 groupCount = 0;
    u32 portalCount = 0;
    u32 lightCount = 0;
    u32 doodadNameCount = 0;
    u32 doodadDefCount = 0;
    u32 doodadSetCount = 0;
    Color ambientColor;
    u32 wmoId = 0; ///< WMOAreaTable.WMOID
    Box bounds;
    u16 flags = 0; ///< RootFlag
    /// The client reads this as a byte and clamps it to 5.
    u8 lodCount = 0;
    u8 unused = 0;
};
static_assert(sizeof(Header) == 64);

/// `MOMT`, 64 bytes. A texture slot holds a FileDataID, or an offset into
/// MOTX when the file carries one (Root::textureNames).
struct Material {
    u32 flags = 0;     ///< MaterialFlag
    u32 shader = 0;    ///< Index into the client's shader table; see shaderInfo().
    u32 blendMode = 0; ///< 0 opaque, 1 alpha key, above that blended.
    u32 texture0 = 0;
    Color sidnColor;      ///< Self-illumination colour, when MaterialFlag::Sidn.
    Color frameSidnColor; ///< Runtime field the file leaves in; unread.
    u32 texture1 = 0;
    Color diffuseColor; ///< Also the liquid colour of an interior group.
    u32 groundType = 0; ///< TerrainType
    u32 texture2 = 0;
    /// The last six texture slots. Shaders 22–24 read them; the older layout
    /// called the first two "colour2" and "flags2".
    std::array<u32, 6> textureExtra{};

    /// Slot @p i, 0–8, in the order the client loads them.
    u32 texture(u32 i) const {
        switch (i) {
        case 0:
            return texture0;
        case 1:
            return texture1;
        case 2:
            return texture2;
        default:
            return i < 9 ? textureExtra[i - 3] : 0;
        }
    }
};
static_assert(sizeof(Material) == 64);

enum class MaterialFlag : u32 {
    None = 0,
    Unlit = 0x001,
    Unfogged = 0x002,
    TwoSided = 0x004,
    ExteriorLight = 0x008,
    Sidn = 0x010, ///< Self-illuminated by day/night: sidnColor applies.
    Window = 0x020,
    ClampS = 0x040,
    ClampT = 0x080,
    /// Stops the client's shadow-batch merge at this batch.
    NoShadowMerge = 0x100,
};

inline constexpr bool hasFlag(u32 flags, MaterialFlag flag) {
    return (flags & static_cast<u32>(flag)) != 0;
}

/// `MOUV`, one per material: two texture layers' scroll speeds.
struct MaterialUvAnimation {
    Vector2f speed0{0.0f, 0.0f};
    Vector2f speed1{0.0f, 0.0f};
};
static_assert(sizeof(MaterialUvAnimation) == 16);

/// `MOMX`, one per material: a colour the shader-23 branch reads, and a key
/// that, where a placement carries a texture-override set, picks the
/// material's textures from it. The key sits unaligned at +4.
struct MaterialExtension {
    Color color;
    std::array<u32, 2> key{};
    u32 unknown = 0;

    u64 overrideKey() const {
        return static_cast<u64>(key[0]) | (static_cast<u64>(key[1]) << 32);
    }
};
static_assert(sizeof(MaterialExtension) == 16);

/// `MOGI`, 32 bytes.
struct GroupInfo {
    u32 flags = 0; ///< The group's MOGP flags; see GroupFlag.
    Box bounds;
    i32 nameOffset = -1; ///< Into MOGN, or -1.
};
static_assert(sizeof(GroupInfo) == 32);

/// `MGI2`, 8 bytes, one per group.
struct GroupInfo2 {
    u32 flags2 = 0; ///< The group's MOGP flags2.
    /// The highest LOD level this group has a file for.
    u8 lodIndex = 0;
    std::array<u8, 3> pad{};
};
static_assert(sizeof(GroupInfo2) == 8);

/// `MOPT`, 20 bytes.
struct Portal {
    u16 startVertex = 0; ///< Into Root::portalVertices.
    u16 vertexCount = 0;
    Plane plane;
};
static_assert(sizeof(Portal) == 20);

/// `MOPR`, 8 bytes.
struct PortalRef {
    u16 portalIndex = 0;
    u16 groupIndex = 0; ///< The group on the other side.
    i16 side = 0;
    /// 0x1: every camera query skips it (`CWorldMap_LocateViewer`,
    /// `CMapObj_DistFromClosestExtPortal`); 0x20: the cull offsets its plane by
    /// the portal's MOPE entry.
    u16 flags = 0;
};
static_assert(sizeof(PortalRef) == 8);

/// `MOPE`, 16 bytes: the plane offsets the portal cull uses for a MOPR
/// reference flagged 0x20.
struct PortalExtra {
    u32 portalIndex = 0;
    std::array<u32, 3> unknown{};
};
static_assert(sizeof(PortalExtra) == 16);

/// `MOVB`, 4 bytes: a run of MOVV.
struct VisibleBlock {
    u16 firstVertex = 0;
    u16 count = 0;
};
static_assert(sizeof(VisibleBlock) == 4);

/// `MOLT`, 48 bytes.
struct Light {
    u8 type = 0; ///< 0 omni, 1 spot, 2 directional, 3 ambient
    u8 useAttenuation = 0;
    std::array<u8, 2> pad{};
    Color color;
    Vector3f position{0.0f, 0.0f, 0.0f};
    f32 intensity = 0.0f;
    Quaternion rotation{};
    f32 attenuationStart = 0.0f;
    f32 attenuationEnd = 0.0f;
};
static_assert(sizeof(Light) == 48);

/// `MNLD`, 184 bytes: the Shadowlands-era lights.
struct NewLight {
    i32 type = 0; ///< 0 point, 1 spot
    i32 lightIndex = 0;
    i32 flags = 0; ///< 0x1 blend inner to outer colour, 0x2 casts shadows
    i32 doodadSet = 0;
    Color innerColor;
    Vector3f position{0.0f, 0.0f, 0.0f};
    Vector3f rotation{0.0f, 0.0f, 0.0f}; ///< Euler, radians.
    f32 attenuationStart = 0.0f;
    f32 attenuationEnd = 0.0f;
    f32 intensity = 0.0f;
    Color outerColor;
    f32 blendStart = 0.0f;
    f32 blendEnd = 0.0f;
    u32 gap0 = 0;
    f32 flickerIntensity = 0.0f;
    f32 flickerSpeed = 0.0f;
    i32 flickerMode = 0; ///< 0 off, 1 sine, 2 noise, 3 noise step
    Vector3f field54{0.0f, 0.0f, 0.0f};
    u32 gap1 = 0;
    u32 cookieFileId = 0; ///< +100; a cube map for a point light.
    std::array<u32, 5> gap2{};
    f32 falloff = 0.0f;
    f32 innerAngle = 0.0f;
    f32 outerAngle = 0.0f;
    u16 scaleHalf = 0;               ///< Half float.
    u16 intensityMultiplierHalf = 0; ///< Half float; 0 reads as 1.
    std::array<u32, 11> unused{};
};
static_assert(sizeof(NewLight) == 184);

/// `MODS`, 32 bytes.
struct DoodadSet {
    std::array<char, 20> name{};
    u32 startIndex = 0; ///< Into Root::doodadDefs.
    u32 count = 0;
    u32 pad = 0;

    std::string_view nameView() const {
        std::size_t n = 0;
        while (n < name.size() && name[n] != '\0')
            ++n;
        return {name.data(), n};
    }
};
static_assert(sizeof(DoodadSet) == 32);

/// `MODD`, 40 bytes.
struct DoodadDef {
    /// Low 24 bits: a byte offset into MODN when the file has one, else an
    /// index into MODI. High 8 bits: DoodadFlag.
    u32 nameAndFlags = 0;
    Vector3f position{0.0f, 0.0f, 0.0f};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    f32 scale = 1.0f;
    /// Tints the doodad; with DoodadFlag::LightFromMolt, alpha is a MOLT index.
    Color color;

    u32 nameIndex() const {
        return nameAndFlags & 0x00FFFFFFu;
    }
    u8 flags() const {
        return static_cast<u8>(nameAndFlags >> 24);
    }
};
static_assert(sizeof(DoodadDef) == 40);

enum class DoodadFlag : u8 {
    AcceptProjectedTextures = 0x01,
    InteriorLighting = 0x02,
    /// `color.a` names the MOLT light that lights it.
    LightFromMolt = 0x04,
    /// `color` is used as it stands.
    LightFromColor = 0x08,
    Flag10 = 0x10,
    Flag20 = 0x20,
    Flag40 = 0x40,
    Flag80 = 0x80,
};

/// One fog setting: above water, or under it.
struct FogParams {
    f32 end = 0.0f;
    f32 startScalar = 0.0f; ///< The start is `end * startScalar`.
    Color color;
};
static_assert(sizeof(FogParams) == 12);

/// `MFOG`, 48 bytes.
struct Fog {
    u32 flags = 0; ///< 0x1 infinite radius (skipped by the query)
    Vector3f position{0.0f, 0.0f, 0.0f};
    f32 radiusStart = 0.0f;
    f32 radiusEnd = 0.0f;
    FogParams fog;
    FogParams underwater;
};
static_assert(sizeof(Fog) == 48);

/// `MFOB`, 140 bytes: a fog volume bounded by six planes.
struct FogBox {
    std::array<Plane, 6> planes{}; ///< Inside where `dot(n, p) + d >= 0` for all.
    f32 fadeDistance = 0.0f;
    FogParams fog;
    FogParams underwater;
    u32 flags = 0;
    u16 doodadSetId = 0; ///< 0 applies always, else only while that set is on.
    std::array<u8, 10> pad{};
};
static_assert(sizeof(FogBox) == 140);

/// `MFED`, 16 bytes, parallel to MFOG.
struct FogExtra {
    u16 doodadSetId = 0;
    std::array<u8, 14> pad{};
};
static_assert(sizeof(FogExtra) == 16);

/// `MAVG` / `MAVD`, 48 bytes.
struct AmbientVolume {
    Vector3f position{0.0f, 0.0f, 0.0f};
    f32 start = 0.0f;
    f32 end = 0.0f;
    Color color1;
    Color color2;
    Color color3;
    u32 flags = 0; ///< 0x1: the three colours differ; else color1 for all
    u16 doodadSetId = 0;
    std::array<u8, 10> pad{};
};
static_assert(sizeof(AmbientVolume) == 48);

/// `MBVD`, 128 bytes: an ambient volume bounded by six planes.
struct AmbientBox {
    std::array<Plane, 6> planes{};
    f32 end = 0.0f;
    Color color1;
    Color color2;
    Color color3;
    u32 flags = 0;
    u16 doodadSetId = 0;
    std::array<u8, 10> pad{};
};
static_assert(sizeof(AmbientBox) == 128);

/// `MOLV`, 100 bytes, extending a MOLT light; the client stores it unread.
struct LightExtension {
    struct Term {
        Vector3f direction{0.0f, 0.0f, 0.0f};
        f32 value = 0.0f;
    };
    std::array<Term, 6> terms{};
    std::array<u8, 3> unknown{};
    u8 lightIndex = 0; ///< Into MOLT.
};
static_assert(sizeof(LightExtension) == 100);

/// A record the client stores with a known stride but whose fields nothing in
/// 12.1 decodes: kept as bytes so a file round-trips.
template <std::size_t N>
struct RawRecord {
    std::array<u8, N> bytes{};
};

using ParticulateVolume = RawRecord<4272>; ///< `MPVD`

/// The root file.
struct Root {
    u32 version = 0; ///< MVER; 0 when the file has none.
    Header header;

    /// MOTX, verbatim. Present only in files whose texture slots are offsets
    /// into it rather than FileDataIDs.
    std::optional<std::vector<char>> textureNames;
    std::vector<Material> materials;                   ///< MOMT
    std::vector<MaterialUvAnimation> uvAnimations;     ///< MOUV, by material
    std::vector<MaterialExtension> materialExtensions; ///< MOMX, by material
    /// MOM3, verbatim: an M3SI material block that replaces MOMT when present.
    std::vector<u8> m3Materials;
    /// MOTN: nine MOTX offsets per MOM3 material.
    std::vector<std::array<u32, 9>> m3TextureNames;

    std::vector<char> groupNames;    ///< MOGN, verbatim
    std::vector<GroupInfo> groups;   ///< MOGI
    std::vector<GroupInfo2> groups2; ///< MGI2
    /// MOSB. Empty for the file's empty string; absent without the chunk.
    std::optional<std::string> skyboxName;
    std::optional<u32> skyboxFileId; ///< MOSI

    std::vector<Vector3f> portalVertices;       ///< MOPV
    std::vector<Portal> portals;                ///< MOPT
    std::vector<PortalRef> portalRefs;          ///< MOPR
    std::vector<PortalExtra> portalExtras;      ///< MOPE
    std::vector<Vector3f> visibleBlockVertices; ///< MOVV (12.1 does not read it)
    std::vector<VisibleBlock> visibleBlocks;    ///< MOVB (12.1 does not read it)

    std::vector<Light> lights;                   ///< MOLT
    std::vector<LightExtension> lightExtensions; ///< MOLV
    std::vector<NewLight> newLights;             ///< MNLD

    std::vector<DoodadSet> doodadSets;       ///< MODS
    std::vector<char> doodadNames;           ///< MODN, verbatim
    std::vector<u32> doodadFileIds;          ///< MODI
    std::vector<DoodadDef> doodadDefs;       ///< MODD
    std::vector<f32> doodadColorMultipliers; ///< MDDI, by doodad
    /// MDDL, verbatim: the detail-doodad layers and their per-group placement.
    std::vector<u8> detailDoodads;

    std::vector<Fog> fogs;                             ///< MFOG
    std::vector<FogBox> fogBoxes;                      ///< MFOB
    std::vector<FogExtra> fogExtras;                   ///< MFED, by fog
    std::vector<AmbientVolume> globalAmbients;         ///< MAVG
    std::vector<AmbientVolume> ambientVolumes;         ///< MAVD
    std::vector<AmbientBox> ambientBoxes;              ///< MBVD
    std::vector<ParticulateVolume> particulateVolumes; ///< MPVD
    std::vector<Plane> convexVolumePlanes;             ///< MCVP

    /// GFID: the group files' FileDataIDs, `header.groupCount` per LOD level.
    std::vector<u32> groupFileIds;

    /// The zero-terminated string at @p offset in a verbatim name block, or
    /// empty when the offset is out of range.
    static std::string_view nameAt(const std::vector<char>& block, i64 offset);

    std::string_view groupName(u32 group) const {
        return group < groups.size() ? nameAt(groupNames, groups[group].nameOffset) : "";
    }
    std::string_view textureName(u32 offset) const {
        return textureNames ? nameAt(*textureNames, offset) : "";
    }

    /// How many groups the client makes: MOGI's length, unless MGI2 came later
    /// in the file (both write one count, and the last chunk wins).
    u32 groupCount() const {
        return groupCountFromMgi2 ? static_cast<u32>(groups2.size())
                                  : static_cast<u32>(groups.size());
    }
    bool groupCountFromMgi2 = false;

    /// The group file for @p group at LOD @p lod: `GFID[g + nGroups·lod]`, or
    /// 0 when the file names none.
    u32 groupFileId(u32 group, u32 lod) const {
        const u64 at = static_cast<u64>(group) + static_cast<u64>(header.groupCount) * lod;
        return at < groupFileIds.size() ? groupFileIds[static_cast<std::size_t>(at)] : 0;
    }
};

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
