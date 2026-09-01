// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file repair.h
 * @brief Manifold repair (WEM v3, design §5.3).
 *
 * A strict half-edge kernel cannot represent a non-manifold configuration at
 * all, so the importer must produce a manifold. Five cases, all of which occur
 * in shipped content:
 *
 * | Input | Repair | Recorded as |
 * |---|---|---|
 * | Non-manifold edge (3+ faces) | keep two fans on the original vertices; duplicate the edge's
 * endpoints for each extra fan | one `VertexSplit` per duplicated vertex | | Bowtie vertex | one
 * vertex per fan | `VertexSplit` | | Duplicate face (same vertex set, either orientation) |
 * duplicate the second face's vertices | `VertexSplit` | | Inconsistent winding across an otherwise
 * manifold edge | split rather than flip | `VertexSplit` | | Degenerate face (repeated corner, or
 * zero area) | drop the face | `droppedFaces` |
 *
 * Winding is **split, not flipped**, because flipping changes the normal and the
 * source authored it that way.
 *
 * `Unrepair()` is exact: a merge along the split list in reverse, and the
 * face-vertex list that comes out is the one that went in. A converter that
 * exports to the source profile runs it; a converter that exports to a
 * *different* profile usually should not, because the target's importer will hit
 * the same non-manifold configuration.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/vector_types.h>

#include "ids.h"
#include "topology.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// One duplicated vertex. `created` carries `original`'s attributes and skin.
struct VertexSplit {
    u32 original = kInvalidId;
    u32 created = kInvalidId;

    template <class V>
    void reflect(V& v) {
        v.field("original", original);
        v.field("created", created);
    }
};

/// A face the repair dropped, kept whole so it can be put back — including
/// *where*, because "the face-vertex list that comes out is the one that went
/// in" is only true if the face returns to its original position.
struct FaceRecord {
    std::vector<u32> corners;
    u32 section = 0;
    u32 index = kInvalidId; ///< Position in the input face set.

    template <class V>
    void reflect(V& v) {
        v.field("corners", corners);
        v.field("section", section);
        v.field("index", index);
    }
};

struct RepairLog {
    std::vector<VertexSplit> splits;      ///< In application order.
    std::vector<FaceRecord> droppedFaces; ///< Vertex list + section.

    template <class V>
    void reflect(V& v) {
        v.field("splits", splits);
        v.field("droppedFaces", droppedFaces);
    }

    bool empty() const {
        return splits.empty() && droppedFaces.empty();
    }
    void clear() {
        splits.clear();
        droppedFaces.clear();
    }

    /// Counts by cause, for the §16 P1 corpus sweep — the one number this design
    /// most wants: how much repair real content costs.
    struct Stats {
        u32 verticesAdded = 0;
        u32 facesDropped = 0;
    };
    Stats stats() const {
        return Stats{static_cast<u32>(splits.size()), static_cast<u32>(droppedFaces.size())};
    }
};

/// What `MeshBuilder::build` does when the input is not a manifold.
enum class RepairPolicy : u8 {
    RepairAndRecord, ///< Repair and fill the log. The default and the only lossless option.
    RepairSilently,  ///< Repair without a log; `Unrepair` is then impossible.
    Refuse,          ///< Leave the mesh unbuilt and report. For a caller that wants to know.
};

struct RepairResult {
    FaceSet faces;             ///< The repaired, manifold face set.
    std::vector<u32> sections; ///< Surviving faces' section values, in output order.
    RepairLog log;             ///< Empty when the input was already a manifold.
    bool changed = false;
};

/**
 * @brief Makes @p faces manifold.
 *
 * Deterministic: faces are processed in input order and duplicated vertices are
 * appended in the order the splits happen, so the same input produces the same
 * output and the same log everywhere. That is what makes the repair invisible to
 * the render view's byte-identity property (§5.8), which regroups on
 * `mergeGroup`.
 *
 * @param sections Per-face section values, used only to fill `droppedFaces`.
 *                 May be empty.
 * @param positions Vertex positions. When given, zero-area faces are dropped as
 *                 degenerate too; without them only the combinatorial degeneracy
 *                 (a repeated corner) is detectable.
 */
RepairResult Repair(const FaceSet& faces, std::span<const u32> sections = {},
                    std::span<const Vector3f> positions = {});

/**
 * @brief The exact inverse: merges every split back and restores every dropped
 *        face, reproducing the face set `Repair` was given.
 *
 * The dropped faces come back in their original positions, which is what makes
 * "the face-vertex list that comes out is the one that went in" true rather than
 * approximately true.
 */
FaceSet Unrepair(const FaceSet& faces, const RepairLog& log);

/**
 * @brief The `mergeGroup` layer a repaired mesh must carry (§5.3).
 *
 * Every vertex carries its pre-repair identity: a split copies the original's.
 * Three things read it — `Unrepair`, `WeldVertices` (which never welds *across*
 * groups unless asked) and the render view, which deduplicates on it so that
 * repair does not perturb byte-identity.
 */
std::vector<u32> BuildMergeGroups(u32 vertexCount, const RepairLog& log);

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
