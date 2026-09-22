// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file converters.h
 * @brief The three shipped format converters (WEM v3, design §14).
 *
 * Each one has a byte-level entry point from `FormatConverter` and a **typed**
 * one taking the parsed format struct. The typed entry point is the real
 * interface: `.m2` import needs a sibling bundle that bytes alone cannot supply,
 * and every converter benefits from a caller that has already parsed once.
 *
 * `D3Converter` is P6 and lands in its own header — it needs an `AssetSource`,
 * which none of these do.
 */

#include <optional>
#include <span>
#include <vector>

#include "../gltf/gltf.h"
#include "../m2/structures.h"
#include "../m3/structures.h"
#include "../mdx/structures.h"
#include "../mdx/types.h"
#include "converter_base.h"
#include "native/mdx_native.h"
#include "skinning/quantize.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// MdxConverter
// ============================================================================

/// One edit to a Warcraft III material, as its whole new native block
/// (EDIT_MODE_MATERIALS_DESIGN.md §6.1).
struct MaterialBlockEdit {
    u32 model = 0;
    ProfileId profile = ProfileId::Wc3Classic; ///< `Wc3Classic` or `Wc3Reforged`.
    u32 material = kInvalidIndex;              ///< In that set's `materials`.
    native::MdxMaterial block;                 ///< The whole new block.
    /// Old block layer -> new, `kInvalidIndex` for a removed one. Empty is the
    /// identity: a field edit that moved no layer.
    std::vector<u32> layerRemap;
};

struct MaterialBlockResult {
    bool ok = false;
    /// Layer channels whose ordinal moved with their layer.
    u32 channelsRemapped = 0;
    /// Channels whose layer or feature is gone, each with a diagnostic.
    u32 channelsInvalidated = 0;
    Diagnostics diagnostics;
};

/// A geoset's static colour and alpha as the Mesh workspace edits them
/// (EDIT_MODE_MESH_DESIGN.md §6.2). Hidden and alpha are two values: the file
/// writes 0 for a hidden geoset, and the document keeps its alpha for when it
/// is shown again.
struct GeosetTint {
    Vector3f color{1.0f, 1.0f, 1.0f}; ///< Red first, as the file's static colour.
    f32 alpha = 1.0f;                 ///< The opacity when shown.
    bool hidden = false;              ///< `SectionFlags::Hidden`: written as alpha 0.

    bool operator==(const GeosetTint&) const = default;
};

/// A geoset's two flag words as the Mesh workspace edits them
/// (EDIT_MODE_MESH_DESIGN.md §6.3): every bit, named or not, so one nobody has
/// named can still be reached and still round-trips.
struct GeosetFlags {
    /// The geoset's `selectionFlags`. 0x4 is Unselectable: the game's click
    /// ray skips the geoset (`IModelTestRay`).
    u32 selection = 0;
    /// Its geoset animation's flags. 0x1 is `DropShadow` (the section's
    /// `ProjectedShadow`), 0x2 `Color`, which every record the export makes
    /// carries; the rest are kept as the file had them.
    u32 animation = 0x2;

    static constexpr u32 kUnselectable = 0x4;
    static constexpr u32 kDropShadow = 0x1;

    bool operator==(const GeosetFlags&) const = default;
};

/// A native block and the document textures its making appended — which the
/// caller records so an undo can pop exactly those.
struct MaterialBlockDraft {
    native::MdxMaterial block;
    u32 texturesAppended = 0;
};

/// Where `toMdx` puts what the document names, for a caller that has to find a
/// document node or clip in the `.mdx` it built (EDIT_MODE_ANIMATIONS_DESIGN.md
/// §4.1). `toMdx` numbers from this same answer, so the two cannot disagree.
struct MdxExportMap {
    /// Per node: the `objectId` `toMdx` gives it; `kInvalidIndex` for a node
    /// with none (a camera).
    std::vector<u32> nodeObjectId;
    /// Per `Document::clips` entry: its index in `sequences`; `kInvalidIndex`
    /// for a global loop or another model's clip.
    std::vector<u32> clipSequence;
    /// Per mesh: the geosets `toMdx` writes for it, in order. One per section;
    /// a mesh with no sections still writes one (EDIT_MODE_MESH_DESIGN.md §4.4).
    std::vector<std::vector<u32>> geosetsOfMesh;
};

/// @ref MdxExportMap for `document.models[model]` written as @p profile,
/// computed without exporting. Empty when the document has no such model.
MdxExportMap MdxExportMapOf(const Document& document, u32 model, ProfileId profile);

