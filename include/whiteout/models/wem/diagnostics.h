// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file diagnostics.h
 * @brief Structured conversion and validation reports (WEM v3, design §13).
 *
 * Replaces `std::vector<std::string> issues`, which was the only record of what a
 * conversion lost and was unsearchable, unsortable and untestable. A `DiagCode` is
 * what lets a test assert "this conversion produced exactly these three kinds of
 * loss" — the only way a lossy converter can have a meaningful gate at all — and
 * what a UI groups on.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "profile.h"

namespace whiteout {
namespace models {
namespace wem {

// ============================================================================
// Severity
// ============================================================================

enum class Severity : u8 {
    Info,    ///< Something worth recording that cost nothing.
    Warning, ///< Data was lost or approximated; the result is still usable.
    Error,   ///< The operation did not produce a valid result.
};

const char* ToString(Severity severity);

// ============================================================================
// DiagCode
// ============================================================================

/**
 * @brief What kind of thing happened — the groupable, assertable half of a report.
 *
 * Codes are grouped by area and never renumbered once shipped: the recorded
 * expected-loss lists under `tests/data/wem/` are keyed by name, and the histogram
 * of a corpus sweep is a reviewable diff. Add new codes at the end of their group.
 */
enum class DiagCode : u16 {
    Unspecified = 0,

    // --- geometry: manifold repair (§5.3) -----------------------------------
    NonManifoldEdgeSplit,       ///< An edge with >2 incident faces; the vertex was split.
    NonManifoldVertexSplit,     ///< A bowtie vertex; one copy per fan.
    DegenerateFaceDropped,      ///< A face with repeated or collinear-to-zero-area corners.
    DuplicateFaceDropped,       ///< The same corner set already had a face.
    InconsistentWindingFlipped, ///< A face was reversed to agree with its neighbours.
    NgonTriangulated,           ///< An n-gon was fanned because the target forbids n-gons.
    IsolatedVertexDropped,      ///< A vertex no face referenced.

    // --- geometry: structure (§5.7 Structural) -------------------------------
    ConnectivityCorrupt,    ///< opposite/next/prev/face relations disagree.
    IndexOutOfRange,        ///< A section, material slot or attribute index is out of range.
    AttributeCountMismatch, ///< A layer's element count disagrees with its domain.
    SkinBindingMalformed,   ///< `offsets` not monotonic, or not sized vertexCount + 1.

    // --- materials (§7) -------------------------------------------------------
    LossyBlendMode,              ///< The target has no equivalent; the nearest was written.
    DroppedNativeBlock,          ///< A native material block did not survive an operation.
    SlotNotBound,                ///< A material slot has no binding in a set that needs one.
    MixedShadedUnshadedStack,    ///< A WC3 stack mixes lit and unlit layers (§7.2.1).
    SceneReadingMultiLayerStack, ///< Multi-layer stack over a scene-reading fold (§7.2.1).
    CollapseRefused,             ///< The §7.2.2 collapse was ineligible (first layer not opaque).
    UnknownShaderCombo,          ///< An `.m2` shaderId past the transcribed rows (never row 0).
    UnsupportedMaterialKind,     ///< The target profile's `commonKinds` excludes this kind.
    LossyKindConversion,         ///< A kind change dropped state (§7.2.2 collapse, `Flatten`).
    FeatureDropped,              ///< A §7.2.5 feature the target cannot express.
    LayerDropped,                ///< A layer past the target's capacity or vocabulary.
    TextureUnresolved,           ///< A texture reference names nothing the document holds.
    MaterialSlotUnused,          ///< A slot no section binds — informational on import.
    MaterialBodyInvalid,         ///< A §7.2 body invariant: repeated slot, Orm with the
                                 ///< unpacked three, two features of a kind on one layer.
    LookBindingMalformed,        ///< `SlotBinding::byLook` is not sized to the `LookTable`.
    LookDropped,                 ///< A look the target profile has no table for (§6.6).

    // --- profiles (§6) --------------------------------------------------------
    ProfileNotCarried,         ///< Export asked for a profile the document does not have.
    ProfileCoverageIncomplete, ///< A carried profile has no material for some slot (§6.3).
    BoneInfluenceLimit,        ///< More influences than `maxBoneInfluences`; the tail was cut.
    UvSetLimit,                ///< More UV sets than `maxUvSets`.
    IndexWidthExceeded,        ///< Vertex count exceeds `indexWidth`.
    NgonUnsupported,           ///< The profile forbids n-gons and one is present.
    VertexColorUnsupported,    ///< The profile has no vertex colour and a layer carries one.
    NativeKindProfileMismatch, ///< A native block's kind disagrees with its set's profile (§6.5).
    BonePaletteLimit,          ///< A section needs more bones than `maxBonesPerPalette`.
    GeometryMismatch,          ///< `AddProfileFromImport` geometry did not match (§6.6).
    SectionUndrawn,            ///< A section whose profile mask is empty — nothing draws it.

