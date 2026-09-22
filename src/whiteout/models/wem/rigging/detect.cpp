// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/rigging/detect.h>

#include "detect_parts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

namespace whiteout {
namespace models {
namespace wem {

namespace rig_detect {

// ---- The working state -------------------------------------------------------------

Work::Work(const NodeTree& t, std::vector<NodeRig>& r) : tree(t), rig(r) {
    const u32 count = tree.size();
    stage.assign(count, 0);
    point.resize(count);
    joints.resize(count);
    names.resize(count);
    Vector3f low{std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max(),
                 std::numeric_limits<f32>::max()};
    Vector3f high{-low.x, -low.y, -low.z};
    bool any = false;
    for (u32 n = 0; n < count; ++n) {
        const Node& node = tree.nodes[n];
        // Where a marker stands at rest: the pivot in a pivot rig, the composed
        // bind in an explicit one.
        point[n] = tree.rig == RigConvention::PivotRelative ? node.pivot
                                                            : tree.worldBind(n).translation;
        names[n] = ReadName(node.name);
        if (IsJoint(n)) {
            const u32 parent = Parent(n);
            if (parent != kInvalidNode) {
                joints[parent].push_back(n);
            }
            low = Vector3f{std::min(low.x, point[n].x), std::min(low.y, point[n].y),
                           std::min(low.z, point[n].z)};
            high = Vector3f{std::max(high.x, point[n].x), std::max(high.y, point[n].y),
                            std::max(high.z, point[n].z)};
            any = true;
        }
    }
    if (any) {
        const Vector3f extent = high - low;
        size = std::max({extent.x, extent.y, extent.z, 1e-3f});
        height = std::max(extent.z, 1e-3f);
        if (extent.z < 0.05f * size) {
            height = size; // A flat rig has no height worth measuring against.
        }
    }
}

bool Work::IsJoint(u32 node) const {
    if (node >= tree.size()) {
        return false;
    }
    const NodeKind kind = tree.nodes[node].kind;
    return kind == NodeKind::Bone || kind == NodeKind::Helper;
}

u32 Work::Parent(u32 node) const {
    if (node >= tree.size()) {
        return kInvalidNode;
    }
    const u32 parent = tree.nodes[node].parent;
    return parent < tree.size() && parent != node ? parent : kInvalidNode;
}

void Work::Set(u32 node, RigRole role, RigSide side, RigLimb limb, RigSource source) {
    if (!Free(node) || role == RigRole::None) {
        return;
    }
    NodeRig& r = rig[node];
    r.role = role;
    r.side = side;
    r.limb = limb;
    r.source = source;
}

u32 Work::TopOf(u32 node) const {
    for (u32 steps = 0; node != kInvalidNode && steps <= tree.size(); ++steps) {
        if (IsTop(node)) {
            return node;
        }
        node = Parent(node);
    }
    return kInvalidNode;
}

bool Work::Under(u32 node, u32 ancestor) const {
    for (u32 steps = 0; node != kInvalidNode && steps <= tree.size(); ++steps) {
        if (node == ancestor) {
            return true;
        }
        node = Parent(node);
    }
    return false;
}

f32 Work::Tiny() const {
    return std::max(0.01f, 1e-4f * size);
}

// ---- 1. The file's own labels: WoW's key bones and limb attachments --------------

namespace {

constexpr i64 kNoKey = -1;

/// The WoW limb attachments (the M2 spec's attachment table): the End, the
/// Lower and the Upper of each limb, and which side. The hands run right then
/// left, the feet left then right.
struct WowLimb {
    u32 end;
    u32 lower;
    u32 upper;
    RigLimb limb;
    RigSide side;
};
constexpr WowLimb kWowLimbs[] = {
    {1, 3, 5, RigLimb::Arm, RigSide::Right},  {2, 4, 6, RigLimb::Arm, RigSide::Left},
    {48, 7, 9, RigLimb::Leg, RigSide::Right}, {47, 8, 10, RigLimb::Leg, RigSide::Left},
};

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return (a - b).length();
}

} // namespace

void FileTier(Work& work) {
    const NodeTree& tree = work.tree;
    const u32 count = tree.size();

    // Key bones. Measured on Corpus/WoW (4,000 creatures): `ArmL` stands on the
    // shoulder attachment in 270 of 285 rigs, so it is the upper arm, and
    // `ShoulderL` is its parent, the clavicle. The spine's two and the head
    // are what they say; `Root` moves the whole model.
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsJoint(n)) {
            continue;
        }
        switch (tree.nodes[n].native.value("keyBoneId", kNoKey)) {
        case 0:
            work.Set(n, RigRole::Upper, RigSide::Left, RigLimb::Arm, RigSource::File);
            break;
        case 1:
            work.Set(n, RigRole::Upper, RigSide::Right, RigLimb::Arm, RigSource::File);
            break;
        case 2:
            work.Set(n, RigRole::Clavicle, RigSide::Left, RigLimb::Arm, RigSource::File);
            break;
        case 3:
            work.Set(n, RigRole::Clavicle, RigSide::Right, RigLimb::Arm, RigSource::File);
            break;
        case 4:
        case 5:
            work.Set(n, RigRole::Spine, RigSide::Centre, RigLimb::Other, RigSource::File);
            break;
        case 6:
            work.Set(n, RigRole::Head, RigSide::Centre, RigLimb::Other, RigSource::File);
            break;
        case 26:
            if (work.IsRoot(n)) {
                work.Set(n, RigRole::Body, RigSide::Centre, RigLimb::Other, RigSource::File);
            }
            break;
        default:
            break;
        }
    }

