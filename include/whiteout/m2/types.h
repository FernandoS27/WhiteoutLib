// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file types.h
 * @brief Core type definitions and constants for the M2 format
 *
 * This file defines:
 * - Compressed quaternion type for bone rotations
 * - Bounding extent structure
 * - Animation keyframe and track types
 * - Interpolation modes
 * - Lazy-loading vector for deferred M2Array resolution
 * - M2 version constants for each WoW expansion
 * - Chunk tag constants (MD20, MD21)
 *
 * M2 files use offset/count pairs (M2Array) to reference data blocks.
 * Animation tracks follow a per-sequence nested array layout where each
 * sequence has its own timestamp and value sub-arrays.
 */

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common_types.h"
#include "../compatibility.h"
#include "../vector_types.h"

namespace whiteout {
namespace m2 {

// ============================================================================
// Compressed Quaternion
// ============================================================================

/**
 * @brief Compressed 8-byte quaternion for bone rotations
 *
 * Post-Vanilla M2 bone rotations use this compressed format instead of
 * full 16-byte float quaternions. XYZ components are signed normalized
 * (snorm16: [-1, 1] mapped to [0, 65534] with 32767 = 0). The W component
 * is unsigned normalized (unorm16: [0, 1] mapped to [0, 65535]).
 *
 * Identity quaternion = (32767, 32767, 32767, 65535) → (0, 0, 0, 1)
 */
struct CompatQuaternion {
    union {
        struct {
            u16 x, y, z, w; ///< x,y,z are snorm16; w is unorm16
        };
        std::array<u16, 4> data; ///< Raw 4×u16 access
    };

    CompatQuaternion() = default;
    constexpr CompatQuaternion(u16 x_, u16 y_, u16 z_, u16 w_) : x(x_), y(y_), z(z_), w(w_) {}

    /// @brief Construct from normalized float components
    static constexpr CompatQuaternion fromFloats(f32 fx, f32 fy, f32 fz, f32 fw) {
        return CompatQuaternion(snorm16::from_float(fx).value, snorm16::from_float(fy).value,
                                snorm16::from_float(fz).value, unorm16::from_float(fw).value);
    }

    /// @brief Convert to Vector4f (float quaternion components)
    operator Vector4f() const {
        return Vector4f{
            static_cast<f32>(snorm16::from_raw(x)), static_cast<f32>(snorm16::from_raw(y)),
            static_cast<f32>(snorm16::from_raw(z)), static_cast<f32>(unorm16::from_raw(w))};
    }

    /// @brief Convert to Quaternion type
    operator Quaternion() const {
        return Quaternion{
            static_cast<f32>(snorm16::from_raw(x)), static_cast<f32>(snorm16::from_raw(y)),
            static_cast<f32>(snorm16::from_raw(z)), static_cast<f32>(unorm16::from_raw(w))};
    }
};

// ============================================================================
// Extent (Bounding Box + Sphere)
// ============================================================================

/**
 * @brief Axis-aligned bounding box with bounding sphere
 *
 * Used for both model-level bounding volumes and per-sequence animation bounds.
 * Binary layout: 28 bytes (minimum 12B + maximum 12B + radius 4B).
 */
struct Extent {
    Vector3f minimum;        ///< Minimum corner of the AABB
    Vector3f maximum;        ///< Maximum corner of the AABB
    f32 sphereRadius = 0.0f; ///< Bounding sphere radius
};

// ============================================================================
// Animation Keyframe Types
// ============================================================================

/**
 * @brief Simple animation keyframe with timestamp and value
 *
 * Used for None and Linear interpolation modes.
 *
 * @tparam T Value type (f32, Vector3f, CompatQuaternion, i16, u8, etc.)
 */
template <typename T>
struct AnimationKey {
    u32 timestamp = 0; ///< Time in milliseconds
    T value;           ///< Value at this timestamp
};

/**
 * @brief Spline keyframe with tangent data for smooth curve interpolation
 *
 * Used for Bezier and Hermite interpolation modes. The in/out tangents
 * control the curve shape between keyframes.
 *
 * @tparam T Value type (f32, Vector3f, CompatQuaternion, etc.)
 */
template <typename T>
struct SplineKey {
    u32 timestamp = 0; ///< Time in milliseconds
    T value;           ///< Value at this timestamp
    T inTangent;       ///< Incoming tangent for curve interpolation
    T outTangent;      ///< Outgoing tangent for curve interpolation
};

// ============================================================================
// Interpolation
// ============================================================================

/**
 * @brief Interpolation mode for animation tracks
 *
 * Determines how values between keyframes are calculated.
 * Bezier and Hermite modes use SplineKey with tangent data.
 */
enum class InterpolationType : u16 {
    None = 0,    ///< Step function; value changes instantly at timestamp
    Linear = 1,  ///< Linear interpolation (lerp for vectors; nlerp for quaternions)
    Bezier = 2,  ///< Cubic Bezier spline using in/out tangents
    Hermite = 3, ///< Cubic Hermite spline using in/out tangents
};

// ============================================================================
// Lazy-Loading Vector
// ============================================================================

/**
 * @brief Lazy-loading vector for deferred M2Array resolution
 *
 * M2 data is organized through M2Array offset/count pairs scattered throughout
 * the file. This container supports deferred loading of nested arrays (e.g.,
 * per-animation timestamp/value sub-arrays in animation tracks).
 *
 * @tparam T Element type
 */
template <typename T>
struct lazy_vector {
    /// @brief On-disk M2Array header (count + offset pair, 8 bytes)
    struct M2ArrayHeader {
        u32 count = 0;  ///< Number of elements
        u32 offset = 0; ///< Byte offset to first element (relative to MD20 data base)
    };

