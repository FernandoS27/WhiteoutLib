// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file d3_converter.h
 * @brief Diablo III -> `Document` (WEM v3, design §9, §14).
 *
 * ### An actor is one model, and it is the unit
 *
 * D3 ships assets and a join, not objects. An `.acr` owns nothing drawable: it
 * *names* an appearance, an animset, a look, and things riding its hardpoints.
 * An `.app` on its own is therefore **not a model** — it is the parts of one, and
 * a character's `.app` ships every armour variant it could ever wear at once.
 *
 * So the unit here is the actor, and **one actor converts to one `Model`**. What
 * its hardpoints carry converts to `Model`s of their own in the same document,
 * named from the attach point that reaches them (`AttachmentPayload`).
 *
 * ### Which parts, is the caller's call
 *
 * The converter cannot know what a character is wearing — that is an equipped
 * item's data, in assets an `.app` never names. So dressing is an **input**
 * (`D3ImportOptions::wardrobe`), and with none given the naked variant of every
 * armour slot draws. Nothing is dropped either way: every sub-object imports as a
 * `MeshSection` carrying its parsed descriptor, and the choice is expressed as
 * `SectionFlags::Hidden` — so a host can re-dress a loaded document by flipping
 * flags instead of re-importing.
 *
 * The wardrobe is spelled in the **descriptor's** vocabulary (slot, weight,
 * variant), not in the engine's `(look category, look value)` numbering, because
 * that numbering is not pinned to body parts — `character.h` says so — and
 * baking an inference into a format is how it becomes permanent.
 *
 * ### No bytes out, but a model out
 *
 * Writing SNO is a separate project (§18), so `supportsExport()` is false and
 * `exportToBytes` still refuses. What §18 rules out is a *file the game would
 * load* — and that is not what a viewer needs. `toAppearance` builds the native
 * `Appearances` **in memory**, exactly as `toMdx` / `toM2` / `toM3` build their
 * format's struct, which is what lets a Diablo III `.wem` re-open as Diablo III
 * and reach the whole D3 render path — surfaces, dressing, cloth, hardpoints —
 * without one line of it being written against a WEM type.
 *
 * ### Sharing is the norm, and it is conditional
 *
 * 2.24 actors per appearance across the shipped corpus, worst case 594, so
 * `native::loadActorModel` — which re-parses per reference — is explicitly not
 * the entry point here, the same conclusion the renderer reached. `AssetSource`
 * shares the **parse** unconditionally, and that is where the 594x is.
 *
 * A **model** is shared only when the second actor would build the identical
 * thing: same appearance, same look, and neither side contributing an attach
 * point. An actor is one model, so two actors that equip differently are two
 * models — and the first two corpus actors found sharing an appearance are
 * exactly that case, so reusing unconditionally drops real data.
 */

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/types.h>

#include "converter_base.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// AssetSource
// ============================================================================

/**
 * @brief A parse cache over an `AssetProvider`.
 *
 * Every asset it returns is owned by the cache and stays valid for its lifetime,
 * so a converter can hold pointers across a whole import. A failed load is
 * remembered as a failure — a missing `Shaders` is asked for once per document,
 * not once per sub-object per look.
 *
 * **Group 44 (`Textures`) is deliberately absent.** A WEM `TextureRef` is a
 * *key*; loading the asset would buy pixel data WEM does not hold. Same for
 * group 43 (`Surface`), whose reference rides the section's native bag.
 */
class AssetSource {
public:
    explicit AssetSource(sno::d3::native::AssetProvider& provider);
    ~AssetSource();

    AssetSource(const AssetSource&) = delete;
    AssetSource& operator=(const AssetSource&) = delete;