    // Limb attachments. Nearly every one is parked on a leaf bone of its own
    // (1,772 of the 1,807 anchors measured have no children), so the End is
    // the anchor's parent when the anchor is a leaf and the anchor otherwise,
    // the Lower is the joint above it nearest the elbow or knee attachment,
    // and the Upper the one above that nearest the shoulder or hip attachment.
    // Both come from the limb's own joints: a hip attachment parked on the
    // waist (Azshara) or the root (the Bloodtotem taurens) is still no reason
    // to bend both legs at once. Anything between the Lower and the End is a
    // Hock on a leg and a Twist on an arm.
    struct Found {
        u32 anchor = kInvalidNode;
        Vector3f at{0, 0, 0};
    };
    std::vector<Found> byId(64);
    for (u32 n = 0; n < count; ++n) {
        const Node& node = tree.nodes[n];
        if (node.kind != NodeKind::Attachment) {
            continue;
        }
        const i64 id = node.native.value("m2AttachmentId", kNoKey);
        if (id < 0 || id >= static_cast<i64>(byId.size())) {
            continue;
        }
        byId[static_cast<std::size_t>(id)] = Found{node.parent < count ? node.parent : kInvalidNode,
                                                   work.point[n]};
    }
    const auto endOf = [&](const WowLimb& limb) {
        const u32 anchor = byId[limb.end].anchor;
        if (!work.IsJoint(anchor)) {
            return kInvalidNode;
        }
        return work.joints[anchor].empty() && work.Parent(anchor) != kInvalidNode ? work.Parent(anchor)
                                                                                 : anchor;
    };
    std::vector<u32> ends;
    for (const WowLimb& limb : kWowLimbs) {
        ends.push_back(endOf(limb));
    }
    for (const WowLimb& limb : kWowLimbs) {
        const u32 end = endOf(limb);
        if (end == kInvalidNode) {
            continue;
        }
        // Up to where this limb meets another: a joint above two Ends is theirs
        // together.
        const auto shared = [&](u32 joint) {
            for (const u32 other : ends) {
                if (other != kInvalidNode && other != end && work.Under(other, joint)) {
                    return true;
                }
            }
            return false;
        };
        std::vector<u32> above;
        for (u32 up = work.Parent(end); up != kInvalidNode && !shared(up); up = work.Parent(up)) {
            above.push_back(up);
        }
        const auto nearest = [&](const std::vector<u32>& among, const Vector3f& to) {
            u32 best = kInvalidNode;
            f32 bestDistance = std::numeric_limits<f32>::max();
            for (const u32 node : among) {
                const f32 d = Distance(work.point[node], to);
                if (d < bestDistance) {
                    best = node;
                    bestDistance = d;
                }
            }
            return best;
        };
        work.Set(end, RigRole::End, limb.side, limb.limb, RigSource::File);
        const Found& lowerAt = byId[limb.lower];
        if (lowerAt.anchor == kInvalidNode || above.empty()) {
            continue;
        }
        const u32 lower = nearest(above, lowerAt.at);
        const auto lowerPlace = std::find(above.begin(), above.end(), lower);
        const std::vector<u32> higher(lowerPlace + 1, above.end());
        const Found& upperAt = byId[limb.upper];
        const u32 upper = upperAt.anchor != kInvalidNode ? nearest(higher, upperAt.at)
                          : higher.empty()               ? kInvalidNode
                                                         : higher.front();
        work.Set(lower, RigRole::Lower, limb.side, limb.limb, RigSource::File);
        if (upper != kInvalidNode) {
            work.Set(upper, RigRole::Upper, limb.side, limb.limb, RigSource::File);
        }
        for (auto it = above.begin(); it != lowerPlace; ++it) {
            work.Set(*it, limb.limb == RigLimb::Leg ? RigRole::Hock : RigRole::Twist, limb.side,
                     limb.limb, RigSource::File);
        }
    }
}

// ---- 3. Attachment points -----------------------------------------------------------

namespace {

/// @p name with everything but its letters dropped, lower-cased: `Hand Left
/// Ref`, `Ref_Hand Left` and `@hp_lefthand` all come down to one spelling.
std::string Letters(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

struct EndMark {
    RigLimb limb = RigLimb::Other;
    RigSide side = RigSide::Centre;
};

/// Where an attachment's name says a limb ends: Warcraft III's `Hand Left
/// Ref`, StarCraft II's `Ref_Hand Left`, Diablo III's `@hp_lefthand`.
EndMark EndOfAttachment(const std::string& name) {
    const std::string letters = Letters(name);
    const auto starts = [&](const char* prefix) { return letters.rfind(prefix, 0) == 0; };
    struct Row {
        const char* hand;
        RigLimb limb;
        RigSide side;
    };
    constexpr Row kRows[] = {
        {"handleftref", RigLimb::Arm, RigSide::Left},   {"handrightref", RigLimb::Arm, RigSide::Right},
        {"footleftref", RigLimb::Leg, RigSide::Left},   {"footrightref", RigLimb::Leg, RigSide::Right},
        {"refhandleft", RigLimb::Arm, RigSide::Left},   {"refhandright", RigLimb::Arm, RigSide::Right},
        {"reffootleft", RigLimb::Leg, RigSide::Left},   {"reffootright", RigLimb::Leg, RigSide::Right},
        {"hplefthand", RigLimb::Arm, RigSide::Left},    {"hprighthand", RigLimb::Arm, RigSide::Right},
        {"hpleftfoot", RigLimb::Leg, RigSide::Left},    {"hprightfoot", RigLimb::Leg, RigSide::Right},
    };
    for (const Row& row : kRows) {
        if (starts(row.hand)) {
            return {row.limb, row.side};
        }
    }
    return {};
}

} // namespace

void AttachmentTier(Work& work) {
    const NodeTree& tree = work.tree;
    for (u32 n = 0; n < tree.size(); ++n) {
        if (tree.nodes[n].kind != NodeKind::Attachment) {
            continue;
        }
        const EndMark mark = EndOfAttachment(tree.nodes[n].name);
        if (mark.limb == RigLimb::Other) {
            continue;
        }
        // On the bone it rides, or past a weapon, a helper or a passenger the
        // attachment sits on to the nearest bone that is the limb.
        u32 at = work.Parent(n);
        while (at != kInvalidNode &&
               (tree.nodes[at].kind != NodeKind::Bone || work.rig[at].role == RigRole::Prop ||
                work.rig[at].role == RigRole::Twist)) {
            at = work.Parent(at);
        }
        if (at == kInvalidNode) {
            continue;
        }
        // A numbered joint from three on is only a guess until the End is
        // known, and an attachment on it says it is the End: a leg with no
        // `bone_leg` ends on its `leg_04`. Unless the limb already ends below
        // it: the HD gnolls park the foot attachment on `leg_03`, above their
        // own `bone_leg`.
        NodeRig& rig = work.rig[at];
        bool endBelow = false;
        for (const u32 below : tree.subtree(at)) {
            endBelow = endBelow || (below != at && work.rig[below].role == RigRole::End);
        }
        if (endBelow) {
            continue;
        }
        if (rig.source == RigSource::Name && rig.role == RigRole::Hock && work.names[at].index >= 3) {
            rig.source = RigSource::None;
        }
        work.Set(at, RigRole::End, mark.side, mark.limb, RigSource::Attachment);
    }
}

} // namespace rig_detect

// ---- The Body, the limbs' kinds, the sides, and what rides what -------------------

namespace {

using rig_detect::Work;

bool IsLimbRole(RigRole role) {
    switch (role) {
    case RigRole::Clavicle:
    case RigRole::Upper:
    case RigRole::Lower:
    case RigRole::Hock:
    case RigRole::End:
    case RigRole::Toe:
    case RigRole::Twist:
    case RigRole::Pad:
        return true;
    default:
        return false;
    }
}

/// A joint the detection may still write: nobody set it by hand.
bool Detected(const NodeRig& rig) {
    return rig.source != RigSource::You;
}

/// Whether @p node's subtree holds a hand or a foot.
bool CarriesEnd(const Work& work, u32 node) {
    for (const u32 below : work.tree.subtree(node)) {
        if (work.rig[below].role == RigRole::End) {
            return true;
        }
    }
    return false;
}

/// The Body (§3.4), among the figures' tops.
///
/// First the stages: a root named `root` whose children are bodies — every
/// child carrying a hand or a foot is body-named or sits on one's point — is
/// only where the figures stand, HD's `root_bind_jnt` under a pelvis and a
/// turret. SD Arthas's `Root` is no stage: its `Bone_Chest` carries the arms,
/// so moving the pelvis alone would tear the waist, and the Root is the Body.
/// Then a body name off any top is spine; a top with limbs on a Body top's
/// point joins it (the mount's spine beside its pelvis); and a rig with limbs
/// and no Body at all moves with the top holding most of them.
void BodyTier(Work& work) {
    const u32 count = work.tree.size();
    const f32 near = 0.01f * work.size;
    for (u32 r = 0; r < count; ++r) {
        if (!work.IsRoot(r) || !work.names[r].stage || !Detected(work.rig[r])) {
            continue;
        }
        std::vector<u32> bodies;
        for (const u32 child : work.joints[r]) {
            if (work.names[child].body && !work.names[child].stage) {
                bodies.push_back(child);
            }
        }
        bool stage = !bodies.empty();
        for (const u32 child : work.joints[r]) {
            if (!stage) {
                break;
            }
            if (work.names[child].body || !CarriesEnd(work, child)) {
                continue;
            }
            bool beside = false;
            for (const u32 body : bodies) {
                beside = beside || (work.point[body] - work.point[child]).length() <= near;
            }
            stage = beside;
        }
        work.stage[r] = stage ? 1 : 0;
    }
    for (u32 n = 0; n < count; ++n) {
        NodeRig& rig = work.rig[n];
        if (rig.role != RigRole::Body || !Detected(rig)) {
            continue;
        }
        if (work.stage[n]) {
            rig = NodeRig{RigRole::None, RigSide::Centre, RigLimb::Other, rig.ridesWith, rig.plant,
                          rig.twistShare, RigSource::None, rig.ridesSource};
        } else if (!work.IsTop(n)) {
            rig.role = RigRole::Spine;
        }
    }

    std::vector<u32> bodies;
    for (u32 n = 0; n < count; ++n) {
        if (work.IsTop(n) && work.rig[n].role == RigRole::Body) {
            bodies.push_back(n);
        }
    }
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsTop(n) || work.rig[n].role == RigRole::Body || !Detected(work.rig[n]) ||
            !CarriesEnd(work, n)) {
            continue;
        }
        for (const u32 body : bodies) {
            if (work.names[body].mount == work.names[n].mount &&
                (work.point[body] - work.point[n]).length() <= near) {
                NodeRig& rig = work.rig[n];
                rig.role = RigRole::Body;
                rig.side = RigSide::Centre;
                rig.limb = RigLimb::Other;
                rig.source = work.rig[body].source;
                break;
            }
        }
    }
    bool anyBody = !bodies.empty();
    for (u32 n = 0; n < count && !anyBody; ++n) {
        anyBody = work.IsTop(n) && work.rig[n].role == RigRole::Body;
    }
    if (!anyBody) {
        u32 best = kInvalidNode;
        u32 bestEnds = 0;
        for (u32 n = 0; n < count; ++n) {
            if (!work.IsTop(n) || !work.Free(n)) {
                continue;
            }
            u32 ends = 0;
            for (const u32 below : work.tree.subtree(n)) {
                ends += work.rig[below].role == RigRole::End ? 1 : 0;
            }
            if (ends > bestEnds) {
                best = n;
                bestEnds = ends;
            }
        }
        if (best != kInvalidNode) {
            work.Set(best, RigRole::Body, RigSide::Centre, RigLimb::Other, RigSource::Shape);
        }
    }
}

