// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../common_types.h"
#include "../vector_types.h"

namespace whiteout {
namespace m2 {

struct CompatQuaternion {
    union {
        struct {
            u16 x, y, z, w;
        };
        std::array<u16, 4> data;
    };

    CompatQuaternion() = default;
    constexpr CompatQuaternion(u16 x_, u16 y_, u16 z_, u16 w_) : x(x_), y(y_), z(z_), w(w_) {}

    // From normalized floats
    static constexpr CompatQuaternion fromFloats(f32 fx, f32 fy, f32 fz, f32 fw) {
        return CompatQuaternion(snorm16::from_float(fx).value, snorm16::from_float(fy).value,
                                snorm16::from_float(fz).value, unorm16::from_float(fw).value);
    }

    operator Vector4f() const {
        return Vector4f{
            static_cast<f32>(snorm16::from_raw(x)), static_cast<f32>(snorm16::from_raw(y)),
            static_cast<f32>(snorm16::from_raw(z)), static_cast<f32>(unorm16::from_raw(w))};
    }

    operator Quaternion() const {
        return Quaternion{
            static_cast<f32>(snorm16::from_raw(x)), static_cast<f32>(snorm16::from_raw(y)),
            static_cast<f32>(snorm16::from_raw(z)), static_cast<f32>(unorm16::from_raw(w))};
    }
};

struct Extent {
    Vector3f minimum;
    Vector3f maximum;
    f32 sphereRadius = 0.0f;
};

template <typename T>
struct AnimationKey {
    u32 timestamp = 0;
    T value;
};

template <typename T>
struct SplineKey {
    u32 timestamp = 0;
    T value;
    T inTangent;
    T outTangent;
};

enum class InterpolationType : u16 {
    None = 0,
    Linear = 1,
    Bezier = 2,
    Hermite = 3,
};

template <typename T>
struct lazy_vector {
    struct M2ArrayHeader {
        u32 count = 0;
        u32 offset = 0;
    };

    std::vector<T> backingData;
    std::vector<bool> isLoaded;
    std::vector<std::pair<size_t, M2ArrayHeader>> pendingLoads;

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

struct AnimationTrackBase {
    InterpolationType interpolationType = InterpolationType::None;
    u16 globalSequenceId = 0xFFFF;
    std::vector<std::vector<u32>> timestamps;
};

template <typename T>
struct AnimationTrack : public AnimationTrackBase {
    std::vector<std::vector<T>> values;
};

constexpr u16 M2_VERSION_VANILLA = 256;
constexpr u16 M2_VERSION_BC = 260;
constexpr u16 M2_VERSION_WOTLK = 264;
constexpr u16 M2_VERSION_CATA = 265;
constexpr u16 M2_VERSION_MOP = 272;
constexpr u16 M2_VERSION_WOD = 272;
constexpr u16 M2_VERSION_LEGION = 272;
constexpr u16 M2_VERSION_BFA = 273;
constexpr u16 M2_VERSION_SL = 274;

template <std::size_t N>
constexpr u32 makeTag(const char (&str)[N]) {
    static_assert(N == 5, "Tag must be exactly 4 characters (plus null terminator)");
    return static_cast<u32>(static_cast<u8>(str[0])) |
           (static_cast<u32>(static_cast<u8>(str[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(str[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(str[3])) << 24);
}

constexpr u32 MD20_TAG = makeTag("MD20");
constexpr u32 MD21_TAG = makeTag("MD21");

} // namespace m2
} // namespace whiteout
