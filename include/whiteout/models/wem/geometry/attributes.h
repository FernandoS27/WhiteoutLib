// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file attributes.h
 * @brief Attribute domains and layers (WEM v3, design §5.4).
 *
 * Five domains, the same five OpenMesh's property system uses, which makes the
 * bridge a name-for-name mapping (C9). The load-bearing choice:
 *
 * > Normals, tangents and UVs are **Halfedge** properties, not Vertex. That is
 * > the whole seam fix — two halfedges into one vertex may hold different
 * > normals (a hard edge) or different UVs (a seam) without the vertex
 * > splitting. The render view (§5.8) puts the splits back when it builds GPU
 * > vertices, and only there.
 *
 * Anything not in the reserved-name table is user data and survives a round trip
 * untouched — the extension point that keeps the struct count from growing every
 * time a format has one more field.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/utils/vertex_buffer.h>
#include <whiteout/vector_types.h>

#include "ids.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

// ============================================================================
// Domain and type
// ============================================================================

enum class Domain : u8 {
    Vertex,   ///< Geometric identity: position, mergeGroup, skin binding.
    Halfedge, ///< Shading identity: normal, tangent, UV sets, colour. Seams live here.
    Edge,     ///< sharp / seam / crease.
    Face,     ///< section, smoothing group, face flags.
    Mesh,     ///< bounds, LOD level, source hints.
    Count
};

enum class AttrType : u8 {
    F32,
    F32x2,
    F32x3,
    F32x4,
    U8x4,
    U16,
    U32,
    I32,
    Quat, ///< F32x4 with quaternion semantics; distinguished so tools interpolate it right.
    Bool,
    Count
};

const char* ToString(Domain domain);
const char* ToString(AttrType type);

/// Bytes one element of @p type occupies in an `AttrLayer::data` block.
u32 AttrTypeSize(AttrType type);

/// Scalar components in @p type — 3 for `F32x3`, 4 for `U8x4`, 1 for `Bool`.
u32 AttrTypeComponents(AttrType type);

// ============================================================================
// AttrLayer
// ============================================================================

/**
 * @brief One named attribute over one domain.
 *
 * `data` is always the *unpacked* form — `AttrTypeSize(type) * count` bytes.
 * `storage` says how the layer is packed on disk (§11) and how the render view
 * should encode it; it never changes what `get<T>()` returns.
 */
struct AttrLayer {
    std::string name;
    Domain domain = Domain::Vertex;
    AttrType type = AttrType::F32;
    utils::AttributeEncoding storage = utils::AttributeEncoding::Float32;
    std::vector<u8> data;

    std::size_t count() const {
        const u32 stride = AttrTypeSize(type);
        return stride == 0 ? 0 : data.size() / stride;
    }

    template <class V>
    void reflect(V& v) {
        // `data` is the unpacked form on both sides; `storage` says how the
        // render view should encode it, and does not change these bytes.
        v.field("name", name);
        v.field("domain", domain);
        v.field("type", type);
        v.field("storage", storage);
        v.field("data", data);
    }
};

// ============================================================================
// Reserved names
// ============================================================================

/**
 * @brief The layer names converters and the render view know about (§5.4).
 *
 * `uv0`…`uvN` and `color0`…`colorN` are families: `ReservedLayer` resolves any
 * member of them. Everything else is user data.
 */
namespace names {
inline constexpr const char* kPosition = "position";       ///< Vertex / F32x3, required.
inline constexpr const char* kMergeGroup = "mergeGroup";   ///< Vertex / U32, §5.3.
inline constexpr const char* kNormal = "normal";           ///< Halfedge / F32x3.
inline constexpr const char* kTangent = "tangent";         ///< Halfedge / F32x4, w = sign.
inline constexpr const char* kBinormal = "binormal";       ///< Halfedge / F32x3, D3 authors it.
inline constexpr const char* kCrease = "crease";           ///< Edge / F32.
inline constexpr const char* kSharp = "sharp";             ///< Edge / Bool.
inline constexpr const char* kSeam = "seam";               ///< Edge / Bool.
inline constexpr const char* kSection = "section";         ///< Face / U32, authoritative (§5.5).
inline constexpr const char* kSmoothGroup = "smoothGroup"; ///< Face / U32.

/// "uv0", "uv1", … Valid for @p index < 8; the string is built, not interned.
std::string uv(u32 index);
/// "color0", "color1", …
std::string color(u32 index);
} // namespace names

/// What a reserved name requires. `domain`/`type` are `Count` for a free name.
struct ReservedLayer {
    Domain domain = Domain::Count;
    AttrType type = AttrType::Count;

