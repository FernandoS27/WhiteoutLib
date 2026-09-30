
#pragma once

#include <array>
#include <vector>
#include "../../../compatibility.h"
#include "../types.h"

namespace whiteout {
namespace m2 {

/// @brief The affine frame the Domino chunks store: three basis columns and an
///        origin, twelve floats in all.
///
/// The client reassembles it as `dmMtx{axisX, axisY, axisZ}` -> `dmQuatFromMtx`
/// plus `origin` as the translation, giving a `dmTransform`.
/// @bind value_object
struct PhysicsFrame {
    Vector3f axisX;
    Vector3f axisY;
    Vector3f axisZ;
    Vector3f origin;
};

/// @brief How the client drives a body — the value stored in BODY is inverted
///        relative to Domino's own `dmBodyType`.
enum class PhysicsBodyType : u16 {
    /// Animation-driven collider. Becomes `dmBodyType` 1; the client keeps it
    /// glued to its bone and the simulation only reads it.
    Kinematic = 0,
    /// Simulated. Becomes `dmBodyType` 0 and gets its bone transform written
    /// back every frame. These are the cloth/tassel segments.
    Dynamic = 1,
    /// Any value from 2 up: `dmBodyType` 2. No shipped body uses it.
    Static = 2,
};

/// @brief Which shape chunk a PhysicsShape indexes into.
enum class PhysicsShapeType : u16 {
    Box = 0,      ///< BOXS
    Capsule = 1,  ///< CAPS
    Sphere = 2,   ///< SPHS
    Polytope = 3, ///< PLYT, version 3+
};

/// @brief Which joint chunk a PhysicsJoint indexes into.
enum class PhysicsJointType : u16 {
    Spherical = 0, ///< SPHJ
    Shoulder = 1,  ///< SHOJ / SHJ2
    Weld = 2,      ///< WELJ / WLJ2 / WLJ3
    Revolute = 3,  ///< REVJ / REV2, version 2+
    Prismatic = 4, ///< PRSJ / PRS2, version 2+
    Distance = 5,  ///< DSTJ, version 2+
};

/// @name PhysicsBody::attachment
/// @{
/// Other bodies hang off this one: the dynamic bodies naming it are carried
/// with it when it is snapped to its bone.
constexpr u16 kPhysicsAttachmentHasChildren = 0x8000;
/// The ragdoll's root: its snap carries every body grouped behind a body that
/// is not a parent itself.
constexpr u16 kPhysicsAttachmentRagdollRoot = 0x4000;
/// Without either flag, the index of the body this one hangs off.
constexpr u16 kPhysicsAttachmentParentMask = 0x3FFF;
/// @}

/// @brief One rigid body, bound to a single model bone — BODY/BDY2/BDY3/BDY4.
///
/// The four on-disk layouts are the same fields accreting over time, so they
/// share one struct; PhysicsData::version decides which of them is written
/// back, and fields the older layouts lack keep the values the client's
/// upgrader gives them (`PHYS_FORMAT.md` §5).
struct PhysicsBody {
    PhysicsBodyType type = PhysicsBodyType::Kinematic;
    /// BODY/BDY2 store it as a u32 at +16 and the client keeps the low 16 bits;
    /// BDY3 moved it into the u16 at +2.
    u16 boneIndex = 0;
    /// The body's model-space origin, where the client creates it; the first
    /// step moves it onto its bone's animated pivot.
    Vector3f position;
    /// First entry in PhysicsData::shapes belonging to this body. 32 bits wide
    /// in every layout.
    i32 shapeIndex = 0;
    i32 shapeCount = 0;
    /// BDY2+. 1.0 on all but 45 of 1213 kinematic bodies but tuned freely on
    /// dynamic ones, negatives included — `dmBodyDef+0x30`.
    f32 gravityScale = 1.0f;
    /// BDY3+. 1.0 in 3457 of 3526 bodies, otherwise 1.1-10 — `dmBodyDef+0x2C`.
    f32 inertiaScale = 1.0f;
    /// BDY3+. Zero on 1196 of 1213 kinematic bodies and 0-10 on dynamic ones —
    /// `dmBodyDef+0x24`.
    f32 linearDamping = 0.0f;
    /// BDY3+. Same kinematic/dynamic split as @ref linearDamping —
    /// `dmBodyDef+0x28`.
    f32 angularDamping = 0.0f;
    /// BDY4. The fraction of the way a kinematic body is snapped to its animated
    /// pose each step, ramping to a full teleport when the motion is fast. Not a
    /// Domino parameter. Older layouts get the upgrader's 0.9.
    f32 followFactor = 0.9f;
    /// BDY3+ (+40 in BDY3, +44 in BDY4): see the `kPhysicsAttachment*` constants.
    u16 attachment = 0;
    u16 padding = 0; ///< BDY3+. Zero in every corpus body.
};

/// @brief One collision shape reference — SHAP/SHP2. Points at an entry of the
///        box/capsule/sphere/polytope array named by @ref shapeType.
struct PhysicsShape {
    PhysicsShapeType shapeType = PhysicsShapeType::Box;
    i16 shapeIndex = 0;
    /// `dmFixtureDef.gameFlags`. Zero in every corpus shape.
    u16 gameFlags = 0;
    u16 padding06 = 0;
    f32 friction = 0.0f;
    f32 restitution = 0.0f;
    /// Rescaled by the client for capsules in files of version 4 and below
    /// (`PHYS_FORMAT.md` §4.2).
    f32 density = 0.0f;
    /// @name SHP2+, parsed and never read
    /// The client copies these onto its shape def and no `CreateInstance`
    /// reads them (`PHYS_FORMAT.md` §4.5). SHAP's upgrade gives 0, 1.0 and 0.
    /// @{
    f32 unused14 = 0.0f;
    /// 1.0 in 3229 of 3230 shapes, and still not the fixture scale.
    f32 unused18 = 1.0f;
    u16 unused1c = 0;
    /// @}
    u16 padding1e = 0;  ///< SHP2+. Uninitialised on disk; kept so writes match.
};

/// @brief BOXS — an oriented box. The client turns it straight into a polytope
///        via `CPhysicsBoxShapeDef::SetPolytopeData(frame, halfExtents)`.
struct BoxShape {
    PhysicsFrame frame;
    Vector3f halfExtents;
};

/// @brief CAPS — a capsule between two local points.
struct CapsuleShape {
    Vector3f localPosition1;
    Vector3f localPosition2;
    f32 radius = 0.0f;
};

/// @brief SPHS — a sphere at a local point.
struct SphereShape {
    Vector3f localPosition;
    f32 radius = 0.0f;
};

/// @brief One half-edge of a polytope — Domino's `dmSubEdge`, four bytes.
///
/// Half-edges are stored in twin pairs at adjacent indices, and the ones
/// bounding a face form a cycle through @ref nextEdge.
struct PolytopeHalfEdge {
    /// Signed step to the paired half-edge: the twin of edge `i` is
    /// `i + twinOffset`. Only +1 and -1 occur, in equal numbers.
    i8 twinOffset = 0;
    /// Where this half-edge starts, indexing PolytopeShape::vertices.
    u8 originVertex = 0;
    /// The face this half-edge bounds, indexing PolytopeShape::facePlanes.
    u8 faceIndex = 0;
    /// Next half-edge around @ref faceIndex.
    u8 nextEdge = 0;
};

/// @brief PLYT — a convex hull, version 3+. Domino's `dmPolytope`.
///
/// The chunk stores fixed-size headers and variable-size payloads in two
/// blocks; both halves are folded into this one struct, and the header's
/// counts are recomputed from the vectors on write. The header's four pointer
/// fields are filled in by the client at load time and are zero in every file,
/// so they are not kept.
struct PolytopeShape {
    /// Hull corners.
    std::vector<Vector3f> vertices;
    /// Outward plane of each face, `xyz` normal and `w` offset.
    std::vector<Vector4f> facePlanes;
    /// One entry per face: any half-edge bounding it, as the entry point for
    /// walking the face through PolytopeHalfEdge::nextEdge.
    std::vector<u8> faceFirstEdges;
    std::vector<PolytopeHalfEdge> edges;

