// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// What the modelling sources share: the weld (`prepare.cpp`) and the tools.

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

#include <whiteout/models/wem/geometry/attributes.h>
#include <whiteout/models/wem/geometry/mesh.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace modellingdetail {

inline f64 angleBetween(const std::array<f64, 3>& a, const std::array<f64, 3>& b) {
    const f64 cx = a[1] * b[2] - a[2] * b[1];
    const f64 cy = a[2] * b[0] - a[0] * b[2];
    const f64 cz = a[0] * b[1] - a[1] * b[0];
    return std::atan2(std::sqrt(cx * cx + cy * cy + cz * cz), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

inline std::array<f64, 3> toArray(const Vector3f& v) {
    return {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}

inline std::span<const Vector3f> positionsOf(const Mesh& mesh) {
    return mesh.attributes.get<const Vector3f>(names::kPosition, Domain::Vertex);
}

inline std::vector<u32> sortedUnique(std::vector<u32> list) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    return list;
}

} // namespace modellingdetail
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