/// Per geoset `toMdx` writes for `document.models[model]` (numbered as
/// `MdxExportMap::geosetsOfMesh` numbers them): the WEM vertex of each of its
/// vertices, in the geoset's own order (EDIT_MODE_SKIN_DESIGN.md §12.4). Built
/// by the code the export slices geosets with, so there is one answer. It
/// builds every mesh's render view, so a host asks at a rebuild, not per frame.
std::vector<std::vector<u32>> MdxGeosetVertices(const Document& document, u32 model,
                                                ProfileId profile);

/// One influence as a Warcraft III file holds it (EDIT_MODE_SKIN_DESIGN.md §12.3).
struct WrittenInfluence {
    u32 node = 0;   ///< Global index into `Model::nodes`.
    f32 weight = 0; ///< What the game blends with: the byte / 255, or 1/k in a group.
    u8 byte = 0;    ///< The `SKIN` byte; 0 in a classic group.

    bool operator==(const WrittenInfluence&) const = default;
};

/// One geoset's skin as the file holds it.
struct WrittenGeosetSkin {
    /// Per geoset vertex, its WEM vertex: `MdxGeosetVertices`'s numbering.
    std::vector<u32> vertices;
    /// Per geoset vertex, what it binds, heaviest first. Empty only in a geoset
    /// where no vertex binds a node the file writes.
    std::vector<std::vector<WrittenInfluence>> influences;
    /// Geoset vertices that bound nothing and were given the first bound
    /// vertex's heaviest node, ascending (both encodings name a bone for every
    /// vertex of a skinned geoset): what the file holds for them is invented.
    std::vector<u32> unbound;
    /// Classic only: the Skin Quantizer's groups and counts.
    skinning::ClassicSkin classic;
};

/// One mesh's skin as the file holds it: `MdxConverter::writtenSkin`.
struct WrittenSkin {
    bool classic = false; ///< Matrix groups, not `SKIN`.
    /// The mesh's geosets, in `MdxExportMap::geosetsOfMesh` order.
    std::vector<WrittenGeosetSkin> geosets;
    /// Vertices with more influences than the encoding holds: folded to four
    /// for `SKIN`, the heaviest eight kept for a group.
    u32 overLimit = 0;
    Diagnostics diagnostics;
};

/// The `.mdx` version a file for @p profile is written at: 800 for classic, and
/// for Reforged 1800 — Warcraft III 3.0's, the version every file it ships is
/// at — and never older, since v1300 and v1600 hold a light's shadow range and
/// falloff and v1400 a skin past 256 bones. Zero for a profile that is not
/// Warcraft III.
u32 MdxFileVersion(ProfileId profile);

/// Makes @p document writable as an `.mdx` FILE of @p version. At v800 and
/// below — the classic game, which has no PopcornFX and no `CORN` chunk — every
/// `Wc3CornEmitter` becomes its placement, a helper, and its channels other than
/// its transform go with their keys; one `NodeKindNotCarried` warning says how
/// many. Returns that count (0 above v800).
///
/// For a file only, and so not inside `toMdx`: the in-memory view of a 3.0
/// model's SD look is `Wc3Classic` drawn at v800, and runs its `CORN` as the
/// game does. Before `toMdx` rather than after, because the helper has to take
/// its object id in `HELP`'s place, or whatever hangs off the emitter would
/// name an id the file never writes.
u32 RetireNodesUnwritableAt(Document& document, u32 version, Diagnostics& out);

/**
 * @brief Warcraft III `.mdx`, both directions, serving both WC3 profiles.
 *
 * One import produces up to **two** material sets from one file by partitioning
 * layers on §7.2.1's HD test. A section whose material has layers for only one
 * profile gets a mask naming only that profile, which is what makes a
 * classic-only file produce no Reforged set at all rather than an empty one.
 */
class MdxConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<Document> importFromBytes(std::span<const u8> data) const override;
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<Document> fromMdx(const mdx::Model& source) const;

    /// @p profile picks which set's materials are written and which sections
    /// are drawn; @p targetVersion is the `.mdx` version stamped on the result,
    /// and 0 is @p profile's own (`MdxFileVersion`).
    ///
    /// The skin is `writtenSkin`'s: the Skin Quantizer's groups at v800 and
    /// below, `SKIN` above. @p skinAs `Wc3Classic` writes the classic groups
    /// above v800 too, and no `SKIN`: everything else stays @p profile's. That
    /// is how an editor shows the classic file in a Reforged session
    /// (EDIT_MODE_SKIN_DESIGN.md §12.6); the export dialog never sets it.
    ///
    /// Fails, naming the mesh, when a geoset's skin cannot be written as asked:
    /// a `SKIN` palette past 256 bones below v1400, or classic pins that alone
    /// need more than 256 groups.
    Result<mdx::Model> toMdx(const Document& document, ProfileId profile,
                             u32 targetVersion = 0,
                             std::optional<ProfileId> skinAs = std::nullopt) const;

    /**
     * @brief What the file holds for each vertex of @p mesh (EDIT_MODE_SKIN_DESIGN.md
     *        §12.3): the one answer `toMdx` writes and an editor shows.
     *
     * The skin is @p skinAs's, @p profile's own when absent: the Skin
     * Quantizer's group with the mesh's `classicBones` pins for `Wc3Classic`,
     * and otherwise `SKIN`, folded to four over the skeleton and quantised to
     * bytes that sum to 255. It answers for the version each profile is written
     * at -- Classic at v800, Reforged above -- and so for every file a host
     * writes. A vertex binds only nodes the file writes, and a duplicate bone is
     * merged.
     */
    WrittenSkin writtenSkin(const Document& document, u32 model, ProfileId profile, u32 mesh,
                            std::optional<ProfileId> skinAs = std::nullopt) const;

    // ---- Editing a Warcraft III material (EDIT_MODE_MATERIALS_DESIGN.md §6) ----
    //
    // An editor writes a material's NATIVE block, and the common view is
    // re-derived from it by the import's own projection, so the material stays
    // `InSync` and `toMdx` writes the block as it stands. Here rather than in a
    // free function because the projection is this converter's private core.

    /**
     * @brief Replace one material's native block and re-derive its common view.
     *
     * 1. Refuses (nothing written) a block holding a layer of the other
     *    family: the import keeps only the profile's layers and would drop it
     *    without a word.
     * 2. Projects the block through the import with the context `fromMdx`
     *    had: the document's textures by identity, the block's own version.
     *    The material keeps its name.
     * 3. Carries the features. A re-derived feature takes the id of the old
     *    one of its kind on the same layer (through @ref MaterialBlockEdit::
     *    layerRemap); a new one takes an id no feature or channel has used.
     *    Every `UvAnimation` feature is carried, its layer remapped, and so is
     *    any feature a channel joins on — typing a keyed fresnel's strength to
     *    zero keeps its feature, refreshed from the block. A feature whose
     *    layer was removed goes, and the channels joining it are invalidated.
     * 4. Remaps the `MaterialLayer` channels on every slot bound to the
     *    material: old ordinal -> old layer -> `layerRemap` -> new layer -> new
     *    ordinal, invalidating one whose layer is gone.
     */
    MaterialBlockResult setMaterialBlock(Document& document, const MaterialBlockEdit& edit) const;

    /**
     * @brief The plain default material of @p profile over document texture
     *        @p colourMap, at @p sourceVersion.
     *
     * `Wc3Classic`: one opaque `SD` layer, the colour map where a block of that
     * version keeps it (`textureId` at v800, `subTextures[0]` from v900).
     * `Wc3Reforged`: one opaque `HD` layer over six sub-textures in slot order,
     * the colour map first and Warcraft III's stock neutral in every other
     * slot, interned into `document.textures` (see @ref internStockTexture).
     */
    MaterialBlockDraft defaultMaterialBlock(Document& document, ProfileId profile, u32 colourMap,
                                            u32 sourceVersion) const;

    /// The document texture holding Warcraft III's stock neutral for @p slot,
    /// appended when no entry has the same path (compared case- and
    /// slash-insensitively) and replaceable id. `kInvalidIndex` for a slot the
    /// game has no neutral for. @p appended, when given, says whether it was.
    u32 internStockTexture(Document& document, mdx::Layer::SlotType slot,
                           bool* appended = nullptr) const;

    // ---- A geoset's static colour and alpha (EDIT_MODE_MESH_DESIGN.md §6.2) ----
    //
    // The section keeps them in its native bag and flags, in the import's own
    // representation, so an edit back to the imported value compares equal.
    // `fromMdx` and `toMdx` go through these two as well: the bag keys are
    // this converter's vocabulary and are spelled nowhere else.

    /// White, 1 and shown for a section that carries none of them.
    GeosetTint geosetTint(const MeshSection& section) const;

    /// Writes @p tint as the import would have. White and an alpha of 1 carry
    /// no bag entry. An alpha of 0 or less is Hidden, and leaves the stored
    /// alpha as it was, so showing the geoset again gives its opacity back.
    void setGeosetTint(MeshSection& section, const GeosetTint& tint) const;

    /// The section's two flag words; `selection` 0 and `animation` `Color`
    /// for a section that carries neither.
    GeosetFlags geosetFlags(const MeshSection& section) const;

    /// Writes @p flags as the import would have: `DropShadow` as the section's
    /// `ProjectedShadow`, the rest of the animation word in the bag only when
    /// it is not plain `Color`.
    void setGeosetFlags(MeshSection& section, const GeosetFlags& flags) const;

    /**
     * @brief What `toMdx` would say writing @p mesh, as one of
     *        `document.models[model]`'s, at @p profile and @p targetVersion —
     *        without writing it.
     *
     * The same code path the export runs per geoset (the render view, the
     * vertex slice, the skin encoding), on a mesh that need not be in the
     * document yet: a merge asks it of a trial merge (EDIT_MODE_MESH_DESIGN.md
     * §7.2). The limits that matter come back by code, as warnings, because the
     * export itself writes on past them: `IndexWidthExceeded` (more than 65,536
     * vertices in one geoset, whose indices are then truncated) and
     * `BonePaletteLimit`: at v800, where the groups are the skin, a warning
     * that the Skin Quantizer merged groups past 256; above it, an error for a
     * palette of more than 256 bones below v1400, which the export refuses.
     * @p writtenVertices, when given, receives the vertices the geosets hold
     * after the split at seams.
     */
    Diagnostics checkGeoset(const Document& document, u32 model, const Mesh& mesh,
                            ProfileId profile, u32 targetVersion,
                            u32* writtenVertices = nullptr) const;
};

