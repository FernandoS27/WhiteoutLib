// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file topology.h
 * @brief The half-edge kernel (WEM v3, design §5.2, §5.9, §5.10).
 *
 * Six arrays, the classic kernel's field for field, plus status. Three implicit
 * relations replace the arrays a naive implementation would add:
 *
 *     opposite(h) = h ^ 1              halfedges are allocated in pairs
 *     edge(h)     = h >> 1
 *     halfedges(e) = { 2e, 2e + 1 }
 *     from(h)     = to(opposite(h))
 *
 * Boundary is modelled OpenMesh's way: a boundary halfedge *exists*, its face is
 * invalid, and boundary halfedges chain into loops through `next`. That keeps
 * `opposite()` total — there are no null checks anywhere in traversal, which is
 * most of what makes half-edge code pleasant.
 *
 * Connectivity is **built from an indexed face set**, not stored (§5.9): the
 * bulk conversion path never pays the ~52 B/triangle. The build is
 * deterministic — same input, same halfedge and edge numbering, on every
 * platform — which is what lets a golden test compare handles at all, and what
 * lets the build be lazy without the result depending on when it ran.
 *
 * Traversal is spelled the way an OpenMesh user expects, so knowing one is
 * knowing the other.
 */

#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "ids.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

/// Why a `Build` refused. Every one of these is a §5.3 repair case that should
/// have been handled before the kernel saw the mesh.
enum class BuildError : u8 {
    None = 0,
    NonManifoldEdge,     ///< Three or more faces on one edge.
    NonManifoldVertex,   ///< A vertex whose outgoing halfedges form more than one fan.
    InconsistentWinding, ///< Two faces traverse an edge in the same direction.
    DegenerateFace,      ///< A face with fewer than three corners, or a repeated corner.
    VertexOutOfRange,    ///< A corner names a vertex the mesh does not have.
};

const char* ToString(BuildError error);

/// What a failed `Build` was looking at, so a caller can report it usefully.
struct BuildResult {
    BuildError error = BuildError::None;
    u32 face = kInvalidId;   ///< The face involved, when the error names one.
    u32 vertex = kInvalidId; ///< The vertex involved, when the error names one.

    bool ok() const {
        return error == BuildError::None;
    }
    explicit operator bool() const {
        return ok();
    }
};

/// The indexed face set the kernel is built from — the on-disk form (§5.9).
struct FaceSet {
    u32 vertexCount = 0;
    std::vector<u32> faceValence;  ///< Corners per face; all 3 for a triangle mesh.
    std::vector<u32> cornerVertex; ///< Concatenated corner lists, in face order.

    template <class V>
    void reflect(V& v) {
        v.field("vertexCount", vertexCount);
        v.field("faceValence", faceValence);
        v.field("cornerVertex", cornerVertex);
    }

    std::size_t faceCount() const {
        return faceValence.size();
    }
    void addTriangle(u32 a, u32 b, u32 c);
    void addFace(std::span<const u32> corners);
    void clear();
};

// ============================================================================
// Topology
// ============================================================================

class Topology {
public:
    // --- construction -------------------------------------------------------

    /**
     * @brief Build connectivity from an indexed face set.
     *
     * O(n) after one sort over the corner keys `(min(a,b), max(a,b))`. On failure
     * the topology is left cleared and the result names the offending element:
     * a strict half-edge kernel cannot represent a non-manifold configuration at
     * all, so refusing is the only honest option. `MeshBuilder` (§12) is what
     * callers use to avoid ever hitting one.
     */
    BuildResult build(const FaceSet& faces);

    void clear();

    bool empty() const {
        return halfedgeTo_.empty() && vertexOutgoing_.empty();
    }

    /// Recovers the indexed face set — the inverse of `build`, and what the
    /// writer serialises. Deleted faces are skipped.
    FaceSet toFaceSet() const;

    // --- counts -------------------------------------------------------------
    //
    // These are slot counts, deleted elements included; `GarbageCollection()`
    // is what makes them dense again (C8).

    u32 vertexCount() const {
        return static_cast<u32>(vertexOutgoing_.size());
    }
    u32 halfedgeCount() const {
        return static_cast<u32>(halfedgeTo_.size());
    }
    u32 edgeCount() const {
        return static_cast<u32>(halfedgeTo_.size() / 2);
    }
    u32 faceCount() const {
        return static_cast<u32>(faceHalfedge_.size());
    }