/// Which figure each top is: Body tops on one point, of one tag, are one
/// figure (the pelvis and the turret); every other top is its own.
std::vector<u32> FigureOfTops(const Work& work) {
    const u32 count = work.tree.size();
    std::vector<u32> figure(count, kInvalidNode);
    const f32 near = 0.05f * work.size;
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsTop(n) || work.rig[n].role != RigRole::Body) {
            continue;
        }
        figure[n] = n;
        for (u32 m = 0; m < n; ++m) {
            if (figure[m] != kInvalidNode && work.names[m].mount == work.names[n].mount &&
                (work.point[m] - work.point[n]).length() <= near) {
                figure[n] = figure[m];
                break;
            }
        }
    }
    return figure;
}

/// P1: on a figure whose arm Ends stand level with its leg Ends at rest, within
/// 15% of the figure's height, the arms are legs — a horse's or a wolf's front
/// legs, named `arm_01..03` by the rig.
void LimbKinds(Work& work) {
    const u32 count = work.tree.size();
    const std::vector<u32> figure = FigureOfTops(work);
    std::vector<u32> of(count, kInvalidNode);
    bool anyFigure = false;
    for (u32 n = 0; n < count; ++n) {
        const u32 top = work.TopOf(n);
        if (top != kInvalidNode) {
            of[n] = figure[top];
            anyFigure = anyFigure || of[n] != kInvalidNode;
        }
    }
    // With no Body the whole rig is one figure.
    if (!anyFigure) {
        for (u32 n = 0; n < count; ++n) {
            if (work.IsJoint(n)) {
                of[n] = 0;
            }
        }
    }
    for (u32 f = 0; f < count; ++f) {
        f32 armZ = 0.0f;
        f32 legZ = 0.0f;
        u32 arms = 0;
        u32 legs = 0;
        f32 low = std::numeric_limits<f32>::max();
        f32 high = -low;
        for (u32 n = 0; n < count; ++n) {
            if (of[n] != f) {
                continue;
            }
            low = std::min(low, work.point[n].z);
            high = std::max(high, work.point[n].z);
            if (work.rig[n].role != RigRole::End) {
                continue;
            }
            if (work.rig[n].limb == RigLimb::Arm) {
                armZ += work.point[n].z;
                ++arms;
            } else if (work.rig[n].limb == RigLimb::Leg) {
                legZ += work.point[n].z;
                ++legs;
            }
        }
        if (arms == 0 || legs == 0) {
            continue;
        }
        const f32 tall = std::max(high - low, 1e-3f);
        if (std::abs(armZ / static_cast<f32>(arms) - legZ / static_cast<f32>(legs)) > 0.15f * tall) {
            continue;
        }
        for (u32 n = 0; n < count; ++n) {
            NodeRig& rig = work.rig[n];
            if (of[n] == f && rig.limb == RigLimb::Arm && Detected(rig) && IsLimbRole(rig.role)) {
                rig.limb = RigLimb::Leg;
            }
        }
    }
}

