// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// Vertex Buffer Builder — comprehensive test suite
// Usage: vertex_buffer_test (no arguments, returns 0 on success)

#include <catch2/catch_all.hpp>

#include <whiteout/utils/vertex_buffer.h>
#include <whiteout/utils/simple_thread_pool.h>
#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace whiteout;
using namespace whiteout::utils;

// ============================================================================





// Helper: read typed value from byte buffer at offset
template <typename T>
static T readAt(const u8* data, size_t offset) {
    T val;
    std::memcpy(&val, data + offset, sizeof(T));
    return val;
}

// ============================================================================
// Tests: Empty builder
// ============================================================================

TEST_CASE("empty_builder", "[vertex_buffer]") {
    VertexBufferBuilder builder;
    VertexBuffer vb = builder.build();
    CHECK(vb.data.empty());
    CHECK(vb.layout.empty());
    CHECK(vb.vertex_stride == 0);
}

// ============================================================================
// Tests: Single scalar f32 attribute → Float32
// ============================================================================

TEST_CASE("single_f32_float32", "[vertex_buffer]") {
    std::vector<f32> values = {1.0f, 2.0f, 3.0f};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout.size() == 1);
    CHECK(vb.layout[0].attr_class == AttributeClass::Position);
    CHECK(vb.layout[0].encoding == AttributeEncoding::Float32);
    CHECK(vb.layout[0].component_count == 1);
    CHECK(vb.vertex_stride == 4);
    CHECK(vb.data.size() == 12); // 3 verts * 4 bytes

    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
}

// ============================================================================
// Tests: Vector3f → Float32 (3 components auto-detected)
// ============================================================================

TEST_CASE("vector3f_float32", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {
        {1.0f, 2.0f, 3.0f},
        {4.0f, 5.0f, 6.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout.size() == 1);
    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.vertex_stride == 12); // 3 * sizeof(f32)
    CHECK(vb.data.size() == 24);   // 2 verts * 12 bytes

    // Vertex 0
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
    // Vertex 1
    CHECK(readAt<f32>(vb.data.data(), 12) == Catch::Approx(4.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 16) == Catch::Approx(5.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 20) == Catch::Approx(6.0f).margin(1e-6f));
}

// ============================================================================
// Tests: Vector3f → Float16 (half-precision)
// ============================================================================

TEST_CASE("vector3f_float16", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {
        {1.0f, -1.0f, 0.5f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.vertex_stride == 6); // 3 * sizeof(u16)

    // Decode halfs back to float
    u16 raw0 = readAt<u16>(vb.data.data(), 0);
    u16 raw1 = readAt<u16>(vb.data.data(), 2);
    u16 raw2 = readAt<u16>(vb.data.data(), 4);
    CHECK(static_cast<f32>(f16::from_raw(raw0).to_float()) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-3f));
    CHECK(static_cast<f32>(f16::from_raw(raw1).to_float()) == Catch::Approx(static_cast<f32>(-1.0f)).margin(1e-3f));
    CHECK(static_cast<f32>(f16::from_raw(raw2).to_float()) == Catch::Approx(static_cast<f32>(0.5f)).margin(1e-3f));
}

// ============================================================================
// Tests: Vector2f → SNorm16
// ============================================================================

TEST_CASE("vector2f_snorm16", "[vertex_buffer]") {
    std::vector<Vector2f> uvs = {
        {0.5f, -0.5f},
        {1.0f, -1.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::SNorm16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 2);
    CHECK(vb.vertex_stride == 4); // 2 * sizeof(i16)

    // Decode snorm16 and check
    i16 s0 = readAt<i16>(vb.data.data(), 0);
    i16 s1 = readAt<i16>(vb.data.data(), 2);
    f32 decoded0 = static_cast<f32>(s0) / 32767.0f;
    f32 decoded1 = static_cast<f32>(s1) / 32767.0f;
    CHECK(static_cast<f32>(decoded0) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.001f));
    CHECK(static_cast<f32>(decoded1) == Catch::Approx(static_cast<f32>(-0.5f)).margin(0.001f));

    i16 s2 = readAt<i16>(vb.data.data(), 4);
    i16 s3 = readAt<i16>(vb.data.data(), 6);
    f32 decoded2 = static_cast<f32>(s2) / 32767.0f;
    f32 decoded3 = static_cast<f32>(s3) / 32767.0f;
    CHECK(static_cast<f32>(decoded2) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.001f));
    CHECK(static_cast<f32>(decoded3) == Catch::Approx(static_cast<f32>(-1.0f)).margin(0.001f));
}

// ============================================================================
// Tests: Vector3f → SNorm8 (e.g. normals)
// ============================================================================

