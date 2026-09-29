// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/rigging/tpose.h"

#include "whiteout/models/wem/rigging/detect.h"
#include "whiteout/models/wem/rigging/ik.h"
#include "whiteout/models/wem/skinning/mirror.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

/// The child @p node's bone runs to: its first joint child with a length, every
/// point read through @p at. A prop or a pad rides the node without being part
/// of it — a sword numbered before the fingers is not where a hand points — so
/// neither answers, as neither is in `SpreadOf`.
template <class At>
u32 AimChildOf(const NodeTree& tree, u32 node, const At& at) {
    for (const u32 child : tree.children(node)) {
        const RigRole role = tree.nodes[child].rig.role;
        if (IsJoint(tree, child) && role != RigRole::Prop && role != RigRole::Pad &&
            (at(child) - at(node)).length_squared() > 1e-12f) {
            return child;
        }
    }
    return kInvalidNode;
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
        const auto at = [&](u32 n) { return RestPoint(tree, n); };
        const u32 aim = AimChildOf(tree, node, at);
        return aim == kInvalidNode ? Vector3f{0, 0, 0} : at(aim) - at(node);
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

constexpr f32 kDeg = 180.0f / kPi;

Vector3f Unit(const Vector3f& v) {
    const f32 length = v.length();
    return length > 1e-9f ? v * (1.0f / length) : Vector3f{0, 0, 0};
}

/// @p from turned @p deg toward @p toward, in the plane the two span; @p from
/// itself when they are parallel.
Vector3f Toward(const Vector3f& from, const Vector3f& toward, f32 deg) {
    const Vector3f axis = ::whiteout::cross(from, toward);
    if (axis.length_squared() < 1e-12f) {
        return from;
    }
    return Unit(Rotate(Quaternion::from_axis_angle(axis.normalized(), deg / kDeg), from));
}

f32 SideSign(RigSide side) {
    return side == RigSide::Right ? -1.0f : 1.0f;
}

/// @p deg wrapped into (-180, 180].
f32 WrapDeg(f32 deg) {
    while (deg > 180.0f) {
        deg -= 360.0f;
    }
    while (deg <= -180.0f) {
        deg += 360.0f;
    }
    return deg;
}

/// The line @p end's own joints spread along, square to @p bone (see
/// `LimbDirections::spread`), with every point read through @p at. Props and
/// pads ride a hand without being part of it, and are left out.
template <class At>
Vector3f SpreadOf(const NodeTree& tree, u32 end, const Vector3f& bone, const At& at) {
    const Vector3f axis = Unit(bone);
    if (axis.length_squared() == 0) {
        return {0, 0, 0};
    }
    const Vector3f origin = at(end);
    f64 scatter[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    u32 points = 0;
    for (const u32 n : tree.subtree(end)) {
        const RigRole role = tree.nodes[n].rig.role;
        if (n == end || !IsJoint(tree, n) || role == RigRole::Prop || role == RigRole::Pad) {
            continue;
        }
        const Vector3f d = at(n) - origin;
        const Vector3f across = d - axis * d.dot(axis);
        const f64 v[3] = {across.x, across.y, across.z};
        for (u32 r = 0; r < 3; ++r) {
            for (u32 c = 0; c < 3; ++c) {
                scatter[r][c] += v[r] * v[c];
            }
        }
        ++points;
    }
    if (points < 2) {
        return {0, 0, 0};
    }
    const Eigen eigen = Symmetric3(scatter);
    if (eigen.value[0] < 1e-12) {
        return {0, 0, 0};
    }
    return Unit(Vector3f{static_cast<f32>(eigen.vector[0][0]), static_cast<f32>(eigen.vector[0][1]),
                         static_cast<f32>(eigen.vector[0][2])});
}

/// The turn about @p axis that lays line @p from on line @p to, both read
/// square to it: the smaller of the two, because a line has no sign.
Quaternion RollOnto(const Vector3f& from, const Vector3f& to, const Vector3f& axis) {
    const Vector3f u = Unit(axis);
    const Vector3f a = Unit(from - u * from.dot(u));
    const Vector3f b = Unit(to - u * to.dot(u));
    if (u.length_squared() == 0 || a.length_squared() == 0 || b.length_squared() == 0) {
        return Quaternion::identity();
    }
    f32 psi = std::atan2(u.dot(::whiteout::cross(a, b)), a.dot(b));
    if (psi > kPi * 0.5f) {
        psi -= kPi;
    } else if (psi < -kPi * 0.5f) {
        psi += kPi;
    }
    return Quaternion::from_axis_angle(u, psi);
}

/// Where a role’s bone aims (R§3.0’s ladder): the donor’s direction for the same
/// limb, side and role where it has one, else the canon’s. A foot’s target is a
/// heading and level, because a foot’s slope is the bind’s (R§1.6): a donor’s
/// foot gives only where it points, and one pointing nearly down gives none.
/// A donor’s hand with no bone of its own continues its forearm, as
/// `MeasureTPose` reads it: the canon’s would kink the wrist by the difference.
Vector3f TargetOf(RigLimb limb, RigSide side, RigRole role, const TPoseRules& rules,
                  const LimbDirections* twin, bool* fromDonor) {
    {
        if (twin != nullptr) {
            Vector3f d = role == RigRole::Upper ? twin->upperDir
                         : role == RigRole::End ? twin->bone
                                                : twin->lowerDir;
            if (limb == RigLimb::Leg && role == RigRole::End) {
                d.z = 0;
            } else if (role == RigRole::End && d.length_squared() == 0) {
                d = twin->lowerDir;
            }
            if (d.length_squared() > 1e-6f) {
                if (fromDonor != nullptr) {
                    *fromDonor = true;
                }
                return Unit(d);
            }
        }
    }
    if (fromDonor != nullptr) {
        *fromDonor = false;
    }
    return rules.canon.Direction(limb, side, role);
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

/// @p direction after @p move's turn, row vectors: what `Moved` does to a
/// point, less what it does to the origin.
Vector3f Turned(const Vector3f& direction, const Matrix44f& move) {
    return Moved(direction, move) - Moved(Vector3f{0, 0, 0}, move);
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

/// R§3.2's ranking of two hinges: the document's own record, then the clips
/// by their share, then the plane the limb rests in; nothing ranks last.
f32 HingeRank(const HingeFit& fit) {
    switch (fit.source) {
    case HingeSource::Joint:
        return 3.0f;
    case HingeSource::Clips:
        return 2.0f + std::clamp(fit.share, 0.0f, 1.0f) * 0.5f;
    case HingeSource::Fallback:
        return 1.0f;
    case HingeSource::None:
        break;
    }
    return 0.0f;
}

/// @p fit seen from the other side of the figure's plane: `MirroredTurn`'s
/// reflection applied to an axis, so a positive bend still takes the Lower
/// the way it went. The plane is the figure's only once it is squared — a DE
/// rig is bound yawed by up to 60 degrees — so the axis is carried into the
/// squared frame by @p from (its own Lower's), reflected there, and carried
/// back out by @p to (the partner's), the frame every hinge is stored in.
/// `from` stays: it names whose evidence this is.
HingeFit MirroredHinge(const HingeFit& fit, const Matrix44f& from, const Matrix44f& to) {
    HingeFit out = fit;
    const Vector3f squared = Unit(Turned(fit.axis, from));
    out.axis = Unit(Turned(Vector3f{-squared.x, squared.y, -squared.z}, Matrix44f::inverse(to)));
    return out;
}

/// The bone @p node owns: its rest point to its aim child's (`AimChildOf`).
/// Zero for a leaf, which has no direction of its own to aim.
Vector3f BoneOf(const NodeTree& tree, u32 node, const std::vector<Matrix44f>& carried) {
    const auto at = [&](u32 n) { return Moved(RestPoint(tree, n), carried[node]); };
    const u32 aim = AimChildOf(tree, node, at);
    return aim == kInvalidNode ? Vector3f{0, 0, 0} : at(aim) - at(node);
}

/// One limb's rest press.
struct LimbRest {
    LimbPress press;
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

/// @p hinge squared to the bones at @p press. A measured hinge is a few degrees
/// off the plane the rest bones bend in (a knee twists as it bends), and a hinge
/// not square to both bones sweeps the Lower round a cone that never reaches the
/// Upper's line — the DE grunt's straight twin was out of reach by 18 degrees.
/// At rest the hinge only says which way the joint bends, so: the plane the
/// limb bends in now, turned to agree with it, or for a limb pressed straight
/// the hinge squared off its line.
Vector3f SquaredHinge(const Vector3f& hinge, const LimbPress& press) {
    const Vector3f u = press.lower - press.upper;
    const Vector3f v = press.end - press.lower;
    const Vector3f plane = ::whiteout::cross(u, v);
    const f32 sine = plane.length() / std::max(u.length() * v.length(), 1e-12f);
    if (sine > std::sin(2.0f / kDeg)) {
        return Unit(plane) * (plane.dot(hinge) < 0 ? -1.0f : 1.0f);
    }
    const Vector3f line = Unit(u + v);
    const Vector3f square = hinge - line * hinge.dot(line);
    return square.length() > 1e-3f ? Unit(square) : hinge;
}

/// How far @p limb's Lower stands bent about @p hinge (unit, the bind's frame)
/// from its bind: its turn against its parent's, both carried from the bind —
/// what `LimbSetup::bendDeg` asks for, so the limits bound the whole bend and
/// not only the solve's share of it.
f32 BendFromBind(const NodeTree& tree, const Limb& limb, const Vector3f& hinge,
                 const std::vector<Matrix44f>& carried) {
    const u32 parent = ParentOf(tree, limb.lower);
    const Quaternion above =
        parent == kInvalidNode ? Quaternion::identity() : FromMatrix(carried[parent]).rotation;
    const Quaternion own = (above.conjugate() * FromMatrix(carried[limb.lower]).rotation).normalized();
    f32 along = own.x * hinge.x + own.y * hinge.y + own.z * hinge.z;
    f32 w = own.w;
    if (w < 0.0f) {
        along = -along;
        w = -w;
    }
    return 2.0f * std::atan2(along, w) * kDeg;
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
/// @p steps, when given, are the saved pose's moves, each in its parent's space
/// as a delta keeps it: the turns above carry it, and it carries what hangs
/// from it.
std::vector<Matrix44f> Accumulate(const NodeTree& tree, const std::vector<u32>& order,
                                  const std::vector<TPoseJoint>& joints,
                                  const std::vector<Vector3f>* steps = nullptr) {
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
        if (steps != nullptr && n < steps->size() && (*steps)[n].length_squared() > 0) {
            carried[n] = carried[n] * Matrix44f::translation(Turned((*steps)[n], above));
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

/// Ankles closer than this, as a share of the leg, are level: raising one
/// would rewrite its keys for nothing.
constexpr f32 kLevelFloor = 1e-3f;
/// Past this share of the leg an ankle gap is not a stance but a rig — a
/// creature with a short leg, a mount's rider — and is left for the panel to
/// show rather than bent away.
constexpr f32 kLevelCeiling = 0.1f;

/// A solve of a limb whose joints may already hold rows — a first solve's, or a
/// clip frame the solve starts from. A row is applied after everything above
/// it, so the solve's world turns cannot simply be multiplied on: each joint's
/// row becomes what puts it where the solve says, given what its parent's row
/// now does — `D(k) * r(k) * D(parent)^-1`, D being the solve's composite at k.
/// With no rows there, that is the solve's own turns. @p endSource names the
/// End's row when the End has none yet.
void PushOnto(TPoseResult& result, const Limb& limb, const LimbSolve& solve, TPoseSource source,
              TPoseSource endSource) {
    const auto rowOf = [&](u32 node) {
        for (const TPoseJoint& joint : result.joints) {
            if (joint.node == node) {
                return joint.turn;
            }
        }
        return Quaternion::identity();
    };
    const Quaternion dUpper = solve.upperDelta;
    const Quaternion dLower = solve.lowerDelta * dUpper;
    const Quaternion dEnd = solve.endDelta * dLower;
    const Quaternion rLower = rowOf(limb.lower);
    const Quaternion rEnd = rowOf(limb.end);
    // PushJoint composes `turn * row`, so each turn is the new row times the
    // old one's inverse.
    PushJoint(result, limb.upper, dUpper, source);
    PushJoint(result, limb.lower, dLower * rLower * dUpper.conjugate() * rLower.conjugate(), source);
    PushJoint(result, limb.end, dEnd * rEnd * dLower.conjugate() * rEnd.conjugate(), endSource);
}

/// Joints the solve starts from where the saved pose has them (§3.1). Each keeps
/// its delta in its PARENT's space, as the store keeps it, so its row is read
/// again whenever a rule turns something above it (`Carry`), its move with it.
/// A `Kept` joint is never solved over: every one set by hand, and the rest of a
/// limb one of them is in. A `Start` joint — a clip frame taken as the pose — is
/// where the solve begins (D§3: onto the stored deltas), and a rule that turns
/// it takes it over (`Release`).
struct Held {
    enum : u8 { Free = 0, Kept = 1, Start = 2 };
    std::vector<u8> on;
    std::vector<Quaternion> delta;
    /// The saved moves, kept after a release: the solve turns, and never moves.
    std::vector<Vector3f> step;
    std::vector<TPoseSource> source;

    bool operator[](u32 node) const {
        return node < on.size() && on[node] != Free;
    }
    bool kept(u32 node) const {
        return node < on.size() && on[node] == Kept;
    }
};

/// A `Start` joint a rule is about to turn: its row is the rule's from here on,
/// under @p source. A kept joint stays kept, and a free one has nothing to give.
void Release(Held& held, TPoseResult& result, u32 node, TPoseSource source) {
    if (node >= held.on.size() || held.on[node] != Held::Start) {
        return;
    }
    held.on[node] = Held::Free;
    for (TPoseJoint& joint : result.joints) {
        if (joint.node == node) {
            joint.source = source;
        }
    }
}

/// `Accumulate`, with every held joint's row first read again from its delta
/// under the turns now above it, and the saved moves carried.
std::vector<Matrix44f> Carry(const NodeTree& tree, const std::vector<u32>& order, const Held& held,
                             TPoseResult& result) {
    std::vector<u32> row(tree.size(), kInvalidIndex);
    for (u32 i = 0; i < result.joints.size(); ++i) {
        if (result.joints[i].node < tree.size()) {
            row[result.joints[i].node] = i;
        }
    }
    std::vector<Quaternion> composite(tree.size(), Quaternion::identity());
    for (const u32 n : order) {
        const u32 parent = ParentOf(tree, n);
        const Quaternion above = parent == kInvalidNode ? Quaternion::identity() : composite[parent];
        if (held[n]) {
            // `TPoseDeltaTurns`'s identity, read the other way: row * P = P * delta.
            const Quaternion turn = (above * held.delta[n] * above.conjugate()).normalized();
            if (row[n] != kInvalidIndex) {
                result.joints[row[n]].turn = turn;
                result.joints[row[n]].turnDeg = DegreesOf(turn);
            } else if (std::fabs(turn.w) <= 0.9999999f) {
                row[n] = static_cast<u32>(result.joints.size());
                PushJoint(result, n, turn, held.source[n]);
            }
        }
        const Quaternion turn = row[n] == kInvalidIndex ? Quaternion::identity() : result.joints[row[n]].turn;
        composite[n] = (turn * above).normalized();
    }
    return Accumulate(tree, order, result.joints, &held.step);
}

/// A chain's world turns, root first — each applied after the ones above it,
/// as `SolveChain` hands them back — onto rows the joints may already hold:
/// the row of each becomes `D(k) * r(k) * D(parent)^-1`, D the composite so
/// far, as `PushOnto` does for a limb. A kept joint takes no turn: it rides the
/// joint above it (`Carry`), so D(parent) below it is that joint's, while the
/// joints below still take the turns the chain asked of them.
void PushChain(TPoseResult& result, const std::vector<u32>& chain, const std::vector<Quaternion>& turns,
               Held& held, TPoseSource source) {
    const auto rowOf = [&](u32 node) {
        for (const TPoseJoint& joint : result.joints) {
            if (joint.node == node) {
                return joint.turn;
            }
        }
        return Quaternion::identity();
    };
    Quaternion asked = Quaternion::identity();
    Quaternion taken = Quaternion::identity();
    for (std::size_t i = 0; i < chain.size() && i < turns.size(); ++i) {
        const Quaternion here = (turns[i] * asked).normalized();
        if (!held.kept(chain[i])) {
            Release(held, result, chain[i], source);
            const Quaternion row = rowOf(chain[i]);
            PushJoint(result, chain[i], here * row * taken.conjugate() * row.conjugate(), source);
            taken = here;
        }
        asked = here;
    }
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

/// Whether @p node hangs below another joint that starts the same limb on the
/// same side, and so does not start it itself. The DE abomination is the case:
/// the record reads every joint of its hanging third arm as an Upper, and a
/// figure whose shoulder is the average of those sits out at the dangling hand.
bool BelowAnotherUpper(const NodeTree& tree, u32 node, RigLimb limb, RigSide side) {
    for (u32 n = ParentOf(tree, node), step = tree.size(); n != kInvalidNode && step > 0;
         n = ParentOf(tree, n), --step) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (RoleOf(tree, n) == RigRole::Upper && rig.limb == limb && rig.side == side) {
            return true;
        }
    }
    return false;
}

Sides SidesOf(const NodeTree& tree, RigLimb limb) {
    Sides out;
    for (u32 n = 0; n < tree.size(); ++n) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (RoleOf(tree, n) != RigRole::Upper || rig.limb != limb ||
            BelowAnotherUpper(tree, n, limb, rig.side)) {
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
/// Run a second time after the trunk, which turns what the limbs hang from.
/// @p framed marks what it turned, because a frame turn is not a limb turn: the
/// symmetry rule below reads "a solved turn above me" as "my limb is already
/// solved", and a turn at the root would mean that of every node in the model.
void SquareFigure(const NodeTree& tree, const std::vector<u32>& order, Held& yours,
                  const RigDirections* donor, TPoseResult& result, std::vector<Matrix44f>& carried,
                  std::vector<u8>& framed) {
    // Onto the twin's own axes where it has them (R§3.0: the twin's frame is a
    // target like its limbs), else onto +Y — level.
    const auto want = [&](const Vector3f& twin) {
        const Vector3f flat{twin.x, twin.y, 0};
        return donor != nullptr && flat.length() > 1e-6f ? Unit(twin) : Vector3f{0, 1, 0};
    };
    const Vector3f hipsWant = want(donor != nullptr ? donor->hips : Vector3f{0, 0, 0});
    const Vector3f shouldersWant = want(donor != nullptr ? donor->shoulders : Vector3f{0, 0, 0});
    // Two turns at the node the sides hang from: about +Z, which squares the
    // figure on the floor, then about the horizontal line square to the axis,
    // which levels it — a DE pelvis bound with one hip higher stands its
    // aimed legs' ankles apart by exactly that (R§3.5). Nothing else: a turn
    // about the axis itself would pitch the figure over.
    const auto squareAt = [&](u32 meet, const Sides& sides, const Vector3f& onto) {
        if (meet == kInvalidNode || yours.kept(meet)) {
            return;
        }
        const Vector3f across =
            MiddleOf(tree, sides.left, carried) - MiddleOf(tree, sides.right, carried);
        const Vector3f flat{across.x, across.y, 0};
        const Vector3f ontoFlat{onto.x, onto.y, 0};
        if (flat.length() < 1e-4f || ontoFlat.length() < 1e-6f) {
            return;
        }
        const Quaternion yaw = ArcBetween(flat, ontoFlat);
        const Quaternion turn = (ArcBetween(Rotate(yaw, across), onto) * yaw).normalized();
        if (DegreesOf(turn) < kSquareFloor) {
            return;
        }
        Release(yours, result, meet, TPoseSource::Record);
        PushJoint(result, meet, turn, TPoseSource::Record);
        framed[meet] = 1;
        carried = Carry(tree, order, yours, result);
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
        squareAt(hipMeet, hips, hipsWant);
    }
    // Untwisting never turns the hips again: the same node, or one above it,
    // would undo the square. Below it, or on a branch of its own — a spine
    // beside the pelvis under a yawed root — the shoulders are squared too.
    if (shoulders.both() &&
        (hipMeet == kInvalidNode || (armMeet != hipMeet && !Holds(tree, armMeet, hipMeet)))) {
        squareAt(armMeet, shoulders, shouldersWant);
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

const char* ToString(TPoseLanding landing) {
    switch (landing) {
    case TPoseLanding::Refused:
        return "Refused";
    case TPoseLanding::OnTarget:
        return "OnTarget";
    case TPoseLanding::Off:
        return "Off";
    }
    return "Refused";
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
    // Every turn here is about a pivot in a rest whose locals are the identity,
    // and every delta is read that way: an explicit bind is neither.
    if (tree.rig != RigConvention::PivotRelative) {
        return result;
    }
    const std::vector<u32> order = PreOrder(tree);
    std::vector<Matrix44f> carried(tree.size(), Matrix44f::identity());

    // What a person set by hand is never solved over (§3.1): those joints keep
    // the turn the saved pose holds, and the solve leaves their limb alone. By
    // the source, not the turn: a joint only moved is yours as well. A clip
    // frame taken as the pose is where the solve starts from instead.
    Held yours;
    yours.on.assign(tree.size(), Held::Free);
    yours.delta.assign(tree.size(), Quaternion::identity());
    yours.step.assign(tree.size(), Vector3f{0, 0, 0});
    yours.source.assign(tree.size(), TPoseSource::None);
    const auto saved = [&](u32 node) -> const PoseDelta* {
        const NodeSkinSetup& skin = tree.nodes[node].skin;
        return inputs.keep < owner.testPoses.size() && inputs.keep < skin.poseDeltas.size()
                   ? &skin.poseDeltas[inputs.keep]
                   : nullptr;
    };
    const auto savedSource = [&](u32 node) {
        const NodeSkinSetup& skin = tree.nodes[node].skin;
        return inputs.keep < owner.testPoses.size() && inputs.keep < skin.poseSources.size()
                   ? skin.poseSources[inputs.keep]
                   : TPoseSource::None;
    };
    const auto hold = [&](u32 node, u8 kind) {
        if (node >= tree.size() || yours.kept(node)) {
            return;
        }
        yours.on[node] = kind;
        yours.source[node] = savedSource(node);
        if (const PoseDelta* delta = saved(node)) {
            yours.delta[node] = delta->rotation.normalized();
            yours.step[node] = delta->translation;
        }
    };
    for (u32 n = 0; n < tree.size(); ++n) {
        if (savedSource(n) == TPoseSource::You) {
            hold(n, Held::Kept);
        } else if (savedSource(n) == TPoseSource::Clips) {
            hold(n, Held::Start);
        }
    }
    carried = Carry(tree, order, yours, result);

    // --- the figure's own frame --------------------------------------------
    //
    // Not a rule of its own: every rule below aims in this frame, so a figure
    // that does not stand in it has to be put there first.
    const RigDirections* donor = inputs.donor;
    std::vector<u8> framed(tree.size(), 0);
    SquareFigure(tree, order, yours, donor, result, carried, framed);

    // --- the spine, neck and head (§3.2; R§3.6: before the limbs) ------------
    //
    // The trunk moves what the limbs hang from, so it goes first and the
    // limbs are pressed where it leaves them; the square then runs once more
    // on the trunk it left, and nothing after it turns the root again.
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
        if (!above.empty() && !yours.kept(tip)) {
            // Where the spine stands NOW, for the reason `RestOf` says: the
            // square has already moved it, and a press taken from the file's
            // rest would solve a figure that is no longer there.
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
                // R§3.6: the head where the twin's trunk puts it, the chain's
                // own span kept so a curve that is the twin's stays one; else
                // straight up. The twin's neck is its last segment's aim, not
                // the whole trunk's — along it the figure would pitch by the
                // neck's lean — so with a neck to lean, the chain below it
                // reaches for where the neck must stand for the head to land
                // there, and the neck then leans as the twin's.
                const bool twinned = donor != nullptr && donor->trunk.length_squared() > 0;
                const bool leans = twinned && donor->headBone.length_squared() > 0 && above.size() >= 2;
                const Vector3f head = twinned ? press.front() + Unit(donor->trunk) * Gap(press.back(), press.front())
                                              : press.front() + Vector3f{0, 0, 1} * reach;
                const Vector3f neck = press.back() - press[press.size() - 2];
                IkGoal goal;
                goal.position = leans ? head - Unit(donor->headBone) * neck.length() : head;
                goal.holdOrientation = false;
                const ChainSolve solve = SolveChain(
                    std::span<const Vector3f>(press.data(), leans ? above.size() : press.size()),
                    Quaternion::identity(), goal);
                if (solve.ok) {
                    // Composed onto what each joint already holds — the square
                    // may have turned one — given what its parent now does.
                    std::vector<u32> chain = above;
                    chain.push_back(tip);
                    std::vector<Quaternion> turns(solve.deltas.begin(), solve.deltas.end());
                    if (leans) {
                        // The neck's own turn: its segment, carried by the turns
                        // below it, onto the twin's. The head rides it.
                        Quaternion asked = Quaternion::identity();
                        for (const Quaternion& turn : turns) {
                            asked = (turn * asked).normalized();
                        }
                        turns.push_back(ArcBetween(Rotate(asked, neck), donor->headBone));
                        turns.push_back(Quaternion::identity());
                    } else {
                        turns.resize(above.size(), Quaternion::identity());
                        turns.push_back(solve.endDelta);
                    }
                    PushChain(result, chain, turns, yours, TPoseSource::Record);
                    carried = Carry(tree, order, yours, result);
                }
            }
        }
    }

    if (rules.spineAndHead) {
        SquareFigure(tree, order, yours, donor, result, carried, framed);
    }

    // --- the hinges, one per mirrored pair (R§3.2) ---------------------------
    //
    // Every limb's hinge before any limb turns, so a pair can be judged
    // together: the better-evidenced side's axis serves both, reflected, and
    // the row says whose it was. Under the symmetry switch, which is the user
    // saying the figure is symmetric.
    std::vector<std::optional<Limb>> limbOf(tree.size());
    std::vector<HingeFit> hingeOf(tree.size());
    std::vector<u32> ordinalOf(tree.size(), 0);
    {
        // As `DirectionsOf` numbers a twin's limbs, so the two agree.
        u32 seen[3][3] = {};
        for (u32 n = 0; n < tree.size(); ++n) {
            const NodeRig& rig = tree.nodes[n].rig;
            if (!IsJoint(tree, n) || rig.role != RigRole::End || rig.limb == RigLimb::Other ||
                !LimbOf(tree, n)) {
                continue;
            }
            u32& count = seen[static_cast<u32>(rig.limb) % 3][static_cast<u32>(rig.side) % 3];
            ordinalOf[n] = count++;
        }
    }
    for (const u32 n : order) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (rig.role != RigRole::End || rig.limb == RigLimb::Other) {
            continue;
        }
        limbOf[n] = LimbOf(tree, n);
        if (limbOf[n]) {
            hingeOf[n] = HingeOf(document, model, *limbOf[n]);
            hingeOf[n].from = hingeOf[n].source == HingeSource::None ? RigSide::Centre : rig.side;
        }
    }
    // Each limb's twin: the donor's limb of the same kind and side whose knee
    // stands nearest, read in the frame the square just put the figure in — the
    // donor's own — and scaled by height, as `DirectionsOf` reads the donor.
    std::vector<const LimbDirections*> twinOf(tree.size(), nullptr);
    if (donor != nullptr) {
        Vector3f centre{0, 0, 0};
        u32 starts = 0;
        f32 low = std::numeric_limits<f32>::max();
        f32 high = std::numeric_limits<f32>::lowest();
        for (u32 n = 0; n < tree.size(); ++n) {
            if (limbOf[n] && IsJoint(tree, n)) {
                centre = centre + Moved(RestPoint(tree, limbOf[n]->lower), carried[limbOf[n]->lower]);
                ++starts;
            }
            if (IsJoint(tree, n)) {
                const f32 z = Moved(RestPoint(tree, n), carried[n]).z;
                low = std::min(low, z);
                high = std::max(high, z);
            }
        }
        const f32 height = high > low ? high - low : 1.0f;
        if (starts > 0) {
            centre = centre * (1.0f / static_cast<f32>(starts));
        }
        std::vector<LimbDirections> figure;
        for (u32 n = 0; n < tree.size(); ++n) {
            // The limbs `DirectionsOf` lists: joints only.
            if (limbOf[n] && IsJoint(tree, n)) {
                LimbDirections d;
                d.end = n;
                d.limb = tree.nodes[n].rig.limb;
                d.side = tree.nodes[n].rig.side;
                d.ordinal = ordinalOf[n];
                d.knee = (Moved(RestPoint(tree, limbOf[n]->lower), carried[limbOf[n]->lower]) - centre) *
                          (1.0f / height);
                figure.push_back(d);
            }
        }
        const std::vector<const LimbDirections*> paired = PairLimbs(figure, *donor);
        for (std::size_t i = 0; i < figure.size(); ++i) {
            twinOf[figure[i].end] = paired[i];
        }
    }

    if (rules.symmetry) {
        const std::vector<skinning::BoneMirror> mirror =
            skinning::BuildBoneMirror(tree, skinning::MirrorAxis::Y);
        for (u32 n = 0; n < mirror.size() && n < tree.size(); ++n) {
            const u32 partner = mirror[n].node;
            // Each pair once, from its lower index.
            if (partner >= tree.size() || partner <= n || !limbOf[n] || !limbOf[partner]) {
                continue;
            }
            const NodeRig& a = tree.nodes[n].rig;
            const NodeRig& b = tree.nodes[partner].rig;
            if (a.limb != b.limb || a.side == b.side) {
                continue;
            }
            const f32 mine = HingeRank(hingeOf[n]);
            const f32 theirs = HingeRank(hingeOf[partner]);
            const u32 lowerA = limbOf[n]->lower;
            const u32 lowerB = limbOf[partner]->lower;
            if (theirs > mine) {
                hingeOf[n] = MirroredHinge(hingeOf[partner], carried[lowerB], carried[lowerA]);
            } else if (mine > theirs) {
                hingeOf[partner] = MirroredHinge(hingeOf[n], carried[lowerA], carried[lowerB]);
            }
        }
    }

    // --- the limbs (§4) ----------------------------------------------------
    //
    // In the hierarchy's order, so a limb nested under another is pressed where
    // the one above it has left it rather than where the file rested it.
    std::vector<u8> inBand(tree.size(), 0);
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
        const std::optional<Limb>& limb = limbOf[n];
        if (!limb) {
            row.refusal = TPoseRefusal::NoChain;
            result.limbs.push_back(row);
            continue;
        }
        const u32 joints[] = {limb->upper, limb->lower, limb->hock, limb->end};
        if (std::any_of(std::begin(joints), std::end(joints), [&](u32 j) { return yours.kept(j); })) {
            // Where a person put it is where it goes, and the rest of the limb
            // stays where it stood with it: a joint of it left out would be
            // stored as the bind under the one that was set.
            for (const u32 j : joints) {
                hold(j, Held::Kept);
            }
            carried = Carry(tree, order, yours, result);
            row.landing = TPoseLanding::OnTarget;
            row.held = true;
            result.limbs.push_back(row);
            continue;
        }
        // Two limbs sharing a joint is a rig the record names twice — a hand
        // and the weapon in it, both walked up to the same elbow. Turning the
        // pair twice adds the two solves together, so the second is reported
        // rather than worked around (§5.3): on `chaosspaceorc.mdx` that is the
        // difference between an arm 98 degrees off +Y and one on it. A row the
        // solve started from is no turn of another limb's.
        const auto alreadyTurned = [&](u32 node) {
            return node != kInvalidNode && !yours[node] &&
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
        row.hinge = hingeOf[n];
        if (row.hinge.source == HingeSource::None) {
            row.refusal = TPoseRefusal::NoHinge;
            result.limbs.push_back(row);
            continue;
        }
        LimbSetup setup;
        // R§3.1: the axis was read where the file rests the limb, and the
        // press is where the turns above have put it — the square, a limb
        // above, yours. An axis left behind by the figure's own yaw is not
        // square to the bones any more, and a hinge that is not square cannot
        // bend the limb where it is asked; the swivel below puts the plane
        // where the target has it.
        setup.hinge = SquaredHinge(Unit(Turned(row.hinge.axis, carried[limb->lower])), rest.press);
        ApplyLimits(tree, *limb, row.hinge, setup);
        setup.bendDeg = BendFromBind(tree, *limb, Unit(row.hinge.axis), carried);

        // R§3.0: the twin’s directions for this limb, else the canon’s; and
        // the joint says which. Which rung the hinge came off is the row's
        // own field, not the joint's source.
        bool fromDonor = false;
        const Vector3f upperWant =
            TargetOf(rig.limb, rig.side, RigRole::Upper, rules, twinOf[n], &fromDonor);
        const Vector3f lowerWant =
            TargetOf(rig.limb, rig.side, RigRole::Lower, rules, twinOf[n], nullptr);
        const TPoseSource source = fromDonor ? TPoseSource::Donor : TPoseSource::Record;
        // R§2's band: the table is a median, and a limb already within the
        // canon's spread of it is a correct one, left alone — both its bones,
        // or a forearm folded across the chest under a good upper arm would be
        // left too. The Lower is judged against the Upper it hangs from: the
        // table's, carried by the turn that stands the table's Upper where this
        // one stands, so the bend is measured and not the Upper's lean twice.
        // A twin is exact.
        if (!fromDonor && rules.canon.spreadDeg > 0) {
            const auto within = [&](const Vector3f& stands, const Vector3f& want, f32 spread) {
                return stands.length() > 1e-6f && AngleDeg(stands, want) <= spread;
            };
            const Vector3f upperStands = rest.press.lower - rest.press.upper;
            const f32 bend = rig.limb == RigLimb::Arm ? rules.canon.elbowSpreadDeg : rules.canon.kneeSpreadDeg;
            if (within(upperStands, upperWant, rules.canon.spreadDeg) &&
                within(rest.press.end - rest.press.lower, Rotate(ArcBetween(upperWant, upperStands), lowerWant), bend)) {
                row.inCanon = true;
                row.landing = TPoseLanding::OnTarget;
                for (const u32 j : joints) {
                    if (j < tree.size()) {
                        inBand[j] = 1;
                    }
                }
                // Left alone, but a foot still stands at its bind: the square
                // may have levelled the pelvis it hangs from.
                if (rig.limb == RigLimb::Leg) {
                    Release(yours, result, limb->end, TPoseSource::Record);
                    PushJoint(result, limb->end, FromMatrix(carried[limb->end]).rotation.conjugate(),
                              TPoseSource::Record);
                    carried = Carry(tree, order, yours, result);
                }
                result.limbs.push_back(row);
                continue;
            }
        }
        // R§3.5: the goal is the End where the two target directions put it,
        // the bend kept — not a point past the limb that straightens it. The
        // Lower to the End is one segment (a Hock rides inside it), so its
        // whole length goes along the Lower's target. Soft reach is a drag's
        // feel and would land a bent target short of it, so what is asked for
        // is the distance the ease lands on the target.
        const f32 upperLength = Gap(rest.press.lower, rest.press.upper);
        const f32 lowerLength = Gap(rest.press.end, rest.press.lower);
        const Vector3f bent = upperWant * upperLength + lowerWant * lowerLength;
        IkGoal goal;
        goal.position = rest.press.upper +
                        Unit(bent) * AskedReach(bent.length(), Gap(rest.press.end, rest.press.upper),
                                                upperLength + lowerLength);
        // R§3.4: a foot rides nothing — the leg's turn would tip it off the
        // floor — so it keeps the rotation it stood in and turns about +Z
        // below. A hand rides the arm: held, it would point where the bent
        // limb left it.
        goal.holdOrientation = rig.limb == RigLimb::Leg;
        // Held at its BIND orientation: the square above may have levelled
        // the pelvis, and a foot that rode that would tilt with it.
        LimbPress press = rest.press;
        press.endWorld = FromMatrix(carried[limb->end]).rotation;
        goal.orientation = Quaternion::identity();

        LimbSolve solve = SolveLimb(press, setup, goal);
        if (!solve.ok) {
            // `SolveLimb` refuses a bone with no length or a limb with no hinge,
            // and says which by nothing else.
            row.refusal = setup.hinge.length_squared() == 0 ? TPoseRefusal::NoHinge : TPoseRefusal::ZeroLength;
            result.limbs.push_back(row);
            continue;
        }
        const bool reached = solve.reached;
        // R§3.3: the bend is about the hinge, and its plane is wherever the
        // hinge put it. The whole limb turns about its own Upper-to-End line
        // until that plane holds the target's — the twin's, else the canon's
        // (a knee toward +X, an elbow toward -X). The End stays where the
        // solve put it, by `SwivelLimb`'s contract; and with the triangle's
        // three sides already the target's, the two bones then land on their
        // directions exactly. A straight target has no plane, and keeps the
        // swivel `SolveLimb` chose: the hinge nearest its press.
        {
            const Vector3f chord = solve.end - rest.press.upper;
            const Vector3f have =
                ::whiteout::cross(solve.lower - rest.press.upper, solve.end - solve.lower);
            const Vector3f want =
                ::whiteout::cross(upperWant * upperLength, lowerWant * lowerLength);
            const f32 floor = 1e-4f * upperLength * lowerLength;
            if (chord.length_squared() > 1e-12f && have.length() > floor &&
                want.length() > floor) {
                const f32 psi = std::atan2(Unit(chord).dot(::whiteout::cross(have, want)),
                                           have.dot(want));
                LimbPress landed = rest.press;
                landed.lower = solve.lower;
                landed.end = solve.end;
                landed.hock = solve.hock;
                const LimbSolve swivel = SwivelLimb(landed, psi);
                if (swivel.ok && std::fabs(psi) > 1e-6f) {
                    // The swivel goes on after the solve: the Upper takes it
                    // whole, the Lower's own turn moves with it, and the End
                    // rides it or, held, turns back out of it.
                    const Quaternion s = swivel.upperDelta;
                    const Quaternion back = s.conjugate();
                    solve.upperDelta = (s * solve.upperDelta).normalized();
                    solve.lowerDelta = (s * solve.lowerDelta * back).normalized();
                    solve.endDelta = goal.holdOrientation
                                         ? (solve.endDelta * back).normalized()
                                         : (s * solve.endDelta * back).normalized();
                    solve.lower = swivel.lower;
                    solve.end = swivel.end;
                    solve.hock = swivel.hock;
                }
            }
        }
        // R§3.8: how far each bone came out from its target, as the solver
        // landed it. The joints go in either way: a limb its limits or its
        // hinge stopped short is applied and says how far, not forced further.
        row.aimDeg = AngleDeg(solve.lower - rest.press.upper, upperWant);
        row.landedDeg = std::max(row.aimDeg, AngleDeg(solve.end - solve.lower, lowerWant));
        row.landing = row.landedDeg > kAimTolerance ? TPoseLanding::Off : TPoseLanding::OnTarget;
        if (row.landing == TPoseLanding::Off) {
            // The limits are the reason only where they bound: the same limb
            // free of them reaches the goal this one stopped short of.
            bool limited = false;
            if (setup.bendLimitsDeg && !reached) {
                LimbSetup free = setup;
                free.bendLimitsDeg.reset();
                limited = SolveLimb(press, free, goal).reached;
            }
            row.refusal = limited ? TPoseRefusal::JointLimits : TPoseRefusal::Unreachable;
        }

        Quaternion endTurn = solve.endDelta;
        TPoseSource endSource = source;
        if (rules.handsAndFeet) {
            // R§3.4. The End's rest bone is carried by the turns above it
            // first — for a foot that is nothing, it was held — so what is
            // left is the turn from there to the target.
            const Vector3f bone = BoneOf(tree, limb->end, carried);
            if (bone.length_squared() > 1e-12f) {
                const auto carry = [&](const Vector3f& v) {
                    return Rotate(endTurn, Rotate(solve.lowerDelta, Rotate(solve.upperDelta, v)));
                };
                const Vector3f now = carry(bone);
                bool endFromDonor = false;
                const Vector3f want =
                    TargetOf(rig.limb, rig.side, RigRole::End, rules, twinOf[n], &endFromDonor);
                if (rig.limb == RigLimb::Leg) {
                    // A foot turns about +Z only, from its heading to the
                    // target's (level by construction): its pitch and its roll
                    // are the bind's, which is flat on every HD rig. Any other
                    // axis lifts it off the floor.
                    const Vector3f flat{now.x, now.y, 0};
                    if (flat.length() > 1e-3f * bone.length()) {
                        endTurn = ArcBetween(flat, want) * endTurn;
                    }
                } else {
                    // A hand aims its bone, then rolls about it until its
                    // fingers spread along the twin's. Without a twin, or a
                    // hand with no fingers, it keeps the roll the arm gave it.
                    endTurn = ArcBetween(now, want) * endTurn;
                    const LimbDirections* twin = twinOf[n];
                    if (twin != nullptr && twin->spread.length_squared() > 0) {
                        const Vector3f own = SpreadOf(tree, limb->end, bone, [&](u32 j) {
                            return Moved(RestPoint(tree, j), carried[j]);
                        });
                        if (own.length_squared() > 0) {
                            endTurn = RollOnto(carry(own), twin->spread, want) * endTurn;
                        }
                    }
                }
                if (endFromDonor) {
                    endSource = TPoseSource::Donor;
                }
            }
        }
        // Onto whatever the joints started from: a clip frame's rows are the
        // press this solve was taken at.
        for (const u32 j : {limb->upper, limb->lower, limb->end}) {
            Release(yours, result, j, j == limb->end ? endSource : source);
        }
        solve.endDelta = endTurn;
        PushOnto(result, *limb, solve, source, endSource);

        for (const TPoseJoint& joint : result.joints) {
            if (joint.node == limb->upper || joint.node == limb->lower || joint.node == limb->end) {
                row.turnDeg = std::max(row.turnDeg, joint.turnDeg);
            }
        }
        result.limbs.push_back(row);
        // The next limb down is pressed where this one has left it.
        carried = Carry(tree, order, yours, result);
    }

    // --- the ankles, level (R§3.5) -------------------------------------------
    //
    // A leg aimed on its target keeps its own length, so two legs of unequal
    // length, or hung from a tilted pelvis, stand their ankles at different
    // heights. The lower one is raised to the higher by bending its own knee
    // further — the leg's degree of freedom, and one that keeps the foot's
    // orientation — never by moving it. Legs paired as their ordinals pair
    // them, the way a twin's are.
    if (rules.legs) {
        const std::vector<skinning::BoneMirror> mirrorOf =
            skinning::BuildBoneMirror(tree, skinning::MirrorAxis::Y);
        // The figure's height, as a twin's is measured: its joints along +Z.
        f32 lowest = std::numeric_limits<f32>::max();
        f32 highest = std::numeric_limits<f32>::lowest();
        for (u32 n = 0; n < tree.size(); ++n) {
            if (IsJoint(tree, n)) {
                const f32 z = Moved(RestPoint(tree, n), carried[n]).z;
                lowest = std::min(lowest, z);
                highest = std::max(highest, z);
            }
        }
        const f32 height = highest > lowest ? highest - lowest : 0;
        const auto rowOf = [&](u32 end) -> TPoseLimb* {
            for (TPoseLimb& row : result.limbs) {
                if (row.end == end) {
                    return &row;
                }
            }
            return nullptr;
        };
        for (u32 left = 0; left < tree.size(); ++left) {
            const NodeRig& rig = tree.nodes[left].rig;
            if (rig.role != RigRole::End || rig.limb != RigLimb::Leg || rig.side != RigSide::Left ||
                !limbOf[left]) {
                continue;
            }
            // Its partner by the mirror map — a quadruped numbers its legs in
            // no order a front leg can be matched by — else by order.
            u32 right = kInvalidNode;
            if (left < mirrorOf.size()) {
                const u32 m = mirrorOf[left].node;
                if (m < tree.size() && limbOf[m] && tree.nodes[m].rig.role == RigRole::End &&
                    tree.nodes[m].rig.limb == RigLimb::Leg && tree.nodes[m].rig.side == RigSide::Right) {
                    right = m;
                }
            }
            for (u32 n = 0; n < tree.size() && right == kInvalidNode; ++n) {
                const NodeRig& other = tree.nodes[n].rig;
                if (other.role == RigRole::End && other.limb == RigLimb::Leg &&
                    other.side == RigSide::Right && limbOf[n] && ordinalOf[n] == ordinalOf[left]) {
                    right = n;
                }
            }
            TPoseLimb* a = rowOf(left);
            TPoseLimb* b = right == kInvalidNode ? nullptr : rowOf(right);
            // Only two legs the solve placed: one it refused, or one it left
            // in the band, is where it was and is not this rule's to move.
            if (a == nullptr || b == nullptr || a->landing != TPoseLanding::OnTarget ||
                b->landing != TPoseLanding::OnTarget || a->inCanon || b->inCanon || a->held ||
                b->held) {
                continue;
            }
            // Level means the twin's own gap, to scale — none on nearly every
            // HD rig, and exactly its own on a rig that is its own twin.
            f32 want = 0;
            if (donor != nullptr && donor->height > 0) {
                const LimbDirections* l = twinOf[left];
                const LimbDirections* r = right == kInvalidNode ? nullptr : twinOf[right];
                if (l != nullptr && r != nullptr) {
                    want = (l->endHeight - r->endHeight) * (height / donor->height);
                }
            }
            const f32 zLeft = Moved(RestPoint(tree, left), carried[left]).z;
            const f32 zRight = Moved(RestPoint(tree, right), carried[right]).z;
            const f32 off = (zLeft - zRight) - want;
            const u32 low = off < 0 ? left : right;
            TPoseLimb& row = low == left ? *a : *b;
            const Limb& limb = *limbOf[low];
            const LimbRest now = RestOf(tree, limb, carried);
            const f32 length =
                Gap(now.press.lower, now.press.upper) + Gap(now.press.end, now.press.lower);
            const f32 gap = std::fabs(off);
            if (!now.ok || gap <= kLevelFloor * length || gap > kLevelCeiling * length) {
                continue;
            }
            // The hinge and the limits the leg's own solve bent it by, and the
            // bend it already has, so the limits bound the whole of it.
            LimbSetup setup;
            setup.hinge = SquaredHinge(Unit(Turned(row.hinge.axis, carried[limb.lower])), now.press);
            ApplyLimits(tree, limb, row.hinge, setup);
            setup.bendDeg = BendFromBind(tree, limb, Unit(row.hinge.axis), carried);
            IkGoal goal;
            goal.position = now.press.end + Vector3f{0, 0, gap};
            goal.position = now.press.upper +
                            Unit(goal.position - now.press.upper) *
                                AskedReach((goal.position - now.press.upper).length(),
                                           Gap(now.press.end, now.press.upper), length);
            // The foot keeps the rotation it stands in: bending the knee must
            // not tip it.
            goal.holdOrientation = true;
            const LimbSolve solve = SolveLimb(now.press, setup, goal);
            if (!solve.ok || std::fabs(solve.end.z - (now.press.end.z + gap)) > 0.1f * gap + 1e-4f) {
                continue;
            }
            // Onto the rows the leg's own solve wrote, whose source stays.
            PushOnto(result, limb, solve, TPoseSource::Record, TPoseSource::Record);
            row.raised = gap;
            carried = Carry(tree, order, yours, result);
            // The row says where the leg stands now: its aim traded for the
            // stance, and the turns that took it there.
            const LimbRest raised = RestOf(tree, limb, carried);
            const Vector3f upperWant = TargetOf(RigLimb::Leg, row.side, RigRole::Upper, rules, twinOf[low], nullptr);
            const Vector3f lowerWant = TargetOf(RigLimb::Leg, row.side, RigRole::Lower, rules, twinOf[low], nullptr);
            row.aimDeg = AngleDeg(raised.press.lower - raised.press.upper, upperWant);
            row.landedDeg = std::max(row.aimDeg, AngleDeg(raised.press.end - raised.press.lower, lowerWant));
            row.turnDeg = 0;
            for (const TPoseJoint& joint : result.joints) {
                if (joint.node == limb.upper || joint.node == limb.lower || joint.node == limb.end) {
                    row.turnDeg = std::max(row.turnDeg, joint.turnDeg);
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
        // A node the FRAME turned does not count: squaring the figure turns the
        // root, and reading that as "solved" would read every node in the model
        // as solved and leave this rule with nothing to do.
        const auto solvedAt = [&](u32 n) { return turned[n] != kInvalidIndex && !framed[n]; };
        std::vector<u8> near(tree.size(), 0);
        for (const u32 n : order) {
            const u32 parent = ParentOf(tree, n);
            if (parent != kInvalidNode && (near[parent] || solvedAt(parent))) {
                near[n] = 1;
            }
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const u32 parent = ParentOf(tree, *it);
            if (parent != kInvalidNode && (near[*it] || solvedAt(*it))) {
                near[parent] = 1;
            }
        }
        const std::vector<skinning::BoneMirror> mirror =
            skinning::BuildBoneMirror(tree, skinning::MirrorAxis::Y);
        for (u32 n = 0; n < mirror.size() && n < tree.size(); ++n) {
            const u32 partner = mirror[n].node;
            // Nor a limb the band left alone: it is already where the rule asks.
            // Nor a partner's clip frame, which is a pose and not a solve.
            if (partner >= tree.size() || partner == n || yours[n] || inBand[n] || near[n] ||
                turned[n] != kInvalidIndex || turned[partner] == kInvalidIndex ||
                yours.on[partner] == Held::Start) {
                continue;
            }
            // The solve's reflection of a hand-set turn, not a hand-set one.
            const TPoseJoint& from = solved[turned[partner]];
            PushJoint(result, n, MirroredTurn(from.turn),
                      from.source == TPoseSource::You ? TPoseSource::Record : from.source);
        }
        Carry(tree, order, yours, result);
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

std::vector<Quaternion> TPoseDeltaTurns(const NodeTree& tree, const TPoseResult& result) {
    std::vector<Quaternion> row(tree.size(), Quaternion::identity());
    for (const TPoseJoint& joint : result.joints) {
        if (joint.node < tree.size()) {
            row[joint.node] = joint.turn;
        }
    }
    // A row turns after its parent's composite P, a delta before it: the same
    // node turned both ways is `row * P = P * delta`.
    std::vector<Quaternion> composite(tree.size(), Quaternion::identity());
    std::vector<Quaternion> delta(tree.size(), Quaternion::identity());
    for (const u32 n : PreOrder(tree)) {
        const u32 parent = ParentOf(tree, n);
        const Quaternion above = parent == kInvalidNode ? Quaternion::identity() : composite[parent];
        composite[n] = (row[n] * above).normalized();
        delta[n] = (above.conjugate() * row[n] * above).normalized();
    }
    return delta;
}

TPoseResult TPoseFromDeltas(const Model& model, u32 slot) {
    TPoseResult result;
    const NodeTree& tree = model.nodes;
    if (slot >= model.testPoses.size()) {
        return result;
    }
    std::vector<Quaternion> composite(tree.size(), Quaternion::identity());
    for (const u32 n : PreOrder(tree)) {
        const u32 parent = ParentOf(tree, n);
        const Quaternion above = parent == kInvalidNode ? Quaternion::identity() : composite[parent];
        const NodeSkinSetup& skin = tree.nodes[n].skin;
        const Quaternion delta =
            slot < skin.poseDeltas.size() ? skin.poseDeltas[slot].rotation.normalized() : Quaternion::identity();
        composite[n] = (above * delta).normalized();
        if (std::fabs(delta.w) > 0.9999999f) {
            continue;
        }
        TPoseJoint joint;
        joint.node = n;
        joint.turn = (above * delta * above.conjugate()).normalized();
        joint.turnDeg = DegreesOf(joint.turn);
        joint.source = slot < skin.poseSources.size() ? skin.poseSources[slot] : TPoseSource::None;
        result.joints.push_back(joint);
    }
    return result;
}


// ============================================================================
// The canon, and the measurement (R§1, R§2, R§5)
// ============================================================================


TPoseCanon TPoseCanon::StrictT() {
    TPoseCanon t;
    t.armDownDeg = t.armBackDeg = t.elbowDeg = t.legOutDeg = t.legForwardDeg = t.kneeDeg = t.footOutDeg = 0;
    // A T is asked for, not measured: nothing is close enough to it to be left.
    t.spreadDeg = 0;
    return t;
}

Vector3f TPoseCanon::Direction(RigLimb limb, RigSide side, RigRole role) const {
    const f32 sign = SideSign(side);
    if (limb == RigLimb::Leg) {
        const f32 out = legOutDeg / kDeg;
        const f32 forward = legForwardDeg / kDeg;
        const Vector3f upper = Unit(
            {std::sin(forward) * std::cos(out), sign * std::sin(out), -std::cos(forward) * std::cos(out)});
        if (role == RigRole::Upper) {
            return upper;
        }
        if (role == RigRole::End) {
            const f32 heading = footOutDeg / kDeg;
            return {std::cos(heading), sign * std::sin(heading), 0};
        }
        // The shin goes back from a knee that points forward.
        return Toward(upper, {-1, 0, 0}, kneeDeg);
    }
    const f32 down = armDownDeg / kDeg;
    const f32 back = armBackDeg / kDeg;
    const Vector3f upper =
        Unit({-std::sin(back) * std::cos(down), sign * std::cos(back) * std::cos(down), -std::sin(down)});
    if (role == RigRole::Upper) {
        return upper;
    }
    // The forearm goes forward from an elbow that points back; the hand rides it.
    return Toward(upper, {1, 0, 0}, elbowDeg);
}

std::vector<const LimbDirections*> PairLimbs(std::span<const LimbDirections> figure,
                                             const RigDirections& twin) {
    std::vector<const LimbDirections*> out(figure.size(), nullptr);
    std::vector<u8> done(figure.size(), 0);
    for (std::size_t i = 0; i < figure.size(); ++i) {
        if (done[i]) {
            continue;
        }
        // One kind on one side at a time.
        std::vector<std::size_t> mine;
        for (std::size_t j = i; j < figure.size(); ++j) {
            if (figure[j].limb == figure[i].limb && figure[j].side == figure[i].side) {
                mine.push_back(j);
                done[j] = 1;
            }
        }
        std::vector<const LimbDirections*> theirs;
        for (const LimbDirections& d : twin.limbs) {
            if (d.limb == figure[i].limb && d.side == figure[i].side) {
                theirs.push_back(&d);
            }
        }
        if (theirs.empty()) {
            continue;
        }
        const auto cost = [&](std::size_t a, const LimbDirections* b) {
            // The order first; where the counts differ, the knees settle it.
            return (figure[a].ordinal == b->ordinal ? 0.0f : 1.0f) + 1e-3f * (figure[a].knee - b->knee).length();
        };
        // Past eight a side, nearest-first: no rig has that many, and the
        // exhaustive search would not end.
        if (mine.size() > 8 || theirs.size() > 8) {
            for (const std::size_t a : mine) {
                const LimbDirections* best = nullptr;
                for (const LimbDirections* b : theirs) {
                    if (best == nullptr || cost(a, b) < cost(a, best)) {
                        best = b;
                    }
                }
                out[a] = best;
            }
            continue;
        }
        // Every one-to-one assignment of the smaller group into the larger.
        std::vector<i32> pick(mine.size(), -1), bestPick = pick;
        std::vector<u8> used(theirs.size(), 0);
        f32 bestCost = std::numeric_limits<f32>::max();
        const std::size_t assign = std::min(mine.size(), theirs.size());
        const auto search = [&](auto&& self, std::size_t at, std::size_t placed, f32 sum) -> void {
            if (sum >= bestCost) {
                return;
            }
            if (at == mine.size()) {
                if (placed == assign) {
                    bestCost = sum;
                    bestPick = pick;
                }
                return;
            }
            for (std::size_t b = 0; b < theirs.size(); ++b) {
                if (!used[b]) {
                    used[b] = 1;
                    pick[at] = static_cast<i32>(b);
                    self(self, at + 1, placed + 1, sum + cost(mine[at], theirs[b]));
                    used[b] = 0;
                }
            }
            // More of mine than theirs: this one may go without.
            if (mine.size() - at > assign - placed) {
                pick[at] = -1;
                self(self, at + 1, placed, sum);
            }
        };
        search(search, 0, 0, 0.0f);
        for (std::size_t k = 0; k < mine.size(); ++k) {
            out[mine[k]] = bestPick[k] < 0 ? nullptr : theirs[static_cast<std::size_t>(bestPick[k])];
        }
    }
    return out;
}

const LimbDirections* RigDirections::Find(RigLimb limb, RigSide side, u32 ordinal) const {
    for (const LimbDirections& d : limbs) {
        if (d.limb == limb && d.side == side && d.ordinal == ordinal) {
            return &d;
        }
    }
    return nullptr;
}

RigDirections DirectionsOf(const Model& model, std::span<const Transform> rest) {
    RigDirections out;
    const NodeTree& tree = model.nodes;
    if (rest.size() < tree.size()) {
        return out;
    }
    const auto at = [&](u32 n) { return rest[n].translation; };
    f32 low = std::numeric_limits<f32>::max();
    f32 high = std::numeric_limits<f32>::lowest();
    for (u32 n = 0; n < tree.size(); ++n) {
        if (!IsJoint(tree, n)) {
            continue;
        }
        low = std::min(low, at(n).z);
        high = std::max(high, at(n).z);
        const NodeRig& rig = tree.nodes[n].rig;
        if (rig.role == RigRole::End && rig.limb != RigLimb::Other) {
            const std::optional<Limb> limb = LimbOf(tree, n);
            if (!limb) {
                continue;
            }
            LimbDirections d;
            d.end = n;
            d.lower = limb->lower;
            d.upper = limb->upper;
            d.limb = rig.limb;
            d.side = rig.side;
            for (const LimbDirections& before : out.limbs) {
                d.ordinal += before.limb == d.limb && before.side == d.side ? 1u : 0u;
            }
            d.upperDir = Unit(at(limb->lower) - at(limb->upper));
            d.lowerDir = Unit(at(limb->end) - at(limb->lower));
            // The bone the solve aims (`BoneOf`).
            const u32 toe = AimChildOf(tree, n, at);
            d.bone = toe == kInvalidNode ? Vector3f{0, 0, 0} : Unit(at(toe) - at(n));
            d.up = Rotate(rest[n].rotation, Vector3f{0, 0, 1});
            d.spread = SpreadOf(tree, n, toe == kInvalidNode ? d.lowerDir : d.bone, at);
            d.endHeight = at(n).z;
            out.limbs.push_back(d);
        }
        if (rig.role == RigRole::Head && out.head == kInvalidNode) {
            out.head = n;
        }
    }
    out.height = high > low ? high - low : 0;
    if (!out.limbs.empty()) {
        Vector3f centre{0, 0, 0};
        for (const LimbDirections& d : out.limbs) {
            centre = centre + at(d.lower);
        }
        centre = centre * (1.0f / static_cast<f32>(out.limbs.size()));
        const f32 scale = out.height > 0 ? 1.0f / out.height : 1.0f;
        for (LimbDirections& d : out.limbs) {
            d.knee = (at(d.lower) - centre) * scale;
        }
    }
    if (out.head != kInvalidNode) {
        const u32 parent = ParentOf(tree, out.head);
        if (parent != kInvalidNode) {
            out.headBone = Unit(at(out.head) - at(parent));
        }
        out.headUp = Rotate(rest[out.head].rotation, Vector3f{0, 0, 1});
        // The chain the spine rule turns, read the way it reads it.
        const std::vector<u32> chain = ChainAbove(tree, out.head, kSpineChain);
        if (!chain.empty()) {
            out.trunk = Unit(at(out.head) - at(chain.front()));
        }
    }
    // The axes the solve squares a figure by (`SquareFigure`): every limb a
    // side starts, averaged. The first Upper alone is a diagonal on a spider
    // whose right legs are numbered back to front.
    const auto axis = [&](RigLimb limb) {
        const Sides sides = SidesOf(tree, limb);
        if (!sides.both()) {
            return Vector3f{0, 0, 0};
        }
        const auto middle = [&](const std::vector<u32>& nodes) {
            Vector3f sum{0, 0, 0};
            for (const u32 n : nodes) {
                sum = sum + at(n);
            }
            return sum * (1.0f / static_cast<f32>(nodes.size()));
        };
        return Unit(middle(sides.left) - middle(sides.right));
    };
    out.hips = axis(RigLimb::Leg);
    out.shoulders = axis(RigLimb::Arm);
    return out;
}

RigDirections DirectionsAtBind(const Model& model) {
    return DirectionsOf(model, TPoseRest(model, TPoseResult{}));
}

std::vector<u32> TPoseNodes(const Document& document, u32 model) {
    std::vector<u32> out;
    if (model >= document.models.size()) {
        return out;
    }
    // `DetectRig` reads the tree alone.
    Model scratch;
    scratch.nodes = document.models[model].nodes;
    EnsureRig(scratch);
    const NodeTree& tree = scratch.nodes;
    std::vector<u8> taken(tree.size(), 0);
    const auto keep = [&](u32 node) {
        if (node < tree.size() && !taken[node]) {
            taken[node] = 1;
            out.push_back(node);
        }
    };
    const auto at = [&](u32 n) { return RestPoint(tree, n); };
    for (u32 n = 0; n < tree.size(); ++n) {
        const NodeRig& rig = tree.nodes[n].rig;
        if (rig.role != RigRole::End || rig.limb == RigLimb::Other) {
            continue;
        }
        // The limb itself, every joint from its Upper down to its End — a twist
        // between them carries the Lower, and the record names it again after
        // the reduction.
        if (const std::optional<Limb> limb = LimbOf(tree, n)) {
            for (u32 joint = limb->end, steps = tree.size(); joint != kInvalidNode && steps > 0;
                 joint = ParentOf(tree, joint), --steps) {
                keep(joint);
                if (joint == limb->upper) {
                    break;
                }
            }
        }
        // Every joint the hand's roll is read from (`SpreadOf`), the aim among
        // them.
        for (const u32 below : tree.subtree(n)) {
            const RigRole role = tree.nodes[below].rig.role;
            if (below != n && IsJoint(tree, below) && role != RigRole::Prop && role != RigRole::Pad) {
                keep(below);
            }
        }
        // And every joint child before the aim (`AimChildOf`): reduced, a
        // prop or a pad would hand its own children up to the End ahead of it.
        const u32 aim = AimChildOf(tree, n, at);
        for (const u32 child : tree.children(n)) {
            if (child == aim) {
                break;
            }
            if (IsJoint(tree, child)) {
                keep(child);
            }
        }
        // Children are listed in index order, and a reduced sibling hands its
        // own to the End: one numbered below the aim would be read first, so
        // that sibling stays too.
        for (const u32 child : tree.children(n)) {
            if (aim == kInvalidNode || child <= aim) {
                continue;
            }
            for (const u32 below : tree.subtree(child)) {
                if (below != child && below < aim && IsJoint(tree, below)) {
                    keep(child);
                    break;
                }
            }
        }
    }
    return out;
}

f32 PitchDeg(const Vector3f& direction) {
    const Vector3f u = Unit(direction);
    return u.length_squared() > 0 ? std::atan2(u.z, std::hypot(u.x, u.y)) * kDeg : 0.0f;
}

f32 DroopDeg(const Vector3f& direction) {
    return -PitchDeg(direction);
}

f32 SweepDeg(const Vector3f& direction, RigSide side) {
    const Vector3f u = Unit(direction);
    if (std::hypot(u.x, u.y) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(u.x, SideSign(side) * u.y) * kDeg;
}

f32 HeadingDeg(const Vector3f& direction) {
    const Vector3f u = Unit(direction);
    if (std::hypot(u.x, u.y) < 1e-6f) {
        return 0.0f;
    }
    return std::atan2(u.y, u.x) * kDeg;
}

f32 AngleDeg(const Vector3f& a, const Vector3f& b) {
    const Vector3f ua = Unit(a);
    const Vector3f ub = Unit(b);
    if (ua.length_squared() == 0 || ub.length_squared() == 0) {
        return 0.0f;
    }
    return std::acos(std::clamp(ua.dot(ub), -1.0f, 1.0f)) * kDeg;
}

f32 LineAngleDeg(const Vector3f& a, const Vector3f& b) {
    const f32 deg = AngleDeg(a, b);
    return std::min(deg, 180.0f - deg);
}

const char* ToString(TPoseScoreKind kind) {
    switch (kind) {
    case TPoseScoreKind::Aim:
        return "Aim";
    case TPoseScoreKind::Heading:
        return "Heading";
    case TPoseScoreKind::Level:
        return "Level";
    case TPoseScoreKind::Roll:
        return "Roll";
    case TPoseScoreKind::Mirror:
        return "Mirror";
    case TPoseScoreKind::AnkleHeight:
        return "AnkleHeight";
    case TPoseScoreKind::Head:
        return "Head";
    case TPoseScoreKind::Hips:
        return "Hips";
    }
    return "Aim";
}

TPoseScore MeasureTPose(const Model& model, std::span<const Transform> rest,
                        const TPoseCanon& canon, const RigDirections* donor) {
    TPoseScore score;
    const RigDirections dirs = DirectionsOf(model, rest);
    const auto push = [&](u32 node, RigLimb limb, RigSide side, RigRole role, TPoseScoreKind kind,
                          f32 value) {
        score.rows.push_back({node, limb, side, role, kind, value});
        if (kind != TPoseScoreKind::AnkleHeight) {
            score.worstDeg = std::max(score.worstDeg, value);
        }
    };
    const auto have = [](const Vector3f& v) { return v.length_squared() > 0; };
    const std::vector<const LimbDirections*> paired =
        donor != nullptr ? PairLimbs(dirs.limbs, *donor)
                         : std::vector<const LimbDirections*>(dirs.limbs.size(), nullptr);
    for (std::size_t i = 0; i < dirs.limbs.size(); ++i) {
        const LimbDirections& d = dirs.limbs[i];
        const LimbDirections* twin = paired[i];
        const Vector3f upperWant = twin && have(twin->upperDir)
                                       ? twin->upperDir
                                       : canon.Direction(d.limb, d.side, RigRole::Upper);
        const Vector3f lowerWant = twin && have(twin->lowerDir)
                                       ? twin->lowerDir
                                       : canon.Direction(d.limb, d.side, RigRole::Lower);
        push(d.upper, d.limb, d.side, RigRole::Upper, TPoseScoreKind::Aim, AngleDeg(d.upperDir, upperWant));
        push(d.lower, d.limb, d.side, RigRole::Lower, TPoseScoreKind::Aim, AngleDeg(d.lowerDir, lowerWant));
        if (have(d.bone)) {
            if (d.limb == RigLimb::Leg) {
                const f32 want = twin && have(twin->bone) ? HeadingDeg(twin->bone)
                                                           : SideSign(d.side) * canon.footOutDeg;
                push(d.end, d.limb, d.side, RigRole::End, TPoseScoreKind::Heading,
                     std::fabs(WrapDeg(HeadingDeg(d.bone) - want)));
            } else {
                const Vector3f want = twin && have(twin->bone) ? twin->bone : d.lowerDir;
                push(d.end, d.limb, d.side, RigRole::End, TPoseScoreKind::Aim, AngleDeg(d.bone, want));
            }
        }
        // A foot stands on the floor; a hand's own +Z is whatever its roll
        // made it, and the twin's fingers are what it is scored on.
        if (d.limb == RigLimb::Leg) {
            push(d.end, d.limb, d.side, RigRole::End, TPoseScoreKind::Level, AngleDeg(d.up, Vector3f{0, 0, 1}));
        }
        if (d.limb == RigLimb::Arm && twin != nullptr && have(d.spread) && have(twin->spread)) {
            // Both read square to the twin's hand bone, the axis the roll is about.
            const Vector3f axis = have(twin->bone) ? twin->bone : twin->lowerDir;
            const Vector3f a = d.spread - Unit(axis) * d.spread.dot(Unit(axis));
            const Vector3f b = twin->spread - Unit(axis) * twin->spread.dot(Unit(axis));
            push(d.end, d.limb, d.side, RigRole::End, TPoseScoreKind::Roll, LineAngleDeg(a, b));
        }
    }
    // The figure against itself: each left limb and the right one of its kind
    // whose knee stands nearest its own, mirrored — at a pose that is meant
    // to be symmetric, that is its partner, whatever order the rig numbers
    // its legs in.
    for (const LimbDirections& l : dirs.limbs) {
        if (l.side != RigSide::Left) {
            continue;
        }
        const LimbDirections* r = nullptr;
        for (const LimbDirections& d : dirs.limbs) {
            if (d.limb != l.limb || d.side != RigSide::Right) {
                continue;
            }
            const Vector3f mirrored{d.knee.x, -d.knee.y, d.knee.z};
            if (r == nullptr ||
                (mirrored - l.knee).length() < (Vector3f{r->knee.x, -r->knee.y, r->knee.z} - l.knee).length()) {
                r = &d;
            }
        }
        if (r == nullptr) {
            continue;
        }
        const Vector3f mirrored{r->upperDir.x, -r->upperDir.y, r->upperDir.z};
        push(l.upper, l.limb, l.side, RigRole::Upper, TPoseScoreKind::Mirror, AngleDeg(l.upperDir, mirrored));
        if (l.limb == RigLimb::Leg) {
            if (have(l.bone) && have(r->bone)) {
                push(l.end, l.limb, l.side, RigRole::End, TPoseScoreKind::Mirror,
                     std::fabs(WrapDeg(HeadingDeg(l.bone) + HeadingDeg(r->bone))));
            }
            // Against the twin's own gap, to scale, where it has both legs:
            // a creature's twin may stand one foot higher, and so should it.
            f32 want = 0;
            const LimbDirections* tl = paired[static_cast<std::size_t>(&l - dirs.limbs.data())];
            const LimbDirections* tr = paired[static_cast<std::size_t>(r - dirs.limbs.data())];
            if (tl != nullptr && tr != nullptr && donor->height > 0) {
                want = (tl->endHeight - tr->endHeight) * (dirs.height / donor->height);
            }
            push(l.end, l.limb, l.side, RigRole::End, TPoseScoreKind::AnkleHeight,
                 std::fabs(l.endHeight - r->endHeight - want));
        }
    }
    if (dirs.head != kInvalidNode && have(dirs.headBone)) {
        const Vector3f want = donor && have(donor->headBone) ? donor->headBone : Vector3f{0, 0, 1};
        push(dirs.head, RigLimb::Other, RigSide::Centre, RigRole::Head, TPoseScoreKind::Head,
             AngleDeg(dirs.headBone, want));
    }
    if (have(dirs.hips)) {
        push(kInvalidNode, RigLimb::Leg, RigSide::Centre, RigRole::Upper, TPoseScoreKind::Hips,
             LineAngleDeg(dirs.hips, donor && have(donor->hips) ? donor->hips : Vector3f{0, 1, 0}));
    }
    return score;
}

} // namespace wem
} // namespace models
} // namespace whiteout