    const sno::d3::native::Appearances* appearance(i32 snoId);
    const sno::d3::native::Actor* actor(i32 snoId);
    /// Group 6. One `.ani` holds every permutation of one action.
    const sno::d3::native::Anim* anim(i32 snoId);
    /// Group 8. The 30 tag maps an actor plays through (§10.8.4).
    const sno::d3::native::AnimSet* animSet(i32 snoId);
    const sno::d3::native::ShaderMap* shaderMap(i32 snoId);
    const sno::d3::native::Shaders* shaders(i32 snoId);
    /// Group 57. The converter does not read it — a sub-object's embedded
    /// `UberMaterial` wins over the standalone asset it names — but the
    /// reference is kept on the native block, and re-resolving it is what this
    /// is for.
    const sno::d3::native::Material* material(i32 snoId);

    /// What the cache did. `loads` is provider round trips and `hits` is what
    /// the cache saved, so a test asserting sharing reads these.
    struct Stats {
        u32 requests = 0;
        u32 loads = 0;
        u32 hits = 0;
        u32 failures = 0;
    };
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

// ============================================================================
// Options
// ============================================================================

/**
 * @brief One armour choice, in the parsed descriptor's own vocabulary.
 *
 * See the file comment for why this is not `(category, value)`. A `variant` of 0
 * means the slot's plain piece — the shipped tokens run `NKD` with no variant,
 * `LIT` with `A` only, `MED` and `HVY` up to `C`.
 */
struct D3WardrobePiece {
    sno::d3::native::LookSlot slot = sno::d3::native::LookSlot::Unknown;
    sno::d3::native::ArmourWeight weight = sno::d3::native::ArmourWeight::Naked;
    char variant = 0;
};

struct D3ImportOptions {
    /// Which look supplies the materials. Empty means the engine's own default,
    /// `"A"`; a name the appearance does not carry falls back to it and reports
    /// `LookDropped`.
    std::string materialLook;

    /// What is worn. A slot named here draws that piece; a slot **not** named
    /// draws its naked variant. Sections whose descriptor has no slot — effect
    /// meshes, hair, the five that do not parse — always draw.
    std::vector<D3WardrobePiece> wardrobe;

    /// How far to follow an attach point that names another actor. 0 names the
    /// asset and stops; 1 (the default) converts the child and stops there.
    /// Bounded because the graph has cycles in it.
    u32 attachmentDepth = 1;

    /// Whether an actor's `snoAnimSet` is followed. On by default, because an
    /// actor without its clips is half an actor — but a caller drawing a
    /// thumbnail grid loads one `.ans` and every `.ani` it names per cell, which
    /// is a lot of parsing for a still frame.
    bool importAnimation = true;
};

// ============================================================================
// Export
// ============================================================================

/// The SNO ids a Diablo III document that has none of its own is given.
///
/// A document derived from another profile has no `.app` behind it, and a host
/// cache still needs a key. Both sit above every shipped id — the 2.8 install
/// tops out near 800,000 — and below `d3_anim`'s synthetic animation range, so
/// the three can never collide with each other or with a real asset.
inline constexpr i32 d3SyntheticAppearanceId = 0x3FFFFFFE;
inline constexpr i32 d3SyntheticAnimSetId = 0x3FFFFFFF;

struct D3ExportOptions {
    /// Which `Document::models[]` becomes the appearance. A document holding an
    /// actor and what rides its hardpoints holds several, and each is its own
    /// `.app`.
    u32 model = 0;

    /// The look the result defaults to. Empty means the set's own
    /// `defaultLook`, which is what the import recorded.
    std::string look;
};

/**
 * @brief What one `toAppearance` produced.
 *
 * The appearance, plus the two things a D3 host needs that an `.app` has
 * nowhere to keep — because in Diablo III **neither is in the asset**. Which
 * armour draws is equipment state (`ActorModel_ApplyLook`), and which look each
 * piece resolves under is per equipped item. WEM does hold both, on the section
 * and on the profile set, so they come back beside the appearance rather than
 * being dropped for having no field to land in.
 */
struct D3AppearanceExport {
    sno::d3::native::Appearances appearance;

    /// Which `arLooks` entry the set said to draw. An `.app` has no field for
    /// it — a look is chosen by the actor and by what is equipped, never by the
    /// appearance — so it comes back here for the caller to hand a renderer.
    u32 look = 0;