    std::vector<T> backingData;                                 ///< Resolved element storage
    std::vector<bool> isLoaded;                                 ///< Per-index load status
    std::vector<std::pair<size_t, M2ArrayHeader>> pendingLoads; ///< Deferred load requests

    /// @brief Check if a specific index has been loaded
    bool isLoadedAt(size_t index) const {
        return index < isLoaded.size() && isLoaded[index];
    }

    T& operator[](size_t index) {
        return backingData[index];
    }

    const T& operator[](size_t index) const {
        return backingData[index];
    }

    T* data() {
        return backingData.data();
    }

    const T* data() const {
        return backingData.data();
    }
};

// ============================================================================
// Animation Tracks
// ============================================================================

/**
 * @brief Base animation track with interpolation metadata and timestamps
 *
 * Animation tracks are the backbone of the M2 format. Each animatable property
 * uses a track with per-sequence nested arrays: the outer index corresponds to
 * the animation sequence, the inner array holds keyframe timestamps.
 *
 * A track with globalSequenceId != 0xFFFF ignores per-animation timelines and
 * loops within [0, globalLoops[globalSequenceId].timestamp]. Such tracks always
 * have exactly one sub-array (index 0).
 */
struct AnimationTrackBase {
    InterpolationType interpolationType =
        InterpolationType::None;              ///< How to interpolate between keyframes
    u16 globalSequenceId = 0xFFFF;            ///< Global sequence index, or 0xFFFF if per-animation
    std::vector<std::vector<u32>> timestamps; ///< [animation][keyframe] timestamps in milliseconds
};

/**
 * @brief Typed animation track with keyframe values
 *
 * Extends AnimationTrackBase with a parallel values array. The i-th entry
 * in values[anim][key] corresponds to timestamps[anim][key].
 *
 * @tparam T Value type (f32, Vector3f, CompatQuaternion, i16, u8, Vector2f, etc.)
 */
template <typename T>
struct AnimationTrack : public AnimationTrackBase {
    std::vector<std::vector<T>> values; ///< [animation][keyframe] values
};

// ============================================================================
// Version Constants
// ============================================================================

/**
 * @brief M2 format version constants by expansion
 *
 * The version field in the MD20 header encodes a major.minor pair as
 * major * 256 + minor. Version determines which structural fields and
 * parsing branches are active (e.g., inline vs external skins, camera
 * FOV format, particle record size).
 */
constexpr u16 M2_VERSION_VANILLA = 256; ///< 1.0 — Classic / Pre-Release
constexpr u16 M2_VERSION_BC = 260;      ///< 1.4 — The Burning Crusade
constexpr u16 M2_VERSION_WOTLK = 264;   ///< 1.8 — Wrath of the Lich King (external skins)
constexpr u16 M2_VERSION_CATA = 265;    ///< 1.9 — Cataclysm (animated camera FOV, shadow batches)
constexpr u16 M2_VERSION_MOP = 272;     ///< 1.16 — Mists of Pandaria
constexpr u16 M2_VERSION_WOD = 272;     ///< 1.16 — Warlords of Draenor
constexpr u16 M2_VERSION_LEGION = 272;  ///< 1.16 — Legion (MD21 chunked format introduced)
constexpr u16 M2_VERSION_BFA = 273;     ///< 1.17 — Battle for Azeroth (TXID replaces filenames)
constexpr u16 M2_VERSION_SL = 274;      ///< 1.18 — Shadowlands

// ============================================================================
// Chunk Tags
// ============================================================================

/**
 * @brief Create a chunk tag from a 4-character string at compile time
 *
 * M2 chunk tags are stored as little-endian u32 values. Unlike other WoW
 * chunked formats, M2 tags are NOT byte-reversed in the file.
 *
 * @tparam N String literal size (must be 5 including null terminator)
 * @param str 4-character tag string
 * @return u32 Little-endian chunk tag value
 */
template <std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0])) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 24);
}

constexpr u32 MD20_TAG =
    makeTag("MD20"); ///< Classic flat format magic (file starts with MD20 header)
constexpr u32 MD21_TAG =
    makeTag("MD21"); ///< Chunked format magic (Legion+; wraps MD20 payload in chunks)

} // namespace m2
} // namespace whiteout