// ============================================================================
// M2Converter
// ============================================================================

/**
 * @brief World of Warcraft `.m2`.
 *
 * `importFromBytes` is unsupported and says so: an `.m2` is a bundle — the base
 * file carries no geometry batches at all, those live in the `.skin` files — so
 * the typed entry point taking an already-assembled `m2::Model` is the only
 * honest byte-free interface. Export writes the base `.m2` only.
 */
class M2Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    /// @p sourceVersion is the `.m2` header version the bundle was read from.
    /// `m2::Model` does not carry it — the parser consumes it and keeps nothing
    /// — but the native block records it, so the caller has to say.
    Result<Document> fromM2(const m2::Model& source, u32 sourceVersion = 274) const;
    Result<m2::Model> toM2(const Document& document, ProfileId profile,
                           u32 targetVersion = 274) const;
};

// ============================================================================
// M3Converter
// ============================================================================

/**
 * @brief StarCraft II / Heroes of the Storm `.m3`.
 *
 * The profile follows the `MODL` version — v30+ is Heroes, where `MADD` is the
 * load-time truth — and a caller that knows better can override it. Geometry
 * arrives in SC2's basis and is rebased into WEM's canonical space on import
 * (§6.2); the rebase is an axis permutation with determinant +1, so it is
 * bit-exact and preserves winding.
 */
/// What a caller may tell `toM3` beyond the document
/// (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5).
struct M3ExportSettings {
    /// Open a composite section for every pass the one material can only
    /// approximate, instead of folding it with a diagnostic.
    bool exactPasses = false;
    /// Per `Document::textures` entry, an `m3_core::TextureAlphaClass` byte;
    /// empty when nobody decoded the textures.
    std::vector<u8> textureAlphaClasses;
    /// Warcraft III's node carriers (WC3_TO_SC2_COMPLETION_PLAN.md §2.3).
    ///
    /// Every particle and ribbon emitter node becomes a bone -- but a second
    /// record of its parent's system, an identity child that keys nothing and
    /// shares its parent's visibility, which rides the parent's -- and so does
    /// an attachment, light or camera whose transform or visibility is keyed;
    /// a light's and a camera's transform tracks ride that bone. A node whose
    /// visibility is keyed and that has children gets a `<name>_Vis` leaf
    /// bone carrying the visibility alone, because an `.m3` bone hides its
    /// whole subtree and a Warcraft III node hides only itself.
    ///
    /// Off, `toM3` writes what it always has: an `.m3`-imported emitter node
    /// has no transform and no channel, and a StarCraft II visibility IS
    /// hierarchical, so no other source wants it.
    bool effectNodeBones = false;
};

/// A node native key: this node draws only while its parent does, so a
/// visibility on the parent needs no leaf bone to keep it off this node --
/// the helper an effect crossing plants under its emitter says so.
inline constexpr const char* kNodeSharesParentVisibility = "sharesParentVisibility";

