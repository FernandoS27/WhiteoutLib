// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file triangulation.h
 * @brief The stored triangulation, and the one function every consumer cuts
 *        faces with (EDIT_MODE_MODELLING_DESIGN.md §2.3).
 *
 * A face of four or more corners carries the triangles it is drawn and written
 * as, so a polygon made from a file's triangles exports exactly those triangles,
 * whatever a later build's automatic rule would pick. A triangle carries none.
 * Every consumer -- the render view and so every exporter, picking, skinning's
 * triangle lists, the point table's ring -- cuts through `TriangulateFace`, so
 * the picture, the pick and the file agree.
 */

#include <span>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {

class Mesh;

namespace geom {

/**
 * @brief Per face SLOT (indexed like Face layers), the triangles it is drawn and
 *        written as, CSR like `SkinBinding`.
 *
 * Each triangle is three VERTEX ids of the face in emission order: a face never
 * repeats a vertex (§5.10 C6), so a vertex names its corner, and a row survives
 * a re-rooted loop and any halfedge renumbering. A row may be empty: that face
 * is cut by the automatic rule. A triangle's row is always empty.
 */
struct FaceTriangulation {
    std::vector<u32> offsets;  ///< Exactly slotCount + 1, or empty: no face stores one.
    std::vector<u32> vertices; ///< 3 per triangle.

    template <class V>
    void reflect(V& v) {
        v.field("offsets", offsets);
        v.field("vertices", vertices);
    }

    bool empty() const {
        return offsets.empty();
    }

    /// Face @p face's triangles, or nothing (no table, or a slot past it).
    std::span<const u32> row(u32 face) const;

    /// `allocFace`'s half: one more slot, with an empty row. Nothing while the
    /// table is empty, since an empty table already means "no row anywhere".
    void appendFace();

    void clear();

    /// `GarbageCollect`'s face table (`remap[old]` = new, or kInvalidId).
    void remapFaces(std::span<const u32> remap, u32 newCount);

    /// A global vertex table (`remap[old]` = new): a weld's, `GarbageCollect`'s.
    /// A row naming a vertex the table drops is cleared.
    void remapVertices(std::span<const u32> remap);

    /// One face's own renames, @p from[i] becoming @p to[i]: a per-fan relink
    /// gives one old vertex different new ids in different faces, which a
    /// global table cannot.
    void rewriteRow(u32 face, std::span<const u32> from, std::span<const u32> to);

    /// Each triangle of @p face turned the other way: its loop was reversed.
    void reverseRow(u32 face);

    /// Replaces face @p face's row (@p triangles empty clears it), creating the
    /// table for @p slotCount slots when there is none. One face at a time is
    /// linear in the table; `FaceTriangulationBuilder` builds a whole one.
    void setRow(u32 face, std::span<const u32> triangles, u32 slotCount);
};

/// Builds a whole table at once: CSR assignment one face at a time is quadratic.
class FaceTriangulationBuilder {
public:
    explicit FaceTriangulationBuilder(u32 slotCount);

    /// Face @p face's row: three vertex ids per triangle, or empty.
    void set(u32 face, std::span<const u32> triangles);

    /// The table, or an empty one when no row was set.
    FaceTriangulation build() const;

private:
    std::vector<std::vector<u32>> rows_;
};

/// What `TriangulateFace` did beyond the plain rule, for a caller that notes it.
enum class CutNote : u8 {
    None,       ///< A triangle, or the automatic rule with nothing to report.
    Stored,     ///< The face's stored row, valid, was used.
    Folded,     ///< A quad whose two cuts both fold: 0-2 was taken anyway.
    NoEar,      ///< Ear clipping found no ear and clipped a last-resort corner.
    Degenerate, ///< The face has no Newell normal (it is a line): fanned from corner 0.
};

/**
 * @brief The triangles a face is drawn, written and picked as: corner ordinals
 *        into its loop, three per triangle, appended to @p out.
 *
 * The stored row (vertex ids, @p storedRow) when valid (`RowValid`), else the
 * automatic rule. "Facing" means a signed area in the Newell plane above 1e-6
 * times the square of the face's longest side.
 * - 3 corners: the triangle, from the first corner.
 * - 4 corners: the shorter diagonal of the two whose triangles both face the
 *   Newell normal, 0-2 on a tie within 1e-6 relative (so a square does not
 *   flicker under a drag); when only one does (concave), that one; when neither
 *   does (a folded quad), 0-2, noted.
 * - 5 and up: ear clipping in the Newell plane. An ear is a convex corner whose
 *   triangle holds no other corner, its boundary included. Strict ears are
 *   clipped before zero-area ones; with no ear, the non-reflex corner of largest
 *   signed area; with none of those either, the corner of largest signed area,
 *   noted. Then Lawson flips, each only across a strictly convex quad and only on
 *   a gain above 1e-6 rad in the smaller minimum angle, so ties -- every
 *   cocircular polygon, to within its corners' rounding -- never flip back.
 * A face with no Newell normal -- its area within the facing threshold, a line --
 * falls back to the fan from the first corner, noted. Every branch clips one
 * corner per step: the result always has valence - 2 triangles.
 *
 * @param cornerVertices the face's loop, as vertex ids.
 * @param positions indexed by vertex id.
 */
CutNote TriangulateFace(std::span<const u32> cornerVertices, std::span<const Vector3f> positions,
                        std::span<const u32> storedRow, std::vector<u32>& out);

/**
 * @brief Whether @p row (vertex ids, three per triangle) is a valid cut of the
 *        face (§2.3), all of:
 *
 * the face has four or more corners; the row holds valence - 2 triangles; each
 * triangle has three distinct corners of the face and turns the loop's way with
 * a signed area above the facing threshold; every loop edge is in exactly one
 * triangle, in the loop's direction; every other side appears exactly twice,
 * once each way; and no two diagonals cross, by loop ordinals.
 */
bool RowValid(std::span<const u32> cornerVertices, std::span<const Vector3f> positions,
              std::span<const u32> row);

/// Each face of `mesh.faceSet()`, in order, as its slot (the index rows and Face
/// layers use). The two differ only while lazily deleted faces wait for
/// `GarbageCollect`.
std::vector<u32> FaceSetSlots(const Mesh& mesh);

/**
 * @brief Every face of @p mesh cut as it is drawn, in face order: three vertex
 *        ids per triangle into @p triangles, and each triangle's face SLOT into
 *        @p faceOf when given. The loop every consumer shares; it reads the
 *        mesh's face set, so it needs no connectivity.
 */
void TriangulateMesh(const Mesh& mesh, std::vector<u32>& triangles,
                     std::vector<u32>* faceOf = nullptr);

/**
 * @brief Stores `TriangulateFace`'s automatic cut for every face of four or more
 *        corners in @p faces (face slots; all when empty) whose row is empty or
 *        fails `RowValid`. Returns how many rows it wrote.
 *
 * Called at every commit that can leave a polygon without a valid row: a
 * tool's finish, a move's commit, a weld. Needs connectivity.
 */
u32 MaterialiseRows(Mesh& mesh, std::span<const u32> faces = {});

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
