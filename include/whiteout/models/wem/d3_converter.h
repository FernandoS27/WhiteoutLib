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
 * ### Import only
 *
 * Writing SNO is a separate project (§18). `supportsExport()` is false and
 * `exportToBytes` refuses, rather than producing a file the game would not load.
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
};

} // namespace wem
} // namespace models
} // namespace whiteout