    // --- the implicit relations (C1) ----------------------------------------

    static constexpr HalfedgeId opposite(HalfedgeId h) {
        return HalfedgeId(h.value() ^ 1u);
    }
    static constexpr EdgeId edge(HalfedgeId h) {
        return EdgeId(h.value() >> 1u);
    }
    static constexpr HalfedgeId halfedge(EdgeId e, u32 which) {
        return HalfedgeId((e.value() << 1u) | (which & 1u));
    }

    // --- traversal ----------------------------------------------------------

    VertexId to(HalfedgeId h) const {
        return halfedgeTo_[h.index()];
    }
    VertexId from(HalfedgeId h) const {
        return halfedgeTo_[opposite(h).index()];
    }
    FaceId face(HalfedgeId h) const {
        return halfedgeFace_[h.index()];
    }
    HalfedgeId next(HalfedgeId h) const {
        return halfedgeNext_[h.index()];
    }
    HalfedgeId prev(HalfedgeId h) const {
        return halfedgePrev_[h.index()];
    }
    HalfedgeId outgoing(VertexId v) const {
        return vertexOutgoing_[v.index()];
    }
    HalfedgeId halfedge(FaceId f) const {
        return faceHalfedge_[f.index()];
    }

    bool isBoundary(HalfedgeId h) const {
        return !halfedgeFace_[h.index()].valid();
    }
    bool isBoundary(EdgeId e) const {
        return isBoundary(halfedge(e, 0)) || isBoundary(halfedge(e, 1));
    }
    /// A vertex is on the boundary when one of its outgoing halfedges is. The
    /// build points `vertexOutgoing_` at that halfedge when there is one, so
    /// this is O(1), not a circulation.
    bool isBoundary(VertexId v) const;

    u32 valence(VertexId v) const;
    u32 valence(FaceId f) const;

    // --- status -------------------------------------------------------------

    Status status(VertexId v) const {
        return vertexStatus_[v.index()];
    }
    Status status(HalfedgeId h) const {
        return halfedgeStatus_[h.index()];
    }
    Status status(EdgeId e) const {
        return edgeStatus_[e.index()];
    }
    Status status(FaceId f) const {
        return faceStatus_[f.index()];
    }

    void setStatus(VertexId v, Status s) {
        vertexStatus_[v.index()] = s;
    }
    void setStatus(HalfedgeId h, Status s) {
        halfedgeStatus_[h.index()] = s;
    }
    void setStatus(EdgeId e, Status s) {
        edgeStatus_[e.index()] = s;
    }
    void setStatus(FaceId f, Status s) {
        faceStatus_[f.index()] = s;
    }

    bool isDeleted(VertexId v) const {
        return hasStatus(vertexStatus_[v.index()], Status::Deleted);
    }
    bool isDeleted(HalfedgeId h) const {
        return hasStatus(halfedgeStatus_[h.index()], Status::Deleted);
    }
    bool isDeleted(EdgeId e) const {
        return hasStatus(edgeStatus_[e.index()], Status::Deleted);
    }
    bool isDeleted(FaceId f) const {
        return hasStatus(faceStatus_[f.index()], Status::Deleted);
    }

    // --- lazy deletion (§5.2) ------------------------------------------------

    /// Marks the face deleted and turns its loop into boundary halfedges. The
    /// edges and vertices survive unless nothing else uses them.
    void deleteFace(FaceId f, bool deleteIsolatedVertices = true);
    void deleteEdge(EdgeId e, bool deleteIsolatedVertices = true);
    void deleteVertex(VertexId v);

    bool hasDeleted() const;

    /**
     * @brief Compacts the arrays and returns the remap tables.
     *
     * The one addition over OpenMesh: WEM has references *into* the mesh from
     * outside it — sections, the skin binding, the repair log — and silently
     * invalidating them is the classic way this bites. `remap[old]` is the new
     * handle, or `kInvalidId` for a dropped element.
     */
    struct Remap {
        std::vector<u32> vertices;
        std::vector<u32> halfedges;
        std::vector<u32> edges;
        std::vector<u32> faces;
        u32 newVertexCount = 0;
        u32 newHalfedgeCount = 0;
        u32 newEdgeCount = 0;
        u32 newFaceCount = 0;
    };
    Remap garbageCollect();