    Vector3f centroid;       ///< Volume-weighted, not the vertex average.
    f32 volume = 0.0f;       ///< Hull volume.
    f32 surfaceArea = 0.0f;  ///< Hull surface area.

    /// The four-byte gaps each count leaves in front of its 64-bit pointer, and
    /// the one that trails the header. Uninitialised in the files — some carry
    /// fragments of unrelated strings — so they are kept verbatim for writing.
    u32 padding04 = 0;
    u32 padding14 = 0;
    u32 padding2c = 0;
    u32 padding4c = 0;
};

/// @brief JOIN — connects two bodies with the joint named by @ref jointType.
struct PhysicsJoint {
    u32 bodyAIndex = 0;
    u32 bodyBIndex = 0;
    u32 padding08 = 0; ///< Zero in every corpus joint.
    PhysicsJointType jointType = PhysicsJointType::Spherical;
    /// Entry index within the joint array @ref jointType selects.
    i16 jointId = 0;
};

/// @brief WELJ/WLJ2/WLJ3 — a soft rigid connection. Zero frequency means the
///        axis is solved as a hard constraint.
struct WeldJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    f32 angularFrequencyHz = 0.0f;
    f32 angularDampingRatio = 0.0f;
    f32 linearFrequencyHz = 0.0f;  ///< WLJ2+
    f32 linearDampingRatio = 0.0f; ///< WLJ2+
    /// WLJ3+. Copied onto the weld def and never sent to Domino. Zero in 265 of
    /// 274 weld joints.
    f32 unused70 = 0.0f;
};

/// @brief SPHJ — a ball joint between two anchor points.
struct SphericalJoint {
    Vector3f anchorA;
    Vector3f anchorB;
    f32 frictionTorque = 0.0f;
};

/// @brief SHOJ/SHJ2 — a twist-and-cone joint, the one that chains cloth.
struct ShoulderJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    /// Degrees, like the cone; the client converts both to radians and enables
    /// the twist limit when `upper > lower`.
    f32 lowerTwistAngle = 0.0f;
    f32 upperTwistAngle = 0.0f;
    /// Degrees: the corpus holds 20, 35, 45 and 60. Stored as authored, so the
    /// conversion is the consumer's.
    f32 coneAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    u32 motorMode = 0;            ///< low byte: 0 off, 1 position, 2 velocity
    /// SHJ2. A SHOJ gets the upgrader's 1.0 Hz and 0.7 (`PHYS_FORMAT.md` §5).
    f32 motorFrequencyHz = 1.0f;
    f32 motorDampingRatio = 0.7f; ///< SHJ2
};