    // --- nodes (§10) ----------------------------------------------------------
    NodeRemovalRefused,      ///< `SkinPolicy::Refuse` and the node had influences.
    SkinInfluenceReassigned, ///< `SkinPolicy::ReassignToParent` moved weights up (§10.6).
    DanglingNodeReference,   ///< A referencer pointed at a node that no longer exists.
    BindPoseRecomposed,      ///< `preserveWorld` rebuilt a survivor's local transform.

    // --- animation (§10.8) ----------------------------------------------------
    MixedInterpolationInTrack, ///< A sub-track mixes interpolation modes (§10.8.2).
    AnimChannelInvalidated,    ///< A channel's target was removed.
    ClipTargetMissing,         ///< A clip references a model or set that is absent.
    AnimTrackDropped,          ///< A track the target format cannot express.

    // --- actors (§9) ----------------------------------------------------------
    EventPayloadMismatch, ///< `ActorEvent` kind and payload group disagree (§9.5).
    AssetUnresolved,      ///< An `AssetKey` the `AssetSource` could not load.
    HardpointUnresolved,  ///< A hardpoint names a node the model does not have.

    // --- container / format (§11) ---------------------------------------------
    UnknownChunkPreserved,  ///< An unrecognised chunk was carried through unchanged.
    OrphanChunk,            ///< A preserved chunk's owner is gone (§11.4).
    UnsupportedVersion,     ///< A file version this build cannot read.
    LegacyDocumentUpgraded, ///< A v2 file was read through the compatibility path (§11.5).
    OperationUnsupported,   ///< The converter does not implement this direction at all.

    Count
};

/// Stable spelling of a code — what goldens and UI rows key on.
const char* ToString(DiagCode code);

// ============================================================================
// ElementRef
// ============================================================================

enum class ElementKind : u8 {
    None,
    Document,
    Mesh,
    Face,
    Vertex,
    Halfedge,
    Edge,
    Section,
    Node,
    Material,
    Layer,
    Slot,
    Texture,
    Look,
    Feature,
    Actor,
    Event,
    Clip,
    Channel,
    Track,
    Chunk,
};

const char* ToString(ElementKind kind);

/**
 * @brief Where a diagnostic happened, in the document's own coordinates.
 *
 * `index` addresses the element within its kind; `sub` addresses a part of it
 * (a layer within a material, a corner within a face) and is `kInvalidIndex`
 * when the diagnostic is about the whole element.
 */
struct ElementRef {
    static constexpr u32 kInvalidIndex = ~0u;

    ElementKind kind = ElementKind::None;
    u32 index = kInvalidIndex;
    u32 sub = kInvalidIndex;

    ElementRef() = default;
    ElementRef(ElementKind k, u32 i, u32 s = kInvalidIndex) : kind(k), index(i), sub(s) {}

    bool valid() const {
        return kind != ElementKind::None;
    }
};

/// "material[3].layer[1]" / "mesh[0]" / "" — for messages and golden lines.
std::string Describe(const ElementRef& ref);

// ============================================================================
// Diagnostic
// ============================================================================

struct Diagnostic {
    Severity severity = Severity::Info;
    DiagCode code = DiagCode::Unspecified;
    std::string message;
    ElementRef where;
    ProfileId profile = ProfileId::Count; ///< `Count` = not about one profile.

    bool hasProfile() const {
        return profile != ProfileId::Count;
    }
};

// ============================================================================
// Diagnostics
// ============================================================================

/**
 * @brief An ordered, groupable report.
 *
 * Order is insertion order and is stable, because a recorded expected-loss list
 * is diffed literally. Nothing here iterates an unordered container.
 */
/// @bind methods
class Diagnostics {
public:
    /// One entry per distinct code, in ascending code order — the golden's shape.
    struct CodeCount {
        DiagCode code;
        u32 count;
    };

    void add(Severity severity, DiagCode code, std::string message, ElementRef where = {},
             ProfileId profile = ProfileId::Count);

    void info(DiagCode code, std::string message, ElementRef where = {},
              ProfileId profile = ProfileId::Count);
    void warn(DiagCode code, std::string message, ElementRef where = {},
              ProfileId profile = ProfileId::Count);
    void error(DiagCode code, std::string message, ElementRef where = {},
               ProfileId profile = ProfileId::Count);

    /// Append another report, preserving its order after this one's.
    void append(const Diagnostics& other);

    bool hasErrors() const;
    bool empty() const {
        return entries_.empty();
    }
    std::size_t size() const {
        return entries_.size();
    }
    void clear() {
        entries_.clear();
        errorCount_ = 0;
    }

    std::span<const Diagnostic> all() const {
        return std::span<const Diagnostic>(entries_.data(), entries_.size());
    }

    std::vector<Diagnostic> byCode(DiagCode code) const;
    std::vector<Diagnostic> byProfile(ProfileId profile) const;
    std::vector<Diagnostic> bySeverity(Severity severity) const;

    u32 countOf(DiagCode code) const;

    /// The expected-loss list's canonical form: codes ascending, with counts.
    std::vector<CodeCount> histogram() const;

    /// One `CODE count` line per histogram row — the golden file body.
    std::string formatHistogram() const;

private:
    std::vector<Diagnostic> entries_;
    u32 errorCount_ = 0;
};

} // namespace wem
} // namespace models
} // namespace whiteout
