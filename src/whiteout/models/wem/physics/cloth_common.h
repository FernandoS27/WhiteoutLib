// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// What the cloth sources share: a vertex's skin and a cloth's cage.

#include <set>
#include <span>
#include <vector>

#include <whiteout/models/wem/geometry/mesh.h>
#include <whiteout/models/wem/physics/physics.h>

namespace whiteout {
namespace models {
namespace wem {
namespace clothdetail {

/// A vertex's normalised skin.
inline std::vector<geom::Influence> SkinOf(const Mesh& mesh, u32 vertex) {
    std::vector<geom::Influence> out;
    if (vertex >= mesh.skin.vertexCount()) {
        return out;
    }
    f32 total = 0.0f;
    for (const geom::Influence& influence : mesh.skin.forVertex(vertex)) {
        if (influence.weight > 0.0f) {
            out.push_back(influence);
            total += influence.weight;
        }
    }
    for (geom::Influence& influence : out) {
        influence.weight /= total;
    }
    return out;
}

/// The cage vertices of @p cloth: its cage section's.
inline std::set<u32> CageOf(const Mesh& mesh, const Cloth& cloth) {
    const geom::FaceSet& set = mesh.faceSet();
    const std::span<const u32> sections = mesh.faceSections();
    std::set<u32> cage;
    for (std::size_t f = 0, corner = 0; f < set.faceValence.size(); corner += set.faceValence[f], ++f) {
        if (f < sections.size() && sections[f] == cloth.cage.section) {
            for (u32 k = 0; k < set.faceValence[f]; ++k) {
                cage.insert(set.cornerVertex[corner + k]);
            }
        }
    }
    return cage;
}

} // namespace clothdetail
} // namespace wem
} // namespace models
} // namespace whiteout