TEST_CASE("vector3f_snorm8", "[vertex_buffer]") {
    std::vector<Vector3f> normals = {
        {0.0f, 1.0f, 0.0f},
        {-1.0f, 0.0f, 0.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.vertex_stride == 3); // 3 * sizeof(i8)

    // Vertex 0: (0, 1, 0)
    i8 n00 = readAt<i8>(vb.data.data(), 0);
    i8 n01 = readAt<i8>(vb.data.data(), 1);
    i8 n02 = readAt<i8>(vb.data.data(), 2);
    CHECK(static_cast<f32>(static_cast<f32>(n00) / 127.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
    CHECK(static_cast<f32>(static_cast<f32>(n01) / 127.0f) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(static_cast<f32>(n02) / 127.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));

    // Vertex 1: (-1, 0, 0)
    i8 n10 = readAt<i8>(vb.data.data(), 3);
    i8 n11 = readAt<i8>(vb.data.data(), 4);
    i8 n12 = readAt<i8>(vb.data.data(), 5);
    CHECK(static_cast<f32>(static_cast<f32>(n10) / 127.0f) == Catch::Approx(static_cast<f32>(-1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(static_cast<f32>(n11) / 127.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
    CHECK(static_cast<f32>(static_cast<f32>(n12) / 127.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
}

// ============================================================================
// Tests: Vector4f → UNorm8 (e.g. blend weights)
// ============================================================================

TEST_CASE("vector4f_unorm8", "[vertex_buffer]") {
    std::vector<Vector4f> weights = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.25f, 0.125f, 0.125f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(weights, AttributeClass::BlendWeights, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 4); // 4 * sizeof(u8)

    // Vertex 0: (1.0, 0.0, 0.0, 0.0)
    CHECK(vb.data[0] == 255);
    CHECK(vb.data[1] == 0);
    CHECK(vb.data[2] == 0);
    CHECK(vb.data[3] == 0);

    // Vertex 1: (0.5, 0.25, 0.125, 0.125) — approximate
    CHECK(static_cast<f32>(vb.data[4] / 255.0f) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[5] / 255.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[6] / 255.0f) == Catch::Approx(static_cast<f32>(0.125f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[7] / 255.0f) == Catch::Approx(static_cast<f32>(0.125f)).margin(0.01f));
}

// ============================================================================
// Tests: std::array<u8,4> → UInt8 (e.g. blend indices)
// ============================================================================

TEST_CASE("array_u8_4_uint8", "[vertex_buffer]") {
    std::vector<std::array<u8, 4>> indices = {
        {0, 1, 2, 3},
        {10, 20, 30, 40},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 4);

    CHECK(vb.data[0] == 0);
    CHECK(vb.data[1] == 1);
    CHECK(vb.data[2] == 2);
    CHECK(vb.data[3] == 3);
    CHECK(vb.data[4] == 10);
    CHECK(vb.data[5] == 20);
    CHECK(vb.data[6] == 30);
    CHECK(vb.data[7] == 40);
}

// ============================================================================
// Tests: std::array<u32,4> → UInt32
// ============================================================================

TEST_CASE("array_u32_4_uint32", "[vertex_buffer]") {
    std::vector<std::array<u32, 4>> indices = {
        {100, 200, 300, 400},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(indices, AttributeClass::BlendIndices, AttributeEncoding::UInt32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 16); // 4 * sizeof(u32)

    CHECK(readAt<u32>(vb.data.data(), 0) == 100);
    CHECK(readAt<u32>(vb.data.data(), 4) == 200);
    CHECK(readAt<u32>(vb.data.data(), 8) == 300);
    CHECK(readAt<u32>(vb.data.data(), 12) == 400);
}

// ============================================================================
// Tests: u8 scalar → UInt8
// ============================================================================

TEST_CASE("scalar_u8_uint8", "[vertex_buffer]") {
    std::vector<u8> values = {10, 20, 30};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UInt8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 1);
    CHECK(vb.vertex_stride == 1);
    CHECK(vb.data[0] == 10);
    CHECK(vb.data[1] == 20);
    CHECK(vb.data[2] == 30);
}

// ============================================================================
// Tests: u16 scalar → UInt16
// ============================================================================

TEST_CASE("scalar_u16_uint16", "[vertex_buffer]") {
    std::vector<u16> values = {1000, 2000, 3000};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UInt16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 1);
    CHECK(vb.vertex_stride == 2);
    CHECK(readAt<u16>(vb.data.data(), 0) == 1000);
    CHECK(readAt<u16>(vb.data.data(), 2) == 2000);
    CHECK(readAt<u16>(vb.data.data(), 4) == 3000);
}

// ============================================================================
// Tests: u32 scalar → UInt32
// ============================================================================

TEST_CASE("scalar_u32_uint32", "[vertex_buffer]") {
    std::vector<u32> values = {0xDEADBEEF, 0xCAFEBABE};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UInt32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 1);
    CHECK(vb.vertex_stride == 4);
    CHECK(readAt<u32>(vb.data.data(), 0) == 0xDEADBEEF);
    CHECK(readAt<u32>(vb.data.data(), 4) == 0xCAFEBABE);
}

// ============================================================================
// Tests: Vector2f attribute
// ============================================================================

TEST_CASE("vector2f_float32", "[vertex_buffer]") {
    std::vector<Vector2f> uvs = {
        {0.0f, 1.0f},
        {0.5f, 0.5f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 2);
    CHECK(vb.vertex_stride == 8);

    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(0.5f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 12) == Catch::Approx(0.5f).margin(1e-6f));
}

// ============================================================================
// Tests: Quaternion → Float32 (4 components)
// ============================================================================

TEST_CASE("quaternion_float32", "[vertex_buffer]") {
    std::vector<Quaternion> quats = {
        {0.0f, 0.0f, 0.0f, 1.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(quats, AttributeClass::Tangent, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 16);

    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 12) == Catch::Approx(1.0f).margin(1e-6f));
}

// ============================================================================
// Tests: Quaternion → SNorm16 (4 components compressed)
// ============================================================================

TEST_CASE("quaternion_snorm16", "[vertex_buffer]") {
    std::vector<Quaternion> quats = {
        {0.0f, 0.707107f, 0.0f, 0.707107f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(quats, AttributeClass::Tangent, AttributeEncoding::SNorm16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 8); // 4 * sizeof(i16)

    i16 s0 = readAt<i16>(vb.data.data(), 0);
    i16 s1 = readAt<i16>(vb.data.data(), 2);
    i16 s2 = readAt<i16>(vb.data.data(), 4);
    i16 s3 = readAt<i16>(vb.data.data(), 6);

    CHECK(static_cast<f32>(static_cast<f32>(s0) / 32767.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.001f));
    CHECK(static_cast<f32>(static_cast<f32>(s1) / 32767.0f) == Catch::Approx(static_cast<f32>(0.707107f)).margin(0.001f));
    CHECK(static_cast<f32>(static_cast<f32>(s2) / 32767.0f) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.001f));
    CHECK(static_cast<f32>(static_cast<f32>(s3) / 32767.0f) == Catch::Approx(static_cast<f32>(0.707107f)).margin(0.001f));
}

// ============================================================================
// Tests: Vector3u → UInt32
// ============================================================================

TEST_CASE("vector3u_uint32", "[vertex_buffer]") {
    std::vector<Vector3u> data = {
        {10, 20, 30},
        {40, 50, 60},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::BlendIndices, AttributeEncoding::UInt32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.vertex_stride == 12);

    CHECK(readAt<u32>(vb.data.data(), 0) == 10);
    CHECK(readAt<u32>(vb.data.data(), 4) == 20);
    CHECK(readAt<u32>(vb.data.data(), 8) == 30);
    CHECK(readAt<u32>(vb.data.data(), 12) == 40);
    CHECK(readAt<u32>(vb.data.data(), 16) == 50);
    CHECK(readAt<u32>(vb.data.data(), 20) == 60);
}

// ============================================================================
// Tests: Vector3u → UInt8 (truncation)
// ============================================================================

TEST_CASE("vector3u_uint8", "[vertex_buffer]") {
    std::vector<Vector3u> data = {
        {1, 2, 3},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.vertex_stride == 3);
    CHECK(vb.data[0] == 1);
    CHECK(vb.data[1] == 2);
    CHECK(vb.data[2] == 3);
}

// ============================================================================
// Tests: Vector4i → Int32
// ============================================================================

TEST_CASE("vector4i_int32", "[vertex_buffer]") {
    std::vector<Vector4i> data = {
        {-1, 0, 1, 2},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::BlendIndices, AttributeEncoding::Int32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 16);
    CHECK(readAt<i32>(vb.data.data(), 0) == -1);
    CHECK(readAt<i32>(vb.data.data(), 4) == 0);
    CHECK(readAt<i32>(vb.data.data(), 8) == 1);
    CHECK(readAt<i32>(vb.data.data(), 12) == 2);
}

// ============================================================================
// Tests: std::array<f32, 4> → Float32
// ============================================================================

TEST_CASE("array_f32_4_float32", "[vertex_buffer]") {
    std::vector<std::array<f32, 4>> data = {
        {1.0f, 2.0f, 3.0f, 4.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::Color, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 16);
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 12) == Catch::Approx(4.0f).margin(1e-6f));
}

// ============================================================================
// Tests: std::array<f32, 4> → UNorm8 (blend weights typical case)
// ============================================================================

TEST_CASE("array_f32_4_unorm8", "[vertex_buffer]") {
    std::vector<std::array<f32, 4>> weights = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 0.0f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(weights, AttributeClass::BlendWeights, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 4);

    CHECK(vb.data[0] == 255);
    CHECK(vb.data[1] == 0);
    CHECK(vb.data[2] == 0);
    CHECK(vb.data[3] == 0);

    CHECK(static_cast<f32>(vb.data[4] / 255.0f) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[5] / 255.0f) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.01f));
}

// ============================================================================
// Tests: UNorm16 encoding
// ============================================================================

TEST_CASE("vector2f_unorm16", "[vertex_buffer]") {
    std::vector<Vector2f> uvs = {
        {0.0f, 1.0f},
        {0.5f, 0.25f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::UNorm16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 2);
    CHECK(vb.vertex_stride == 4); // 2 * sizeof(u16)

    u16 v0 = readAt<u16>(vb.data.data(), 0);
    u16 v1 = readAt<u16>(vb.data.data(), 2);
    CHECK(v0 == 0);
    CHECK(v1 == 65535);

    u16 v2 = readAt<u16>(vb.data.data(), 4);
    u16 v3 = readAt<u16>(vb.data.data(), 6);
    CHECK(static_cast<f32>(static_cast<f32>(v2) / 65535.0f) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.001f));
    CHECK(static_cast<f32>(static_cast<f32>(v3) / 65535.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.001f));
}

// ============================================================================
// Tests: Multi-attribute interleaving
// ============================================================================

TEST_CASE("multi_attribute_interleave", "[vertex_buffer]") {
    // 2 vertices: position (Vector3f→Float32) + normal (Vector3f→SNorm8) + UV (Vector2f→Float16)
    std::vector<Vector3f> positions = {
        {1.0f, 2.0f, 3.0f},
        {4.0f, 5.0f, 6.0f},
    };
    std::vector<Vector3f> normals = {
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    std::vector<Vector2f> uvs = {
        {0.0f, 1.0f},
        {0.5f, 0.5f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout.size() == 3);

    // Position: 3*4=12 bytes at offset 0
    CHECK(vb.layout[0].attr_class == AttributeClass::Position);
    CHECK(vb.layout[0].component_count == 3);
    CHECK(vb.layout[0].offset == 0);

    // Normal: 3*1=3 bytes at offset 12
    CHECK(vb.layout[1].attr_class == AttributeClass::Normal);
    CHECK(vb.layout[1].component_count == 3);
    CHECK(vb.layout[1].offset == 12);

    // UV: 2*2=4 bytes at offset 15
    CHECK(vb.layout[2].attr_class == AttributeClass::UV);
    CHECK(vb.layout[2].component_count == 2);
    CHECK(vb.layout[2].offset == 15);

    u32 expected_stride = 12 + 3 + 4; // 19
    CHECK(vb.vertex_stride == expected_stride);

    // Check vertex 0 position
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));

    // Check vertex 1 position
    size_t v1_off = vb.vertex_stride;
    CHECK(readAt<f32>(vb.data.data(), v1_off + 0) == Catch::Approx(4.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), v1_off + 4) == Catch::Approx(5.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), v1_off + 8) == Catch::Approx(6.0f).margin(1e-6f));

    // Check vertex 0 normal (snorm8): (0, 1, 0) → (0, 127, 0)
    CHECK(readAt<i8>(vb.data.data(), 12) == 0);
    CHECK(readAt<i8>(vb.data.data(), 13) == 127);
    CHECK(readAt<i8>(vb.data.data(), 14) == 0);

    // Check vertex 0 UV (float16): (0.0, 1.0)
    u16 uv0_u = readAt<u16>(vb.data.data(), 15);
    u16 uv0_v = readAt<u16>(vb.data.data(), 17);
    CHECK(static_cast<f32>(f16::from_raw(uv0_u).to_float()) == Catch::Approx(static_cast<f32>(0.0f)).margin(1e-3f));
    CHECK(static_cast<f32>(f16::from_raw(uv0_v).to_float()) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-3f));
}

// ============================================================================
// Tests: Realistic game vertex (Position + Normal + UV + BlendIdx + BlendWt)
// ============================================================================

TEST_CASE("realistic_game_vertex", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {
        {1.0f, 2.0f, 3.0f},
        {4.0f, 5.0f, 6.0f},
        {7.0f, 8.0f, 9.0f},
    };
    std::vector<Vector3f> normals = {
        {0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    std::vector<Vector2f> uvs = {
        {0.0f, 0.0f},
        {1.0f, 1.0f},
        {0.5f, 0.5f},
    };
    std::vector<std::array<u8, 4>> blend_indices = {
        {0, 1, 2, 3},
        {4, 5, 6, 7},
        {8, 9, 10, 11},
    };
    std::vector<std::array<f32, 4>> blend_weights = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 0.0f},
        {0.25f, 0.25f, 0.25f, 0.25f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float16);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float16);
    builder.declareAttribute(blend_indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
    builder.declareAttribute(blend_weights, AttributeClass::BlendWeights, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout.size() == 5);
    // Position: 3*2=6, Normal: 3*1=3, UV: 2*2=4, BlendIdx: 4*1=4, BlendWt: 4*1=4
    // Total stride = 6+3+4+4+4 = 21
    u32 expected_stride = 6 + 3 + 4 + 4 + 4;
    CHECK(vb.vertex_stride == expected_stride);
    CHECK(vb.data.size() == 3 * expected_stride);

    // Validate blend indices for vertex 1 (at offset stride*1 + offset of indices)
    size_t idx_offset = vb.layout[3].offset;
    size_t v1 = vb.vertex_stride * 1 + idx_offset;
    CHECK(vb.data[v1 + 0] == 4);
    CHECK(vb.data[v1 + 1] == 5);
    CHECK(vb.data[v1 + 2] == 6);
    CHECK(vb.data[v1 + 3] == 7);

    // Validate blend weights for vertex 2 (0.25 each → ~64)
    size_t wt_offset = vb.layout[4].offset;
    size_t v2 = vb.vertex_stride * 2 + wt_offset;
    CHECK(static_cast<f32>(vb.data[v2 + 0] / 255.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[v2 + 1] / 255.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[v2 + 2] / 255.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.01f));
    CHECK(static_cast<f32>(vb.data[v2 + 3] / 255.0f) == Catch::Approx(static_cast<f32>(0.25f)).margin(0.01f));
}

// ============================================================================
// Tests: Chaining API
// ============================================================================

TEST_CASE("chaining_api", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};

    VertexBuffer vb = VertexBufferBuilder()
                          .declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32)
                          .declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8)
                          .build();

    CHECK(vb.layout.size() == 2);
    CHECK(vb.data.size() > 0);
}