/// @brief PRSJ/PRS2 — a sliding joint, version 2+.
struct PrismaticJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    /// Distances, not angles; the limit is enabled when `upper > lower`.
    f32 lowerLimit = 0.0f;
    f32 upperLimit = 0.0f;
    /// The zero point the limit is measured from. No `dmJointDef` slot: the
    /// client writes it into the live joint after creation. Zero in all twelve
    /// corpus prismatics.
    f32 referenceTranslation = 0.0f;
    f32 maxMotorForce = 0.0f;
    /// Target velocity, written into the live joint like @ref referenceTranslation.
    f32 motorSpeed = 0.0f;
    u32 motorMode = 0;
    f32 motorFrequencyHz = 1.0f;  ///< PRS2; the upgrader's value for a PRSJ
    f32 motorDampingRatio = 0.7f; ///< PRS2
};

/// @brief REVJ/REV2 — a hinge, version 2+.
struct RevoluteJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    /// Degrees; the limit is enabled when `upper > lower`.
    f32 lowerAngle = 0.0f;
    f32 upperAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    /// 1: position mode (frequency > 0), 2: velocity mode.
    u32 motorMode = 0;
    f32 motorFrequencyHz = 1.0f;  ///< REV2; the upgrader's value for a REVJ
    f32 motorDampingRatio = 0.7f; ///< REV2
};

/// @brief DSTJ — holds two anchors a fixed distance apart, version 2+.
struct DistanceJoint {
    Vector3f localAnchorA;
    Vector3f localAnchorB;
    f32 distance = 0.0f;
};

/// @brief PHYV — the per-model vegetation push: the six `physVeg*` console
///        variables in registration order (`PHYS_FORMAT.md` §3.9).
///
/// Read only for a `PHYT` 2 model, which becomes a phantom pushed by units
/// walking through it rather than a ragdoll.
struct PhysicsTuning {
    /// Yards a bone may be pushed from its base before it is clamped.
    f32 posMaxPush = 1.25f;
    /// The fraction of its target a bone is pushed each frame while a unit moves
    /// along it; 12.1 does not scale it by dt.
    f32 posPushAmt = 0.25f;
    /// How fast the bone returns to rest once the unit leaves.
    f32 posRelaxSpeed = 8.0f;
    /// Extra push along a moving unit's velocity.
    f32 velMaxPush = 0.1f;
    /// How fast the bone sways along that velocity.
    f32 velSpeed = 20.0f;
    /// **Squared** distance inside which a unit starts pushing. The client uses
    /// 8.0 for a model with no PHYV.
    f32 minPushDist = 4.0f;
};

/// @brief What a model's `PHYT` makes of it (`PHYS_FORMAT.md` §3.10).
///
/// A file with no PHYT reads as 0.
enum class PhysicsObjectKind : u32 {
    /// Ragdoll whose kinematic bodies follow the model, not their bones: items
    /// worn on a character. 0 and 1 take the same branch in the client.
    AttachedRagdoll = 0,
    AttachedRagdollAlt = 1,
    /// Vegetation phantom built from the model's bounds and pushed by units.
    /// Needs a PHYV, and builds no bodies.
    VegetationPhantom = 2,
    /// Ragdoll in the shared physics world.
    Ragdoll = 3,
    /// Ragdoll in a physics world of its own, solved with twelve position
    /// iterations rather than two.
    PrivateWorldRagdoll = 4,
};

/// @brief PHAO — the host skeletons this file's follow factors were tuned on.
///
/// On a host whose key-bone-4 name CRC is not listed, the client discards every
/// body's @ref PhysicsBody::followFactor for a flat 0.7. No shipped file has one.
struct PhysicsAllowList {
    /// The chunk's leading u32. The client keeps its low byte and never reads it.
    u32 header = 0;
    std::vector<u32> keys;
};

/// @brief A `.phys` chunk this library does not know, kept verbatim so a
///        parse/write cycle does not drop it.
struct PhysicsUnknownChunk {
    /// FourCC as written on disk — `.phys` stores chunk ids reversed, so a
    /// PHYS chunk reads as "SYHP".
    std::array<char, 4> tag = {};
    std::vector<u8> data;
};

/// @brief A whole `.phys` file — Blizzard's Domino rigid-body setup for a
///        model, either a `.phys` sibling or an M2's inline PFDC chunk.
///
/// Bodies attach to bones and are linked by joints; each body owns a run of
/// @ref shapes, and each shape indexes the array its type names.
struct PhysicsData {
    /// 0 (MoP) through 6. Decides which layout each chunk is written in — see
    /// the per-field version notes on the structs above.
    u16 version = 6;
    /// PHYT, version 1+: a PhysicsObjectKind, kept raw so any value round-trips.
    std::optional<u32> phyt;
    /// PHAO. Absent in every shipped file.
    std::optional<PhysicsAllowList> allowList;

    std::vector<PhysicsBody> bodies;
    std::vector<PhysicsShape> shapes;

    std::vector<BoxShape> boxShapes;
    std::vector<CapsuleShape> capsuleShapes;
    std::vector<SphereShape> sphereShapes;
    std::vector<PolytopeShape> polytopeShapes;

    std::vector<PhysicsJoint> joints;
    std::vector<WeldJoint> weldJoints;
    std::vector<SphericalJoint> sphericalJoints;
    std::vector<ShoulderJoint> shoulderJoints;
    std::vector<PrismaticJoint> prismaticJoints;
    std::vector<RevoluteJoint> revoluteJoints;
    std::vector<DistanceJoint> distanceJoints;

    /// PHYV. A file that has one carries nothing else.
    std::vector<PhysicsTuning> tuning;

    /// @bind skip — round-trip ballast for chunks this library does not model.
    std::vector<PhysicsUnknownChunk> unknownChunks;
};

} // namespace m2
} // namespace whiteout