    // --- circulators ---------------------------------------------------------
    //
    // Named as OpenMesh names them. Each is a forward range over a cycle, and
    // each terminates on returning to its start rather than on a null handle —
    // which only works because boundary halfedges exist (C3).

    /// Outgoing halfedges around a vertex, counter-clockwise.
    class VertexOHalfedgeRange;
    /// The neighbouring vertices of a vertex.
    class VertexVertexRange;
    /// The faces around a vertex; boundary gaps are skipped.
    class VertexFaceRange;
    /// The halfedges of a face's loop, starting at `halfedge(f)`.
    class FaceHalfedgeRange;
    /// The vertices of a face's loop.
    class FaceVertexRange;
    /// The faces sharing an edge with a face; boundary neighbours are skipped.
    class FaceFaceRange;
    /// The halfedges of any `next` cycle — a face loop or a boundary loop.
    class HalfedgeLoopRange;

    VertexOHalfedgeRange voh(VertexId v) const;
    VertexVertexRange vv(VertexId v) const;
    VertexFaceRange vf(VertexId v) const;
    FaceHalfedgeRange fh(FaceId f) const;
    FaceVertexRange fv(FaceId f) const;
    FaceFaceRange ff(FaceId f) const;
    HalfedgeLoopRange hl(HalfedgeId h) const;

    /// The halfedge from @p a to @p b, or invalid. Circulates @p a, so it is
    /// O(valence) — fine for edit operations, not for an inner loop.
    HalfedgeId findHalfedge(VertexId a, VertexId b) const;

    // --- raw arrays, for the serializer and the interop contract (C8) --------

    std::span<const HalfedgeId> vertexOutgoingArray() const {
        return {vertexOutgoing_.data(), vertexOutgoing_.size()};
    }
    std::span<const VertexId> halfedgeToArray() const {
        return {halfedgeTo_.data(), halfedgeTo_.size()};
    }
    std::span<const FaceId> halfedgeFaceArray() const {
        return {halfedgeFace_.data(), halfedgeFace_.size()};
    }
    std::span<const HalfedgeId> halfedgeNextArray() const {
        return {halfedgeNext_.data(), halfedgeNext_.size()};
    }
    std::span<const HalfedgeId> halfedgePrevArray() const {
        return {halfedgePrev_.data(), halfedgePrev_.size()};
    }
    std::span<const HalfedgeId> faceHalfedgeArray() const {
        return {faceHalfedge_.data(), faceHalfedge_.size()};
    }

    // --- mutation, for the edit operations in ops.h --------------------------
    //
    // These are the kernel's write primitives. They maintain nothing on their
    // own; `ops.h` composes them into operations that do.

    void setTo(HalfedgeId h, VertexId v) {
        halfedgeTo_[h.index()] = v;
    }
    void setFace(HalfedgeId h, FaceId f) {
        halfedgeFace_[h.index()] = f;
    }
    void setNext(HalfedgeId h, HalfedgeId n) {
        halfedgeNext_[h.index()] = n;
        halfedgePrev_[n.index()] = h;
    }
    void setOutgoing(VertexId v, HalfedgeId h) {
        vertexOutgoing_[v.index()] = h;
    }
    void setFaceHalfedge(FaceId f, HalfedgeId h) {
        faceHalfedge_[f.index()] = h;
    }

    VertexId addVertex();
    FaceId addFaceSlot();
    /// Allocates a halfedge pair and returns the even one.
    HalfedgeId addEdgePair();

    /// Points `vertexOutgoing_[v]` at a boundary halfedge when one exists — the
    /// invariant `isBoundary(VertexId)` and the circulators both rely on.
    void adjustOutgoing(VertexId v);

private:
    /// Rebuilds the boundary `next`/`prev` chains after the face loops are set.
    BuildResult linkBoundaryLoops();

    std::vector<HalfedgeId> vertexOutgoing_;
    std::vector<VertexId> halfedgeTo_;
    std::vector<FaceId> halfedgeFace_;
    std::vector<HalfedgeId> halfedgeNext_;
    std::vector<HalfedgeId> halfedgePrev_;
    std::vector<HalfedgeId> faceHalfedge_;

    std::vector<Status> vertexStatus_;
    std::vector<Status> halfedgeStatus_;
    std::vector<Status> edgeStatus_;
    std::vector<Status> faceStatus_;
};

