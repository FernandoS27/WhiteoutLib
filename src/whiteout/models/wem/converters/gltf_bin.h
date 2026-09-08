// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file gltf_bin.h
 * @brief BIN-chunk building shared by the glTF geometry and animation export
 *        halves. Internal to the converter — nothing outside
 *        `converters/gltf_*.cpp` includes this.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/gltf/gltf.h>

namespace whiteout {
namespace models {
namespace wem {
namespace gltf_detail {

struct BinBuilder {
    std::vector<u8> bytes;

    /// Appends @p size bytes at 4-alignment and returns their offset.
    u32 append(const void* data, std::size_t size) {
        while (bytes.size() % 4 != 0) {
            bytes.push_back(0);
        }
        const u32 offset = static_cast<u32>(bytes.size());
        const u8* raw = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), raw, raw + size);
        return offset;
    }
};

inline u32 AddView(gltf::Asset& asset, BinBuilder& bin, const void* data, std::size_t size,
                   gltf::BufferTarget target) {
    gltf::BufferView view;
    view.buffer = 0;
    view.byteOffset = bin.append(data, size);
    view.byteLength = static_cast<u32>(size);
    view.target = target;
    asset.bufferViews.push_back(view);
    return static_cast<u32>(asset.bufferViews.size() - 1);
}

/// One tightly-packed float accessor. @p target is `ArrayBuffer` for vertex
/// data and `None` for everything nothing binds as a vertex stream (inverse
/// binds, animation samplers) — a view reused across usages trips the
/// validator's target check. @p withBounds computes the min/max the spec
/// mandates on POSITION and animation inputs.
inline u32 AddFloatAccessor(gltf::Asset& asset, BinBuilder& bin, std::span<const f32> values,
                            u32 components, gltf::AccessorType type, bool withBounds,
                            gltf::BufferTarget target = gltf::BufferTarget::ArrayBuffer) {
    const u32 count = static_cast<u32>(values.size() / components);
    const u32 view = AddView(asset, bin, values.data(), values.size() * sizeof(f32), target);
    gltf::Accessor accessor;
    accessor.bufferView = view;
    accessor.componentType = gltf::ComponentType::F32;
    accessor.count = count;
    accessor.type = type;
    if (withBounds && count > 0) {
        accessor.min.assign(components, 0.0);
        accessor.max.assign(components, 0.0);
        for (u32 c = 0; c < components; ++c) {
            accessor.min[c] = values[c];
            accessor.max[c] = values[c];
        }
        for (u32 i = 1; i < count; ++i) {
            for (u32 c = 0; c < components; ++c) {
                const f64 value = values[static_cast<std::size_t>(i) * components + c];
                if (value < accessor.min[c]) {
                    accessor.min[c] = value;
                }
                if (value > accessor.max[c]) {
                    accessor.max[c] = value;
                }
            }
        }
    }
    asset.accessors.push_back(std::move(accessor));
    return static_cast<u32>(asset.accessors.size() - 1);
}

// --- the basis permutation (GLTF_DESIGN §3), shared by both halves -----------

inline Vector3f PermuteToGltf(const Vector3f& v) {
    return {v.y, v.z, v.x};
}

inline Quaternion PermuteToGltf(const Quaternion& q) {
    // A rotation conjugated by a pure permutation permutes its vector part the
    // same way; w is untouched.
    return {q.y, q.z, q.x, q.w};
}

} // namespace gltf_detail
} // namespace wem
} // namespace models
} // namespace whiteout