/// A limb joint no source gave a side takes the one its place across the model
/// says: +Y is left in the Blizzard space.
void SidesByPlace(Work& work) {
    const f32 near = 0.01f * work.size;
    for (u32 n = 0; n < work.tree.size(); ++n) {
        NodeRig& rig = work.rig[n];
        if (!IsLimbRole(rig.role) || rig.side != RigSide::Centre || !Detected(rig)) {
            continue;
        }
        const f32 y = work.point[n].y;
        if (std::abs(y) > near) {
            rig.side = y > 0.0f ? RigSide::Left : RigSide::Right;
        }
    }
}

/// The node among @p roles nearest @p to, outside the subtree of @p outside:
/// of the figure @p mount says, or of either when @p anyTag. With @p ownTop the
/// node's top must be of that figure too — a mount's spine is a mount figure's,
/// and a `mount_bone_chest` inside a rider-named body is no mount.
u32 NearestWithRole(const Work& work, std::initializer_list<RigRole> roles, bool mount,
                    const Vector3f& to, u32 outside, bool ownTop = false) {
    u32 best = kInvalidNode;
    f32 bestDistance = std::numeric_limits<f32>::max();
    for (u32 n = 0; n < work.tree.size(); ++n) {
        if (!work.IsJoint(n) || work.names[n].mount != mount || work.Under(n, outside)) {
            continue;
        }
        if (std::find(roles.begin(), roles.end(), work.rig[n].role) == roles.end()) {
            continue;
        }
        if (ownTop) {
            const u32 top = work.TopOf(n);
            if (top == kInvalidNode || work.names[top].mount != mount) {
                continue;
            }
        }
        const f32 d = (work.point[n] - to).length();
        if (d < bestDistance) {
            best = n;
            bestDistance = d;
        }
    }
    return best;
}