/// Where `toM3` put what the document named, for a caller that adds records
/// beside the ones WEM carries (WC3_TO_SC2_COMPLETION_PLAN.md §2.2).
struct M3ExportMap {
    /// Per node: the bone its records ride -- its own, or the nearest ancestor's
    /// when it got none. `kInvalidIndex` when nothing above it is a bone.
    std::vector<u32> nodeBone;
    /// Per node: the bone that carries its visibility, and so the bone its
    /// record points at -- the `_Vis` leaf where there is one, else `nodeBone`.
    std::vector<u32> nodeVisBone;
    /// Per `Document::clips` entry: the SEQS it became, `kInvalidIndex` for a
    /// clip of another model.
    std::vector<u32> clipSequence;
    /// Per SEQS: the STC_ its clip's first container became -- the one that
    /// holds the clip's events, and the one a crossed property joins.
    std::vector<u32> sequenceStc;
    /// The first animId no stream or AnimRef the export wrote uses.
    u32 nextAnimId = 1;
};

class M3Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    Result<Document> importFromBytes(std::span<const u8> data) const override;
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    /// The profile `fromM3` picks for a model of @p modelVersion.
    static ProfileId ProfileForVersion(u32 modelVersion);

    /// @p profileOverride of `ProfileId::Count` means "decide from the version".
    Result<Document> fromM3(const m3::Model& source,
                            ProfileId profileOverride = ProfileId::Count) const;
    /// @p targetVersion of 0 means the newest MODL @p profile's own game reads:
    /// v29 for StarCraft II, v30 for Heroes of the Storm. The two ranges are
    /// the difference between the games — a v30 file is one StarCraft II
    /// refuses, and `ProfileForVersion` reads it back as Heroes.
    /// @p map, when given, receives where the document's nodes and clips
    /// landed (`M3ExportMap`).
    Result<m3::Model> toM3(const Document& document, ProfileId profile, u32 targetVersion = 0,
                           const M3ExportSettings& settings = {},
                           M3ExportMap* map = nullptr) const;

    /**
     * @brief Merges an external animation file (`.m3a`) into an imported model.
     *
     * Nothing in a `.m3` names its `.m3a` — the caller decides which pair up —
     * and the join inside is the **animId** alone. The merge therefore adds
     * clips and containers that reference channels @p document already declares;
     * a channel the base model never declared is skipped rather than invented,
     * because an external file can supply new motion for a target but not a new
     * target (§10.8.1).
     *
     * @return how many clips were added, or an error when @p model is not in the
     *         document.
     */
    Result<u32> mergeAnimation(Document& document, u32 model, const m3::Model& external) const;
};

// ============================================================================
// GltfConverter
// ============================================================================

/// What `toGltf` may leave out. Rescaling is **not** here — a caller that wants
/// another scale runs `RescaleDocument` on a staged copy first, the same
/// division of labour as the MDX and M3 export drivers.
struct GltfWriteOptions {
    /// Drop meshes above the base level of detail, matching the other exporters.
    bool baseLodOnly = true;
    /// Parent a model referenced by an `AttachmentPayload` under its attach
    /// point — how a D3 actor's child models ride along (§7).
    bool bakeChildModels = true;
};

/**
 * @brief glTF 2.0 / GLB — the first crossing outside the Blizzard family
 *        (GLTF_DESIGN).
 *
 * glTF is not a game, so it brings **no profile of its own**: import produces a
 * `Generic`-profile document with a `PBRDeferred` set (`NativeSync::Absent`,
 * there being no native block to record), and export accepts any profile the
 * document carries, lowering that set to metallic-roughness with declared loss.
 * `profiles()` therefore answers `{Generic}` — the first converter ever to
 * serve it, which §6.1 designed it for; "`Generic` is a source, never a
 * `DeriveProfile` target" is untouched.
 *
 * The basis change (Blizzard +X-forward/+Z-up ⇄ glTF +Z-forward/+Y-up) is a
 * pure cyclic permutation baked into the data at this boundary; neither
 * `CoordSpace` enum grows a value for it.
 */
class GltfConverter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    /// Sniffs the GLB magic; anything else is taken as `.gltf` JSON text.
    Result<Document> importFromBytes(std::span<const u8> data) const override;

    /// A self-contained `.glb`: images stay URI references (the library never
    /// sees a pixel — embedding is the app driver's job), geometry is the BIN
    /// chunk.
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    Result<Document> fromGltf(const gltf::Asset& source) const;
    Result<gltf::Asset> toGltf(const Document& document, ProfileId profile,
                               const GltfWriteOptions& options = {}) const;
};

/// Registers the built-in converters. Called by `ConverterRegistry`'s
/// constructor; exposed so a host that builds its own registry can reuse it.
void RegisterBuiltinConverters(ConverterRegistry& registry);

} // namespace wem
} // namespace models
} // namespace whiteout
