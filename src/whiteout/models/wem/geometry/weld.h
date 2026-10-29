// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// The weld engine (rule 3's fixed point), shared by the prepare (`prepare.cpp`)
/// and the Weld tool (`modelling.cpp`). The functions are documented where
/// `prepare.cpp` defines them.

#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/models/wem/geometry/mesh.h>
#include <whiteout/models/wem/geometry/prepare.h>
#include <whiteout/vector_types.h>

#include "rebuild.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace welddetail {

/// The mesh's face set, indexed for the weld.
struct FaceTable {
    const FaceSet* set = nullptr;
    std::vector<u32> base;                  ///< Per face, its first corner.
    std::vector<std::vector<u32>> facesOf;  ///< Per vertex, the faces holding it, ascending.
};

/// A partition of the vertices into merge groups, each group's members ascending.
struct Partition {
    std::vector<u32> groupOf;
    std::vector<std::vector<u32>> groups;

    explicit Partition(u32 vertexCount) : groupOf(vertexCount), groups(vertexCount) {
        for (u32 v = 0; v < vertexCount; ++v) {
            groupOf[v] = v;
            groups[v] = {v};
        }
    }

    /// Makes @p members (ascending, all currently singletons) one group.
    void join(const std::vector<u32>& members) {
        if (members.size() < 2) {
            return;
        }
        const u32 target = groupOf[members[0]];
        for (std::size_t i = 1; i < members.size(); ++i) {
            const u32 from = groupOf[members[i]];
            for (const u32 v : groups[from]) {
                groupOf[v] = target;
                groups[target].push_back(v);
            }
            groups[from].clear();
        }
        std::sort(groups[target].begin(), groups[target].end());
    }

    /// Moves @p movers out of their group into a new one of their own.
    void split(const std::vector<u32>& movers) {
        if (movers.empty()) {
            return;
        }
        const u32 from = groupOf[movers[0]];
        const u32 fresh = static_cast<u32>(groups.size());
        groups.emplace_back();
        for (const u32 v : movers) {
            groupOf[v] = fresh;
            groups.back().push_back(v);
        }
        std::vector<u32>& rest = groups[from];
        rest.erase(std::remove_if(rest.begin(), rest.end(),
                                  [&](u32 v) { return groupOf[v] != from; }),
                   rest.end());
        std::sort(groups.back().begin(), groups.back().end());
    }
};

/// The face set a partition welds to, in survivors' old ids.
struct Candidate {
    FaceSet faces;
    std::vector<u32> sections;
    std::vector<u32> sourceFace;        ///< Per candidate face.
    std::vector<u32> sourceCorner;      ///< Per candidate corner, into the original corners.
    std::vector<u32> candidateOfCorner; ///< Per original corner, or kInvalidId (its face went).
    std::vector<u32> faceOfCandidateCorner;
};

struct Settled {
    std::vector<u32> survivorOf;  ///< Per vertex: its group's survivor.
    std::vector<Vector3f> places; ///< Per vertex: where it ends up.
    Candidate candidate;
    u32 keptNonManifold = 0;
};

using Chooser = std::function<u32(const std::vector<u32>&)>;
using Placer = std::function<void(const Partition&, std::span<const u8>, std::vector<Vector3f>&)>;

FaceTable tableOf(const FaceSet& set);
Candidate candidateOf(const FaceTable& table, std::span<const u32> sections,
                      std::span<const u32> survivorOf, bool collapse);
Settled settle(const FaceTable& table, std::span<const u32> sections, Partition& partition,
               const Chooser& choose, const Placer& place, bool collapse);
detail::RebuildResult commit(Mesh& mesh, const Settled& settled, u32 faceCount, PrepareReport* report);
bool sameBinding(const Mesh& mesh, std::span<const std::optional<u32>> rigidOf,
                 std::span<const u16> pins, u32 a, u32 b);
bool shareFaceApart(const FaceTable& table, u32 a, u32 b);
std::vector<std::vector<u32>> greedy(const std::vector<u32>& members,
                                     const std::function<bool(u32, u32)>& conflicts);
bool mergesAny(const Partition& partition);

} // namespace welddetail
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