void Ride(Work& work, u32 rider, u32 host, RigSource source) {
    if (host == kInvalidNode || host == rider) {
        return;
    }
    work.rig[rider].ridesWith = host;
    work.rig[rider].ridesSource = source;
}

/// Rides by the clips (§3.4): each top not placed yet rides the node whose
/// offset to it varies least over every clip's key times, when that is under
/// 1% of the rig's height and 2°.
void RidesByClips(Work& work, const RigPoses& poses, const std::vector<u32>& riders) {
    const u32 count = work.tree.size();
    const u32 clips = poses.clipCount();
    struct Seen {
        bool first = true;
        Matrix44f reference = Matrix44f::identity();
        f32 moved = 0.0f;
        f32 turned = 0.0f;
    };
    std::vector<std::vector<Seen>> seen(riders.size(), std::vector<Seen>(count));
    bool sampled = false;
    for (u32 clip = 0; clip < clips; ++clip) {
        std::vector<f32> times = poses.keyTimes(clip);
        if (times.size() > kRigSamplesPerClip) {
            std::vector<f32> kept;
            for (u32 i = 0; i < kRigSamplesPerClip; ++i) {
                kept.push_back(times[i * (times.size() - 1) / (kRigSamplesPerClip - 1)]);
            }
            times = std::move(kept);
        }
        for (const f32 ms : times) {
            const std::vector<Matrix44f> frames = poses.worldAt(clip, ms);
            if (frames.size() < count) {
                continue;
            }
            sampled = true;
            for (std::size_t r = 0; r < riders.size(); ++r) {
                const u32 rider = riders[r];
                for (u32 host = 0; host < count; ++host) {
                    if (!work.IsJoint(host) || work.stage[host] || work.Under(host, rider)) {
                        continue;
                    }
                    // The rider's frame in the host's: what riding holds still.
                    const Matrix44f offset = frames[rider] * Matrix44f::inverse(frames[host]);
                    Seen& s = seen[r][host];
                    if (s.first) {
                        s.first = false;
                        s.reference = offset;
                        continue;
                    }
                    const Vector3f a{s.reference.data[3][0], s.reference.data[3][1],
                                     s.reference.data[3][2]};
                    const Vector3f b{offset.data[3][0], offset.data[3][1], offset.data[3][2]};
                    s.moved = std::max(s.moved, (a - b).length());
                    const Quaternion qa = FromMatrix(s.reference).rotation.normalized();
                    const Quaternion qb = FromMatrix(offset).rotation.normalized();
                    const f32 dot = std::min(1.0f, std::abs(qa.x * qb.x + qa.y * qb.y +
                                                            qa.z * qb.z + qa.w * qb.w));
                    s.turned = std::max(s.turned, 2.0f * std::acos(dot) * 57.29577951308232f);
                }
            }
        }
    }
    if (!sampled) {
        return;
    }
    const f32 still = 0.01f * work.height;
    for (std::size_t r = 0; r < riders.size(); ++r) {
        u32 best = kInvalidNode;
        f32 bestScore = std::numeric_limits<f32>::max();
        for (u32 host = 0; host < count; ++host) {
            const Seen& s = seen[r][host];
            if (s.first || s.moved >= still || s.turned >= 2.0f) {
                continue;
            }
            const f32 score = s.moved / std::max(still, 1e-6f) + s.turned / 2.0f;
            if (score < bestScore) {
                best = host;
                bestScore = score;
            }
        }
        Ride(work, riders[r], best, RigSource::Shape);
    }
}