    /// One byte per **emitted** sub-object — `tGeoSet0`'s then `tGeoSet1`'s, in
    /// order — set where the section carried `SectionFlags::Hidden`.
    ///
    /// Aligned with a D3 host's own geoset numbering by construction: a section
    /// with no faces writes no sub-object, so this list and the one a renderer
    /// builds by skipping empty sub-objects are the same list.
    std::vector<u8> hidden;

    /// Per emitted sub-object, the look its materials resolve under, or
    /// `kInvalidIndex` for "the appearance's own".
    ///
    /// The look is per *item* in the original — each equipped piece carries its
    /// own look name on tag `0x10401`, and one material serves a whole weight
    /// class — so a heavy chest and heavy boots from two different sets are one
    /// material read at two variant indices. A model-wide index cannot say that,
    /// which is why this is a list and not a scalar. Empty when nothing
    /// overrides.
    std::vector<u32> looks;

    /// Per emitted sub-object, the dye row (0 undyed, 2..22 a `dye_ramp` row).
    /// Empty when nothing is dyed. Dye 1 means *hidden* and is resolved to the
    /// naked look upstream; it never reaches here.
    std::vector<i32> dyes;

    /// Parallel to @ref hidden: the `(mesh, section)` each sub-object came from.
    std::vector<std::pair<u32, u32>> sourceSections;
};

// ============================================================================
// D3Converter
// ============================================================================

class D3Converter final : public FormatConverter {
public:
    std::string formatId() const override;
    std::string formatName() const override;
    std::span<const ProfileId> profiles() const override;
    bool supportsImport() const override;
    bool supportsExport() const override;
    u32 defaultExportVersion() const override;

    /**
     * @brief What the caller already knows about these bytes.
     *
     * The detection cascade is **extension first, CoreTOC group second, version
     * heuristic last** — the renderer's own — because a SNO version word is not
     * a namespace: other groups reuse 260 and 282. A host that knows the group
     * says so and the heuristic never runs.
     */
    enum class AssetHint : u8 {
        Unknown = 0, ///< Fall back to the version word.
        Appearance,  ///< Group 9, `.app`.
        Actor,       ///< Group 1, `.acr`.
    };

    /// Sniffs the group from the bytes alone, with no provider — so no materials
    /// resolve and no attach point finds its child. Honest, and rarely what a
    /// caller wants; the typed entry points below are the real interface.
    Result<Document> importFromBytes(std::span<const u8> data) const override;

    /// As above with the cascade's earlier two steps supplied, and a provider.
    Result<Document> importFromBytes(std::span<const u8> data, AssetHint hint, AssetSource* assets,
                                     const D3ImportOptions& options = {}) const;

    /// Refuses with `OperationUnsupported`; see the file comment.
    Result<std::vector<u8>> exportToBytes(const Document& document, ProfileId profile,
                                          u32 version = 0) const override;

    /// An actor: one `Model` at `models[0]`, plus a `Model` for each attach
    /// point that named one and resolved.
    Result<Document> fromActor(const sno::d3::native::Actor& source, AssetSource& assets,
                               const D3ImportOptions& options = {}) const;

    /**
     * @brief The parts one `.app` holds, as a document with a single `Model`.
     *
     * **This is not a finished actor.** Nothing is attached, no animset is
     * named, and the look is whatever @p options asked for or the default. It
     * exists because the parts are worth loading on their own — a wardrobe
     * browser, the P6 gate — and because `fromActor` is built on it.
     *
     * @p assets may be null; it buys the `Shaders` resolution and nothing else,
     * so the geometry, the nodes and the slot join are identical without it.
     */
    Result<Document> fromAppearance(const sno::d3::native::Appearances& source,
                                    AssetSource* assets = nullptr,
                                    const D3ImportOptions& options = {}) const;

