// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/rigging/tpose.h"

#include "whiteout/models/wem/rigging/ik.h"
#include "whiteout/models/wem/skinning/mirror.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;

/// Every figure faces +X in the Blizzard space: the side a knee bends to.
const Vector3f kForward{1, 0, 0};

/// Below this a key's own axis is noise, not evidence: a bone that has barely
/// moved points its axis anywhere. Three degrees is the floor §3.4 measured at.
constexpr f64 kFloorRad = 3.0 * 3.14159265358979323846 / 180.0;

/// Where @p node stands at rest — its pivot in a pivot rig, its composed bind
/// in an explicit one.
Vector3f RestPoint(const NodeTree& tree, u32 node) {
    if (node >= tree.size()) {
        return {0, 0, 0};
    }
    return tree.rig == RigConvention::PivotRelative ? tree.nodes[node].pivot
                                                    : tree.worldBind(node).translation;
}

/// The bend record on @p node, or null. `Twist`, `Spin` and `Slide` are not
/// hinges and do not answer here.
const Joint* BendJointOf(const NodeTree& tree, u32 node) {
    if (node >= tree.size()) {
        return nullptr;
    }
    const std::optional<Joint>& joint = tree.nodes[node].skin.joint;
    return joint.has_value() && joint->motion == JointMotion::Bend ? &*joint : nullptr;
}

/// The joint's axis as its record states it. A node's frame has turned by
/// nothing at rest, so the model's space and the parent's agree there and one
/// vector serves both — which is why `Joint::parentSpace` does not appear.
Vector3f JointAxisAt(const NodeTree& tree, u32 node, const Joint& joint) {
    switch (joint.axis) {
    case JointAxis::X:
        return {1, 0, 0};
    case JointAxis::Y:
        return {0, 1, 0};
    case JointAxis::Z:
        return {0, 0, 1};
    case JointAxis::Along: {
        const NodeRange children = tree.children(node);
        if (children.empty()) {
            return {0, 0, 0};
        }
        return RestPoint(tree, children[0]) - RestPoint(tree, node);
    }
    }
    return {0, 0, 0};
}

/// A hinge and its negation are the same hinge, so one of the two is the
/// answer: the one whose largest component is positive.
Vector3f CanonicalSign(const Vector3f& axis) {
    u32 largest = 0;
    f32 best = std::fabs(axis.x);
    if (std::fabs(axis.y) > best) {
        largest = 1;
        best = std::fabs(axis.y);
    }
    if (std::fabs(axis.z) > best) {
        largest = 2;
    }
    const f32 at = largest == 0 ? axis.x : (largest == 1 ? axis.y : axis.z);
    return at < 0.0f ? axis * -1.0f : axis;
}

/// The eigenvalues and eigenvectors of a symmetric 3x3, by cyclic Jacobi. Small
/// and exact enough that the scatter's top direction is not a tolerance.
struct Eigen {
    f64 value[3] = {0, 0, 0};
    f64 vector[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}; ///< `vector[i]` for `value[i]`.
};

