// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

// What the WoW 12.1 client derives from a WMO at load and at draw time, as
// pure functions over the parsed file. The parser keeps the file as written;
// these reproduce the client's reading of it, addresses cited per function.

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "group.h"
#include "parser.h"
#include "root.h"

namespace whiteout {
namespace models {
namespace wow {
namespace wmo {

/// One row of `s_wmoShaderMetaData` (0x1444440C0).
struct ShaderInfo {
    u8 textureCount = 1;
    u8 uvSets = 1;
    u8 colorSets = 1;
};

/// The client knows shaders 0–25.
constexpr u32 kShaderCount = 26;

/// Shader @p shader's row; shader 0's for an index past the table.
ShaderInfo shaderInfo(u32 shader);

/// The shader a material draws with: its own, or Opaque (4) when a texture
/// its shader needs is missing — except shaders 21 and 23, which keep theirs
/// (`CMapObj_CreateMaterial` 0x141A7FF90).
u32 effectiveShader(const Root& root, const Material& material);

/// Whether texture slot @p slot of @p material names a texture: a FileDataID
/// other than 0, or with MOTX a non-empty name at an offset inside it.
bool hasTexture(const Root& root, const Material& material, u32 slot);

/// Shaders 10, 14 and 16 are not ordinary batches: the first two draw through
/// the window effect, the last through terrain blending.
inline constexpr bool isSpecialShader(u32 shader) {
    return shader == 10 || shader == 14 || shader == 16;
}

/// Whether 12.1 draws any batch of @p group (`CMapObjGroup_Create`
/// 0x141A80B00): antiportals, no-render groups and attachment meshes have their
/// batch counts zeroed. Their doodads are still made.
bool groupDraws(const Root& root, const Group& group);

/// Whether @p batch draws as an ordinary batch: it names a material record
/// (`CMapObjGroup_Create`) whose shader is not a special one.
bool batchDraws(const Root& root, const Batch& batch);

/// The highest LOD level the client loads: none without RootFlag::Lod, else
/// `lodCount - 1` (2 when the count is 0), less the extra level it strips.
u32 maxLodLevel(const Root& root);

/// Whether group @p group has a file at LOD @p lod: LOD 0 always; above it
/// only with MOGI flag 0x400, and with MGI2 only up to its `lodIndex`.
bool groupHasLod(const Root& root, u32 group, u32 lod);

/// 12.1's LOD context for a root (`CMapObj_AllocGroups` 0x141A7F370,
/// `CMapObj_AssignLodGroups` 0x141A7F920).
struct LodContext {
    u32 maxLod = 0;             ///< maxLodLevel()
    u32 special20 = 0xFF;       ///< MOHD 0x20: the far proxy's level
    u32 special200 = 0xFF;      ///< MOHD 0x200 kept past the strip (maxLod 0): its level
    u32 firstExterior = 0xFFFF; ///< MOHD & 0x220: the first group with MOGI 0x8
    /// Per group, the highest level with an object of its own. A group that is
    /// neither an LOD group (MOGI 0x400) nor pinned (antiportal, flags2 0x100)
    /// has 1 and a placeholder there, so it vanishes past the first threshold.
    std::vector<u32> lodIndex;
    /// `CMapObj+0x3E1 & 4`: a lone light group whose level is picked per draw
    /// from the root's box.
    bool smallPath = false;
};
LodContext lodContext(const Root& root);

/// `WmoLodCtx_ComputeThresholds` 0x141B33030: the distance, in the WMO's yards,
/// each level starts at: 0, B·s, 1.75·B·s, 3·B·s, 4·B·s with
/// `B = wmoLodDistScale·wmoLodDist` and `s` the placement's byte / 128 (raised
/// until `wmoLodDist·s ≥ 200`), a far proxy's at @p horizonDistance, each at
/// least 66.666664 past the last.
std::array<f32, 5> lodThresholds(const Root& root, const LodContext& context, f32 horizonDistance,
                                 u8 placementLodScale = 128, f32 wmoLodDist = 300.0f,
                                 f32 wmoLodDistScale = 1.0f);

/// `WmoLodCtx_DistanceLod` 0x141B34060: the highest level up to @p cap whose
/// threshold the distance from @p camera to @p box reaches, at most
/// @p maxLodClamp. Within a yard of the box the squared distance is used.
u32 distanceLod(const Box& box, const Vector3f& camera, const std::array<f32, 5>& thresholds,
                u32 cap, u32 maxLodClamp = 4);

/// One update of a placement's levels (`WmoLodCtx_UpdatePlacementLods`
/// 0x141B33530, `WmoLodCtx_GroupTargetLod` 0x141B33F50) for a camera at
/// @p camera in WMO space.
struct LodTargets {
    u32 wmoLod = 0;
    /// Per group. A level with no object of the group's own is a placeholder,
    /// which draws nothing.
    std::vector<u32> target;
    /// The emissive fade's distance, `(int)t[1]`; 0xFFFF without LOD.
    u16 sdfd = 0xFFFF;
};
LodTargets lodTargets(const Model& model, const LodContext& context, const Vector3f& camera,
                      bool cameraInside, f32 horizonDistance, u8 placementLodScale = 128,
                      u32 maxLodClamp = 4);

/// Whether @p group has an object of its own at @p lod: LOD 0, a level up to
/// its lodIndex, or the far proxy on the first exterior group.
bool lodHasObject(const Root& root, const LodContext& context, u32 group, u32 lod);

/// The uber program's emissive fade (cb1[8].xy) for a group fade distance
/// @p sdfd: `(−s, s·0.99d)` with `s = 1/(0.99d − 0.85d)`, and (0, 1) for 0.
Vector2f emissiveFade(u16 sdfd);

/// How many leading vertices are transition vertices
/// (`CMapObjGroup_GetTransVertexMax` 0x141A83020, plus one): all of them with
/// GroupFlag::Lod, else up to the highest vertex a transition batch uses.
u32 transitionVertexCount(const Group& group);

/// `CMapObj_AttenTransVerts` (0x141A83480): fade the transition vertices of
/// @p colors (the group's colour set 0) toward the exterior groups their
/// portals lead to. A no-op with RootFlag::NoTransitionAttenuation or with no
/// transition batches.
void attenuateTransitionVertices(const Root& root, const Group& group, std::vector<Color>& colors);

/// `CMapObjGroup_FixColorVertexAlpha` (0x141A830A0): take the root ambient
/// out of colour set 0, halve it, and set the non-transition vertices' alpha
/// to 255 for an exterior group and 0 otherwise.
void fixVertexColorAlpha(const Root& root, const Group& group, std::vector<Color>& colors);

/// Colour set 0 as the group holds it after load (`group+0x2C0`): through
/// attenuateTransitionVertices, with transition batches, then
/// fixVertexColorAlpha. Empty without one.
std::vector<Color> loadedColorSet0(const Root& root, const Group& group);

/// The three per-vertex colours of the client's 68-byte vertex
/// (`CMapObjGroup_FillVertexBuffer` 0x141B133E0), in RGBA byte order.
struct VertexColors {
    std::vector<u32> color0; ///< Set 0 after both fix-ups; (0,0,0,255) without one.
    std::vector<u32> color1; ///< Set 1; (0,0,0,255) without one.
    std::vector<u32> color2; ///< MOC2; 0 without one.
};

/// Every vertex's three colours as the client uploads them: set 0 through
/// attenuateTransitionVertices and fixVertexColorAlpha, alpha forced to 255
/// on an interior, exterior-lit group; set 1 and MOC2 as they stand.
VertexColors vertexColors(const Root& root, const Group& group);

/// 12.1's CRandom (`CRandom_Seed` 0x143679C50, `CRandom_NextU32` 0x1402DE090),
/// the generator behind the detail doodads.
class CRandom {
public:
    explicit CRandom(u32 seed);
    u32 next();

private:
    u32 acc_ = 0;
    u32 index_ = 0; ///< four byte offsets into the table, cycling mod 47, 53, 59 and 61
};

/// One MDDL layer (`CMapObjDetailDoodadLayer`).
struct DetailDoodadLayer {
    struct Entry {
        u32 id = 0; ///< a GroundEffectDoodad row in the low 24 bits; 0 an empty slot
        u8 weight = 0;
    };
    u8 threshold = 0; ///< a location stays iff threshold >= (r >> 20) % 24
    std::vector<Entry> entries;
    u32 totalWeight = 0;
};

/// MDDL as 12.1 parses it (`CMapObjDetailDoodadData_ParseMDDL` 0x14368E110).
struct DetailDoodadData {
    f32 density = 1.0f;
    u8 flags = 0; ///< 1: the draw keeps the placement's tilt
    std::vector<DetailDoodadLayer> layers;
    /// Per group, where its placement stream starts in Root::detailDoodads, or -1.
    std::vector<i64> blocks;
};

/// Empty where 12.1 discards the chunk: absent, malformed, or without layers.
std::optional<DetailDoodadData> parseDetailDoodads(const Root& root);

/// One detail doodad (`CMapObjDetailDoodadLoc`): a triangle of the group, its
/// 16-bit barycentric weights, and the layer entry's id.
struct DetailDoodadLoc {
    u32 triangle = 0;
    u16 u = 0;
    u16 v = 0;
    u32 id = 0;
};

/// A group's detail doodads as 12.1 generates them (`CMapObjGroup_DetailDoodadJob`
/// 0x141A80960): slot counts from the triangle areas, the layers placed from the
/// group's block, then the client's own sort by id and a hash of (u, v).
/// @p group is the LOD-0 file; empty without a block.
std::vector<DetailDoodadLoc> detailDoodadLocs(const Root& root, const DetailDoodadData& data,
                                              const Group& group, u32 groupIndex);

/// WmoMaxScale's cap (`CMapObjGroup_ApplyWmoMaxScale` 0x141A80410): with a
/// @p maxScale below the density only the first count·(maxScale/density)² stay,
/// so whole higher ids drop first. Returns the detail scale.
f32 capDetailDoodads(std::vector<DetailDoodadLoc>& locs, f32 density, std::optional<f32> maxScale);

/// The GroundEffectDoodad fields an instance reads (names from their use).
struct DetailDoodadModel {
    bool alignToNormal = false; ///< field 1 bit 0
    f32 amount = 0.0f;          ///< field 3: instance byte 3
    f32 minScale = 1.0f;        ///< field 4
    f32 maxScale = 1.0f;        ///< field 5
    f32 yawMin = 0.0f;          ///< field 6, degrees
    f32 yawMax = 0.0f;          ///< field 7, degrees
};

/// One instance as 12.1 packs it (`CDetailDoodadBatch_FillWmoInstances` 0x141AB8B60).
struct DetailDoodadInstance {
    std::array<u8, 3> normal{}; ///< the triangle's, (n + 1)·127.5
    u8 amount = 0;
    std::array<u8, 4> fixed{}; ///< 7F 7F 7F FF
    std::array<u8, 4> color{}; ///< colour set 0 blended, RGBA; 00 00 00 FF without one
    Vector3f position;         ///< group space
    std::array<u8, 3> axis{};  ///< the rotation axis, (a + 1)·127.5
    u8 angle = 0;              ///< the rotation angle·255/2π, wrapping
    u8 scale = 0;              ///< scale·127.5
    u8 sign = 0;               ///< a random ±[0, 1)·255
    u16 pad = 0;
};
static_assert(sizeof(DetailDoodadInstance) == 32);

/// @p loc's instance, or empty past the group's triangles, where 12.1 stops
/// filling the batch. @p colorSet0 is loadedColorSet0(), or empty.
std::optional<DetailDoodadInstance> detailDoodadInstance(const Group& group, u32 groupIndex,
                                                         std::span<const Color> colorSet0,
                                                         const DetailDoodadLoc& loc,
                                                         const DetailDoodadModel& model);

/// The root's three ambient colours (`CMapObj_ComputeAmbientColors`
/// 0x141A9D530): the first MAVG entry whose non-zero doodad set is on, else
/// MAVG's first; else MAVD's first; else MOHD's colour. An entry without
/// flag 1 gives its first colour three times.
std::array<Color, 3> ambientColors(const Root& root, std::span<const u16> activeDoodadSets);

/// The ambient triple of the placement the camera stands in
/// (`CMapObjDef_UpdateCameraAmbient` 0x141AF1690). The MAVD spheres and MBVD
/// boxes @p cameraGroup lists are weighted at @p camera (WMO space) and topped
/// up with the placement's triple (`ambientColors`) to a whole weight; that
/// fades back to the placement's triple over the last 10 yd before an exterior
/// portal, @p portalDistance, which is 0 when no camera group is interior. A
/// root with neither MAVG nor MAVD keeps the placement's triple.
std::array<Color, 3> cameraAmbientColors(const Model& model, u32 cameraGroup,
                                         const Vector3f& camera, std::span<const u16> activeSets,
                                         f32 portalDistance);

/// The fog of the WMO the camera stands in (`CMapObj_QueryGroupFog`
/// 0x141AF3CE0): MFOG[0], blended at @p camera (WMO space) toward the fogs
/// @p cameraGroup names (MFVR, else its four fog ids). Without MFOG[0] flag
/// 0x1000 each fog is lerped over the result farthest first
/// (`CMapObj_BlendFogsSorted` 0x141AF4930); with it the fogs and the group's
/// MFOB boxes are averaged by weight (`CMapObj_BlendFogsWeighted` 0x141AF3F40).
/// Empty for a lone MFOG entry without 0x1000, or 0x1000 with 0x10000.
std::optional<Fog> cameraFog(const Model& model, u32 cameraGroup, const Vector3f& camera,
                             std::span<const u16> activeSets);

/// Where the camera stands in one WMO (`CWorldMap_LocateViewer` 0x141B6E3E0).
struct CameraLocation {
    /// A surface of this WMO is below the eye, nearer than the limit.
    bool hit = false;
    /// And it is an interior group's.
    bool inside = false;
    /// That group, then an interior group whose portal the eye is within 1/3 yd
    /// of; 0xFFFF for none.
    std::array<u16, 2> groups{0xFFFF, 0xFFFF};
    /// The nearest hit, as a fraction of the drop: what ranks placements.
    f32 t = 1.0f;
};

/// The camera at @p eye, with @p below the point 10000 yd straight down in the
/// world, both in the WMO's space. The nearest surface below the eye names its
/// group: any face of a group whose MOGI box the drop touches (MOGI flags
/// 0x410080 excluded; the BSP, two-sided, no face filter), or a portal the drop
/// crosses no more than 1 yd further, whose eye side then counts. An exterior
/// group (MOGP 0x8) there means the camera is not inside. @p worldScale is
/// world yards per WMO unit, which orders the candidates; a hit must be nearer
/// than @p tLimit (1, or the best another placement found).
CameraLocation locateCamera(const Model& model, const Vector3f& eye, const Vector3f& below,
                            f32 worldScale = 1.0f, f32 tLimit = 1.0f);

/// `CMapObj_DistFromClosestExtPortal` 0x141A83320 over @p location's interior
/// groups (a split child walks from its parent): the distance from @p eye to
/// the nearest portal into an exterior group, walking up to three portals
/// through interior ones. 0 without an interior camera group; FLT_MAX with no
/// such portal within 25.
f32 exteriorPortalDistance(const Model& model, const CameraLocation& location, const Vector3f& eye);

/// One material's MOUV scroll at @p timeMs (`CMapObj_ComputeUVAnim`
/// 0x141B86FE0): per component, the phase of `timeMs` in a period of
/// `1000 / speed` milliseconds, run backwards for a negative speed.
struct UvScroll {
    Vector2f layer0{0.0f, 0.0f};
    Vector2f layer1{0.0f, 0.0f};
    /// False where the material has no MOUV entry or both layers are still.
    bool animated = false;
};
UvScroll uvScroll(const Root& root, u32 material, u32 timeMs);
UvScroll uvScroll(const MaterialUvAnimation& animation, u32 timeMs);

/// The group's LiquidType id (`CMapObjGroup_Create` 0x141A80BC9 and the
/// legacy tile rule in its MLIQ read), before CreateLiquid's interior-water
/// substitution. 0 for none.
u32 groupLiquidType(const Root& root, const Group& group);

/// Whether @p group makes a liquid (`CMapObjDefGroup_CreateLiquid` 0x141B3E140):
/// MOGP 0x1000 on a group that is not a split child (flags2 0x80).
bool hasLiquid(const Group& group);

/// Whether a group's liquid counts as exterior (`CMapObjGroup_CreateLiquid`
/// 0x141B15640): the group is (MOGP 0x48), or @p typeFlags, its LiquidType's
/// Flags, carry 0x200.
bool liquidExterior(const Group& group, u32 typeFlags);

/// The LiquidType a liquid draws with: @p type (groupLiquidType), but basic
/// water (1, 5, 9, 13, 17) in an interior becomes 17.
u32 liquidDrawType(u32 type, bool exterior);

/// `Liquid_GetDepthCurve` 0x141AC5050: a liquid's depth byte to its A/B
/// weight, `c0 + c1·t + c2·t² + c3·t³`. An LVF outside {0, 2, 3, 4} is
/// (0, 1, 0, 1); else Int[0] ≥ 2 takes @p coefficients, 1 is linear, and the
/// rest (0, 6.0714, 0, 0).
std::array<f32, 4> liquidDepthCurve(i32 lvf, u32 int0, const std::array<f32, 4>& coefficients);

/// One liquid vertex as 12.1's formats 13 and 18 carry it, less the colour
/// and its weight, which are the liquid's own.
struct LiquidMeshVertex {
    Vector3f position{0.0f, 0.0f, 0.0f}; ///< WMO space
    Vector2f uv{0.0f, 0.0f};
    f32 depth = 0.0f; ///< min(curve(depth byte / 255), 1)
};

/// `CMapObjGroup_BuildLiquidMesh` 0x141AE5F70 and `CMapObjGroup_EmitSharedLiquidTiles`
/// 0x141AE5970 for @p group of @p model: every grid vertex, stepped from the corner by
/// 4.1666665 a column and a row, at its height; two triangles per tile that
/// has liquid (low nibble not 0xF) and is not shared (0x80), in the order
/// 12.1's strip draws them; then each shared tile, clipped against the plane
/// of every portal whose neighbour's liquid rectangle overlaps it, as its own
/// strip. UVs are the MLIQ `s, t / 256` with @p hasUV (the magma object),
/// else `x, y · 0.06` with @p worldUV (the map's Flags[1] 0x400), else the
/// grid column and row (a shared tile's `(p − corner) · 0.24`). The depth
/// byte is vertex byte 0, the low byte of `s` for a UV mesh.
struct LiquidMesh {
    std::vector<LiquidMeshVertex> vertices;
    std::vector<u32> indices; ///< a triangle list
};
LiquidMesh buildLiquidMesh(const Model& model, u32 group, bool hasUV, bool worldUV,
                           const std::array<f32, 4>& depthCurve);

/// Per group: whether its vertices' colour add reaches full weight on
/// exterior-lit geometry (`group+1161` bit 0, set in `CMapObjGroup_Create`
/// 0x141A80B00). An exterior group (0x48, not a split child) with batches and
/// colour set 0 sets it on itself and on every interior group a portal of its
/// leads to.
std::vector<u8> exteriorAmbientGroups(const Model& model);

/// Group @p group's doodads that are drawn with @p activeSets on: MODR entries
/// in the default set (0) or in an active set, as indices into MODD.
std::vector<u32> activeDoodads(const Root& root, const Group& group,
                               std::span<const u16> activeSets);

/// The set MODS assigns doodad @p doodad to: the last set whose range covers
/// it (sets are laid in order, so a later one overwrites), or 0xFFFF.
u16 doodadSetOf(const Root& root, u32 doodad);

/// A doodad's model: by name when the root has MODN, by FileDataID otherwise.
/// An out-of-range MODI index reads MODI[0], as the client does.
struct DoodadModel {
    std::string_view path;
    u32 fileId = 0;
};
DoodadModel doodadModel(const Root& root, const DoodadDef& def);

/// A group's sun-shadow casters (`CMapObjGroup_BuildShadowBatches` 0x141A819C0).
/// An LOD root's group flagged 0x400, or any with flags2 0x20, keeps its file
/// MOBS. Every other rebuilds it from MOBA: the leading run of batches whose
/// materials blend 0 or 1 without flag 0x100 (the first other one ends the
/// list), neighbours with one key (blend, F_UNCULLED, and the batch itself for
/// blend 1) joined. A record without flag 0x80 is alpha-tested on its
/// material's first texture. Only exterior groups (MOGI 0x48) cast.
std::vector<Batch> shadowBatches(const Root& root, const Group& group);

/// One local light as 12.1 gathers it from a group (`CMapObjGroup_GatherGroup`
/// 0x141B80B30), in WMO space. Colours are BGRA, no gamma.
struct GatheredLight {
    enum class Source : u8 { Molp = 0, Mols = 1, Mop2 = 2, Mos2 = 3, Mnld = 4 };
    Source source = Source::Molp;
    u32 id = 0; ///< The record's id (MNLD +4): the light's key and flicker seed.
    bool spot = false;
    Vector3f position{0.0f, 0.0f, 0.0f};
    /// Euler (x, y, z) radians, applied x first: a spot's axis is local +Z.
    /// MOLP keeps none.
    Vector3f rotation{0.0f, 0.0f, 0.0f};
    /// The colour at the light, and past the blend; the same without MNLD flag 1.
    Color colorA;
    Color colorB;
    f32 attenuationStart = 0.0f;
    f32 attenuationEnd = 0.0f; ///< Also the cull radius.
    f32 intensity = 0.0f;
    /// Where colorA ends and colorB starts (MNLD flag 1), else the attenuation pair.
    f32 blendNear = 0.0f;
    f32 blendFar = 0.0f;
    u32 flags = 0;         ///< MNLD bits 0..3: 1 blend, 2 RT shadow, 4 RT only, 8 keep cookie
    f32 falloff = 0.0f;    ///< A spot's cone exponent.
    f32 innerAngle = 0.0f; ///< Full cone angles, radians.
    f32 outerAngle = 0.0f;
    f32 flickerAmount = 0.0f; ///< Percent.
    f32 flickerSpeed = 0.0f;  ///< As stored; the client scales it by 0.1.
    i16 flickerMode = 0;      ///< 0 none, 1 sine, 2 noise, 3 noise step
    u32 cookieFileId = 0;
};

/// @p group's lights (`CMapObjGroup_GatherGroup`'s gatherers): MOLP, MOLS, MOP2 and MOS2
/// through their set tables (entry 0 always, entry i while set i is active),
/// then MNLR's MNLD records (an out-of-range reference is MNLD[0]) but those
/// of an inactive set, and flag 4's without @p rtShadows. MNLD types past 1
/// are left out, as neither renderer draws them.
std::vector<GatheredLight> gatherLights(const Model& model, u32 group,
                                        std::span<const u16> activeSets, bool rtShadows = false);

/// A light's axis for @p rotation, `R = Rz·Ry·Rx` on +Z: the third row of
/// `C44Matrix_FromEulerXYZ` 0x14367DD30.
Vector3f lightDirection(const Vector3f& rotation);

/// A doodad a placement creates (`CMapObjGroup_CreateDoodadDefs` 0x141B3D960,
/// `CMapObj_CreateDoodadDef` 0x141AC7A50): one per MODD index however many
/// groups name it, in the order the groups first do.
struct PlacedDoodad {
    u32 index = 0; ///< Into MODD.
    u16 set = 0;   ///< Its MODS set.
    /// The first group whose MODR names it, the one its lighting reads.
    u32 group = 0;
    /// Every group that names it is interior (MOGI flags & 0x48 clear), so it
    /// lights by `doodadLighting` instead of the zone
    /// (`CMapObjGroup_CreateDoodadRef` 0x141B3D810).
    bool interior = false;
};

/// The doodads @p model's loaded groups create with @p activeSets on.
std::vector<PlacedDoodad> placedDoodads(const Model& model, std::span<const u16> activeSets);

/// What an interior doodad lights by in place of the zone, as 12.1 stores it
/// (`CMapObj_ComputeDoodadLighting` 0x141AFF7B0, then
/// `CMapObjDef_DoodadAdjustLighting` 0x141AFFDD0 with the camera outside the
/// WMO). `CMapDoodadDef_SelectLights` 0x141AFF200 adds the three ambients to
/// the model's and makes `direct` its one directional light.
struct DoodadLighting {
    Color sky;
    Color horizon;
    Color ground;
    Color direct;
    /// Where the light travels, in the WMO's space.
    Vector3f direction{-0.30822f, -0.30822f, -0.9f};
};

/// MODD entry @p index's lighting. @p boundsMin and @p boundsMax: its
/// model's bounding box as placed in the WMO. @p group: the group that
/// created it. @p ambient: the placement's triple (`ambientColors`).
DoodadLighting doodadLighting(const Root& root, u32 index, const Group& group,
                              const Vector3f& boundsMin, const Vector3f& boundsMax,
                              const std::array<Color, 3>& ambient);

/// 12.1's float-to-byte (0x1436807C0): @p value + 512, its mantissa's integer
/// part, which truncates. Callers pass `unit · 255`.
u8 colorByte(f32 value);

/// `CMapObjDef_DoodadBrightenDirect112` (0x141AFF600): a colour whose largest
/// channel is under 112 has its HSV value raised to 112/255, then returns
/// truncated with alpha 255.
Color brightenDirect(Color c);

/// `CMapObjDef_DoodadClampAmbient96` (0x141AFF710): a colour whose largest
/// channel is over 96 is scaled so it is about 96, alpha kept.
Color clampAmbient(Color c);

/// `CMapObjDef_DoodadAddColorSaturate` (0x141AFFCC0): @p ambient's RGB plus
/// @p add, divided by its largest channel when that is over 1, into
/// @p out's RGB; @p out's alpha is kept.
Color addColorSaturate(Color out, const Vector3f& add, Color ambient);

} // namespace wmo
} // namespace wow
} // namespace models
} // namespace whiteout