// ============================================================================
// Tests: f32 → UNorm8 edge cases
// ============================================================================

TEST_CASE("f32_unorm8_edge_cases", "[vertex_buffer]") {
    std::vector<f32> values = {0.0f, 1.0f, 0.5f};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.data[0] == 0);
    CHECK(vb.data[1] == 255);
    CHECK(static_cast<f32>(vb.data[2] / 255.0f) == Catch::Approx(static_cast<f32>(0.5f)).margin(0.01f));
}

// ============================================================================
// Tests: f32 → Int16
// ============================================================================

TEST_CASE("f32_int16", "[vertex_buffer]") {
    std::vector<f32> values = {100.0f, -200.0f, 0.0f};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertex_stride == 2);
    CHECK(readAt<i16>(vb.data.data(), 0) == 100);
    CHECK(readAt<i16>(vb.data.data(), 2) == -200);
    CHECK(readAt<i16>(vb.data.data(), 4) == 0);
}

// ============================================================================
// Tests: u8 → UNorm8 (passthrough raw bytes)
// ============================================================================

TEST_CASE("u8_unorm8", "[vertex_buffer]") {
    std::vector<u8> values = {0, 128, 255};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.data[0] == 0);
    CHECK(vb.data[1] == 128);
    CHECK(vb.data[2] == 255);
}

