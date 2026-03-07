
#include <whiteout/m3/m3.h>

namespace whiteout {
namespace m3 {

template <typename T, typename = void>
struct ChunkTagTraits {
    static constexpr u32 value = T::tag;
    static constexpr u32 max_version = T::max_version;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<char> {
    static constexpr u32 value = TAG_CHAR;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u8> {
    static constexpr u32 value = TAG_U8;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u16> {
    static constexpr u32 value = TAG_U16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u32> {
    static constexpr u32 value = TAG_U32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u64> {
    static constexpr u32 value = TAG_U64;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i16> {
    static constexpr u32 value = TAG_I16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i32> {
    static constexpr u32 value = TAG_I32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<f32> {
    static constexpr u32 value = TAG_REAL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Flag> {
    static constexpr u32 value = TAG_FLAG;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector2f> {
    static constexpr u32 value = TAG_VEC2;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector3f> {
    static constexpr u32 value = TAG_VEC3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector4f> {
    static constexpr u32 value = TAG_VEC4;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Quaternion> {
    static constexpr u32 value = TAG_QUAT;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<ColorBGRA> {
    static constexpr u32 value = TAG_COL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Extent> {
    static constexpr u32 value = TAG_BNDS;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimBlock<Event>> {
    static constexpr u32 value = TAG_SDEV;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Vector2f>> {
    static constexpr u32 value = TAG_SD2V;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Vector3f>> {
    static constexpr u32 value = TAG_SD3V;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<f32>> {
    static constexpr u32 value = TAG_SDR3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Quaternion>> {
    static constexpr u32 value = TAG_SD4Q;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<ColorBGRA>> {
    static constexpr u32 value = TAG_SDCC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u8>> {
    static constexpr u32 value = TAG_SDU8;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<i16>> {
    static constexpr u32 value = TAG_SDS6;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u16>> {
    static constexpr u32 value = TAG_SDU6;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<i32>> {
    static constexpr u32 value = TAG_SDS3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u32>> {
    static constexpr u32 value = TAG_SDU3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Flag>> {
    static constexpr u32 value = TAG_SDFG;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Extent>> {
    static constexpr u32 value = TAG_SDMB;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimRef<i32>> {
    static constexpr u32 value = TAG_SS32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<u32>> {
    static constexpr u32 value = TAG_SU32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<f32>> {
    static constexpr u32 value = TAG_SR32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<Vector3f>> {
    static constexpr u32 value = TAG_SVC3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<Vector2f>> {
    static constexpr u32 value = TAG_SVC2;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u16, 7>> {
    static constexpr u32 value = TAG_MT16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u32, 7>> {
    static constexpr u32 value = TAG_MT32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::string> {
    static constexpr u32 value = TAG_SCHR;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

} // namespace m3
} // namespace whiteout