// ============================================================================
// Circulators
// ============================================================================
//
// Each range is a cheap value: a topology pointer and a start handle. The
// iterators are forward iterators that compare equal to `end()` once they have
// come back round, so `for (auto h : mesh.voh(v))` reads the way it does in
// OpenMesh.

namespace detail {

/// Shared machinery: walk a cycle from `start`, stopping when the step function
/// returns to it. `Step` maps a halfedge to the next one in the cycle.
template <class Derived, class Value>
class CirculatorBase {
public:
    class Iterator {
    public:
        Iterator() = default;
        Iterator(const Derived* owner, HalfedgeId current, bool active)
            : owner_(owner), current_(current), active_(active) {}

        Value operator*() const {
            return owner_->value(current_);
        }
        Iterator& operator++() {
            current_ = owner_->step(current_);
            if (current_ == owner_->start()) {
                active_ = false;
            }
            return *this;
        }
        friend bool operator==(const Iterator& a, const Iterator& b) {
            return a.active_ == b.active_ && (!a.active_ || a.current_ == b.current_);
        }
        friend bool operator!=(const Iterator& a, const Iterator& b) {
            return !(a == b);
        }

        HalfedgeId halfedge() const {
            return current_;
        }

    private:
        const Derived* owner_ = nullptr;
        HalfedgeId current_;
        bool active_ = false;
    };

    Iterator begin() const {
        const Derived& self = static_cast<const Derived&>(*this);
        return Iterator(&self, self.start(), self.start().valid());
    }
    Iterator end() const {
        return Iterator(static_cast<const Derived*>(this), HalfedgeId(), false);
    }
    bool empty() const {
        return begin() == end();
    }
};

} // namespace detail

/// Outgoing halfedges around a vertex: `opposite(prev(h))` walks the fan.
class Topology::VertexOHalfedgeRange
    : public detail::CirculatorBase<Topology::VertexOHalfedgeRange, HalfedgeId> {
public:
    VertexOHalfedgeRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return Topology::opposite(mesh_->prev(h));
    }
    HalfedgeId value(HalfedgeId h) const {
        return h;
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

class Topology::VertexVertexRange
    : public detail::CirculatorBase<Topology::VertexVertexRange, VertexId> {
public:
    VertexVertexRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return Topology::opposite(mesh_->prev(h));
    }
    VertexId value(HalfedgeId h) const {
        return mesh_->to(h);
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

/// Faces around a vertex. A boundary gap yields an invalid handle rather than
/// being skipped silently, so a caller that cares can see the hole; `vfFaces()`
/// on the mesh is the filtered form.
class Topology::VertexFaceRange : public detail::CirculatorBase<Topology::VertexFaceRange, FaceId> {
public:
    VertexFaceRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return Topology::opposite(mesh_->prev(h));
    }
    FaceId value(HalfedgeId h) const {
        return mesh_->face(h);
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

class Topology::FaceHalfedgeRange
    : public detail::CirculatorBase<Topology::FaceHalfedgeRange, HalfedgeId> {
public:
    FaceHalfedgeRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return mesh_->next(h);
    }
    HalfedgeId value(HalfedgeId h) const {
        return h;
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

class Topology::FaceVertexRange
    : public detail::CirculatorBase<Topology::FaceVertexRange, VertexId> {
public:
    FaceVertexRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return mesh_->next(h);
    }
    VertexId value(HalfedgeId h) const {
        return mesh_->to(h);
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

class Topology::FaceFaceRange : public detail::CirculatorBase<Topology::FaceFaceRange, FaceId> {
public:
    FaceFaceRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return mesh_->next(h);
    }
    FaceId value(HalfedgeId h) const {
        return mesh_->face(Topology::opposite(h));
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

class Topology::HalfedgeLoopRange
    : public detail::CirculatorBase<Topology::HalfedgeLoopRange, HalfedgeId> {
public:
    HalfedgeLoopRange(const Topology* mesh, HalfedgeId start) : mesh_(mesh), start_(start) {}
    HalfedgeId start() const {
        return start_;
    }
    HalfedgeId step(HalfedgeId h) const {
        return mesh_->next(h);
    }
    HalfedgeId value(HalfedgeId h) const {
        return h;
    }

private:
    const Topology* mesh_;
    HalfedgeId start_;
};

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