// ============================================================================
// Tests: std::array<u8,4> → UInt32 (promote + widen)
// ============================================================================

TEST_CASE("array_u8_4_uint32", "[vertex_buffer]") {
    std::vector<std::array<u8, 4>> data = {
        {1, 2, 3, 4},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::BlendIndices, AttributeEncoding::UInt32);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 4);
    CHECK(vb.vertex_stride == 16);
    CHECK(readAt<u32>(vb.data.data(), 0) == 1);
    CHECK(readAt<u32>(vb.data.data(), 4) == 2);
    CHECK(readAt<u32>(vb.data.data(), 8) == 3);
    CHECK(readAt<u32>(vb.data.data(), 12) == 4);
}

// ============================================================================
// Tests: i32 scalar → Int32
// ============================================================================

TEST_CASE("scalar_i32_int32", "[vertex_buffer]") {
    std::vector<i32> values = {-100, 0, 100};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::Int32);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertex_stride == 4);
    CHECK(readAt<i32>(vb.data.data(), 0) == -100);
    CHECK(readAt<i32>(vb.data.data(), 4) == 0);
    CHECK(readAt<i32>(vb.data.data(), 8) == 100);
}

// ============================================================================
// Tests: Vector2i → Int16
// ============================================================================

TEST_CASE("vector2i_int16", "[vertex_buffer]") {
    std::vector<Vector2i> data = {
        {-100, 200},
        {300, -400},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::UV, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].component_count == 2);
    CHECK(vb.vertex_stride == 4);
    CHECK(readAt<i16>(vb.data.data(), 0) == -100);
    CHECK(readAt<i16>(vb.data.data(), 2) == 200);
    CHECK(readAt<i16>(vb.data.data(), 4) == 300);
    CHECK(readAt<i16>(vb.data.data(), 6) == -400);
}

// ============================================================================
// Tests: Large vertex count stress test
// ============================================================================

TEST_CASE("large_vertex_count", "[vertex_buffer]") {
    const size_t N = 10000;
    std::vector<Vector3f> positions(N);
    std::vector<Vector3f> normals(N);
    std::vector<Vector2f> uvs(N);

    for (size_t i = 0; i < N; ++i) {
        f32 t = static_cast<f32>(i) / static_cast<f32>(N);
        positions[i] = {t, t * 2.0f, t * 3.0f};
        normals[i] = {0.0f, 1.0f, 0.0f};
        uvs[i] = {t, 1.0f - t};
    }

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float16);
    VertexBuffer vb = builder.build();

    // stride = 12 + 3 + 4 = 19
    CHECK(vb.vertex_stride == 19);
    CHECK(vb.data.size() == N * 19);

    // Spot check last vertex position
    size_t last_off = (N - 1) * vb.vertex_stride;
    f32 expected_t = static_cast<f32>(N - 1) / static_cast<f32>(N);
    CHECK(readAt<f32>(vb.data.data(), last_off + 0) == Catch::Approx(expected_t).margin(1e-5f));
    CHECK(readAt<f32>(vb.data.data(), last_off + 4) == Catch::Approx(expected_t * 2.0f).margin(1e-5f));
    CHECK(readAt<f32>(vb.data.data(), last_off + 8) == Catch::Approx(expected_t * 3.0f).margin(1e-5f));
}

// ============================================================================
// Tests: Int8 encoding from float
// ============================================================================

TEST_CASE("f32_int8", "[vertex_buffer]") {
    std::vector<f32> values = {-50.0f, 0.0f, 100.0f};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::Int8);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertex_stride == 1);
    CHECK(readAt<i8>(vb.data.data(), 0) == -50);
    CHECK(readAt<i8>(vb.data.data(), 1) == 0);
    CHECK(readAt<i8>(vb.data.data(), 2) == 100);
}

// ============================================================================
// Tests: UInt16 encoding from u32 source
// ============================================================================

TEST_CASE("u32_uint16", "[vertex_buffer]") {
    std::vector<u32> values = {0, 1000, 65535};

    VertexBufferBuilder builder;
    builder.declareAttribute(values, AttributeClass::Color, AttributeEncoding::UInt16);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertex_stride == 2);
    CHECK(readAt<u16>(vb.data.data(), 0) == 0);
    CHECK(readAt<u16>(vb.data.data(), 2) == 1000);
    CHECK(readAt<u16>(vb.data.data(), 4) == 65535);
}

// ============================================================================
// Tests: Alignment — user's exact example (positions align=16, normals align=4)
// ============================================================================