Eigen Symmetric3(f64 m[3][3]) {
    f64 a[3][3];
    std::memcpy(a, m, sizeof(a));
    f64 v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (u32 sweep = 0; sweep < 32; ++sweep) {
        const f64 off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-18) {
            break;
        }
        for (u32 p = 0; p < 2; ++p) {
            for (u32 q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-20) {
                    continue;
                }
                const f64 theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const f64 t = (theta >= 0 ? 1.0 : -1.0) /
                              (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const f64 c = 1.0 / std::sqrt(t * t + 1.0);
                const f64 s = t * c;
                for (u32 k = 0; k < 3; ++k) {
                    const f64 akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (u32 k = 0; k < 3; ++k) {
                    const f64 apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (u32 k = 0; k < 3; ++k) {
                    const f64 vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    Eigen out;
    u32 order[3] = {0, 1, 2};
    // Three values: one pass of selection sort, largest first.
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = i + 1; j < 3; ++j) {
            if (a[order[j]][order[j]] > a[order[i]][order[i]]) {
                std::swap(order[i], order[j]);
            }
        }
    }
    for (u32 i = 0; i < 3; ++i) {
        const u32 from = order[i];
        out.value[i] = a[from][from];
        for (u32 k = 0; k < 3; ++k) {
            out.vector[i][k] = v[k][from];
        }
    }
    return out;
}

} // namespace

const char* ToString(HingeSource source) {
    switch (source) {
    case HingeSource::None:
        return "None";
    case HingeSource::Joint:
        return "Joint";
    case HingeSource::Clips:
        return "Clips";
    case HingeSource::Fallback:
        return "Fallback";
    }
    return "None";
}

HingeFit HingeFromClips(const Document& document, u32 model, u32 node) {
    HingeFit fit;
    if (model >= document.models.size()) {
        return fit;
    }
    const Model& owner = document.models[model];
    if (node >= owner.nodes.size()) {
        return fit;
    }

    // The channel ids that turn this node. A model declares one per (node,
    // channel) pair, but a `sub` splits same-named properties, so this is a
    // list rather than a lookup.
    std::vector<u32> turning;
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node &&
            channel.target.node == node &&
            channel.target.channel == Channel::Rotation &&
            channel.valueType == geom::AttrType::Quat) {
            turning.push_back(channel.id);
        }
    }
    if (turning.empty()) {
        return fit;
    }

    f64 scatter[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    f64 visited = 0;
    for (const Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                if (std::find(turning.begin(), turning.end(), track.channel) == turning.end()) {
                    continue;
                }
                // Hermite and Bezier keep `{value, inTan, outTan}` per key in
                // one stream: the stride steps over the tangents, which are
                // control points and not poses.
                const u32 stride = ValuesPerKey(track.interp);
                const std::size_t keys =
                    std::min(track.times.size(), track.values.size() / (stride * 4 * sizeof(f32)));
                for (std::size_t k = 0; k < keys; ++k) {
                    f32 q[4];
                    std::memcpy(q, track.values.data() + k * stride * 4 * sizeof(f32), sizeof(q));
                    f64 length =
                        std::sqrt(f64(q[0]) * q[0] + f64(q[1]) * q[1] + f64(q[2]) * q[2] +
                                  f64(q[3]) * q[3]);
                    if (length < 1e-9) {
                        continue;
                    }
                    f64 x = q[0] / length, y = q[1] / length, z = q[2] / length, w = q[3] / length;
                    // The shortest arc: a quaternion and its negation are one
                    // turn, and only one of the two has a non-negative angle.
                    if (w < 0) {
                        x = -x;
                        y = -y;
                        z = -z;
                        w = -w;
                    }
                    const f64 sine = std::sqrt(std::max(0.0, 1.0 - w * w));
                    const f64 angle = 2.0 * std::acos(std::min(1.0, w));
                    if (sine < 1e-9 || angle < kFloorRad) {
                        continue;
                    }
                    ++fit.keys;
                    visited = std::max(visited, angle);
                    // The principal direction of the key axes SCALED by their
                    // angles: the scatter of `angle * axis`, so the weight is
                    // the square. A hundred small turns must not outvote the one
                    // frame that bends the elbow.
                    const f64 axis[3] = {angle * x / sine, angle * y / sine, angle * z / sine};
                    for (u32 r = 0; r < 3; ++r) {
                        for (u32 c = 0; c < 3; ++c) {
                            scatter[r][c] += axis[r] * axis[c];
                        }
                    }
                }
            }
        }
    }

    const f64 trace = scatter[0][0] + scatter[1][1] + scatter[2][2];
    if (trace < 1e-12 || fit.keys < kHingeKeys) {
        // Fewer than six turning keys is not a measurement (§3.4): the fit is
        // reported with no source, so the ladder falls through to the plane.
        return fit;
    }
    const Eigen eigen = Symmetric3(scatter);
    fit.axis = CanonicalSign(Vector3f{static_cast<f32>(eigen.vector[0][0]),
                                      static_cast<f32>(eigen.vector[0][1]),
                                      static_cast<f32>(eigen.vector[0][2])});
    const f32 length = fit.axis.length();
    if (length < 1e-6f) {
        fit.axis = {0, 0, 0};
        return fit;
    }
    fit.axis = fit.axis * (1.0f / length);
    fit.share = static_cast<f32>(eigen.value[0] / trace);
    fit.visitedDeg = static_cast<f32>(visited * 180.0 / kPi);
    fit.source = HingeSource::Clips;
    return fit;
}

HingeFit HingeOf(const Document& document, u32 model, const Limb& limb) {
    HingeFit fit;
    if (model >= document.models.size()) {
        return fit;
    }
    const NodeTree& tree = document.models[model].nodes;
    if (limb.lower >= tree.size() || limb.upper >= tree.size() || limb.end >= tree.size()) {
        return fit;
    }

    // Rung 1: what the document says, whatever the clips do. A joint the user
    // set is an answer, not a measurement.
    if (const Joint* joint = BendJointOf(tree, limb.lower)) {
        const Vector3f axis = JointAxisAt(tree, limb.lower, *joint);
        if (axis.length_squared() > 1e-12f) {
            // Not canonicalised: a `Bend` record's `low` and `high` are signed
            // about the axis it states, so flipping it would invert the limits.
            fit.axis = axis.normalized();
            fit.source = HingeSource::Joint;
            return fit;
        }
    }

    // Rung 2: the clips, when they describe a hinge and not a ball.
    const HingeFit measured = HingeFromClips(document, model, limb.lower);
    if (measured.source == HingeSource::Clips && measured.share >= kHingeShare &&
        measured.keys >= kHingeKeys) {
        return measured;
    }

    // Rung 3: the plane the limb rests in. `FallbackHinge` takes a press and a
    // rest; at rest they are the same limb, which is the degenerate case it
    // already handles.
    LimbPress rest;
    rest.upper = RestPoint(tree, limb.upper);
    rest.lower = RestPoint(tree, limb.lower);
    rest.end = RestPoint(tree, limb.end);
    if (limb.hock != kInvalidNode) {
        rest.hock = RestPoint(tree, limb.hock);
    }
    const bool leg = tree.nodes[limb.end].rig.limb == RigLimb::Leg;
    const Vector3f axis = FallbackHinge(rest, rest, kForward, leg);
    if (axis.length_squared() <= 1e-12f) {
        // Nothing answered: the measurement, such as it is, is still worth
        // reporting — a limb with no length has no hinge and the panel says so.
        fit.keys = measured.keys;
        fit.share = measured.share;
        fit.visitedDeg = measured.visitedDeg;
        return fit;
    }
    fit.axis = axis.normalized();
    fit.keys = measured.keys;
    fit.share = measured.share;
    fit.visitedDeg = measured.visitedDeg;
    fit.source = HingeSource::Fallback;
    return fit;
}

