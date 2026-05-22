// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "../common_types.h"
#include "../vector_types.h"

#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace whiteout::interfaces {
class WorkerPool;
}

namespace whiteout::utils {

// ============================================================================
// Enums
// ============================================================================

/// @bind
enum class AttributeClass : u32 {
    Position = 0,
    Normal = 1,
    Tangent = 2,
    UV = 3,
    Color = 4,
    BlendIndices = 5,
    BlendWeights = 6,
    Binormal = 7,
    Count,
};

/// @bind
enum class AttributeEncoding : u32 {
    Float32, // 4 bytes/component — f32 passthrough
    Float16, // 2 bytes/component — f16 half-float
    SNorm8,  // 1 byte/component  — [-1,1] → i8
    SNorm16, // 2 bytes/component — [-1,1] → i16
    UNorm8,  // 1 byte/component  — [0,1]  → u8
    UNorm16, // 2 bytes/component — [0,1]  → u16
    UInt8,   // 1 byte/component  — unsigned integer passthrough
    UInt16,  // 2 bytes/component
    UInt32,  // 4 bytes/component
    Int8,    // 1 byte/component  — signed integer passthrough
    Int16,   // 2 bytes/component
    Int32,   // 4 bytes/component
};

// Backward compatibility
using AttributeType = AttributeEncoding;

// ============================================================================
// VertexBuffer
// ============================================================================

/// @bind methods
struct VertexBuffer {
    /// @bind value_object
    struct Attribute {
        AttributeClass attr_class;
        AttributeEncoding encoding;
        size_t component_count; // 1-4
        size_t offset;
    };
    std::vector<u8> data;          // Gpu-friendly interleaved vertex data
    std::vector<Attribute> layout; // per-vertex byte offset for each attribute class
    u32 vertex_stride = 0;         // total bytes per vertex

    /// @brief Get total number of vertices in the buffer
    size_t vertexCount() const;
    /// @brief Get per-vertex byte stride
    size_t vertexSize() const;
    /// @brief Get number of UV layers present
    size_t UVsNum() const;
    /// @brief Check if vertex color data is present
    bool hasVertexColors() const;

    /// @brief Extract vertex positions (Vector3f, 12 bytes at offset 0)
    std::vector<Vector3f> getPositions() const;
    /// @brief Extract vertex normals (3 x i8 at offset 20, divided by 127.0)
    std::vector<Vector3f> getNormals() const;
    /// @brief Extract tangent vectors (3 x i8 + sign byte at end of vertex)
    std::vector<Vector4f> getTangents() const;
    /// @brief Extract UV coordinates for a layer (i16 pairs, divided by 2048.0)
    std::vector<Vector2f> getUVs(size_t which) const;
    /**
     * @brief Extract UV coordinates using region-level scale/offset (REGN v5+)
     * @param which UV layer index (0-based)
     * @param uvMultiply UV scale factor (default 16.0 in REGN v5+)
     * @param uvOffset UV offset (default 0.0 in REGN v5+)
     * @return Vector of UV coordinates: float_uv = i16_uv * uvMultiply + uvOffset
     */
    std::vector<Vector2f> getUVs(size_t which, f32 uvMultiply, f32 uvOffset) const;
    std::vector<Vector4f> getColors() const;
    /// @brief Extract bone indices (4 x u8 at offset 16, region-local)
    std::vector<std::array<u32, 4>> getBoneIndices() const;
    /// @brief Extract bone weights (4 x u8 at offset 12, divide by 255.0)
    std::vector<std::array<f32, 4>> getBoneWeights() const;
};

// ============================================================================
// vertex_traits<T> — compile-time type introspection for vertex elements
// ============================================================================

// Primary template — undefined (SFINAE: unsupported types cause compile error)
template <typename T, typename Enable = void>
struct vertex_traits;

// --- Floating-point scalars --------------------------------------------------

template <>
struct vertex_traits<f32> {
    static const size_t component_count = 1;
    static const bool is_integer = false;
    static f32 get_float(f32 v, size_t) {
        return v;
    }
};

template <>
struct vertex_traits<f64> {
    static const size_t component_count = 1;
    static const bool is_integer = false;
    static f32 get_float(f64 v, size_t) {
        return static_cast<f32>(v);
    }
};

template <>
struct vertex_traits<f16> {
    static const size_t component_count = 1;
    static const bool is_integer = false;
    static f32 get_float(f16 v, size_t) {
        return v.to_float();
    }
};

// --- Unsigned integer scalars ------------------------------------------------

template <>
struct vertex_traits<u8> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(u8 v, size_t) {
        return static_cast<u32>(v);
    }
};

