// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <cstring>
#include <stdexcept>
#include <whiteout/m3/types.h>

namespace whiteout {
namespace m3 {

// ============================================================================
// VertexBuffer Implementation
// ============================================================================

struct VertexBuffer::Impl {
    size_t stride = 0;
    size_t count = 0;
    size_t numUVs = 0;
    bool hasColor = false;
    size_t colorOffset = 0;
    size_t uvOffset = 0;
};

VertexBuffer::VertexBuffer() = default;

VertexBuffer::~VertexBuffer() = default;

VertexBuffer::VertexBuffer(const VertexBuffer& other) : flags(other.flags), data(other.data) {
    if (other.impl) {
        impl = std::make_unique<Impl>(*other.impl);
    }
}

VertexBuffer& VertexBuffer::operator=(const VertexBuffer& other) {
    if (this != &other) {
        flags = other.flags;
        data = other.data;
        if (other.impl) {
            impl = std::make_unique<Impl>(*other.impl);
        } else {
            impl.reset();
        }
    }
    return *this;
}

VertexBuffer::VertexBuffer(VertexBuffer&& other) noexcept
    : flags(std::move(other.flags)), data(std::move(other.data)), impl(std::move(other.impl)) {}

VertexBuffer& VertexBuffer::operator=(VertexBuffer&& other) noexcept {
    if (this != &other) {
        flags = std::move(other.flags);
        data = std::move(other.data);
        impl = std::move(other.impl);
    }
    return *this;
}

void VertexBuffer::initialize() {
    impl = std::make_unique<Impl>();

    // Parse vertex format flags
    impl->hasColor = hasFlag(flags, VertexFormatFlag::VertexColor);

    // Count UV layers
    impl->numUVs = 0;
    if (hasFlag(flags, VertexFormatFlag::UV1))
        impl->numUVs++;
    if (hasFlag(flags, VertexFormatFlag::UV2))
        impl->numUVs++;
    if (hasFlag(flags, VertexFormatFlag::UV3))
        impl->numUVs++;
    if (hasFlag(flags, VertexFormatFlag::UV4))
        impl->numUVs++;
    if (hasFlag(flags, VertexFormatFlag::UV5))
        impl->numUVs++;

    // Calculate offsets and stride
    // Base structure:
    // - Vector3f position (12 bytes) at offset 0
    // - u8 boneWeights[4] (4 bytes) at offset 12
    // - u8 boneIndices[4] (4 bytes) at offset 16
    // - i8 normal[3] (3 bytes) at offset 20
    // - u8 pad0 (1 byte) at offset 23
    // Total: 24 bytes

    impl->colorOffset = 24;
    impl->uvOffset = impl->hasColor ? 28 : 24;

    // Stride = 24 + (hasColor ? 4 : 0) + (numUVs * 4) + 4 (trailing padding)
    impl->stride = 24 + (impl->hasColor ? 4 : 0) + (impl->numUVs * 4) + 4;

    // Calculate vertex count
    if (impl->stride > 0) {
        impl->count = data.size() / impl->stride;
    } else {
        impl->count = 0;
    }
}

size_t VertexBuffer::vertexCount() const {
    if (!impl) {
        return 0;
    }
    return impl->count;
}

size_t VertexBuffer::vertexSize() const {
    if (!impl) {
        return 0;
    }
    return impl->stride;
}

size_t VertexBuffer::UVsNum() const {
    if (!impl) {
        return 0;
    }
    return impl->numUVs;
}

bool VertexBuffer::hasVertexColors() const {
    if (!impl) {
        return false;
    }
    return impl->hasColor;
}

std::vector<Vector3f> VertexBuffer::getPositions() const {
    std::vector<Vector3f> positions;
    if (!impl || impl->count == 0) {
        return positions;
    }

    positions.reserve(impl->count);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride;
        Vector3f pos;
        std::memcpy(&pos, &data[offset], sizeof(Vector3f));
        positions.push_back(pos);
    }

    return positions;
}

std::vector<Vector3f> VertexBuffer::getNormals() const {
    std::vector<Vector3f> normals;
    if (!impl || impl->count == 0) {
        return normals;
    }

    normals.reserve(impl->count);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride + 20; // Normal at offset 20

        // Read as signed bytes
        i8 nx = static_cast<i8>(data[offset + 0]);
        i8 ny = static_cast<i8>(data[offset + 1]);
        i8 nz = static_cast<i8>(data[offset + 2]);

        // Normalize by dividing by 127.0
        Vector3f normal;
        normal.x = static_cast<f32>(nx) / 127.0f;
        normal.y = static_cast<f32>(ny) / 127.0f;
        normal.z = static_cast<f32>(nz) / 127.0f;

        normals.push_back(normal);
    }

    return normals;
}

std::vector<Vector4f> VertexBuffer::getTangents() const {
    // Tangents are not stored in M3 vertex buffers - they are computed
    // Return empty vector
    return std::vector<Vector4f>();
}

std::vector<Vector2f> VertexBuffer::getUVs(size_t which) const {
    std::vector<Vector2f> uvs;
    if (!impl || impl->count == 0 || which >= impl->numUVs) {
        return uvs;
    }

    uvs.reserve(impl->count);

    // UV data starts at uvOffset, each UV layer is 4 bytes (2 x i16)
    size_t uvLayerOffset = impl->uvOffset + (which * 4);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride + uvLayerOffset;

        // Read as signed 16-bit integers
        i16 u, v;
        std::memcpy(&u, &data[offset + 0], sizeof(i16));
        std::memcpy(&v, &data[offset + 2], sizeof(i16));

        // Normalize by dividing by 2048.0 (as per spec)
        Vector2f uv;
        uv.x = static_cast<f32>(u) / 2048.0f;
        uv.y = static_cast<f32>(v) / 2048.0f;

        uvs.push_back(uv);
    }

    return uvs;
}

std::vector<ColorBGRA> VertexBuffer::getColors() const {
    std::vector<ColorBGRA> colors;
    if (!impl || impl->count == 0 || !impl->hasColor) {
        return colors;
    }

    colors.reserve(impl->count);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride + impl->colorOffset;
        ColorBGRA color;
        std::memcpy(&color, &data[offset], sizeof(ColorBGRA));
        colors.push_back(color);
    }

    return colors;
}

std::vector<std::array<u8, 4>> VertexBuffer::getBoneIndices() const {
    std::vector<std::array<u8, 4>> indices;
    if (!impl || impl->count == 0) {
        return indices;
    }

    indices.reserve(impl->count);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride + 16; // Bone indices at offset 16
        std::array<u8, 4> boneIdx;
        std::memcpy(boneIdx.data(), &data[offset], 4);
        indices.push_back(boneIdx);
    }

    return indices;
}

std::vector<std::array<u8, 4>> VertexBuffer::getBoneWeights() const {
    std::vector<std::array<u8, 4>> weights;
    if (!impl || impl->count == 0) {
        return weights;
    }

    weights.reserve(impl->count);

    for (size_t i = 0; i < impl->count; ++i) {
        size_t offset = i * impl->stride + 12; // Bone weights at offset 12
        std::array<u8, 4> boneWeight;
        std::memcpy(boneWeight.data(), &data[offset], 4);
        weights.push_back(boneWeight);
    }

    return weights;
}

} // namespace m3
} // namespace whiteout