// ============================================================================
// The solve (§3.2, §4)
// ============================================================================

namespace {

/// How far above a head the spine rule looks for joints to turn.
constexpr u32 kSpineChain = 8;

/// The goal goes PAST the limb's own reach, because soft reach eases toward
/// full extension and never arrives: asked for exactly `reach`, the ease lands
/// at `0.95 + 0.05*(1 - e^-1) = 0.982` of it, and on a two-bone limb that is
/// still 22 degrees of bend in the knee — 6.7 degrees of it visible in the
/// upper bone on the HD Footman. `0.95 + 0.05*(1 - e^-6.9)` is 0.99995, and 6.9
/// soft zones past the knee is 1.3 reaches. The End cannot stretch, so it still
/// lands at the limb's own length: the overshoot buys straightness and nothing
/// else.
constexpr f32 kStraighten = 1.3f;

/// A limb this far off its canonical direction did not straighten, and the row
/// says why rather than the number pretending it did.
constexpr f32 kAimTolerance = 1.0f;

/// Where a limb's bone aims in the canonical pose (§3.2): a leg straight down,
/// an arm out to its own side and `armDownDeg` of the way toward the floor.
Vector3f AimOf(RigLimb limb, RigSide side, const TPoseRules& rules) {
    if (limb == RigLimb::Leg) {
        return {0, 0, -1};
    }
    const f32 sign = side == RigSide::Right ? -1.0f : 1.0f;
    const f32 down = rules.armDownDeg * kPi / 180.0f;
    return Vector3f{0, sign * std::cos(down), -std::sin(down)};
}

f32 Gap(const Vector3f& a, const Vector3f& b) {
    return (a - b).length();
}

/// @p q as a matrix in the convention everything here composes in: row vectors,
/// which `ToMatrix` states once for the whole of WEM.
Matrix44f RowRotation(const Quaternion& q) {
    return ToMatrix(Transform{Vector3f{0, 0, 0}, q, Vector3f{1, 1, 1}});
}

/// @p q about @p origin, in the model's space.
Matrix44f TurnAbout(const Quaternion& q, const Vector3f& origin) {
    return Matrix44f::translation(origin * -1.0f) * RowRotation(q) * Matrix44f::translation(origin);
}

/// @p point after @p move, row vectors.
Vector3f Moved(const Vector3f& point, const Matrix44f& move) {
    return Vector3f{point.x * move.data[0][0] + point.y * move.data[1][0] +
                        point.z * move.data[2][0] + move.data[3][0],
                    point.x * move.data[0][1] + point.y * move.data[1][1] +
                        point.z * move.data[2][1] + move.data[3][1],
                    point.x * move.data[0][2] + point.y * move.data[1][2] +
                        point.z * move.data[2][2] + move.data[3][2]};
}

/// The shortest arc from @p from to @p to. The identity when either has no
/// length or they already agree; a half turn about a square axis when they
/// oppose, which is the one case the cross product cannot name.
Quaternion ArcBetween(const Vector3f& from, const Vector3f& to) {
    const f32 lengthFrom = from.length(), lengthTo = to.length();
    if (lengthFrom < 1e-9f || lengthTo < 1e-9f) {
        return Quaternion::identity();
    }
    const Vector3f a = from * (1.0f / lengthFrom), b = to * (1.0f / lengthTo);
    const f32 dot = std::clamp(a.dot(b), -1.0f, 1.0f);
    if (dot > 0.999999f) {
        return Quaternion::identity();
    }
    if (dot < -0.999999f) {
        const Vector3f any = std::fabs(a.x) < 0.9f ? Vector3f{1, 0, 0} : Vector3f{0, 1, 0};
        return Quaternion::from_axis_angle(::whiteout::cross(a, any).normalized(), kPi);
    }
    return Quaternion::from_axis_angle(::whiteout::cross(a, b).normalized(), std::acos(dot));
}

f32 DegreesOf(const Quaternion& q) {
    return 2.0f * std::acos(std::clamp(std::fabs(q.w), 0.0f, 1.0f)) * 180.0f / kPi;
}

/// @p q seen from the other side of the figure's plane (`MirrorAxis::Y`).
/// Reflecting a rotation keeps its angle and flips the two axis components the
/// plane does not hold.
Quaternion MirroredTurn(const Quaternion& q) {
    return Quaternion{-q.x, q.y, -q.z, q.w};
}

/// The bone @p node owns: its rest point to its first joint child's. Zero for a
/// leaf, which has no direction of its own to aim.
Vector3f BoneOf(const NodeTree& tree, u32 node, const std::vector<Matrix44f>& carried) {
    for (const u32 child : tree.children(node)) {
        if (IsJoint(tree, child)) {
            const Vector3f along = Moved(RestPoint(tree, child), carried[node]) -
                                   Moved(RestPoint(tree, node), carried[node]);
            if (along.length_squared() > 1e-12f) {
                return along;
            }
        }
    }
    return {0, 0, 0};
}

/// A model-space pose entry that is not the bind — `BPOS`, where the file had
/// one. `kInvalidIndex` when the tree carries only its own bind.
u32 RestOrientationPose(const NodeTree& tree) {
    for (u32 i = 0; i < tree.poseSchema.size(); ++i) {
        if (i != tree.authoritativePose && tree.poseSchema[i].space == PoseSpace::Model) {
            return i;
        }
    }
    return kInvalidIndex;
}

/// One limb's rest press, and the reach its goal is built from.
struct LimbRest {
    LimbPress press;
    f32 reach = 0;
    bool ok = false;
};

LimbRest RestOf(const NodeTree& tree, const Limb& limb, const std::vector<Matrix44f>& carried) {
    // Where the limb stands NOW, not where the file rests it: a rig that chains
    // three Uppers has the limb the record names starting part way down, and
    // the limb above it may already have been turned.
    const auto at = [&](u32 node) { return Moved(RestPoint(tree, node), carried[node]); };
    LimbRest out;
    out.press.upper = at(limb.upper);
    out.press.lower = at(limb.lower);
    out.press.end = at(limb.end);
    if (limb.hock != kInvalidNode) {
        out.press.hock = at(limb.hock);
    }
    // The chain's OWN reach (§4): a goal at a fixed distance would stretch a
    // short limb and leave a long one bent.
    const Vector3f middle = out.press.hock ? *out.press.hock : out.press.end;
    out.reach = Gap(out.press.lower, out.press.upper) + Gap(middle, out.press.lower);
    if (out.press.hock) {
        out.reach += Gap(out.press.end, *out.press.hock);
    }
    out.ok = Gap(out.press.lower, out.press.upper) > 1e-4f &&
             Gap(out.press.end, out.press.lower) > 1e-4f;
    return out;
}

/// A `Bend` record's limits, turned round with the hinge when its own axis
/// opposes it — the rule the Auto IK drag already applies.
void ApplyLimits(const NodeTree& tree, const Limb& limb, const HingeFit& hinge, LimbSetup& setup) {
    const Joint* joint = BendJointOf(tree, limb.lower);
    if (joint == nullptr) {
        return;
    }
    const Vector3f axis = JointAxisAt(tree, limb.lower, *joint);
    if (axis.length_squared() <= 1e-12f) {
        return;
    }
    setup.bendLimitsDeg = axis.normalized().dot(hinge.axis) < 0.0f
                              ? std::pair{-joint->high, -joint->low}
                              : std::pair{joint->low, joint->high};
}

/// Every node, parents before children. NOT index order: the MDX importer
/// numbers by `objectId`, which is per chunk, so a bone can sit before the
/// helper it hangs from (`jainasea.mdx` has a whole arm running backwards).
std::vector<u32> PreOrder(const NodeTree& tree) {
    std::vector<u32> order;
    order.reserve(tree.size());
    for (const u32 root : tree.roots()) {
        for (const u32 node : tree.subtree(root)) {
            order.push_back(node);
        }
    }
    // A cycle, or a parent index nothing reaches, would leave nodes out; they
    // go on the end rather than being dropped.
    if (order.size() < tree.size()) {
        std::vector<u8> seen(tree.size(), 0);
        for (const u32 node : order) {
            seen[node] = 1;
        }
        for (u32 n = 0; n < tree.size(); ++n) {
            if (!seen[n]) {
                order.push_back(n);
            }
        }
    }
    return order;
}

/// Per node, the model-space motion the turns ABOVE and ON it add up to. One
/// walk, shared by the solve (which needs each limb pressed where the limbs
/// above it have left it) and by @ref TPoseRest (which turns it into a rest).
std::vector<Matrix44f> Accumulate(const NodeTree& tree, const std::vector<u32>& order,
                                  const std::vector<TPoseJoint>& joints) {
    std::vector<const TPoseJoint*> turn(tree.size(), nullptr);
    for (const TPoseJoint& joint : joints) {
        if (joint.node < tree.size()) {
            turn[joint.node] = &joint;
        }
    }
    std::vector<Matrix44f> carried(tree.size(), Matrix44f::identity());
    for (const u32 n : order) {
        const u32 parent = ParentOf(tree, n);
        const Matrix44f above = parent == kInvalidNode ? Matrix44f::identity() : carried[parent];
        carried[n] = above;
        if (turn[n] != nullptr) {
            // About its rest point where the turns above have already left it:
            // the same place the solve measured its own turns about.
            carried[n] = above * TurnAbout(turn[n]->turn, Moved(RestPoint(tree, n), above));
        }
    }
    return carried;
}

/// A turn worth recording. A joint the solve left where it was is not a row:
/// the panel would list it and the re-bind would rewrite its keys for nothing.
///
/// A node two rules both turn gets ONE row carrying both, in the order they
/// asked. `Accumulate` reads a node's turn once, so a second row would silently
/// replace the first — which is what dropped the DE paladin's square when the
/// spine rule reached the same root.
void PushJoint(TPoseResult& result, u32 node, const Quaternion& turn, TPoseSource source) {
    if (node == kInvalidNode || std::fabs(turn.w) > 0.9999999f) {
        return;
    }
    for (TPoseJoint& already : result.joints) {
        if (already.node == node) {
            // `a * b` applies b first, so the row already there goes first. The
            // source stays the one that set it: the turn it named is still in
            // here, and a later degree of trim does not rename it.
            already.turn = (turn * already.turn).normalized();
            already.turnDeg = DegreesOf(already.turn);
            return;
        }
    }
    TPoseJoint joint;
    joint.node = node;
    joint.turn = turn.normalized();
    joint.turnDeg = DegreesOf(joint.turn);
    joint.source = source;
    result.joints.push_back(joint);
}

/// A rig this near square is square. Turning it would put a row in the panel
/// and rewrite every key in the model for a tenth of a degree.
constexpr f32 kSquareFloor = 0.5f;

/// Every joint that starts a limb of @p limb, by the side it is on.
struct Sides {
    std::vector<u32> left;
    std::vector<u32> right;