TEST_CASE("align_positions_16_normals_4", "[vertex_buffer]") {
    // Vector3f → Float32 = 3*4 = 12 bytes natural, align=16 → slot=16
    // Vector3f → SNorm8  = 3*1 = 3 bytes natural, align=4  → slot=4
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 16);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8, 4);
    VertexBuffer vb = builder.build();

    // Position at offset 0, next offset = round_up(12, 16) = 16
    CHECK(vb.layout[0].offset == 0);
    // Normal at offset 16, next offset = 16 + round_up(3, 4) = 20
    CHECK(vb.layout[1].offset == 16);
    CHECK(vb.vertex_stride == 20);

    // Verify position data at offset 0
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));

    // Verify normal data at offset 16
    CHECK(readAt<i8>(vb.data.data(), 16) == 0);
    CHECK(readAt<i8>(vb.data.data(), 17) == 127);
    CHECK(readAt<i8>(vb.data.data(), 18) == 0);
}

// ============================================================================
// Tests: Alignment — no alignment (default, align=0)
// ============================================================================

TEST_CASE("align_zero_is_natural", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    VertexBuffer vb = builder.build();

    // No alignment: position=12 bytes, normal=3 bytes → stride=15
    CHECK(vb.layout[0].offset == 0);
    CHECK(vb.layout[1].offset == 12);
    CHECK(vb.vertex_stride == 15);
}

// ============================================================================
// Tests: Alignment — full realistic vertex with alignment
// ============================================================================

TEST_CASE("align_realistic_game_vertex", "[vertex_buffer]") {
    // Position: Float16 × 3 = 6 bytes, align 8 → slot 8
    // Normal:   SNorm8 × 3 = 3 bytes, align 4 → slot 4
    // UV:       Float16 × 2 = 4 bytes, align 4 → slot 4
    // BlendIdx: UInt8 × 4 = 4 bytes, align 4 → slot 4
    // BlendWt:  UNorm8 × 4 = 4 bytes, align 4 → slot 4
    // Total stride = 8+4+4+4+4 = 24

    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
    std::vector<Vector2f> uvs = {{0.0f, 1.0f}, {0.5f, 0.5f}};
    std::vector<std::array<u8, 4>> indices = {{0, 1, 2, 3}, {4, 5, 6, 7}};
    std::vector<std::array<f32, 4>> weights = {{1.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 0.5f, 0.0f, 0.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float16, 8);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8, 4);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float16, 4);
    builder.declareAttribute(indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8, 4);
    builder.declareAttribute(weights, AttributeClass::BlendWeights, AttributeEncoding::UNorm8, 4);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].offset == 0);   // Position
    CHECK(vb.layout[1].offset == 8);   // Normal (after 8-byte aligned position)
    CHECK(vb.layout[2].offset == 12);  // UV (after 4-byte aligned normal)
    CHECK(vb.layout[3].offset == 16);  // BlendIdx (after 4-byte aligned UV)
    CHECK(vb.layout[4].offset == 20);  // BlendWt (after 4-byte aligned indices)
    CHECK(vb.vertex_stride == 24);     // 20 + round_up(4, 4) = 24
    CHECK(vb.data.size() == 2 * 24);   // 2 vertices × 24 bytes

    // Validate vertex 1 blend indices at correct offset
    size_t v1_idx = vb.vertex_stride * 1 + vb.layout[3].offset;
    CHECK(vb.data[v1_idx + 0] == 4);
    CHECK(vb.data[v1_idx + 1] == 5);
    CHECK(vb.data[v1_idx + 2] == 6);
    CHECK(vb.data[v1_idx + 3] == 7);
}

// ============================================================================
// Tests: Alignment — single attribute with alignment adds padding to stride
// ============================================================================

TEST_CASE("align_single_attribute_padded_stride", "[vertex_buffer]") {
    // Vector3f → Float32 = 12 bytes, align=16 → stride=16
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 16);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].offset == 0);
    CHECK(vb.vertex_stride == 16);
    CHECK(vb.data.size() == 16);

    // Data still at correct position
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
    // Bytes 12-15 should be zero (padding)
    CHECK(vb.data[12] == 0);
    CHECK(vb.data[13] == 0);
    CHECK(vb.data[14] == 0);
    CHECK(vb.data[15] == 0);
}

// ============================================================================
// Tests: Alignment — exact fit (no padding needed)
// ============================================================================

TEST_CASE("align_exact_fit_no_padding", "[vertex_buffer]") {
    // Vector4f → Float32 = 16 bytes, align=16 → slot=16, no padding
    std::vector<Vector4f> data = {{1.0f, 2.0f, 3.0f, 4.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(data, AttributeClass::Position, AttributeEncoding::Float32, 16);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertex_stride == 16);
}

// ============================================================================
// Tests: Alignment — align=1 is equivalent to no alignment
// ============================================================================

TEST_CASE("align_1_is_noop", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 1);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8, 1);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].offset == 0);
    CHECK(vb.layout[1].offset == 12);
    CHECK(vb.vertex_stride == 15);
}

// ============================================================================
// Tests: Alignment — mixed aligned and unaligned attributes
// ============================================================================

TEST_CASE("align_mixed_aligned_unaligned", "[vertex_buffer]") {
    // Position: Float32 × 3 = 12, align=16 → 16
    // Normal:   SNorm8 × 3 = 3, no align → 3
    // UV:       Float16 × 2 = 4, align=8 → 8
    // Expected: offset 0, 16, 19 → stride = 19 + 8 = 27
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};
    std::vector<Vector2f> uvs = {{0.0f, 1.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 16);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float16, 8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].offset == 0);   // Position
    CHECK(vb.layout[1].offset == 16);  // Normal (after 16-byte aligned position)
    CHECK(vb.layout[2].offset == 19);  // UV (after unaligned normal, 16+3=19)
    CHECK(vb.vertex_stride == 27);     // 19 + round_up(4, 8) = 27
}

// ============================================================================
// Tests: Alignment — multiple vertices, data at correct stride offsets
// ============================================================================

TEST_CASE("align_multi_vertex_data_check", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {
        {1.0f, 2.0f, 3.0f},
        {10.0f, 20.0f, 30.0f},
    };
    std::vector<Vector2f> uvs = {
        {0.25f, 0.75f},
        {0.5f, 0.5f},
    };

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 16);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    // Position: 12 bytes, align=16 → slot 16
    // UV: 8 bytes, no align → slot 8
    // Stride = 24
    CHECK(vb.vertex_stride == 24);

    // Vertex 0: position at 0, UV at 16
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 16) == Catch::Approx(0.25f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 20) == Catch::Approx(0.75f).margin(1e-6f));

    // Vertex 1: position at 24, UV at 40
    CHECK(readAt<f32>(vb.data.data(), 24) == Catch::Approx(10.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 28) == Catch::Approx(20.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 32) == Catch::Approx(30.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 40) == Catch::Approx(0.5f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 44) == Catch::Approx(0.5f).margin(1e-6f));
}