template <>
struct vertex_traits<u16> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(u16 v, size_t) {
        return static_cast<u32>(v);
    }
};

template <>
struct vertex_traits<u32> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(u32 v, size_t) {
        return v;
    }
};

// --- Signed integer scalars --------------------------------------------------

template <>
struct vertex_traits<i8> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(i8 v, size_t) {
        u32 r;
        std::memcpy(&r, &v, sizeof(v));
        // Sign-extend via i32 so negative values survive round-trip to signed targets
        i32 ext = static_cast<i32>(v);
        std::memcpy(&r, &ext, sizeof(r));
        return r;
    }
};

template <>
struct vertex_traits<i16> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(i16 v, size_t) {
        u32 r;
        i32 ext = static_cast<i32>(v);
        std::memcpy(&r, &ext, sizeof(r));
        return r;
    }
};

template <>
struct vertex_traits<i32> {
    static const size_t component_count = 1;
    static const bool is_integer = true;
    static u32 get_uint(i32 v, size_t) {
        u32 r;
        std::memcpy(&r, &v, sizeof(r));
        return r;
    }
};

// --- snorm / unorm scalars ---------------------------------------------------

template <typename SInt>
struct vertex_traits<snorm<SInt>> {
    static const size_t component_count = 1;
    static const bool is_integer = false;
    static f32 get_float(snorm<SInt> v, size_t) {
        return static_cast<f32>(v);
    }
};

template <typename UInt>
struct vertex_traits<unorm<UInt>> {
    static const size_t component_count = 1;
    static const bool is_integer = false;
    static f32 get_float(unorm<UInt> v, size_t) {
        return static_cast<f32>(v);
    }
};

// --- Vector2<T> --------------------------------------------------------------

template <typename T>
struct vertex_traits<Vector2<T>, typename std::enable_if<!vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 2;
    static const bool is_integer = false;
    static f32 get_float(const Vector2<T>& v, size_t i) {
        return vertex_traits<T>::get_float(v.data[i], 0);
    }
};

template <typename T>
struct vertex_traits<Vector2<T>, typename std::enable_if<vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 2;
    static const bool is_integer = true;
    static u32 get_uint(const Vector2<T>& v, size_t i) {
        return vertex_traits<T>::get_uint(v.data[i], 0);
    }
};

// --- Vector3<T> --------------------------------------------------------------

template <typename T>
struct vertex_traits<Vector3<T>, typename std::enable_if<!vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 3;
    static const bool is_integer = false;
    static f32 get_float(const Vector3<T>& v, size_t i) {
        return vertex_traits<T>::get_float(v.data[i], 0);
    }
};

template <typename T>
struct vertex_traits<Vector3<T>, typename std::enable_if<vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 3;
    static const bool is_integer = true;
    static u32 get_uint(const Vector3<T>& v, size_t i) {
        return vertex_traits<T>::get_uint(v.data[i], 0);
    }
};

// --- Vector4<T> --------------------------------------------------------------

template <typename T>
struct vertex_traits<Vector4<T>, typename std::enable_if<!vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 4;
    static const bool is_integer = false;
    static f32 get_float(const Vector4<T>& v, size_t i) {
        return vertex_traits<T>::get_float(v.data[i], 0);
    }
};

template <typename T>
struct vertex_traits<Vector4<T>, typename std::enable_if<vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = 4;
    static const bool is_integer = true;
    static u32 get_uint(const Vector4<T>& v, size_t i) {
        return vertex_traits<T>::get_uint(v.data[i], 0);
    }
};

// --- Quaternion --------------------------------------------------------------

template <>
struct vertex_traits<Quaternion> {
    static const size_t component_count = 4;
    static const bool is_integer = false;
    static f32 get_float(const Quaternion& v, size_t i) {
        return v.data[i];
    }
};

// --- std::array<T, N> --------------------------------------------------------

template <typename T, size_t N>
struct vertex_traits<std::array<T, N>,
                     typename std::enable_if<!vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = N;
    static const bool is_integer = false;
    static f32 get_float(const std::array<T, N>& v, size_t i) {
        return vertex_traits<T>::get_float(v[i], 0);
    }
};

template <typename T, size_t N>
struct vertex_traits<std::array<T, N>,
                     typename std::enable_if<vertex_traits<T>::is_integer>::type> {
    static const size_t component_count = N;
    static const bool is_integer = true;
    static u32 get_uint(const std::array<T, N>& v, size_t i) {
        return vertex_traits<T>::get_uint(v[i], 0);
    }
};

// ============================================================================
// VertexBufferBuilder
// ============================================================================

/// @bind methods
class VertexBufferBuilder {
public:
    VertexBufferBuilder();
    ~VertexBufferBuilder();

    VertexBufferBuilder(VertexBufferBuilder&&) noexcept;
    VertexBufferBuilder& operator=(VertexBufferBuilder&&) noexcept;

    VertexBufferBuilder(const VertexBufferBuilder&) = delete;
    VertexBufferBuilder& operator=(const VertexBufferBuilder&) = delete;

    /// Declare a vertex attribute. Component count is auto-deduced from T.
    /// @param align  If non-zero, the attribute's slot is padded to this alignment.
    ///               The next attribute's offset = current_offset + round_up(natural_size, align).
    /// Supported T: f32, f64, f16, u8, u16, u32, i8, i16, i32,
    ///              snorm<S>, unorm<U>, Vector2<T>, Vector3<T>, Vector4<T>,
    ///              Quaternion, std::array<T,N>.
    template <typename T>
    VertexBufferBuilder& declareAttribute(const std::vector<T>& src_data, AttributeClass attr_class,
                                          AttributeEncoding encoding, size_t align = 0) {
        return declareAttributeDispatch(
            src_data, attr_class, encoding, align,
            std::integral_constant<bool, vertex_traits<T>::is_integer>{});
    }

    /// Non-template float-attribute entry point — accepts an already-
    /// flattened `f32` array as a `std::span` so the binding layer can
    /// aim straight at numpy buffers / `Float32Array` views with no
    /// intermediate copy (vertex_count = data.size() / components).
    VertexBufferBuilder& declareFloatAttribute(std::span<const f32> data, size_t components,
                                               AttributeClass attr_class,
                                               AttributeEncoding encoding, size_t align = 0) {
        return declareAttributeFloat(data.data(), data.size() / components, components, attr_class,
                                     encoding, align);
    }

    /// Non-template integer-attribute entry point — accepts an already-
    /// flattened `u32` array. Smaller integer types (u8/u16/i8/i16/i32)
    /// are upcast at the call site. `std::span` keeps the boundary
    /// zero-copy.
    VertexBufferBuilder& declareIntAttribute(std::span<const u32> data, size_t components,
                                             AttributeClass attr_class, AttributeEncoding encoding,
                                             size_t align = 0) {
        return declareAttributeUint(data.data(), data.size() / components, components, attr_class,
                                    encoding, align);
    }

