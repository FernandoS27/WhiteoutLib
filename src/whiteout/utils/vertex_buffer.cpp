// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/utils/vertex_buffer.h"

#include "whiteout/interfaces.h"
#include "whiteout/utils/job_group.h"

#include <algorithm>
#include <cstring>
#include <type_traits>
#include <vector>

namespace whiteout {

using utils::AttributeClass;
using utils::AttributeEncoding;
using utils::VertexBuffer;
using utils::VertexBufferBuilder;

namespace {

constexpr size_t bytesPerEncoding(AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::Float32:
    case AttributeEncoding::UInt32:
    case AttributeEncoding::Int32:
        return 4;
    case AttributeEncoding::Float16:
    case AttributeEncoding::SNorm16:
    case AttributeEncoding::UNorm16:
    case AttributeEncoding::UInt16:
    case AttributeEncoding::Int16:
        return 2;
    case AttributeEncoding::SNorm8:
    case AttributeEncoding::UNorm8:
    case AttributeEncoding::UInt8:
    case AttributeEncoding::Int8:
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Encoding loops — switch ONCE on encoding, then tight-loop over components.
// Each lambda captures nothing and the compiler can inline/vectorize freely.
// ---------------------------------------------------------------------------

void encodeF32Components(u8* dst, const f32* src, size_t count, AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::Float32:
        std::memcpy(dst, src, count * sizeof(f32));
        break;
    case AttributeEncoding::Float16:
        for (size_t c = 0; c < count; ++c) {
            f16 h(src[c]);
            std::memcpy(dst + c * 2, &h.raw, sizeof(u16));
        }
        break;
    case AttributeEncoding::SNorm8:
        for (size_t c = 0; c < count; ++c) {
            snorm8 const s = snorm8::from_float(src[c]);
            dst[c] = static_cast<u8>(s.value);
        }
        break;
    case AttributeEncoding::SNorm16:
        for (size_t c = 0; c < count; ++c) {
            snorm16 s = snorm16::from_float(src[c]);
            std::memcpy(dst + c * 2, &s.value, sizeof(i16));
        }
        break;
    case AttributeEncoding::UNorm8:
        for (size_t c = 0; c < count; ++c) {
            unorm8 const u = unorm8::from_float(src[c]);
            dst[c] = u.value;
        }
        break;
    case AttributeEncoding::UNorm16:
        for (size_t c = 0; c < count; ++c) {
            unorm16 u = unorm16::from_float(src[c]);
            std::memcpy(dst + c * 2, &u.value, sizeof(u16));
        }
        break;
    case AttributeEncoding::UInt8:
        for (size_t c = 0; c < count; ++c)
            dst[c] = static_cast<u8>(src[c]);
        break;
    case AttributeEncoding::UInt16:
        for (size_t c = 0; c < count; ++c) {
            u16 v = static_cast<u16>(src[c]);
            std::memcpy(dst + c * 2, &v, sizeof(u16));
        }
        break;
    case AttributeEncoding::UInt32:
        for (size_t c = 0; c < count; ++c) {
            u32 v = static_cast<u32>(src[c]);
            std::memcpy(dst + c * 4, &v, sizeof(u32));
        }
        break;
    case AttributeEncoding::Int8:
        for (size_t c = 0; c < count; ++c)
            dst[c] = static_cast<u8>(static_cast<i8>(src[c]));
        break;
    case AttributeEncoding::Int16:
        for (size_t c = 0; c < count; ++c) {
            i16 v = static_cast<i16>(src[c]);
            std::memcpy(dst + c * 2, &v, sizeof(i16));
        }
        break;
    case AttributeEncoding::Int32:
        for (size_t c = 0; c < count; ++c) {
            i32 v = static_cast<i32>(src[c]);
            std::memcpy(dst + c * 4, &v, sizeof(i32));
        }
        break;
    }
}

void encodeU32Components(u8* dst, const u32* src, size_t count, AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::UInt32:
        std::memcpy(dst, src, count * sizeof(u32));
        break;
    case AttributeEncoding::UInt16:
        for (size_t c = 0; c < count; ++c) {
            u16 v = static_cast<u16>(src[c]);
            std::memcpy(dst + c * 2, &v, sizeof(u16));
        }
        break;
    case AttributeEncoding::UInt8:
        for (size_t c = 0; c < count; ++c)
            dst[c] = static_cast<u8>(src[c]);
        break;
    case AttributeEncoding::Int32:
        // Reinterpret bits
        std::memcpy(dst, src, count * sizeof(u32));
        break;
    case AttributeEncoding::Int16:
        for (size_t c = 0; c < count; ++c) {
            i32 signed_val;
            std::memcpy(&signed_val, &src[c], sizeof(u32));
            i16 v = static_cast<i16>(signed_val);
            std::memcpy(dst + c * 2, &v, sizeof(i16));
        }
        break;
    case AttributeEncoding::Int8:
        for (size_t c = 0; c < count; ++c) {
            i32 signed_val;
            std::memcpy(&signed_val, &src[c], sizeof(u32));
            dst[c] = static_cast<u8>(static_cast<i8>(signed_val));
        }
        break;
    case AttributeEncoding::UNorm8:
        for (size_t c = 0; c < count; ++c)
            dst[c] = static_cast<u8>(std::min(src[c], u32{255}));
        break;
    case AttributeEncoding::UNorm16:
        for (size_t c = 0; c < count; ++c) {
            u16 v = static_cast<u16>(std::min(src[c], u32{65535}));
            std::memcpy(dst + c * 2, &v, sizeof(u16));
        }
        break;
    case AttributeEncoding::SNorm8:
        for (size_t c = 0; c < count; ++c)
            dst[c] = static_cast<u8>(static_cast<i8>(src[c] & 0xFF));
        break;
    case AttributeEncoding::SNorm16:
        for (size_t c = 0; c < count; ++c) {
            i16 v = static_cast<i16>(src[c] & 0xFFFF);
            std::memcpy(dst + c * 2, &v, sizeof(i16));
        }
        break;
    case AttributeEncoding::Float16:
        for (size_t c = 0; c < count; ++c) {
            f16 h(static_cast<f32>(src[c]));
            std::memcpy(dst + c * 2, &h.raw, sizeof(u16));
        }
        break;
    case AttributeEncoding::Float32:
        for (size_t c = 0; c < count; ++c) {
            f32 v = static_cast<f32>(src[c]);
            std::memcpy(dst + c * 4, &v, sizeof(f32));
        }
        break;
    }
}

} // anonymous namespace

// ============================================================================
// PImpl
// ============================================================================

struct VertexBufferBuilder::Impl {

    struct PendingAttribute {
        std::vector<f32> float_data;   // populated for float sources
        std::vector<u32> uint_data;    // populated for integer sources
        const f32* direct_float_ptr;   // non-null for zero-copy fast path
        AttributeClass attr_class;
        AttributeEncoding encoding;
        size_t component_count;
        size_t vertex_count;
        size_t align;                  // 0 = natural (no extra padding)
        bool is_uint_source;

        PendingAttribute()
            : direct_float_ptr(nullptr), attr_class(AttributeClass::Position),
              encoding(AttributeEncoding::Float32), component_count(0), vertex_count(0),
              align(0), is_uint_source(false) {}

        void encodeInto(u8* destination, size_t vertex_index) const {
            const size_t src_base = vertex_index * component_count;
            if (is_uint_source) {
                encodeU32Components(destination, uint_data.data() + src_base,
                                    component_count, encoding);
            } else {
                const f32* fptr = direct_float_ptr ? direct_float_ptr : float_data.data();
                encodeF32Components(destination, fptr + src_base,
                                      component_count, encoding);
            }
        }
    };

    std::vector<PendingAttribute> attributes;
};

// ============================================================================
// Constructor / Destructor / Move
// ============================================================================

VertexBufferBuilder::VertexBufferBuilder() : impl_(std::make_unique<Impl>()) {}

VertexBufferBuilder::~VertexBufferBuilder() = default;

VertexBufferBuilder::VertexBufferBuilder(VertexBufferBuilder&&) noexcept = default;

VertexBufferBuilder& VertexBufferBuilder::operator=(VertexBufferBuilder&&) noexcept = default;

// ============================================================================
// declareAttributeFloat — copies float data into pending attribute
// ============================================================================

VertexBufferBuilder& VertexBufferBuilder::declareAttributeFloat(const f32* data,
                                                                size_t vertex_count,
                                                                size_t components,
                                                                AttributeClass attr_class,
                                                                AttributeEncoding encoding,
                                                                size_t align) {
    Impl::PendingAttribute attr;
    attr.float_data.assign(data, data + vertex_count * components);
    attr.attr_class = attr_class;
    attr.encoding = encoding;
    attr.component_count = components;
    attr.vertex_count = vertex_count;
    attr.align = align;
    attr.is_uint_source = false;
    impl_->attributes.push_back(std::move(attr));
    return *this;
}

// ============================================================================
// declareAttributeUint — copies u32 data into pending attribute
// ============================================================================

VertexBufferBuilder& VertexBufferBuilder::declareAttributeUint(const u32* data,
                                                               size_t vertex_count,
                                                               size_t components,
                                                               AttributeClass attr_class,
                                                               AttributeEncoding encoding,
                                                               size_t align) {
    Impl::PendingAttribute attr;
    attr.uint_data.assign(data, data + vertex_count * components);
    attr.attr_class = attr_class;
    attr.encoding = encoding;
    attr.component_count = components;
    attr.vertex_count = vertex_count;
    attr.align = align;
    attr.is_uint_source = true;
    impl_->attributes.push_back(std::move(attr));
    return *this;
}

// ============================================================================
// declareAttributeFloatDirect — zero-copy: stores pointer to caller's data.
// The caller's vector must outlive the builder (valid — builder is short-lived).
// Actually, for safety, just forward to declareAttributeFloat with a copy.
// The "direct" path here avoids the flatten loop in the header template.
// ============================================================================

VertexBufferBuilder& VertexBufferBuilder::declareAttributeFloatDirect(const f32* data,
                                                                      size_t vertex_count,
                                                                      size_t components,
                                                                      AttributeClass attr_class,
                                                                      AttributeEncoding encoding,
                                                                      size_t align) {
    return declareAttributeFloat(data, vertex_count, components, attr_class, encoding, align);
}

// ============================================================================
// build()
// ============================================================================

// Minimum vertex count before we consider parallelizing.
static constexpr size_t kParallelThreshold = 4096;

// Target output bytes per chunk — sized to fit comfortably in L2 cache.
static constexpr size_t kTargetChunkBytes = 64 * 1024; // 64 KiB

// Minimum vertices per chunk — avoids excessive task overhead.
static constexpr size_t kMinChunkVertices = 256;

VertexBuffer VertexBufferBuilder::build(interfaces::WorkerPool* pool) const {
    const auto& attributes = impl_->attributes;

    if (attributes.empty()) {
        return {};
    }

    const size_t vertex_count = attributes[0].vertex_count;

    VertexBuffer result;

    // Compute layout and stride
    size_t current_offset = 0;
    for (const auto& attr : attributes) {
        const size_t comp_size = bytesPerEncoding(attr.encoding);
        const size_t natural_size = comp_size * attr.component_count;

        VertexBuffer::Attribute layout_entry;
        layout_entry.attr_class = attr.attr_class;
        layout_entry.encoding = attr.encoding;
        layout_entry.component_count = attr.component_count;
        layout_entry.offset = current_offset;
        result.layout.push_back(layout_entry);

        // If align is specified, round up the slot size to the alignment boundary
        if (attr.align > 0) {
            current_offset += ((natural_size + attr.align - 1) / attr.align) * attr.align;
        } else {
            current_offset += natural_size;
        }
    }
    result.vertex_stride = static_cast<u32>(current_offset);

    // Allocate and zero-fill interleaved data
    result.data.resize(vertex_count * result.vertex_stride, 0);

    // Lambda that encodes a contiguous range of vertices [vi_begin, vi_end).
    // Each range writes to a non-overlapping region of result.data, so no
    // synchronization is needed between chunks.
    auto encodeRange = [&](size_t vi_begin, size_t vi_end) {
        for (size_t vi = vi_begin; vi < vi_end; ++vi) {
            u8* vertex_base = result.data.data() + vi * result.vertex_stride;

            for (size_t ai = 0; ai < attributes.size(); ++ai) {
                u8* attr_dst = vertex_base + result.layout[ai].offset;
                attributes[ai].encodeInto(attr_dst, vi);
            }
        }
    };

    if (pool && vertex_count >= kParallelThreshold) {
        // Determine chunk size based on output cache locality.
        const size_t stride = result.vertex_stride;
        size_t chunk_verts = (stride > 0) ? (kTargetChunkBytes / stride) : vertex_count;
        chunk_verts = std::max(chunk_verts, kMinChunkVertices);

        const size_t chunk_count = (vertex_count + chunk_verts - 1) / chunk_verts;

        utils::JobGroup group;
        group.add(chunk_count);

        for (size_t ci = 0; ci < chunk_count; ++ci) {
            const size_t vi_begin = ci * chunk_verts;
            const size_t vi_end = std::min(vi_begin + chunk_verts, vertex_count);

            interfaces::WorkerTask task;
            task.fn = [&, vi_begin, vi_end]() {
                encodeRange(vi_begin, vi_end);
                group.done();
            };
            pool->submit(task);
        }
        group.wait();
    } else {
        // Sequential path
        encodeRange(0, vertex_count);
    }

    return result;
}

// ============================================================================
// VertexBuffer — read-back helpers
// ============================================================================

namespace {

const VertexBuffer::Attribute* findAttr(const VertexBuffer& vb, AttributeClass cls,
                                        size_t nth = 0) {
    size_t found = 0;
    for (const auto& a : vb.layout) {
        if (a.attr_class == cls) {
            if (found == nth) return &a;
            ++found;
        }
    }
    return nullptr;
}

f32 decodeFloat(const u8* src, AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::Float32: { f32 v; std::memcpy(&v, src, 4); return v; }
    case AttributeEncoding::Float16: { u16 r; std::memcpy(&r, src, 2); return f16::from_raw(r).to_float(); }
    case AttributeEncoding::SNorm8:  return static_cast<f32>(static_cast<i8>(*src)) / 127.0f;
    case AttributeEncoding::SNorm16: { i16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v) / 32767.0f; }
    case AttributeEncoding::UNorm8:  return static_cast<f32>(*src) / 255.0f;
    case AttributeEncoding::UNorm16: { u16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v) / 65535.0f; }
    case AttributeEncoding::UInt8:   return static_cast<f32>(*src);
    case AttributeEncoding::UInt16:  { u16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v); }
    case AttributeEncoding::UInt32:  { u32 v; std::memcpy(&v, src, 4); return static_cast<f32>(v); }
    case AttributeEncoding::Int8:    return static_cast<f32>(static_cast<i8>(*src));
    case AttributeEncoding::Int16:   { i16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v); }
    case AttributeEncoding::Int32:   { i32 v; std::memcpy(&v, src, 4); return static_cast<f32>(v); }
    }
    return 0.0f;
}

f32 decodeRaw(const u8* src, AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::Float32: { f32 v; std::memcpy(&v, src, 4); return v; }
    case AttributeEncoding::Float16: { u16 r; std::memcpy(&r, src, 2); return f16::from_raw(r).to_float(); }
    case AttributeEncoding::SNorm8:
    case AttributeEncoding::Int8:    return static_cast<f32>(static_cast<i8>(*src));
    case AttributeEncoding::SNorm16:
    case AttributeEncoding::Int16:   { i16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v); }
    case AttributeEncoding::Int32:   { i32 v; std::memcpy(&v, src, 4); return static_cast<f32>(v); }
    case AttributeEncoding::UNorm8:
    case AttributeEncoding::UInt8:   return static_cast<f32>(*src);
    case AttributeEncoding::UNorm16:
    case AttributeEncoding::UInt16:  { u16 v; std::memcpy(&v, src, 2); return static_cast<f32>(v); }
    case AttributeEncoding::UInt32:  { u32 v; std::memcpy(&v, src, 4); return static_cast<f32>(v); }
    }
    return 0.0f;
}