// ============================================================================
// Tests: Alignment — integer attribute with alignment
// ============================================================================

TEST_CASE("align_integer_attribute", "[vertex_buffer]") {
    // std::array<u8,4> → UInt8 = 4 bytes, but align=8
    std::vector<std::array<u8, 4>> indices = {{0, 1, 2, 3}};
    std::vector<std::array<u8, 4>> weights = {{128, 64, 32, 31}};

    VertexBufferBuilder builder;
    builder.declareAttribute(indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8, 8);
    builder.declareAttribute(weights, AttributeClass::BlendWeights, AttributeEncoding::UInt8);
    VertexBuffer vb = builder.build();

    CHECK(vb.layout[0].offset == 0);
    CHECK(vb.layout[1].offset == 8); // after 8-byte aligned slot
    CHECK(vb.vertex_stride == 12);   // 8 + 4

    // Verify data
    CHECK(vb.data[0] == 0);
    CHECK(vb.data[1] == 1);
    CHECK(vb.data[2] == 2);
    CHECK(vb.data[3] == 3);
    CHECK(vb.data[8] == 128);
    CHECK(vb.data[9] == 64);
    CHECK(vb.data[10] == 32);
    CHECK(vb.data[11] == 31);
}

// ============================================================================
// Tests: Parallel build — nullptr pool behaves like sequential
// ============================================================================

TEST_CASE("parallel_nullptr_pool_same_as_sequential", "[vertex_buffer]") {
    const size_t N = 100;
    std::vector<Vector3f> positions(N);
    for (size_t i = 0; i < N; ++i)
        positions[i] = Vector3f{f32(i), f32(i + 1), f32(i + 2)};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build(nullptr);

    CHECK(vb.vertex_stride == 12);
    CHECK(vb.data.size() == N * 12);
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), (N - 1) * 12) == Catch::Approx(f32(N - 1)).margin(1e-6f));
}

// ============================================================================
// Tests: Parallel build — small count stays sequential (no crash)
// ============================================================================

TEST_CASE("parallel_small_count_no_crash", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build(&pool);

    CHECK(vb.vertex_stride == 12);
    CHECK(vb.data.size() == 24);
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 12) == Catch::Approx(4.0f).margin(1e-6f));
}

// ============================================================================
// Tests: Parallel build — large vertex count, verify correctness
// ============================================================================

TEST_CASE("parallel_large_vertex_correctness", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    const size_t N = 10000; // well above the 4096 threshold
    std::vector<Vector3f> positions(N);
    std::vector<Vector3f> normals(N);
    std::vector<Vector2f> uvs(N);
    for (size_t i = 0; i < N; ++i) {
        positions[i] = Vector3f{f32(i), f32(i) * 0.5f, f32(i) * -1.0f};
        normals[i] = Vector3f{0.0f, 1.0f, 0.0f};
        uvs[i] = Vector2f{f32(i) / f32(N), 1.0f - f32(i) / f32(N)};
    }

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::Float32);
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float32);
    VertexBuffer vb_par = builder.build(&pool);

    // Build sequentially for comparison
    VertexBufferBuilder builder_seq;
    builder_seq.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder_seq.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::Float32);
    builder_seq.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float32);
    VertexBuffer vb_seq = builder_seq.build(nullptr);

    CHECK(vb_par.vertex_stride == vb_seq.vertex_stride);
    CHECK(vb_par.data.size() == vb_seq.data.size());
    CHECK(vb_par.layout.size() == vb_seq.layout.size());

    // Byte-for-byte identical
    bool identical = (vb_par.data == vb_seq.data);
    CHECK(identical);
}

// ============================================================================
// Tests: Parallel build — matches sequential with mixed encodings
// ============================================================================

TEST_CASE("parallel_mixed_encodings_match_sequential", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    const size_t N = 8192;
    std::vector<Vector3f> positions(N);
    std::vector<Vector3f> normals(N);
    std::vector<Vector2f> uvs(N);
    std::vector<std::array<u8, 4>> colors(N);
    for (size_t i = 0; i < N; ++i) {
        positions[i] = Vector3f{f32(i), f32(i) * 2.0f, f32(i) * 3.0f};
        normals[i] = Vector3f{0.577f, 0.577f, 0.577f};
        uvs[i] = Vector2f{f32(i % 256) / 255.0f, f32(i % 128) / 127.0f};
        colors[i] = {u8(i & 0xFF), u8((i >> 1) & 0xFF), u8((i >> 2) & 0xFF), 255};
    }

    auto makeBuf = [&](interfaces::WorkerPool* p) {
        VertexBufferBuilder b;
        b.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float16);
        b.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
        b.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::UNorm16);
        b.declareAttribute(colors, AttributeClass::Color, AttributeEncoding::UInt8);
        return b.build(p);
    };

    VertexBuffer vb_par = makeBuf(&pool);
    VertexBuffer vb_seq = makeBuf(nullptr);

    CHECK(vb_par.vertex_stride == vb_seq.vertex_stride);
    CHECK(vb_par.data.size() == vb_seq.data.size());
    CHECK(vb_par.data == vb_seq.data);
}

// ============================================================================
// Tests: Parallel build — with alignment
// ============================================================================

TEST_CASE("parallel_with_alignment", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    const size_t N = 5000;
    std::vector<Vector3f> positions(N);
    std::vector<Vector3f> normals(N);
    for (size_t i = 0; i < N; ++i) {
        positions[i] = Vector3f{f32(i), f32(i + 1), f32(i + 2)};
        normals[i] = Vector3f{0.0f, 0.0f, 1.0f};
    }

    auto makeBuf = [&](interfaces::WorkerPool* p) {
        VertexBufferBuilder b;
        b.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32, 16);
        b.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8, 4);
        return b.build(p);
    };

    VertexBuffer vb_par = makeBuf(&pool);
    VertexBuffer vb_seq = makeBuf(nullptr);

    CHECK(vb_par.vertex_stride == vb_seq.vertex_stride);
    CHECK(vb_par.data.size() == vb_seq.data.size());
    CHECK(vb_par.data == vb_seq.data);
}

// ============================================================================
// Tests: Parallel build — empty builder with pool
// ============================================================================

TEST_CASE("parallel_empty_builder", "[vertex_buffer]") {
    SimpleThreadPool pool(2);
    VertexBufferBuilder builder;
    VertexBuffer vb = builder.build(&pool);

    CHECK(vb.data.empty());
    CHECK(vb.layout.empty());
    CHECK(vb.vertex_stride == 0);
}

// ============================================================================
// Tests: Parallel build — single vertex with pool (below threshold)
// ============================================================================

