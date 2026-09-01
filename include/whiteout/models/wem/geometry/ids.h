// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file ids.h
 * @brief Strong element handles for the half-edge kernel (WEM v3, design §5.2).
 *
 * A `u32` with a tag, so a `FaceId` cannot be passed where a `VertexId` belongs.
 * `kInvalid` is `0xFFFFFFFF` and is the default-constructed value, which is what
 * lets `face(h)` on a boundary halfedge return a handle rather than need a
 * separate query.
 *
 * The arithmetic relations of §5.2 (`opposite(h) == h ^ 1`, `edge(h) == h >> 1`)
 * are spelled on `Topology`, not here: a handle knows its index, not its
 * neighbourhood.
 */

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

inline constexpr u32 kInvalidId = 0xFFFFFFFFu;

template <class Tag>
class Id {
public:
    using value_type = u32;
    static constexpr u32 kInvalid = kInvalidId;

    constexpr Id() = default;
    constexpr explicit Id(u32 value) : value_(value) {}

    constexpr u32 value() const {
        return value_;
    }
    constexpr bool valid() const {
        return value_ != kInvalid;
    }
    constexpr explicit operator bool() const {
        return valid();
    }

    /// Index into a per-element array. Only call on a valid handle.
    constexpr std::size_t index() const {
        return static_cast<std::size_t>(value_);
    }

    friend constexpr bool operator==(Id a, Id b) {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(Id a, Id b) {
        return a.value_ != b.value_;
    }
    /// Ordering exists so handles can be sorted deterministically, not because
    /// one element is "less" than another.
    friend constexpr bool operator<(Id a, Id b) {
        return a.value_ < b.value_;
    }

    Id& operator++() {
        ++value_;
        return *this;
    }

private:
    u32 value_ = kInvalid;
};

struct VertexTag;
struct HalfedgeTag;
struct EdgeTag;
struct FaceTag;

using VertexId = Id<VertexTag>;
using HalfedgeId = Id<HalfedgeTag>;
using EdgeId = Id<EdgeTag>;
using FaceId = Id<FaceTag>;

// ============================================================================
// Status
// ============================================================================

/**
 * @brief Per-element status bits. Deletion is lazy (§5.2): the bit is set and
 *        `GarbageCollection()` compacts.
 */
enum class Status : u8 {
    None = 0,
    Deleted = 0x01,  ///< Removed; still occupies its slot until collection.
    Locked = 0x02,   ///< An edit operation must refuse to move this.
    Selected = 0x04, ///< Host-owned selection state; WEM never reads it.
    Hidden = 0x08,   ///< Host-owned; the render view does not consult it.
    Feature = 0x10,  ///< Marked as a feature element (a crease chain, a seam).
    Tagged = 0x20,   ///< Scratch bit for algorithms; never persisted.
};

constexpr Status operator|(Status a, Status b) {
    return static_cast<Status>(static_cast<u8>(a) | static_cast<u8>(b));
}
constexpr Status operator&(Status a, Status b) {
    return static_cast<Status>(static_cast<u8>(a) & static_cast<u8>(b));
}
constexpr Status operator~(Status a) {
    return static_cast<Status>(static_cast<u8>(~static_cast<u8>(a)) & 0x3Fu);
}
inline Status& operator|=(Status& a, Status b) {
    a = a | b;
    return a;
}
inline Status& operator&=(Status& a, Status b) {
    a = a & b;
    return a;
}
constexpr bool hasStatus(Status value, Status bit) {
    return (static_cast<u8>(value) & static_cast<u8>(bit)) != 0;
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