u32 decodeUint(const u8* src, AttributeEncoding enc) {
    switch (enc) {
    case AttributeEncoding::UInt8:
    case AttributeEncoding::UNorm8:  return *src;
    case AttributeEncoding::UInt16:
    case AttributeEncoding::UNorm16: { u16 v; std::memcpy(&v, src, 2); return v; }
    case AttributeEncoding::UInt32:  { u32 v; std::memcpy(&v, src, 4); return v; }
    case AttributeEncoding::Int8:
    case AttributeEncoding::SNorm8:  return static_cast<u32>(static_cast<u8>(*src));
    case AttributeEncoding::Int16:
    case AttributeEncoding::SNorm16: { u16 v; std::memcpy(&v, src, 2); return static_cast<u32>(v); }
    case AttributeEncoding::Int32:   { u32 v; std::memcpy(&v, src, 4); return v; }
    case AttributeEncoding::Float32: { f32 v; std::memcpy(&v, src, 4); return static_cast<u32>(v); }
    case AttributeEncoding::Float16: { u16 r; std::memcpy(&r, src, 2); return static_cast<u32>(f16::from_raw(r).to_float()); }
    }
    return 0;
}

} // anonymous namespace

// ============================================================================
// VertexBuffer — read-back methods
// ============================================================================

size_t VertexBuffer::vertexCount() const {
    return vertex_stride ? data.size() / vertex_stride : 0;
}

size_t VertexBuffer::vertexSize() const {
    return vertex_stride;
}

size_t VertexBuffer::UVsNum() const {
    size_t count = 0;
    for (const auto& a : layout)
        if (a.attr_class == AttributeClass::UV) ++count;
    return count;
}

bool VertexBuffer::hasVertexColors() const {
    for (const auto& a : layout)
        if (a.attr_class == AttributeClass::Color) return true;
    return false;
}

std::vector<Vector3f> VertexBuffer::getPositions() const {
    const auto* attr = findAttr(*this, AttributeClass::Position);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{3});
    std::vector<Vector3f> result(n);
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi].data[c] = decodeFloat(base + c * bpe, attr->encoding);
    }
    return result;
}

std::vector<Vector3f> VertexBuffer::getNormals() const {
    const auto* attr = findAttr(*this, AttributeClass::Normal);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{3});
    std::vector<Vector3f> result(n);
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi].data[c] = decodeFloat(base + c * bpe, attr->encoding);
    }
    return result;
}

std::vector<Vector4f> VertexBuffer::getTangents() const {
    const auto* attr = findAttr(*this, AttributeClass::Tangent);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{4});
    std::vector<Vector4f> result(n);
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi].data[c] = decodeFloat(base + c * bpe, attr->encoding);
    }
    return result;
}

std::vector<Vector2f> VertexBuffer::getUVs(size_t which) const {
    return getUVs(which, 1.0f / 2048.0f, 0.0f);
}

std::vector<Vector2f> VertexBuffer::getUVs(size_t which, f32 uvMultiply, f32 uvOffset) const {
    const auto* attr = findAttr(*this, AttributeClass::UV, which);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{2});
    std::vector<Vector2f> result(n);
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi].data[c] = decodeRaw(base + c * bpe, attr->encoding) * uvMultiply + uvOffset;
    }
    return result;
}

std::vector<Vector4f> VertexBuffer::getColors() const {
    const auto* attr = findAttr(*this, AttributeClass::Color);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{4});
    std::vector<Vector4f> result(n);
    for (size_t vi = 0; vi < n; ++vi) {
        result[vi].data[3] = 1.0f; // default opaque alpha
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi].data[c] = decodeFloat(base + c * bpe, attr->encoding);
    }
    return result;
}

std::vector<std::array<u32, 4>> VertexBuffer::getBoneIndices() const {
    const auto* attr = findAttr(*this, AttributeClass::BlendIndices);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{4});
    std::vector<std::array<u32, 4>> result(n, {0, 0, 0, 0});
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi][c] = decodeUint(base + c * bpe, attr->encoding);
    }
    return result;
}

std::vector<std::array<f32, 4>> VertexBuffer::getBoneWeights() const {
    const auto* attr = findAttr(*this, AttributeClass::BlendWeights);
    if (!attr) return {};
    const size_t n = vertexCount();
    const size_t bpe = bytesPerEncoding(attr->encoding);
    const size_t comps = std::min(attr->component_count, size_t{4});
    std::vector<std::array<f32, 4>> result(n, {0.0f, 0.0f, 0.0f, 0.0f});
    for (size_t vi = 0; vi < n; ++vi) {
        const u8* base = data.data() + vi * vertex_stride + attr->offset;
        for (size_t c = 0; c < comps; ++c)
            result[vi][c] = decodeFloat(base + c * bpe, attr->encoding);
    }
    return result;
}

} // namespace whiteout