TEST_CASE("parallel_single_vertex", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build(&pool);

    CHECK(vb.data.size() == 12);
    CHECK(readAt<f32>(vb.data.data(), 0) == Catch::Approx(1.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 4) == Catch::Approx(2.0f).margin(1e-6f));
    CHECK(readAt<f32>(vb.data.data(), 8) == Catch::Approx(3.0f).margin(1e-6f));
}

// ============================================================================
// Tests: Parallel build — integer attributes
// ============================================================================

TEST_CASE("parallel_integer_attributes", "[vertex_buffer]") {
    SimpleThreadPool pool(4);
    const size_t N = 6000;
    std::vector<std::array<u8, 4>> blend_indices(N);
    std::vector<std::array<u8, 4>> blend_weights(N);
    for (size_t i = 0; i < N; ++i) {
        blend_indices[i] = {u8(i % 80), u8((i + 1) % 80), u8((i + 2) % 80), 0};
        blend_weights[i] = {200, u8(55 - (i % 55)), 0, 0};
    }

    auto makeBuf = [&](interfaces::WorkerPool* p) {
        VertexBufferBuilder b;
        b.declareAttribute(blend_indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
        b.declareAttribute(blend_weights, AttributeClass::BlendWeights, AttributeEncoding::UInt8);
        return b.build(p);
    };

    VertexBuffer vb_par = makeBuf(&pool);
    VertexBuffer vb_seq = makeBuf(nullptr);

    CHECK(vb_par.data.size() == vb_seq.data.size());
    CHECK(vb_par.data == vb_seq.data);
}

// ============================================================================
// Tests: VertexBuffer read-back — vertexCount / vertexSize
// ============================================================================

TEST_CASE("readback_vertex_count_and_size", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertexCount() == 3);
    CHECK(vb.vertexSize() == 12);
}

TEST_CASE("readback_empty_buffer", "[vertex_buffer]") {
    VertexBuffer vb;
    CHECK(vb.vertexCount() == 0);
    CHECK(vb.vertexSize() == 0);
    CHECK(vb.UVsNum() == 0);
    CHECK(vb.hasVertexColors() == false);
    CHECK(vb.getPositions().empty());
    CHECK(vb.getNormals().empty());
    CHECK(vb.getTangents().empty());
    CHECK(vb.getUVs(0).empty());
    CHECK(vb.getColors().empty());
    CHECK(vb.getBoneIndices().empty());
    CHECK(vb.getBoneWeights().empty());
}

// ============================================================================
// Tests: VertexBuffer read-back — UVsNum / hasVertexColors
// ============================================================================

TEST_CASE("readback_uvs_num_and_colors", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1, 2, 3}};
    std::vector<Vector2f> uv0 = {{0.5f, 0.5f}};
    std::vector<Vector2f> uv1 = {{0.1f, 0.9f}};
    std::vector<std::array<u8, 4>> colors = {{255, 128, 64, 255}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(uv0, AttributeClass::UV, AttributeEncoding::Float32);
    builder.declareAttribute(uv1, AttributeClass::UV, AttributeEncoding::Float32);
    builder.declareAttribute(colors, AttributeClass::Color, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.UVsNum() == 2);
    CHECK(vb.hasVertexColors() == true);
}

TEST_CASE("readback_no_colors", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1, 2, 3}};
    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.hasVertexColors() == false);
    CHECK(vb.UVsNum() == 0);
}

// ============================================================================
// Tests: VertexBuffer read-back — getPositions
// ============================================================================

TEST_CASE("readback_positions_float32", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    auto out = vb.getPositions();
    CHECK(out.size() == 2);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(2.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(3.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[1].data[0]) == Catch::Approx(static_cast<f32>(4.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[1].data[1]) == Catch::Approx(static_cast<f32>(5.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[1].data[2]) == Catch::Approx(static_cast<f32>(6.0f)).margin(1e-6f));
}

TEST_CASE("readback_positions_float16", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float16);
    VertexBuffer vb = builder.build();

    auto out = vb.getPositions();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(2.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(3.0f)).margin(0.01f));
}

// ============================================================================
// Tests: VertexBuffer read-back — getNormals
// ============================================================================

TEST_CASE("readback_normals_snorm8", "[vertex_buffer]") {
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    VertexBuffer vb = builder.build();

    auto out = vb.getNormals();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
}

TEST_CASE("readback_normals_float32", "[vertex_buffer]") {
    std::vector<Vector3f> normals = {{0.577f, 0.577f, 0.577f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    auto out = vb.getNormals();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(0.577f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(0.577f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(0.577f)).margin(1e-6f));
}

// ============================================================================
// Tests: VertexBuffer read-back — getTangents
// ============================================================================

TEST_CASE("readback_tangents_snorm8", "[vertex_buffer]") {
    std::vector<Vector4f> tangents = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(tangents, AttributeClass::Tangent, AttributeEncoding::SNorm8);
    VertexBuffer vb = builder.build();

    auto out = vb.getTangents();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.01f));
    CHECK(static_cast<f32>(out[0].data[3]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
}

// ============================================================================
// Tests: VertexBuffer read-back — getUVs
// ============================================================================

TEST_CASE("readback_uvs_int16_default_scale", "[vertex_buffer]") {
    // Store UVs as Int16 raw values: 2048 should decode to 1.0 with /2048 scale
    std::vector<Vector2<i16>> uvs_raw = {{{2048, 1024}}};
    VertexBufferBuilder builder;
    builder.declareAttribute(uvs_raw, AttributeClass::UV, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    auto out = vb.getUVs(0);
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(0.5f)).margin(1e-6f));
}

TEST_CASE("readback_uvs_custom_scale_offset", "[vertex_buffer]") {
    std::vector<Vector2<i16>> uvs_raw = {{{100, 200}}};
    VertexBufferBuilder builder;
    builder.declareAttribute(uvs_raw, AttributeClass::UV, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    // float_uv = i16_uv * 16.0 + 5.0
    auto out = vb.getUVs(0, 16.0f, 5.0f);
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(100.0f * 16.0f + 5.0f)).margin(1e-3f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(200.0f * 16.0f + 5.0f)).margin(1e-3f));
}