/// What rides what (§3.4), for the tops not set by hand: by names, then by the
/// clips.
void Rides(Work& work, const RigPoses& poses) {
    const u32 count = work.tree.size();
    // A mount is a figure of its own: a `mount_` top that is its Body or spine.
    bool mountFigure = false;
    for (u32 n = 0; n < count; ++n) {
        mountFigure = mountFigure || (work.IsTop(n) && work.names[n].mount &&
                                      (work.rig[n].role == RigRole::Body ||
                                       work.rig[n].role == RigRole::Spine));
    }
    std::vector<u32> byClips;
    for (u32 n = 0; n < count; ++n) {
        if (!work.IsTop(n) || work.rig[n].ridesSource != RigSource::None) {
            continue;
        }
        const rig_detect::NameRead& name = work.names[n];
        const NodeRig& rig = work.rig[n];
        if (name.face) {
            // A face rig's root rides its figure's head, or its neck when the
            // figure has no head bone (the HD Knight's rider). A horse with no
            // rider names its face without the `mount_` its bones carry
            // (`knightnorider`), so failing its own figure it rides the other's;
            // and a creature with neither (a revenant) rides its body.
            u32 host = kInvalidNode;
            for (const bool mount : {name.mount, !name.mount}) {
                for (const RigRole role : {RigRole::Head, RigRole::Neck}) {
                    if (host == kInvalidNode) {
                        host = NearestWithRole(work, {role}, mount, work.point[n], n);
                    }
                }
            }
            if (host == kInvalidNode) {
                host = NearestWithRole(work, {RigRole::Spine, RigRole::Body}, name.mount, work.point[n], n);
            }
            Ride(work, n, host, RigSource::Name);
        } else if (rig.role == RigRole::Body) {
            // A rider's Body rides the mount's spine or Body nearest it.
            if (!name.mount && mountFigure) {
                Ride(work, n,
                     NearestWithRole(work, {RigRole::Spine, RigRole::Body}, true, work.point[n], n, true),
                     RigSource::Name);
            }
        } else if (rig.role == RigRole::Prop) {
            // A weapon, a shield or a book rides the hand nearest it at rest.
            u32 best = kInvalidNode;
            f32 bestDistance = std::numeric_limits<f32>::max();
            for (u32 end = 0; end < count; ++end) {
                if (work.rig[end].role != RigRole::End || work.rig[end].limb != RigLimb::Arm ||
                    work.Under(end, n)) {
                    continue;
                }
                const f32 d = (work.point[end] - work.point[n]).length();
                if (d < bestDistance) {
                    best = end;
                    bestDistance = d;
                }
            }
            Ride(work, n, best, RigSource::Name);
        } else {
            byClips.push_back(n);
        }
    }
    if (!byClips.empty() && !poses.empty()) {
        RidesByClips(work, poses, byClips);
    }
}

} // namespace

