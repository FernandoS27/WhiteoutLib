// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file node.h
 * @brief The node (WEM v3, design §10.1, §10.2).
 *
 * **One node model instead of a skeleton plus satellites.** MDX already is this
 * design — a literal generic `Node` with 12 kinds, parent ids and per-kind flag
 * bits. M2, M3 and D3 store the same graph as one bone array plus satellite
 * arrays carrying bone indices, which is a *flattened encoding of parenting*.
 * WEM adopts the MDX shape and the converters flatten and unflatten:
 * **everything with a transform and a place in the hierarchy is a Node**; a bone
 * is the kind of node skin can bind to, not a separate concept.
 *
 * A node is **self-contained by requirement**: removing, copying or reparenting
 * one moves everything that is *about* it, with no parallel array to edit in
 * sync. That is why the bind poses live here (§10.5) and why the payload is
 * inline rather than an index into a per-kind table.
 *
 * **Animated per-node channels are not stored on the node.** A light's colour
 * track, an attachment's visibility track, MDX's per-node KGTR/KGRT/KGSC — all of
 * it becomes sub-tracks driving the node's channels (§10.8). The node holds the
 * static bind value; the channel table declares the property; the clip holds the
 * motion. One rule, no per-kind track fields.
 */

#include <string>
#include <variant>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/models/wem/reflect.h>
#include <whiteout/vector_types.h>

#include "../asset_key.h"
#include "../bounds.h"
#include "../native_bag.h"
#include "../profile.h"

namespace whiteout {
namespace models {
namespace wem {

inline constexpr u32 kInvalidNode = 0xFFFFFFFFu;

// ============================================================================
// Transform
// ============================================================================

/// TRS everywhere in WEM. Nothing stores a matrix that a TRS can express.
struct Transform {
    Vector3f translation{0, 0, 0};
    Quaternion rotation{0, 0, 0, 1};
    Vector3f scale{1, 1, 1};

    static Transform identity() {
        return Transform{};
    }

