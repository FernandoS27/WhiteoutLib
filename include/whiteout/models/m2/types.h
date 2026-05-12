
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../../common_types.h"
#include "../../compatibility.h"
#include "../../vector_types.h"

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

struct ColorBGRA {
    union {
        struct {
            u8 b, g, r, a;
        };
        Vector4<u8> data;
    };

    ColorBGRA() = default;
    constexpr ColorBGRA(u8 b_, u8 g_, u8 r_, u8 a_) : b(b_), g(g_), r(r_), a(a_) {}
    constexpr ColorBGRA(const Vector4<u8>& vec) : data(vec) {}

    operator Vector4f() const {
        return Vector4f{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
    }
};

/// @bind value_object
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

/// @bind
struct AnimationTrackBase {
    InterpolationType interpolationType = InterpolationType::None;
    u16 globalSequenceId = 0xFFFF;
    std::vector<std::vector<u32>> timestamps;
};

/// @bind value_template, instantiate=Vector3f;CompatQuaternion;i16;u8;u16;f32;CameraSpline
template <typename T>
struct AnimationTrack : public AnimationTrackBase {
    std::vector<std::vector<T>> values;
};

// Applies an animation during a particle lifetime
/// @bind value_template, instantiate=Vector3f;Vector2f;unorm16
template <typename T>
struct ParticleAnimationTrack {
    std::vector<unorm16> timestamps; // unorm 0 to 1 where 1 is the end of lifetime.
    std::vector<T> values;
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
