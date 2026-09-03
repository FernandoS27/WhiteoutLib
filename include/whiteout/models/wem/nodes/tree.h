// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file tree.h
 * @brief `NodeTree` (WEM v3, design §10.3, §10.5).
 *
 * Flat array + parent indices is the *storage*; the tree is a derived view. Same
 * pattern as the geometry, for the same three reasons: the disk form stays
 * trivially serializable, the bulk conversion path never builds the view, and
 * determinism makes handles comparable in tests.
 *
 * **"Parents precede children" is the one order invariant.** It is what makes
 * one-pass evaluation — and one-pass world-bind composition — possible, and every
 * structural op maintains it.
 *
 * The bind-pose *schema* is a per-tree fact and lives here; the pose *values*
 * live on each node, so a bone's five poses leave with the bone. The schema is a
 * list because D3 forces it: a `BoneStructure` carries five `PRSTransform`s and
 * bind poses A and B are **not** the same pose — 202 of 2,525 bones (8.0%) across
 * 400 sampled `.app` files disagree, with deltas up to 19.9 units on a ~7-unit
 * character. Collapsing them into one `inverseBindMatrix` throws that away, and
 * *which one is right* is a profile decision, not a storage one.
 */

#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>

#include "node.h"

namespace whiteout {
namespace models {
namespace wem {

enum class PoseSpace : u8 {
    Model,          ///< Relative to the model root.
    ParentRelative, ///< Relative to the node's parent — the same space as `local`.
};

/**
 * @brief Where one schema entry's authoritative value lives.
 *
 * `Trs` is the shape every format authors in and the one every consumer wants,
 * so it is the default and `Node::poses` is the storage.
 *
 * `Matrix` exists because M3's `IREF` is not a TRS. It ships one 4x4 inverse
 * model-space bind matrix per bone, and across 250 corpus `.m3` files 23 of them
 * (9.2%) hold shear that no translation/rotation/scale can reproduce — the error
 * reaches 30.7 units on a character-sized model, and allowing a negative scale
 * component (which would cover a mirrored bone) fixes none of them. A TRS-only
 * pose would therefore have been silently wrong on a tenth of StarCraft II, so
 * the entry says so and `Node::poseMatrices` carries the matrix as shipped.
 */
enum class PoseStorage : u8 {
    Trs,    ///< `Node::poses[i]` is the value.
    Matrix, ///< `Node::poseMatrices[i]` is the value; `poses[i]` is its decomposition.
};

struct PoseSchema {
    std::string name; ///< "bind", "bindA", "bindAInverse", …
    PoseSpace space = PoseSpace::Model;
    bool inverse = false;
    PoseStorage storage = PoseStorage::Trs;

    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("space", space);
        v.field("inverse", inverse);
        // v2: absent means `Trs`, which is what every schema written before
        // this field said by having no alternative.
        v.since(2).field("storage", storage);
    }
};

/**
 * @brief A half-open index range over a flat array, cheap to return and to
 *        iterate. The values are node indices, not nodes.
 */
/// @bind skip — a raw-pointer view into the tree's hierarchy array; it does not
/// outlive an edit and has nothing to marshal. A binding walks `nodes` and
/// follows `parent`.
class NodeRange {
public:
    NodeRange() = default;
    NodeRange(const u32* data, std::size_t size) : data_(data), size_(size) {}

    const u32* begin() const {
        return data_;
    }
    const u32* end() const {
        return data_ + size_;
    }
    std::size_t size() const {
        return size_;
    }
    bool empty() const {
        return size_ == 0;
    }
    u32 operator[](std::size_t i) const {
        return data_[i];
    }

private:
    const u32* data_ = nullptr;
    std::size_t size_ = 0;
};

/// @bind methods
class NodeTree {
public:
    std::vector<Node> nodes; ///< Parents precede children.
    std::vector<PoseSchema> poseSchema;
    u32 authoritativePose = 0; ///< Which schema entry a profile treats as *the* bind pose.

    /**
     * @brief What convention the values above are in — the fact that decides
     *        whether a `.mdx` exporter may write `pivot` and an `.m3` one may
     *        write `IREF` straight through.
     *
     * Set by every importer, read by `RetargetSkeleton` and by the four
     * exporters. It is a property of what the tree *holds*, and
     * `ProfileDesc::rig` is the property of what a format *wants*; a retarget is
     * the operation between them.
     */
    RigConvention rig = RigConvention::PivotRelative;

    // --- structure -------------------------------------------------------------

    u32 size() const {
        return static_cast<u32>(nodes.size());
    }
    bool empty() const {
        return nodes.empty();
    }

    /// Appends a node and returns its index. The caller is responsible for the
    /// parents-precede-children invariant; `Validate` checks it.
    u32 add(Node node);

    void clear();

    /// True when every node's parent index precedes it (or is invalid).
    bool parentsPrecedeChildren() const;

