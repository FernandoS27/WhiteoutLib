// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include "common_types.h"
#include "compatibility.h"

#include <memory>
#include <vector>

namespace whiteout {

class VirtualFileSystem {
public:
    virtual ~VirtualFileSystem() = default;

    /// Read the entire contents of a file into a byte vector.
    virtual std::vector<u8> readFile(const std::string& path) const = 0;

    virtual std::vector<u8> readFile(u32 fileId) const = 0;

    /// Check if a file exists at the given path.
    virtual bool fileExists(const std::string& path) const = 0;

    virtual bool fileExists(u32 fileId) const = 0;
};

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

enum class AttributeType : u32 {
    Float,
    Unorm,
    Snorm,
    Half,
    U32,
    U8,
};

struct VertexBuffer {
    struct Attribute {
        AttributeClass attr_class;
        AttributeType attr_type;
        size_t component_count; // 1-4
        size_t offset;
    };
    std::vector<u8> data;          // Gpu-friendly interleaved vertex data
    std::vector<Attribute> layout; // per-vertex byte offset for each attribute class
    u32 vertex_stride = 0;         // total bytes per vertex
};

class VertexBufferBuilder {
public:
    VertexBufferBuilder();
    ~VertexBufferBuilder();

    VertexBufferBuilder(VertexBufferBuilder&&) noexcept;
    VertexBufferBuilder& operator=(VertexBufferBuilder&&) noexcept;

    VertexBufferBuilder(const VertexBufferBuilder&) = delete;
    VertexBufferBuilder& operator=(const VertexBufferBuilder&) = delete;

    /// Declare a vertex attribute with source float data.
    /// @param src_data    Source float values (vertex_count * components elements)
    /// @param attr_class  The semantic class of this attribute
    /// @param attr_type   Target storage type for encoding
    /// @param components  Number of components per vertex (1-4)
    VertexBufferBuilder& declareAttribute(std::span<const f32> src_data, AttributeClass attr_class,
                                          AttributeType attr_type, size_t components);

    /// Declare a vertex attribute with source u32 data (e.g. blend indices).
    /// @param src_data    Source u32 values (vertex_count * components elements)
    /// @param attr_class  The semantic class of this attribute
    /// @param attr_type   Target storage type for encoding
    /// @param components  Number of components per vertex (1-4)
    VertexBufferBuilder& declareAttribute(std::span<const u32> src_data, AttributeClass attr_class,
                                          AttributeType attr_type, size_t components);

    /// Build the interleaved vertex buffer from all declared attributes.
    VertexBuffer build() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout
