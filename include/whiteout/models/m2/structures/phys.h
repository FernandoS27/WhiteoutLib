
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

/// @brief One rigid body, bound to a single model bone — BODY/BDY2/BDY3/BDY4.
///
/// The four on-disk layouts are the same fields accreting over time, so they
/// share one struct; PhysicsData::version decides which of them is written
/// back, and fields the older layouts lack keep their defaults.
struct PhysicsBody {
    PhysicsBodyType type = PhysicsBodyType::Kinematic;
    u16 boneIndex = 0;
    /// Offset from the bone's animated position, not an absolute position: the
    /// client spawns the body at `bonePosition + position`.
    Vector3f position;
    /// First entry in PhysicsData::shapes belonging to this body. 32 bits wide
    /// in BODY/BDY2, 16 from BDY3 on — writing a larger index back into one of
    /// those truncates it.
    i32 shapeIndex = 0;
    i32 shapeCount = 0;
    /// BDY3+. 1.0 on all but 45 of 1213 kinematic bodies but tuned freely on
    /// dynamic ones, negatives included — the shape of `dmBodyDef::m_gravityScale`.
    f32 gravityScale = 1.0f;
    /// BDY2+. 1.0 in 3457 of 3526 bodies, otherwise 1.1-10 —
    /// `dmBodyDef::m_inertiaScale`.
    f32 inertiaScale = 1.0f;
    /// BDY3+. Zero on 1196 of 1213 kinematic bodies and 0-10 on dynamic ones —
    /// `dmBodyDef::m_linearDamping`.
    f32 linearDamping = 0.0f;
    /// BDY3+. Same kinematic/dynamic split as @ref linearDamping —
    /// `dmBodyDef::m_angularDamping`.
    f32 angularDamping = 0.0f;
    /// BDY3+. Unidentified. Unlike the four above it is set on kinematic and
    /// dynamic bodies alike, so it is not a rigid-body integration parameter;
    /// values cluster on 0.5, 0.01, 0.9 and 0.1.
    f32 unknown28 = 0.89999998f;
    /// BDY4+. Unidentified; 0 in half the corpus, otherwise small values or
    /// 0x8000 alone, which reads like a bit field.
    u16 unknown2c = 0;
    u16 padding2e = 0; ///< BDY4+. Zero in every corpus body.
};

/// @brief One collision shape reference — SHAP/SHP2. Points at an entry of the
///        box/capsule/sphere/polytope array named by @ref shapeType.
struct PhysicsShape {
    PhysicsShapeType shapeType = PhysicsShapeType::Box;
    i16 shapeIndex = 0;
    u32 padding04 = 0; ///< Zero in every corpus shape.
    f32 friction = 0.0f;
    f32 restitution = 0.0f;
    f32 density = 0.0f;
    /// SHP2+. Unidentified, but a float: only 0, 0.01, 0.8 and 1.0 occur. The
    /// one `dmFixtureDef` float the rest of this struct does not account for is
    /// `m_rollingResistance`.
    f32 unknown14 = 0.0f;
    /// SHP2+. 1.0 in 3229 of 3230 shapes, matching the `m_scaleOrRadius` the
    /// client hands every fixture.
    f32 scale = 1.0f;
    u16 unknown1c = 0;  ///< SHP2+. Zero in every corpus shape.
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
    f32 unknown70 = 0.0f;          ///< WLJ3+. Zero in 265 of 274 weld joints.
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
    f32 lowerTwistAngle = 0.0f;
    f32 upperTwistAngle = 0.0f;
    /// Degrees: the corpus holds 20, 35, 45 and 60, while `dmShoulderJoint`
    /// clamps its own cone to [10°, 170°] expressed in radians — so the loader
    /// converts on the way in.
    f32 coneAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;    ///< version 2+
    u32 motorMode = 0;            ///< version 2+
    f32 motorFrequencyHz = 0.0f;  ///< SHJ2
    f32 motorDampingRatio = 0.0f; ///< SHJ2
};

/// @brief PRSJ/PRS2 — a sliding joint, version 2+.
struct PrismaticJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    f32 lowerLimit = 0.0f;
    f32 upperLimit = 0.0f;
    /// Unidentified; zero in all twelve corpus prismatic joints. Domino's
    /// prismatic def carries an enable-limit flag next to the limit pair.
    f32 unknown68 = 0.0f;
    f32 maxMotorForce = 0.0f;
    f32 unknown70 = 0.0f; ///< Unidentified; zero in all twelve.
    u32 motorMode = 0;
    f32 motorFrequencyHz = 0.0f;  ///< PRS2
    f32 motorDampingRatio = 0.0f; ///< PRS2
};

/// @brief REVJ/REV2 — a hinge, version 2+.
struct RevoluteJoint {
    PhysicsFrame frameA;
    PhysicsFrame frameB;
    f32 lowerAngle = 0.0f;
    f32 upperAngle = 0.0f;
    f32 maxMotorTorque = 0.0f;
    /// 1: position mode (frequency > 0), 2: velocity mode.
    u32 motorMode = 0;
    f32 motorFrequencyHz = 0.0f;  ///< REV2
    f32 motorDampingRatio = 0.0f; ///< REV2
};

/// @brief DSTJ — holds two anchors a fixed distance apart, version 2+.
struct DistanceJoint {
    Vector3f localAnchorA;
    Vector3f localAnchorB;
    f32 distance = 0.0f;
};

/// @brief PHYV — six floats that overwrite the head of a tuning block the
///        client otherwise fills with constants. Version 1+.
struct PhysicsTuning {
    std::array<f32, 6> values = {};
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
    /// PHYT, version 1+. A small enum, 0 through 4; meaning unknown.
    std::optional<u32> phyt;

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