    bool reserved() const {
        return domain != Domain::Count;
    }
};

/// The reserved-name table (§5.4), including the `uvN` / `colorN` families.
ReservedLayer LookupReserved(const std::string& name);

// ============================================================================
// AttributeSet
// ============================================================================

/**
 * @brief The layers of one mesh, addressed by `(domain, name)` — C9.
 *
 * The set owns the per-domain element counts as well as the layers, because
 * "every attribute layer's element count matches its domain" is a §5.7
 * structural invariant and keeping the count somewhere else makes it a rule to
 * remember rather than one that holds by construction. `SetDomainCount` resizes
 * every layer of that domain; growth zero-fills.
 */
class AttributeSet {
public:
    // --- domain sizing ---

    u32 domainCount(Domain domain) const {
        return domainCounts_[static_cast<std::size_t>(domain)];
    }
    void setDomainCount(Domain domain, u32 count);

    // --- layer access ---

    std::span<const AttrLayer> layers() const {
        return std::span<const AttrLayer>(layers_.data(), layers_.size());
    }

    template <class V>
    void reflect(V& v) {
        // Public and reaching the private members, for the reason `Material`
        // does: the domain counts are an invariant the set maintains, and a
        // reader that recomputed them from the layers would be guessing at
        // the count of a domain with no layers at all.
        for (u32& domainCount : domainCounts_) {
            v.field("domainCount", domainCount);
        }
        // Insertion order is the layer order and is part of the contract.
        v.field("layers", layers_);
    }

    std::size_t layerCount() const {
        return layers_.size();
    }

    /// Index of the layer, or `kInvalidId`. Layer order is insertion order and
    /// is stable — the writer and the reflection dump both depend on it.
    u32 find(const std::string& name, Domain domain) const;
    bool has(const std::string& name, Domain domain) const {
        return find(name, domain) != kInvalidId;
    }

    const AttrLayer* layer(const std::string& name, Domain domain) const;
    AttrLayer* layer(const std::string& name, Domain domain);

    /**
     * @brief Create a layer, or return the existing one.
     *
     * A name in the reserved table keeps its documented domain and type: passing
     * a different @p type for a reserved name is a caller bug, and the existing
     * layer is returned unchanged rather than silently reinterpreted.
     */
    AttrLayer& create(const std::string& name, Domain domain, AttrType type,
                      utils::AttributeEncoding storage = utils::AttributeEncoding::Float32);

    /// Drops the layer if present. Reserved names may be removed like any other:
    /// a mesh with no `normal` layer is a mesh with no normals.
    bool remove(const std::string& name, Domain domain);

    // --- typed views ---

    /// Typed view over a layer's data, or an empty span when the layer is absent
    /// or `sizeof(T)` disagrees with its type.
    template <class T>
    std::span<T> get(const std::string& name, Domain domain) {
        AttrLayer* found = layer(name, domain);
        if (found == nullptr || AttrTypeSize(found->type) != sizeof(T)) {
            return std::span<T>();
        }
        return std::span<T>(reinterpret_cast<T*>(found->data.data()), found->count());
    }

    template <class T>
    std::span<const T> get(const std::string& name, Domain domain) const {
        const AttrLayer* found = layer(name, domain);
        if (found == nullptr || AttrTypeSize(found->type) != sizeof(T)) {
            return std::span<const T>();
        }
        return std::span<const T>(reinterpret_cast<const T*>(found->data.data()), found->count());
    }

    /// `create` then `get`. The layer is sized to its domain's current count.
    template <class T>
    std::span<T> getOrCreate(const std::string& name, Domain domain, AttrType type) {
        create(name, domain, type);
        return get<T>(name, domain);
    }

    // --- bulk operations, used by GarbageCollection and the repair ---

    /// Appends one zero-filled element to every layer of @p domain and bumps the
    /// domain count. Returns the new element's index.
    u32 appendElement(Domain domain);

    /// Copies element @p from onto element @p to across every layer of @p domain.
    void copyElement(Domain domain, u32 from, u32 to);

    /**
     * @brief Compacts every layer of @p domain through @p remap.
     *
     * `remap[old] == kInvalidId` drops the element. @p newCount is the number of
     * surviving elements; the caller owns building the table so one pass can
     * remap the topology and the attributes together.
     */
    void remapDomain(Domain domain, std::span<const u32> remap, u32 newCount);

    void clear();

private:
    std::vector<AttrLayer> layers_;
    u32 domainCounts_[static_cast<std::size_t>(Domain::Count)] = {0, 0, 0, 0, 1};
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
