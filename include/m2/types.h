#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <array>
#include <map>
#include <memory>
#include <span>

#include "../common/common_types.h"

namespace m2 {

using ::Extent;

template<typename T>
struct AnimationKey {
    u32 timestamp = 0;
    T value;
};

template<typename T>
struct SplineKey {
    u32 timestamp = 0;
    T value;
    T inTangent;
    T outTangent;
};

struct AnimationTrackBase {
    u16 interpolationType = 0;
    u16 globalSequenceId = 0xFFFF;
    std::vector<std::vector<u32>> timestamps;
};

struct M2Extent {
    Vector3f minimum;
    Vector3f maximum;
    f32 sphereRadius = 0.0f;
};

template<typename T>
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


template<std::size_t N>
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