bool HasRig(const Model& model) {
    for (const Node& node : model.nodes.nodes) {
        if (!node.rig.empty()) {
            return true;
        }
    }
    return false;
}

void DetectRig(Model& model, const RigDetectOptions& options) {
    NodeTree& tree = model.nodes;
    const u32 count = tree.size();
    std::vector<NodeRig> rig(count);
    for (u32 n = 0; n < count; ++n) {
        const NodeRig& had = tree.nodes[n].rig;
        NodeRig& fresh = rig[n];
        // What nothing detects stays as it was (the plan's P2).
        fresh.plant = had.plant;
        fresh.twistShare = had.twistShare;
        if (options.keepYours && had.source == RigSource::You) {
            fresh.role = had.role;
            fresh.side = had.side;
            fresh.limb = had.limb;
            fresh.source = had.source;
        }
        if (options.keepYours && had.ridesSource == RigSource::You) {
            fresh.ridesWith = had.ridesWith;
            fresh.ridesSource = had.ridesSource;
        }
    }

    rig_detect::Work work(tree, rig);
    rig_detect::FileTier(work);
    rig_detect::NameTier(work);
    rig_detect::AttachmentTier(work);
    rig_detect::NumberedTier(work);
    rig_detect::LineTier(work);
    rig_detect::ShapeTier(work);
    BodyTier(work);
    LimbKinds(work);
    SidesByPlace(work);
    Rides(work, options.poses);

    for (u32 n = 0; n < count; ++n) {
        tree.nodes[n].rig = rig[n];
    }
}

bool EnsureRig(Model& model, const RigDetectOptions& options) {
    if (HasRig(model)) {
        return false;
    }
    DetectRig(model, options);
    return true;
}

} // namespace wem
} // namespace models
} // namespace whiteout