    /// Reorders `nodes` so parents precede children and returns the remap
    /// (`remap[old] == new`). A cycle leaves the tree untouched and returns an
    /// empty remap.
    std::vector<u32> sortParentsFirst();

    /// The hierarchy view is derived and lazily built, so it is not stored; a
    /// reader rebuilds it on first use exactly as an edit does.
    template <class V>
    void reflect(V& v) {
        v.field("nodes", nodes);
        v.field("poseSchema", poseSchema);
        v.field("authoritativePose", authoritativePose);
        // v2: a tree written before the convention was named carries none, and
        // the schema says which one it was — `inferredRig` reads it back. The
        // repair is a no-op on a v2 chunk, whose stored value already agrees.
        v.since(2).field("rig", rig);
        if constexpr (V::kReading) {
            if (rig == RigConvention::PivotRelative) {
                rig = inferredRig();
            }
            invalidateHierarchy();
        }
    }

    // --- the lazy hierarchy view (§10.3) ---------------------------------------

    /**
     * @brief Builds the child lists and the depth-first pre-order from `parent`.
     *
     * Children come out in index order, so the build is deterministic and golden
     * tests can compare traversal orders. Dropped by any structural mutation.
     *
     * The pre-order is stored as one flat array with a per-node (offset, size),
     * the Euler-tour layout: that makes `subtree()` a contiguous slice rather
     * than a walk, so it can be a real range and two callers can hold two
     * subtree ranges at once.
     */
    void ensureHierarchy() const;
    void invalidateHierarchy() const;
    bool hasHierarchy() const {
        return hierarchyValid_;
    }

    // The four `NodeRange` returns are `@bind skip`ped for the reason on
    // `NodeRange` itself: the view does not outlive the next edit.

    /// @bind skip
    NodeRange roots() const;
    /// @bind skip
    NodeRange children(u32 node) const;
    /// Depth-first pre-order over @p node and everything under it, @p node first.
    /// @bind skip
    NodeRange subtree(u32 node) const;
    /// Every node of @p kind, in index order. Built lazily, per kind.
    /// @bind skip
    NodeRange ofKind(NodeKind kind) const;

    u32 depth(u32 node) const;

    // --- poses -------------------------------------------------------------------

    /**
     * @brief Composes locals up the chain, honouring the inherit flags.
     *
     * The same composition D3's `Skeleton_ComposeWorldPose` performs. A node
     * flagged `ModelSpace` stops the walk: its local *is* its world.
     */
    Transform worldBind(u32 node) const;

    /// The pose value for @p node under schema entry @p pose, or its `local`
    /// (composed to the schema's space) when the node carries no pose array.
    ///
    /// A `PoseStorage::Matrix` entry answers with the *decomposition* of the
    /// stored matrix, which is what `poses` holds — use @ref poseMatrixOf when
    /// the exact value is what matters.
    Transform poseOf(u32 node, u32 pose) const;

    /// The same value as a matrix, and the authoritative one: a
    /// `PoseStorage::Matrix` entry with a stored matrix answers with it
    /// unchanged, and everything else composes @ref poseOf.
    Matrix44f poseMatrixOf(u32 node, u32 pose) const;

    /// Resizes every Bone node's `poses` to `poseSchema.size()`, filling new
    /// entries from `worldBind`/`local` as the schema's space asks.
    void conformPoses();

    /**
     * @brief The matrix a skinning palette multiplies @p node's animated frame
     *        by — `skin = inverseBindMatrix(b) * world(b)`.
     *
     * The one question every exporter and every evaluator actually has, and the
     * only place `rig` needs to be read to answer it: an `ExplicitBind` tree
     * answers with the shipped `IREF`/`tTransform4`, and a `PivotRelative` one
     * with the inverse of the composed rest chain — identity in the animation's
     * own terms, but the pivot chain is what `local` holds there, so the inverse
     * of `worldBind` is what makes the two agree at the bind pose.
     */
    Matrix44f inverseBindMatrix(u32 node) const;

    /// The convention `poseSchema` implies, for a tree that never recorded one:
    /// an inverse or matrix pose entry exists only to carry an explicit bind.
    RigConvention inferredRig() const;

private:
    void buildHierarchy() const;

    mutable std::vector<u32> childrenFlat_; ///< Children grouped by parent, index order.
    mutable std::vector<u32> childOffset_;
    mutable std::vector<u32> childCount_;
    mutable std::vector<u32> roots_;
    mutable std::vector<u32> preorder_;      ///< DFS pre-order over every root.
    mutable std::vector<u32> preorderIndex_; ///< node -> its position in `preorder_`.
    mutable std::vector<u32> subtreeSize_;
    mutable std::vector<std::vector<u32>> byKind_;
    mutable bool hierarchyValid_ = false;
};

} // namespace wem
} // namespace models
} // namespace whiteout