    bool both() const {
        return !left.empty() && !right.empty();
    }
};

Sides SidesOf(const NodeTree& tree, RigLimb limb) {
    Sides out;
    for (u32 n = 0; n < tree.size(); ++n) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (RoleOf(tree, n) != RigRole::Upper || rig.limb != limb) {
            continue;
        }
        if (rig.side == RigSide::Left) {
            out.left.push_back(n);
        } else if (rig.side == RigSide::Right) {
            out.right.push_back(n);
        }
    }
    return out;
}

/// The node every one of @p nodes hangs from. Invalid when they share nothing,
/// or when the answer is one of them: a figure's sides meet ABOVE both, and
/// turning a node that carries only one side would turn that side alone.
u32 MeetOf(const NodeTree& tree, const std::vector<u32>& nodes) {
    u32 meet = kInvalidNode;
    for (const u32 node : nodes) {
        if (meet == kInvalidNode) {
            meet = node;
            continue;
        }
        std::vector<u8> above(tree.size(), 0);
        for (u32 n = meet, step = tree.size(); n != kInvalidNode && step > 0;
             n = ParentOf(tree, n), --step) {
            above[n] = 1;
        }
        meet = kInvalidNode;
        for (u32 n = node, step = tree.size(); n != kInvalidNode && step > 0;
             n = ParentOf(tree, n), --step) {
            if (above[n]) {
                meet = n;
                break;
            }
        }
        if (meet == kInvalidNode) {
            return kInvalidNode;
        }
    }
    return std::find(nodes.begin(), nodes.end(), meet) == nodes.end() ? meet : kInvalidNode;
}

/// Whether @p above carries @p node.
bool Holds(const NodeTree& tree, u32 above, u32 node) {
    for (u32 n = ParentOf(tree, node), step = tree.size(); n != kInvalidNode && step > 0;
         n = ParentOf(tree, n), --step) {
        if (n == above) {
            return true;
        }
    }
    return false;
}

/// Where a set of joints sits on average, once the turns so far have moved it.
Vector3f MiddleOf(const NodeTree& tree, const std::vector<u32>& nodes,
                  const std::vector<Matrix44f>& carried) {
    Vector3f sum{0, 0, 0};
    for (const u32 n : nodes) {
        sum = sum + Moved(RestPoint(tree, n), carried[n]);
    }
    return sum * (1.0f / static_cast<f32>(nodes.size()));
}

/// Squares the figure to the space the aims are written in (the file header:
/// every figure faces +X). Warcraft III's Deluxe Edition rigs are bound yawed —
/// the DE footman's hips sit 25.7 degrees off +Y, its paladin's 55.9, its
/// grunt's 59.6 — while every Reforged HD rig is square to a hundredth of a
/// degree. Aiming a limb at a canonical direction in a frame the figure does
/// not stand in is what threw the DE paladin's arms across its chest.
///
/// Two turns, because a yawed rig is twisted as well: the node both legs hang
/// from takes the hips onto +Y, then the node both arms hang from takes the
/// shoulders back onto it. The first carries the shoulders WITH it — on the DE
/// footman from 6.7 degrees off to 19 — so the second is not optional.
///
/// Before the limbs, because every limb is aimed in this frame; a square rig
/// turns by nothing and pays for none of it.
void SquareFigure(const NodeTree& tree, const std::vector<u32>& order,
                  const std::vector<u8>& yours, TPoseResult& result,
                  std::vector<Matrix44f>& carried) {
    // About +Z alone: a figure that is off-square is turned on the floor, and
    // any other axis would tip it over.
    const auto squareAt = [&](u32 meet, const Sides& sides) {
        if (meet == kInvalidNode || yours[meet]) {
            return;
        }
        const Vector3f across =
            MiddleOf(tree, sides.left, carried) - MiddleOf(tree, sides.right, carried);
        const Vector3f flat{across.x, across.y, 0};
        if (flat.length() < 1e-4f) {
            return;
        }
        const Quaternion turn = ArcBetween(flat, Vector3f{0, 1, 0});
        if (DegreesOf(turn) < kSquareFloor) {
            return;
        }
        PushJoint(result, meet, turn, TPoseSource::Record);
        carried = Accumulate(tree, order, result.joints);
    };

    // Both sides at once, so a quadruped's four legs meet where all four hang
    // from rather than where one pair does.
    const auto meetOf = [&](const Sides& sides) {
        std::vector<u32> all = sides.left;
        all.insert(all.end(), sides.right.begin(), sides.right.end());
        return sides.both() ? MeetOf(tree, all) : kInvalidNode;
    };
    const Sides hips = SidesOf(tree, RigLimb::Leg);
    const Sides shoulders = SidesOf(tree, RigLimb::Arm);
    const u32 hipMeet = meetOf(hips);
    const u32 armMeet = meetOf(shoulders);
    if (hips.both()) {
        squareAt(hipMeet, hips);
    }
    // Untwisting only untwists BELOW the square: the same node again, or one
    // above it, would turn the hips a second time and undo them.
    if (shoulders.both() && (hipMeet == kInvalidNode || Holds(tree, hipMeet, armMeet))) {
        squareAt(armMeet, shoulders);
    }
}

} // namespace