    template <class V>
    void reflect(V& v) {
        v.field("translation", translation);
        v.field("rotation", rotation);
        v.field("scale", scale);
    }
};

/// @p parent composed with @p child, child applied first — the same composition
/// D3's `Skeleton_ComposeWorldPose` performs.
Transform Compose(const Transform& parent, const Transform& child);

/// The transform that undoes @p transform. Exact for a uniform scale; for a
/// non-uniform one the inverse of the scale is componentwise, which is what every
/// TRS pipeline assumes.
Transform Inverse(const Transform& transform);

Vector3f TransformPoint(const Transform& transform, const Vector3f& point);

/**
 * @brief @p transform as a matrix, in the convention every bind and skinning
 *        matrix in WEM uses: **row vectors**, `p * S * R + translation`, with
 *        the translation in `data[3]`.
 *
 * That is the map `TransformPoint` performs and the one `Compose` chains, and
 * it is the layout an `.m3` `IREF` and a D3 `tTransform4` arrive in, so a
 * product composes child-first: `ToMatrix(child) * ToMatrix(parent)`.
 *
 * **Not `Matrix44f::compose`**, which pairs a *column*-vector 3x3 with a
 * row-slot translation and applies the three in the opposite order. The two are
 * self-consistent with `Matrix44f::extract_*` and equal for a pure translation,
 * which is why the pivot formats never noticed.
 */
Matrix44f ToMatrix(const Transform& transform);

/// The inverse of @ref ToMatrix, exact for a matrix that is one. A 3x3 carrying
/// shear decomposes to the nearest scale-rotation, which is what a TRS can hold.
Transform FromMatrix(const Matrix44f& matrix);

// ============================================================================
// Kinds and flags
// ============================================================================

enum class NodeKind : u8 {
    Helper = 0, ///< Transform only (MDX Helper).
    Bone,       ///< Skinnable.
    Attachment, ///< MDX/M2/M3 attachment, D3 hardpoint.
    Light,
    Camera,
    ParticleEmitter, ///< Placement + reference; the system itself is out of scope (§18).
    RibbonEmitter,
    Event, ///< A named anchor clips fire at (MDX EventObject, M2 Event).
    CollisionShape,
    Count
};

const char* ToString(NodeKind kind);

/**
 * @brief What at least two formats mean the same way.
 *
 * The inherit bits and the billboard family are MDX's vocabulary, and M3's
 * `BoneFlag` carries the same concepts. Everything else rides in `native`.
 */
enum class NodeFlags : u32 {
    None = 0,
    Hidden = 0x0001,
    DontInheritTranslation = 0x0002,
    DontInheritRotation = 0x0004,
    DontInheritScale = 0x0008,
    Billboarded = 0x0010,
    BillboardLockX = 0x0020,
    BillboardLockY = 0x0040,
    BillboardLockZ = 0x0080,
    ModelSpace = 0x0100, ///< The node's transform is authored in model space.
};

constexpr NodeFlags operator|(NodeFlags a, NodeFlags b) {
    return static_cast<NodeFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr NodeFlags operator&(NodeFlags a, NodeFlags b) {
    return static_cast<NodeFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
inline NodeFlags& operator|=(NodeFlags& a, NodeFlags b) {
    a = a | b;
    return a;
}
constexpr bool hasFlag(NodeFlags value, NodeFlags bit) {
    return (static_cast<u32>(value) & static_cast<u32>(bit)) != 0;
}

// ============================================================================
// Payloads
// ============================================================================

struct Sphere {
    Vector3f center{0, 0, 0};
    f32 radius = 0;

    template <class V>
    void reflect(V& v) {
        v.field("center", center);
        v.field("radius", radius);
    }
};

enum class LightKind : u8 { Omni, Directional, Spot, Ambient };

/// Empty for a Helper; the alternatives below are one per kind. Both empty
/// payloads still reflect: an empty body writes nothing, and having one keeps
/// every alternative visitable without the payload switch special-casing two.
struct HelperPayload {
    template <class V>
    void reflect(V&) {}
};

struct BonePayload {
    Extent bounds; ///< D3 ships both a box and a sphere, per bone.
    Sphere sphere;

    template <class V>
    void reflect(V& v) {
        v.field("bounds", bounds);
        v.field("sphere", sphere);
    }
};

/**
 * @brief An attach point, and what is riding it.
 *
 * The placement is the node — that is the whole of an MDX or `.m2` attachment,
 * and of a D3 hardpoint, which is skeleton-relative data shipped with the
 * skeleton. The equip-slot key (M2's attachment `id`, the hardpoint's own name)
 * lives in `native`.
 *
 * What rides it is the other half, and it is here rather than in a parallel list
 * for §10.2's reason: removing or reparenting the node has to move everything
 * that is *about* it, and a side table of `{node, child}` pairs would be one more
 * referencer §10.6 has to invalidate.
 *
 * Both halves are optional and independent. `asset` is what the source named —
 * an effect WEM does not contain (§18), or a model it does. `model` is filled
 * when that name resolved to something the document now holds, which is how a D3
 * child actor arrives: it converts to a `Model` of its own and the attach point
 * points at it.
 */
struct AttachmentPayload {
    AssetKey asset;            ///< What the source named, resolved or not.
    u32 model = kInvalidIndex; ///< -> `Document::models[]` once it resolved.

    template <class V>
    void reflect(V& v) {
        v.field("asset", asset);
        v.field("model", model);
    }
};

struct LightPayload {
    LightKind kind = LightKind::Omni;
    Vector3f color{1, 1, 1};
    f32 intensity = 1;
    f32 attenuationStart = 0;
    f32 attenuationEnd = 0;
    f32 hotSpot = 0; ///< Spot cone inner angle.
    f32 falloff = 0; ///< Spot cone outer angle.

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("color", color);
        v.field("intensity", intensity);
        v.field("attenuationStart", attenuationStart);
        v.field("attenuationEnd", attenuationEnd);
        v.field("hotSpot", hotSpot);
        v.field("falloff", falloff);
    }
};

struct CameraPayload {
    f32 fov = 0;
    f32 nearClip = 0;
    f32 farClip = 0;

    template <class V>
    void reflect(V& v) {
        v.field("fov", fov);
        v.field("nearClip", nearClip);
        v.field("farClip", farClip);
    }
};

struct ParticlePayload {
    AssetKey system; ///< WHAT runs here, never how (§18).

    template <class V>
    void reflect(V& v) {
        v.field("system", system);
    }
};

struct RibbonPayload {
    AssetKey system;

    template <class V>
    void reflect(V& v) {
        v.field("system", system);
    }
};

struct EventPayload {
    u32 id = 0;

    template <class V>
    void reflect(V& v) {
        v.field("id", id);
    }
};

enum class CollisionShapeKind : u8 { Box, Sphere, Capsule, Plane, Cylinder, Hull };

struct CollisionShapeDesc {
    CollisionShapeKind kind = CollisionShapeKind::Box;
    Extent box;
    Sphere sphere;
    f32 height = 0; ///< Capsule / cylinder length along the shape's local axis.

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("box", box);
        v.field("sphere", sphere);
        v.field("height", height);
    }
};

struct CollisionPayload {
    CollisionShapeDesc shape;

    template <class V>
    void reflect(V& v) {
        v.field("shape", shape);
    }
};

using NodePayload =
    std::variant<HelperPayload, BonePayload, AttachmentPayload, LightPayload, CameraPayload,
                 ParticlePayload, RibbonPayload, EventPayload, CollisionPayload>;

/// The payload alternative @p kind requires. `kind` and `payload.index()` are
/// redundant on purpose — `kind` is the cheap discriminator the disk format and
/// the bindings key on, and "the payload alternative matches `kind`" is a
/// structural validation rule, the same shape as the native-block/profile check.
constexpr std::size_t PayloadIndexFor(NodeKind kind) {
    return static_cast<std::size_t>(kind);
}

// ============================================================================
// Node
// ============================================================================

/**
 * @brief Format-scoped leftovers: MDX's `nodeFamilyId` and per-kind flag
 *        aliases, M2's attachment id, D3's per-bone collision shapes and
 *        constraints.
 *
 * The shared name/value bag (`native_bag.h`) rather than a struct of its own:
 * a node, a section and the model header all want the same thing, and none of
 * the three has a generated block yet.
 */
using NodeNative = NativeBag;

struct Node {
    std::string name;
    u32 parent = kInvalidNode; ///< Index into `NodeTree::nodes`; invalid = a root.
    NodeKind kind = NodeKind::Helper;
    NodeFlags flags = NodeFlags::None;

    /// MDX `PIVT`; zero for the other three formats — a D3 `PRSTransform` is
    /// complete with no separate pivot.
    Vector3f pivot{0, 0, 0};

    Transform local; ///< Bind-time local TRS.

    /// D3 stores ONE scale float per bone; export asserts on this rather than
    /// silently writing the x component of a non-uniform scale.
    bool uniformScaleOnly = false;

    /// Values for `NodeTree::poseSchema` — sized to it for Bone nodes, empty for
    /// kinds that carry none (§10.5).
    std::vector<Transform> poses;

    /// Matrix values for the `PoseStorage::Matrix` entries of the same schema.
    ///
    /// Empty, or exactly as long as `poses`. Only the entries whose schema says
    /// `Matrix` are authoritative; the rest are identity and must not be read,
    /// because the TRS in `poses` is the value there. For a `Matrix` entry the
    /// reverse holds: `poses[i]` is the decomposition, kept so a TRS-only
    /// consumer still gets a usable bind pose, and never read back on export.
    ///
    /// M3's `IREF` is the reason this exists — see `PoseStorage`.
    ///
    /// @bind skip — the generator flattens `Vector2f`/`Vector3f`/`Vector4f`/
    /// `Quaternion` into typed arrays and knows no such shape for a `Matrix44f`,
    /// so a `std::vector<Matrix44f>` has no element binding to name. Saying so
    /// here is the difference between a deliberate omission and a silent one.
    std::vector<Matrix44f> poseMatrices;

    NodePayload payload;
    NodeNative native;

    /**
     * @brief Transient removal marker (§10.6).
     *
     * `RemoveNode` marks; `CompactNodes` collects. It lives on the node rather
     * than in a parallel status array for the same reason the poses do: removing
     * or copying a node has to move everything that is about it. Never
     * serialized — a written document is always compacted first.
     */
    bool removed = false;

    /// True when `payload` holds the alternative `kind` requires.
    bool payloadMatchesKind() const {
        return payload.index() == PayloadIndexFor(kind);
    }

    /// Replaces `payload` with the default alternative for `kind`.
    void resetPayloadForKind();

    /**
     * @brief The node's serialized form. `removed` is deliberately not in it.
     *
     * A removal marker is transient state between `RemoveNode` and
     * `CompactNodes`, and a document is compacted before it is written;
     * carrying the flag to disk would let a reader load a tree that is mid-edit
     * and has no consistent meaning.
     */
    template <class V>
    void reflect(V& v) {
        v.field("name", name);
        v.field("parent", parent);
        v.field("kind", kind);
        v.field("flags", flags);
        v.field("pivot", pivot);
        v.field("local", local);
        v.field("uniformScaleOnly", uniformScaleOnly);
        v.field("poses", poses);
        // v2: a `NODE` written before matrix poses existed has none, and
        // `poseMatrices` being empty is exactly what "every pose is a TRS"
        // means — so an old chunk reads correctly by reading nothing.
        v.since(2).field("poseMatrices", poseMatrices);

        // `kind` and the payload alternative are redundant by design, and the
        // payload rides its own discriminator rather than `kind` so a file whose
        // two disagree still reads -- and then says so through
        // `payloadMatchesKind`, which is a validation rule and not a parse error.
        const NodeKind payloadKind = VariantKind<NodeKind>(v, "payloadKind", payload);
        switch (payloadKind) {
        case NodeKind::Helper:
            v.field("helper", VariantAs<HelperPayload>(payload));
            break;
        case NodeKind::Bone:
            v.field("bone", VariantAs<BonePayload>(payload));
            break;
        case NodeKind::Attachment:
            v.field("attachment", VariantAs<AttachmentPayload>(payload));
            break;
        case NodeKind::Light:
            v.field("light", VariantAs<LightPayload>(payload));
            break;
        case NodeKind::Camera:
            v.field("camera", VariantAs<CameraPayload>(payload));
            break;
        case NodeKind::ParticleEmitter:
            v.field("particle", VariantAs<ParticlePayload>(payload));
            break;
        case NodeKind::RibbonEmitter:
            v.field("ribbon", VariantAs<RibbonPayload>(payload));
            break;
        case NodeKind::Event:
            v.field("event", VariantAs<EventPayload>(payload));
            break;
        case NodeKind::CollisionShape:
            v.field("collision", VariantAs<CollisionPayload>(payload));
            break;
        case NodeKind::Count:
            break;
        }

        v.field("native", native);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