TEST_CASE("readback_uvs_multiple_layers", "[vertex_buffer]") {
    std::vector<Vector2<i16>> uv0 = {{{2048, 0}}};
    std::vector<Vector2<i16>> uv1 = {{{0, 2048}}};
    VertexBufferBuilder builder;
    builder.declareAttribute(uv0, AttributeClass::UV, AttributeEncoding::Int16);
    builder.declareAttribute(uv1, AttributeClass::UV, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    auto out0 = vb.getUVs(0);
    auto out1 = vb.getUVs(1);
    CHECK(out0.size() == 1);
    CHECK(out1.size() == 1);
    CHECK(static_cast<f32>(out0[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out0[0].data[1]) == Catch::Approx(static_cast<f32>(0.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out1[0].data[0]) == Catch::Approx(static_cast<f32>(0.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(out1[0].data[1]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f));
}

TEST_CASE("readback_uvs_out_of_range_layer", "[vertex_buffer]") {
    std::vector<Vector2<i16>> uv0 = {{{2048, 0}}};
    VertexBufferBuilder builder;
    builder.declareAttribute(uv0, AttributeClass::UV, AttributeEncoding::Int16);
    VertexBuffer vb = builder.build();

    auto out = vb.getUVs(5); // no 5th layer
    CHECK(out.empty());
}

TEST_CASE("readback_uvs_float32_with_scale", "[vertex_buffer]") {
    // Float32 UVs: decodeRaw returns the float as-is, then scale/offset applied
    std::vector<Vector2f> uvs = {{0.5f, 0.25f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(uvs, AttributeClass::UV, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    auto out = vb.getUVs(0, 1.0f, 0.0f);
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(0.5f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(0.25f)).margin(1e-6f));
}

// ============================================================================
// Tests: VertexBuffer read-back — getColors
// ============================================================================

TEST_CASE("readback_colors_unorm8", "[vertex_buffer]") {
    std::vector<std::array<u8, 4>> colors = {{255, 128, 0, 255}};
    VertexBufferBuilder builder;
    builder.declareAttribute(colors, AttributeClass::Color, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    auto out = vb.getColors();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(128.0f / 255.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0].data[3]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
}

TEST_CASE("readback_colors_default_alpha", "[vertex_buffer]") {
    // 3-component color: alpha should default to 1.0
    std::vector<Vector3f> colors = {{0.5f, 0.6f, 0.7f}};
    VertexBufferBuilder builder;
    builder.declareAttribute(colors, AttributeClass::Color, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    auto out = vb.getColors();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0].data[0]) == Catch::Approx(static_cast<f32>(0.5f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[1]) == Catch::Approx(static_cast<f32>(0.6f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[2]) == Catch::Approx(static_cast<f32>(0.7f)).margin(1e-6f));
    CHECK(static_cast<f32>(out[0].data[3]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f)); // default opaque
}

// ============================================================================
// Tests: VertexBuffer read-back — getBoneIndices / getBoneWeights
// ============================================================================

TEST_CASE("readback_bone_indices_uint8", "[vertex_buffer]") {
    std::vector<std::array<u8, 4>> indices = {{0, 5, 10, 20}};
    VertexBufferBuilder builder;
    builder.declareAttribute(indices, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
    VertexBuffer vb = builder.build();

    auto out = vb.getBoneIndices();
    CHECK(out.size() == 1);
    CHECK(out[0][0] == 0);
    CHECK(out[0][1] == 5);
    CHECK(out[0][2] == 10);
    CHECK(out[0][3] == 20);
}

TEST_CASE("readback_bone_weights_unorm8", "[vertex_buffer]") {
    std::vector<std::array<u8, 4>> weights = {{255, 128, 64, 0}};
    VertexBufferBuilder builder;
    builder.declareAttribute(weights, AttributeClass::BlendWeights, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    auto out = vb.getBoneWeights();
    CHECK(out.size() == 1);
    CHECK(static_cast<f32>(out[0][0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0][1]) == Catch::Approx(static_cast<f32>(128.0f / 255.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0][2]) == Catch::Approx(static_cast<f32>(64.0f / 255.0f)).margin(0.004f));
    CHECK(static_cast<f32>(out[0][3]) == Catch::Approx(static_cast<f32>(0.0f)).margin(0.004f));
}

// ============================================================================
// Tests: VertexBuffer read-back — missing attributes return empty
// ============================================================================

TEST_CASE("readback_missing_attributes", "[vertex_buffer]") {
    std::vector<Vector3f> positions = {{1, 2, 3}};
    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    VertexBuffer vb = builder.build();

    CHECK(vb.getNormals().empty());
    CHECK(vb.getTangents().empty());
    CHECK(vb.getUVs(0).empty());
    CHECK(vb.getColors().empty());
    CHECK(vb.getBoneIndices().empty());
    CHECK(vb.getBoneWeights().empty());
}

// ============================================================================
// Tests: VertexBuffer read-back — full game vertex round-trip
// ============================================================================

TEST_CASE("readback_full_game_vertex", "[vertex_buffer]") {
    const size_t N = 3;
    std::vector<Vector3f> positions = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}, {7.0f, 8.0f, 9.0f}};
    std::vector<Vector3f> normals = {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    std::vector<Vector4f> tangents = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
    std::vector<std::array<u8, 4>> boneIdx = {{0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11}};
    std::vector<std::array<u8, 4>> boneWt = {{255, 0, 0, 0}, {128, 127, 0, 0}, {85, 85, 85, 0}};
    std::vector<std::array<u8, 4>> colors = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};

    VertexBufferBuilder builder;
    builder.declareAttribute(positions, AttributeClass::Position, AttributeEncoding::Float32);
    builder.declareAttribute(normals, AttributeClass::Normal, AttributeEncoding::SNorm8);
    builder.declareAttribute(tangents, AttributeClass::Tangent, AttributeEncoding::SNorm8);
    builder.declareAttribute(boneIdx, AttributeClass::BlendIndices, AttributeEncoding::UInt8);
    builder.declareAttribute(boneWt, AttributeClass::BlendWeights, AttributeEncoding::UNorm8);
    builder.declareAttribute(colors, AttributeClass::Color, AttributeEncoding::UNorm8);
    VertexBuffer vb = builder.build();

    CHECK(vb.vertexCount() == N);
    CHECK(vb.hasVertexColors());

    auto pos = vb.getPositions();
    auto nrm = vb.getNormals();
    auto tan = vb.getTangents();
    auto bi  = vb.getBoneIndices();
    auto bw  = vb.getBoneWeights();
    auto col = vb.getColors();

    CHECK(pos.size() == N);
    CHECK(nrm.size() == N);
    CHECK(tan.size() == N);
    CHECK(bi.size() == N);
    CHECK(bw.size() == N);
    CHECK(col.size() == N);

    // Spot-check vertex 0
    CHECK(static_cast<f32>(pos[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(1e-6f));
    CHECK(static_cast<f32>(nrm[0].data[1]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(tan[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(static_cast<f32>(tan[0].data[3]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.01f));
    CHECK(bi[0][0] == 0);
    CHECK(bi[0][3] == 3);
    CHECK(static_cast<f32>(bw[0][0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
    CHECK(static_cast<f32>(col[0].data[0]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
    CHECK(static_cast<f32>(col[0].data[3]) == Catch::Approx(static_cast<f32>(1.0f)).margin(0.004f));
}