    /**
     * @brief Appends @p source and what it reaches to @p document.
     *
     * The bulk entry point: a document holding many actors holds **one `Model`
     * per distinct appearance**, keyed on the appearance's own SNO id, which the
     * model's `Diablo3` set carries as `appearanceSnoId`. In the document rather
     * than in a side table, so a document written out and read back still
     * shares.
     *
     * Returns the model index, or `kInvalidIndex` on refusal.
     */
    Result<u32> appendActor(Document& document, const sno::d3::native::Actor& source,
                            AssetSource& assets, const D3ImportOptions& options = {}) const;

    /// The SNO group @p data claims, from the 16-byte header's version word —
    /// the **last** step of the cascade. `Group::Unknown` when it matches none.
    static sno::d3::native::Group SniffGroup(std::span<const u8> data);

    /**
     * @brief Imports @p snoId's `.ans` and everything it names into @p document.
     *
     * One `AnimSet` per tag map — the core one plus the 29 keyed by what the
     * character is holding — with each weapon map's `baseAnimSet` pointing at
     * the core one, because that **is** the runtime's fallback: a weapon map
     * with no row for a tag uses core's. A 30-wide struct would say the same
     * thing in a shape nothing else could use.
     *
     * Each `.ani` a tag names is imported once, however many tags name it, and
     * a tag maps to its first permutation's clip.
     *
     * Returns the index of the core set, or `kInvalidIndex`.
     */
    Result<u32> importAnimSet(Document& document, u32 model, i32 snoId, AssetSource& assets) const;

    /// The model in @p document built from appearance @p snoId, or
    /// `kInvalidIndex`.
    static u32 FindAppearanceModel(const Document& document, i32 snoId);

    /**
     * @brief One `Model` back to the native `Appearances` a D3 renderer draws.
     *
     * The mirror of @ref fromAppearance and the reason a Diablo III `.wem` can
     * be opened as Diablo III at all. It writes a struct, never bytes (§18).
     *
     * **One section is one `SubObject`.** That is the unit D3 draws, joins a
     * material to and flips visibility on, so a section that shares a mesh with
     * thirty others still leaves as its own record with its own vertex slice and
     * its own local `u16` indices — the same disjoint-slice rule `toM3` follows,
     * and for the same reason: a sub-object's face corners address its own
     * array.
     *
     * **The materials are a restore, not a re-derivation.** D3's native block is
     * attached authoritative (§7.3), so the `UberMaterial` that comes back is
     * the one that was read: the textures are the shipped SNO ids and the
     * colours are the shipped floats. A material with no D3 block is written
     * from the common projection and reported, because that is the only case
     * where anything is being invented.
     *
     * Fails when the model carries no `Diablo3` set. What it cannot carry is
     * reported rather than assumed: the per-bone ragdoll rig and the
     * appearance's octree were never imported, a cloth section's simulation has
     * no native block to have ridden (see the file comment), and a `.wem` that
     * came from another format has no D3 material to restore.
     *
     * Measured over the whole shipped corpus — 11,347 appearances through
     * `fromAppearance` and back — every one comes out with the same bones, the
     * same five poses, the same hardpoints, looks, materials, texture ids,
     * visibility bits, both join strings, both vertex colours and the same
     * positions, UVs and normals. The only thing that shrinks is what §5.3's
     * manifold repair took on the way in.
     */
    Result<D3AppearanceExport> toAppearance(const Document& document,
                                            const D3ExportOptions& options = {}) const;

    /**
     * @brief The model's clips back as the `.ani` set they came from.
     *
     * One `Anim` per distinct source `.ani` — the clips of one animation are its
     * permutations, and the import recorded which they belonged to on
     * `Clip::native["animSnoId"]`. A clip with no such id (a document converted
     * from another format) is given one from a synthetic range and reported, so
     * the anim set can still name it.
     *
     * The `AnimSet` is the model's own, with the weapon maps that named a base
     * of the core set restored to the tag maps they came from.
     */
    struct D3AnimExport {
        std::vector<sno::d3::native::Anim> anims;
        sno::d3::native::AnimSet animSet;
    };
    Result<D3AnimExport> toAnimSet(const Document& document, u32 model) const;
};

} // namespace wem
} // namespace models
} // namespace whiteout