const char* ToString(TPoseRefusal refusal) {
    switch (refusal) {
    case TPoseRefusal::None:
        return "None";
    case TPoseRefusal::NoChain:
        return "NoChain";
    case TPoseRefusal::ZeroLength:
        return "ZeroLength";
    case TPoseRefusal::NoHinge:
        return "NoHinge";
    case TPoseRefusal::JointLimits:
        return "JointLimits";
    case TPoseRefusal::Unreachable:
        return "Unreachable";
    case TPoseRefusal::Overlaps:
        return "Overlaps";
    }
    return "None";
}

const char* ToString(TPoseSource source) {
    switch (source) {
    case TPoseSource::None:
        return "None";
    case TPoseSource::Donor:
        return "Donor";
    case TPoseSource::Record:
        return "Record";
    case TPoseSource::Clips:
        return "Clips";
    case TPoseSource::BindFrame:
        return "BindFrame";
    case TPoseSource::You:
        return "You";
    }
    return "None";
}

TPoseResult SolveTPose(const Document& document, u32 model, const TPoseRules& rules,
                       const TPoseInputs& inputs) {
    TPoseResult result;
    if (model >= document.models.size()) {
        return result;
    }
    const Model& owner = document.models[model];
    const NodeTree& tree = owner.nodes;
    const bool haveBindFrames = RestOrientationPose(tree) != kInvalidIndex;
    const std::vector<u32> order = PreOrder(tree);
    std::vector<Matrix44f> carried(tree.size(), Matrix44f::identity());

    // What a person set by hand is never solved over (§3.1): those joints keep
    // the turn the saved pose holds, and the solve leaves their limb alone.
    std::vector<u8> yours(tree.size(), 0);
    if (inputs.keep < owner.testPoses.size()) {
        for (u32 n = 0; n < tree.size(); ++n) {
            const NodeSkinSetup& skin = tree.nodes[n].skin;
            if (inputs.keep < skin.poseSources.size() &&
                skin.poseSources[inputs.keep] == TPoseSource::You &&
                inputs.keep < skin.poseDeltas.size()) {
                yours[n] = 1;
                // At rest a pivot rig's parent frame is the identity, so the
                // saved delta's rotation is already the turn this hands back.
                PushJoint(result, n, skin.poseDeltas[inputs.keep].rotation, TPoseSource::You);
            }
        }
    }

    if (!result.joints.empty()) {
        carried = Accumulate(tree, order, result.joints);
    }

    // --- the figure's own frame --------------------------------------------
    //
    // Not a rule of its own: every rule below aims in this frame, so a figure
    // that does not stand in it has to be put there first.
    SquareFigure(tree, order, yours, result, carried);

    // --- the limbs (§4) ----------------------------------------------------
    //
    // In the hierarchy's order, so a limb nested under another is pressed where
    // the one above it has left it rather than where the file rested it.
    for (const u32 n : order) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (rig.role != RigRole::End || rig.limb == RigLimb::Other) {
            continue;
        }
        if (rig.limb == RigLimb::Arm ? !rules.arms : !rules.legs) {
            continue;
        }
        TPoseLimb row;
        row.end = n;
        row.side = rig.side;
        row.limb = rig.limb;
        const std::optional<Limb> limb = LimbOf(tree, n);
        if (!limb) {
            row.refusal = TPoseRefusal::NoChain;
            result.limbs.push_back(row);
            continue;
        }
        if (yours[limb->upper] || yours[limb->lower] || yours[limb->end]) {
            result.limbs.push_back(row);
            continue;
        }
        // Two limbs sharing a joint is a rig the record names twice — a hand
        // and the weapon in it, both walked up to the same elbow. Turning the
        // pair twice adds the two solves together, so the second is reported
        // rather than worked around (§5.3): on `chaosspaceorc.mdx` that is the
        // difference between an arm 98 degrees off +Y and one on it.
        const auto alreadyTurned = [&](u32 node) {
            return node != kInvalidNode &&
                   std::any_of(result.joints.begin(), result.joints.end(),
                               [node](const TPoseJoint& joint) { return joint.node == node; });
        };
        if (alreadyTurned(limb->upper) || alreadyTurned(limb->lower) ||
            alreadyTurned(limb->end) || alreadyTurned(limb->hock)) {
            row.refusal = TPoseRefusal::Overlaps;
            result.limbs.push_back(row);
            continue;
        }
        const LimbRest rest = RestOf(tree, *limb, carried);
        if (!rest.ok) {
            row.refusal = TPoseRefusal::ZeroLength;
            result.limbs.push_back(row);
            continue;
        }
        row.hinge = HingeOf(document, model, *limb);
        if (row.hinge.source == HingeSource::None) {
            row.refusal = TPoseRefusal::NoHinge;
            result.limbs.push_back(row);
            continue;
        }
        const TPoseSource source =
            row.hinge.source == HingeSource::Clips ? TPoseSource::Clips : TPoseSource::Record;

        LimbSetup setup;
        setup.hinge = row.hinge.axis;
        ApplyLimits(tree, *limb, row.hinge, setup);

        const Vector3f aim = AimOf(rig.limb, rig.side, rules);
        IkGoal goal;
        goal.position = rest.press.upper + aim * (rest.reach * kStraighten);
        goal.orientation = rest.press.endWorld;
        // The hand rides the arm: holding its world rotation would leave it
        // pointing where the bent limb left it.
        goal.holdOrientation = false;

        const LimbSolve solve = SolveLimb(rest.press, setup, goal);
        if (!solve.ok) {
            row.refusal = TPoseRefusal::Unreachable;
            result.limbs.push_back(row);
            continue;
        }
        // `reached` is false by construction here — the goal is past the limb's
        // length on purpose — so what is asked instead is whether the limb came
        // out pointing where the rule said. A `Bend` record that clamps it is
        // the usual reason, and the row says so rather than forcing it.
        const Vector3f landed = solve.lower - rest.press.upper;
        if (landed.length() > 1e-6f) {
            row.aimDeg = std::acos(std::clamp(landed.normalized().dot(aim), -1.0f, 1.0f)) * 180.0f /
                         kPi;
            if (row.aimDeg > kAimTolerance) {
                row.refusal =
                    setup.bendLimitsDeg ? TPoseRefusal::JointLimits : TPoseRefusal::Unreachable;
            }
        }
        PushJoint(result, limb->upper, solve.upperDelta, source);
        PushJoint(result, limb->lower, solve.lowerDelta, source);

        Quaternion endTurn = solve.endDelta;
        TPoseSource endSource = source;
        if (rules.handsAndFeet) {
            // §3.2: a hand's own bone aims along the arm, a foot's along +X and
            // level. The End's rest bone is carried by the turns above it
            // first, so what is left is the arc from there to the rule. Where
            // the file carries `BPOS` that rest bone is a real orientation and
            // not a guess, which is what the row then says.
            const Vector3f bone = BoneOf(tree, limb->end, carried);
            if (bone.length_squared() > 1e-12f) {
                const Vector3f carried =
                    Rotate(endTurn, Rotate(solve.lowerDelta, Rotate(solve.upperDelta, bone)));
                const Vector3f want = rig.limb == RigLimb::Leg ? Vector3f{1, 0, 0} : aim;
                endTurn = ArcBetween(carried, want) * endTurn;
                if (haveBindFrames) {
                    endSource = TPoseSource::BindFrame;
                }
            }
        }
        PushJoint(result, limb->end, endTurn, endSource);

        for (const TPoseJoint& joint : result.joints) {
            if (joint.node == limb->upper || joint.node == limb->lower || joint.node == limb->end) {
                row.turnDeg = std::max(row.turnDeg, joint.turnDeg);
            }
        }
        result.limbs.push_back(row);
        // The next limb down is pressed where this one has left it.
        carried = Accumulate(tree, order, result.joints);
    }

    // --- the spine, neck and head (§3.2) -----------------------------------
    if (rules.spineAndHead) {
        u32 tip = kInvalidNode;
        for (u32 n = 0; n < tree.size(); ++n) {
            const RigRole role = RoleOf(tree, n);
            if (role == RigRole::Head) {
                tip = n;
                break;
            }
            if (role == RigRole::Neck && tip == kInvalidNode) {
                tip = n;
            }
        }
        const std::vector<u32> above =
            tip == kInvalidNode ? std::vector<u32>{} : ChainAbove(tree, tip, kSpineChain);
        if (!above.empty() && !yours[tip]) {
            // Where the spine stands NOW, for the reason `RestOf` says: the
            // square above it and the limbs below it have already moved it, and
            // a press taken from the file's rest would solve a figure that is
            // no longer there.
            std::vector<Vector3f> press;
            press.reserve(above.size() + 1);
            for (const u32 joint : above) {
                press.push_back(Moved(RestPoint(tree, joint), carried[joint]));
            }
            press.push_back(Moved(RestPoint(tree, tip), carried[tip]));
            f32 reach = 0;
            for (std::size_t i = 0; i + 1 < press.size(); ++i) {
                reach += Gap(press[i + 1], press[i]);
            }
            if (reach > 1e-4f) {
                IkGoal goal;
                goal.position = press.front() + Vector3f{0, 0, 1} * reach;
                goal.holdOrientation = false;
                const ChainSolve solve = SolveChain(press, Quaternion::identity(), goal);
                if (solve.ok) {
                    for (std::size_t i = 0; i < above.size() && i < solve.deltas.size(); ++i) {
                        // §3.1: a joint a person set is not solved over.
                        if (!yours[above[i]]) {
                            PushJoint(result, above[i], solve.deltas[i], TPoseSource::Record);
                        }
                    }
                    PushJoint(result, tip, solve.endDelta, TPoseSource::Record);
                    carried = Accumulate(tree, order, result.joints);
                }
            }
        }
    }

    // --- symmetry (§3.1) ---------------------------------------------------
    //
    // A canonical goal is symmetric by construction, so both sides of a rig
    // that solves come out mirrored already. This is for the side that did not:
    // its partner's turn, reflected, rather than nothing at all.
    if (rules.symmetry && !result.joints.empty()) {
        const std::vector<TPoseJoint> solved = result.joints;
        std::vector<u32> turned(tree.size(), kInvalidIndex);
        for (u32 i = 0; i < solved.size(); ++i) {
            if (solved[i].node < tree.size()) {
                turned[solved[i].node] = i;
            }
        }
        // A node with a solved turn above it or below it is left alone. Rigs
        // that chain three Uppers (`L_upr_arm`, `_01`, `_02`) have the limb the
        // record names starting part way down, and mirroring onto a node ABOVE
        // that would turn a limb which is already where the rule asked — which
        // is what put the left arm of `unbrokendarkweaver_portrait` 94 degrees
        // off +Y.
        std::vector<u8> near(tree.size(), 0);
        const std::vector<u32> order = PreOrder(tree);
        for (const u32 n : order) {
            const u32 parent = ParentOf(tree, n);
            if (parent != kInvalidNode && (near[parent] || turned[parent] != kInvalidIndex)) {
                near[n] = 1;
            }
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const u32 parent = ParentOf(tree, *it);
            if (parent != kInvalidNode && (near[*it] || turned[*it] != kInvalidIndex)) {
                near[parent] = 1;
            }
        }
        const std::vector<skinning::BoneMirror> mirror =
            skinning::BuildBoneMirror(tree, skinning::MirrorAxis::Y);
        for (u32 n = 0; n < mirror.size() && n < tree.size(); ++n) {
            const u32 partner = mirror[n].node;
            if (partner >= tree.size() || partner == n || yours[n] || near[n] ||
                turned[n] != kInvalidIndex || turned[partner] == kInvalidIndex) {
                continue;
            }
            const TPoseJoint& from = solved[turned[partner]];
            PushJoint(result, n, MirroredTurn(from.turn), from.source);
        }
    }

    return result;
}

std::vector<Transform> TPoseRest(const Model& model, const TPoseResult& result) {
    const NodeTree& tree = model.nodes;
    const std::vector<Matrix44f> carried = Accumulate(tree, PreOrder(tree), result.joints);
    std::vector<Transform> rest(tree.size());
    for (u32 n = 0; n < tree.size(); ++n) {
        rest[n] = FromMatrix(ToMatrix(tree.worldBind(n)) * carried[n]);
    }
    return rest;
}

} // namespace wem
} // namespace models
} // namespace whiteout