    /// Build the interleaved vertex buffer from all declared attributes.
    /// @param pool  Optional WorkerPool for parallel encoding. When non-null
    ///              and the vertex count is large enough, the vertex encoding
    ///              loop is split into cache-friendly chunks across workers.
    VertexBuffer build(interfaces::WorkerPool* pool = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Non-template PImpl methods — these cross the C++11/C++20 boundary
    VertexBufferBuilder& declareAttributeFloat(const f32* data, size_t vertex_count,
                                               size_t components, AttributeClass attr_class,
                                               AttributeEncoding encoding, size_t align);
    VertexBufferBuilder& declareAttributeUint(const u32* data, size_t vertex_count,
                                              size_t components, AttributeClass attr_class,
                                              AttributeEncoding encoding, size_t align);

    // Zero-copy fast path for contiguous float types (Vector2f, Vector3f, Vector4f, Quaternion)
    VertexBufferBuilder& declareAttributeFloatDirect(const f32* data, size_t vertex_count,
                                                     size_t components, AttributeClass attr_class,
                                                     AttributeEncoding encoding, size_t align);

    // Float source — flatten and forward
    template <typename T>
    VertexBufferBuilder& declareAttributeDispatch(const std::vector<T>& src_data,
                                                  AttributeClass attr_class,
                                                  AttributeEncoding encoding, size_t align,
                                                  std::false_type /*is_integer*/) {
        typedef vertex_traits<T> Traits;
        const size_t N = Traits::component_count;
        const size_t vert_count = src_data.size();
        std::vector<f32> flat(vert_count * N);
        for (size_t v = 0; v < vert_count; ++v)
            for (size_t i = 0; i < N; ++i)
                flat[v * N + i] = Traits::get_float(src_data[v], i);
        return declareAttributeFloat(flat.data(), vert_count, N, attr_class, encoding, align);
    }

    // Integer source — flatten and forward
    template <typename T>
    VertexBufferBuilder& declareAttributeDispatch(const std::vector<T>& src_data,
                                                  AttributeClass attr_class,
                                                  AttributeEncoding encoding, size_t align,
                                                  std::true_type /*is_integer*/) {
        typedef vertex_traits<T> Traits;
        const size_t N = Traits::component_count;
        const size_t vert_count = src_data.size();
        std::vector<u32> flat(vert_count * N);
        for (size_t v = 0; v < vert_count; ++v)
            for (size_t i = 0; i < N; ++i)
                flat[v * N + i] = Traits::get_uint(src_data[v], i);
        return declareAttributeUint(flat.data(), vert_count, N, attr_class, encoding, align);
    }
};

// ============================================================================
// Zero-copy fast-path specializations for contiguous float vector types.
// These avoid the flatten copy when the in-memory layout is already packed f32.
// ============================================================================

template <>
inline VertexBufferBuilder& VertexBufferBuilder::declareAttributeDispatch<Vector2f>(
    const std::vector<Vector2f>& src_data, AttributeClass attr_class, AttributeEncoding encoding,
    size_t align, std::false_type) {
    return declareAttributeFloatDirect(reinterpret_cast<const f32*>(src_data.data()),
                                       src_data.size(), 2, attr_class, encoding, align);
}

template <>
inline VertexBufferBuilder& VertexBufferBuilder::declareAttributeDispatch<Vector3f>(
    const std::vector<Vector3f>& src_data, AttributeClass attr_class, AttributeEncoding encoding,
    size_t align, std::false_type) {
    return declareAttributeFloatDirect(reinterpret_cast<const f32*>(src_data.data()),
                                       src_data.size(), 3, attr_class, encoding, align);
}

template <>
inline VertexBufferBuilder& VertexBufferBuilder::declareAttributeDispatch<Vector4f>(
    const std::vector<Vector4f>& src_data, AttributeClass attr_class, AttributeEncoding encoding,
    size_t align, std::false_type) {
    return declareAttributeFloatDirect(reinterpret_cast<const f32*>(src_data.data()),
                                       src_data.size(), 4, attr_class, encoding, align);
}

template <>
inline VertexBufferBuilder& VertexBufferBuilder::declareAttributeDispatch<Quaternion>(
    const std::vector<Quaternion>& src_data, AttributeClass attr_class, AttributeEncoding encoding,
    size_t align, std::false_type) {
    return declareAttributeFloatDirect(reinterpret_cast<const f32*>(src_data.data()),
                                       src_data.size(), 4, attr_class, encoding, align);
}

template <>
inline VertexBufferBuilder& VertexBufferBuilder::declareAttributeDispatch<f32>(
    const std::vector<f32>& src_data, AttributeClass attr_class, AttributeEncoding encoding,
    size_t align, std::false_type) {
    return declareAttributeFloatDirect(src_data.data(), src_data.size(), 1, attr_class, encoding,
                                       align);
}

} // namespace whiteout::utils
