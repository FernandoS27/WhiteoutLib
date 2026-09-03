// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/wem/chunk_traits.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/materials/native.h>

#include <array>

namespace whiteout {
namespace models {
namespace wem {

/// The **container** — §11.1's "the M3-derived meta structure is kept, not
/// replaced". The index table, the inline `Reference` and the 32-byte header
/// come straight from `.m3`; what changed at v3 is what the chunks hold, not how
/// the file addresses them.
///
/// One registry, at `wem` scope: the generic visitors name *one* traits template,
/// and a per-generation registry would have meant a policy parameter on every
/// visitor or two copies of every basic-type tag.

constexpr u32 kWoemMagic = 0x4D454F57; // "WOEM" in little-endian

constexpr u32 kCurrentVersion = 3; // The format this build reads and writes

// ============================================================================
// Index table structures (on disk)
// ============================================================================

/// Index table entry (16 bytes). Located at header.indexOffset.
struct IndexEntry {
    u32 tag = 0;     ///< FourCC tag identifying the data type
    u32 offset = 0;  ///< Byte offset to data in file
    u32 count = 0;   ///< Number of items
    u32 version = 0; ///< Structure version
};

/// Reference to data via index table (12 bytes). Written inline within structures.
struct Reference {
    u32 entries = 0; ///< Number of elements (0 = null reference)
    u32 index = 0;   ///< Index into the index table
    u32 flags = 0;   ///< Reserved
};

/// File header (32 bytes, always at offset 0).
struct WEMHeader {
    u32 magic = 0;           ///< kWoemMagic
    u32 version = 0;         ///< `kCurrentVersion`
    u32 indexOffset = 0;     ///< Byte offset to index table
    u32 indexCount = 0;      ///< Number of IndexEntry records
    Reference documentRef{}; ///< Root chunk: `WDOC`
    u32 profileMask = 0;     ///< Bit per `ProfileId`.
};

static_assert(sizeof(IndexEntry) == 16, "IndexEntry must be 16 bytes");
static_assert(sizeof(Reference) == 12, "Reference must be 12 bytes");
static_assert(sizeof(WEMHeader) == 32, "WEMHeader must be 32 bytes");

// ============================================================================
// Chunk tag traits — associates each type with its FourCC tag and properties
// ============================================================================

// --- Basic types ---

template <>
struct ChunkTagTraits<char> {
    static constexpr u32 value = kTag("CHAR");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u8> {
    static constexpr u32 value = kTag("U8__");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u16> {
    static constexpr u32 value = kTag("U16_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u32> {
    static constexpr u32 value = kTag("U32_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u64> {
    static constexpr u32 value = kTag("U64_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i16> {
    static constexpr u32 value = kTag("I16_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i32> {
    static constexpr u32 value = kTag("I32_");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<f32> {
    static constexpr u32 value = kTag("REAL");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector2f> {
    static constexpr u32 value = kTag("VEC2");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector3f> {
    static constexpr u32 value = kTag("VEC3");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector4f> {
    static constexpr u32 value = kTag("VEC4");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Quaternion> {
    static constexpr u32 value = kTag("QUAT");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Extent> {
    static constexpr u32 value = kTag("BNDS");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u8, 4>> {
    static constexpr u32 value = kTag("BYT4");
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

// ============================================================================
// v3 (§11.3)
// ============================================================================

/// Every type that is written as a chunk -- an element of a `v.field` vector, or
/// the referent of a `v.chunk`. A type that is only ever visited inline (a body
/// factor, a `TextureInput`, a `Transform` inside a `Node`'s pose run) needs no
/// tag, and deliberately does not get one: an index entry per such record would
/// double the index table to make addressable something nothing addresses.
///
/// `max_version` is 1 across the board because v3 is the first version of every
/// one of these. The field exists so a later build can add a `v.since(2)` field
/// and bump exactly the chunk it changed.

template <>
struct ChunkTagTraits<Document> {
    static constexpr u32 value = kTag("WDOC");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

/// v2 adds `NodeTree::rig`, which is reflected inline in the model's body.
template <>
struct ChunkTagTraits<Model> {
    static constexpr u32 value = kTag("MODL");
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ProfileMaterialSet> {
    static constexpr u32 value = kTag("PROF");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SlotBinding> {
    static constexpr u32 value = kTag("SBND");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Mesh> {
    static constexpr u32 value = kTag("MESH");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<MeshSection> {
    static constexpr u32 value = kTag("SECT");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<geom::AttrLayer> {
    static constexpr u32 value = kTag("ATTR");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<geom::Influence> {
    static constexpr u32 value = kTag("INFL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<geom::VertexSplit> {
    static constexpr u32 value = kTag("VSPL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<geom::FaceRecord> {
    static constexpr u32 value = kTag("FDRP");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Transform> {
    static constexpr u32 value = kTag("XFRM");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

/// A pose matrix run (`Node::poseMatrices`). Trivial, unlike `Transform`: a
/// `Matrix44f` has no `reflect()` and 16 floats have nothing to name, so the run
/// is one memcpy rather than 16 fields per bone.
template <>
struct ChunkTagTraits<Matrix44f> {
    static constexpr u32 value = kTag("MTX4");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = true;
};

/// v2 adds `Node::poseMatrices` (§10.5's matrix poses).
template <>
struct ChunkTagTraits<Node> {
    static constexpr u32 value = kTag("NODE");
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<NativeBag::Entry> {
    static constexpr u32 value = kTag("NBAG");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

/// v2 adds `PoseSchema::storage`.
template <>
struct ChunkTagTraits<PoseSchema> {
    static constexpr u32 value = kTag("PSCH");
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Material> {
    static constexpr u32 value = kTag("MATL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<MaterialFeature> {
    static constexpr u32 value = kTag("MFEA");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<FresnelFeature> {
    static constexpr u32 value = kTag("FFRS");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<UvAnimationFeature> {
    static constexpr u32 value = kTag("FUVA");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CompositeBody> {
    static constexpr u32 value = kTag("MKCP");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CompositeLayer> {
    static constexpr u32 value = kTag("MKCL");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CombinersBody> {
    static constexpr u32 value = kTag("MKCB");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CombinerStage> {
    static constexpr u32 value = kTag("MKCS");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<LegacyDeferredBody> {
    static constexpr u32 value = kTag("MKLD");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PbrDeferredBody> {
    static constexpr u32 value = kTag("MKPB");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TextureRef> {
    static constexpr u32 value = kTag("TEXR");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Look> {
    static constexpr u32 value = kTag("LOOK");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

/// Tags that are claimed and will never be issued. P7 took the last of the
/// reservations, so what is left here is only the two P6 retired.
namespace reserved {
// `ACTR` and `EVNT` were reserved for an `Actor` record and its events. P6
// settled that there is no such record -- an actor is a join applied at
// conversion, not a type WEM stores (§9.1) -- so the two are left claimed and
// unused rather than recycled: a FourCC costs nothing, and a tag that once meant
// something else is worth never meaning anything again.
inline constexpr u32 kRetiredActor = kTag("ACTR");
inline constexpr u32 kRetiredActorEvent = kTag("EVNT");

} // namespace reserved

// --- Animation (§10.8) ---

template <>
struct ChunkTagTraits<AnimChannel> {
    static constexpr u32 value = kTag("ACHN");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SubTrack> {
    static constexpr u32 value = kTag("STRK");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SubTrackContainer> {
    static constexpr u32 value = kTag("STCC");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ClipEvent> {
    static constexpr u32 value = kTag("CLEV");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

/// v2 adds `Clip::bounds` (§10.8's per-clip extent).
template <>
struct ChunkTagTraits<Clip> {
    static constexpr u32 value = kTag("CLIP");
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

/// One row of an `AnimSet`'s map. A chunk of its own because the set holds a
/// vector of them and every vector element type needs a tag.
template <>
struct ChunkTagTraits<AnimTag> {
    static constexpr u32 value = kTag("ATAG");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimSet> {
    static constexpr u32 value = kTag("ASET");
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

// ============================================================================
// The runtime tag set
// ============================================================================

/// A tag the writer emits for an index slot it is holding open -- see the
/// writer's unknown-chunk handling. A reader skips it rather than preserving it,
/// or holes would breed on every round trip.
inline constexpr u32 kHoleTag = kTag("HOLE");

/// Every tag this build understands, for the one question a template cannot
/// answer: given an index entry, is this a chunk we know? §11.4 preservation is
/// the caller, and it runs before any record is parsed, so it has a tag and
/// nothing else.
///
/// Hand-maintained beside the specialisations above, because C++ gives no way to
/// enumerate the specialisations of a template. A tag missing from this list is
/// preserved as unknown *and* parsed as known -- it would be written twice -- so
/// `wem_unknown_chunk_test` asserts the two agree.
inline constexpr u32 kKnownChunkTags[] = {
    ChunkTagTraits<char>::value,
    ChunkTagTraits<u8>::value,
    ChunkTagTraits<u16>::value,
    ChunkTagTraits<u32>::value,
    ChunkTagTraits<u64>::value,
    ChunkTagTraits<i16>::value,
    ChunkTagTraits<i32>::value,
    ChunkTagTraits<f32>::value,
    ChunkTagTraits<Vector2f>::value,
    ChunkTagTraits<Vector3f>::value,
    ChunkTagTraits<Vector4f>::value,
    ChunkTagTraits<Quaternion>::value,
    ChunkTagTraits<Extent>::value,
    ChunkTagTraits<std::array<u8, 4>>::value,
    ChunkTagTraits<Document>::value,
    ChunkTagTraits<Model>::value,
    ChunkTagTraits<ProfileMaterialSet>::value,
    ChunkTagTraits<SlotBinding>::value,
    ChunkTagTraits<Mesh>::value,
    ChunkTagTraits<MeshSection>::value,
    ChunkTagTraits<geom::AttrLayer>::value,
    ChunkTagTraits<geom::Influence>::value,
    ChunkTagTraits<geom::VertexSplit>::value,
    ChunkTagTraits<geom::FaceRecord>::value,
    ChunkTagTraits<Transform>::value,
    ChunkTagTraits<Matrix44f>::value,
    ChunkTagTraits<Node>::value,
    ChunkTagTraits<NativeBag::Entry>::value,
    ChunkTagTraits<PoseSchema>::value,
    ChunkTagTraits<Material>::value,
    ChunkTagTraits<MaterialFeature>::value,
    ChunkTagTraits<FresnelFeature>::value,
    ChunkTagTraits<UvAnimationFeature>::value,
    ChunkTagTraits<CompositeBody>::value,
    ChunkTagTraits<CompositeLayer>::value,
    ChunkTagTraits<CombinersBody>::value,
    ChunkTagTraits<CombinerStage>::value,
    ChunkTagTraits<LegacyDeferredBody>::value,
    ChunkTagTraits<PbrDeferredBody>::value,
    ChunkTagTraits<TextureRef>::value,
    ChunkTagTraits<Look>::value,
    ChunkTagTraits<native::MdxMaterial>::value,
    ChunkTagTraits<native::M2Material>::value,
    ChunkTagTraits<native::M3Material>::value,
    ChunkTagTraits<native::D3Material>::value,
    ChunkTagTraits<AnimChannel>::value,
    ChunkTagTraits<SubTrack>::value,
    ChunkTagTraits<SubTrackContainer>::value,
    ChunkTagTraits<ClipEvent>::value,
    ChunkTagTraits<Clip>::value,
    ChunkTagTraits<AnimTag>::value,
    ChunkTagTraits<AnimSet>::value,
    kWoemMagic, // slot 0 is the header's own entry, not a chunk
    kHoleTag,
};

inline bool IsKnownChunkTag(u32 tag) {
    for (const u32 known : kKnownChunkTags) {
        if (known == tag) {
            return true;
        }
    }
    return false;
}

// Convenience: retrieve tag for a type
template <typename T>
inline constexpr u32 chunkTag = ChunkTagTraits<T>::value;

} // namespace wem
} // namespace models
} // namespace whiteout
