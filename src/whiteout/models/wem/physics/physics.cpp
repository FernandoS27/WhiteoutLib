// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/physics.h>

#include <whiteout/models/wem/anim/curve_keys.h>
#include <whiteout/models/wem/document.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "bake_record.h"

namespace whiteout {
namespace models {
namespace wem {

const char* ToString(PhysicsShapeKind kind) {
    switch (kind) {
    case PhysicsShapeKind::Box:
        return "box";
    case PhysicsShapeKind::Sphere:
        return "sphere";
    case PhysicsShapeKind::Capsule:
        return "capsule";
    case PhysicsShapeKind::Cylinder:
        return "cylinder";
    case PhysicsShapeKind::ConvexHull:
        return "convexHull";
    case PhysicsShapeKind::TriangleMesh:
        return "triangleMesh";
    case PhysicsShapeKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(BodyMotion motion) {
    switch (motion) {
    case BodyMotion::Dynamic:
        return "dynamic";
    case BodyMotion::Kinematic:
        return "kinematic";
    case BodyMotion::Static:
        return "static";
    case BodyMotion::Count:
        break;
    }
    return "invalid";
}

const char* ToString(JointKind kind) {
    switch (kind) {
    case JointKind::Spherical:
        return "spherical";
    case JointKind::Revolute:
        return "revolute";
    case JointKind::ConeTwist:
        return "coneTwist";
    case JointKind::Weld:
        return "weld";
    case JointKind::Prismatic:
        return "prismatic";
    case JointKind::Distance:
        return "distance";
    case JointKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(ClothCageKind kind) {
    switch (kind) {
    case ClothCageKind::Auto:
        return "auto";
    case ClothCageKind::AsModelled:
        return "as_modelled";
    case ClothCageKind::Grid:
        return "grid";
    case ClothCageKind::Strip:
        return "strip";
    case ClothCageKind::Reduced:
        return "reduced";
    case ClothCageKind::FromFaces:
        return "from_faces";
    case ClothCageKind::Count:
        break;
    }
    return "invalid";
}

const char* ToString(JointMotorMode mode) {
    switch (mode) {
    case JointMotorMode::Off:
        return "off";
    case JointMotorMode::Position:
        return "position";
    case JointMotorMode::Velocity:
        return "velocity";
    case JointMotorMode::Count:
        break;
    }
    return "invalid";
}

const char* ToString(WowPhysicsKind kind) {
    switch (kind) {
    case WowPhysicsKind::WornItem:
        return "wornItem";
    case WowPhysicsKind::Vegetation:
        return "vegetation";
    case WowPhysicsKind::Ragdoll:
        return "ragdoll";
    case WowPhysicsKind::PrivateWorld:
        return "privateWorld";
    case WowPhysicsKind::Count:
        break;
    }
    return "invalid";
}

namespace {

Vector3f Row(const Matrix44f& m, int r) {
    return Vector3f{m.data[r][0], m.data[r][1], m.data[r][2]};
}

bool IsIdentity(const Matrix44f& m) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (m.data[r][c] != (r == c ? 1.0f : 0.0f)) {
                return false;
            }
        }
    }
    return true;
}

/// @p p through the row-vector matrix @p m.
Vector3f Through(const Matrix44f& m, const Vector3f& p) {
    return Vector3f{p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
                    p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
                    p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
}

} // namespace

std::pair<Vector3f, Vector3f> CapsuleEnds(const PhysicsShape& shape) {
    if (shape.points.size() == 2) {
        if (IsIdentity(shape.transform)) {
            return {shape.points[0], shape.points[1]};
        }
        return {Through(shape.transform, shape.points[0]), Through(shape.transform, shape.points[1])};
    }
    const Vector3f o = Row(shape.transform, 3);
    const Vector3f z = Row(shape.transform, 2);
    const f32 h = shape.length * 0.5f;
    const Vector3f e{z.x * h, z.y * h, z.z * h};
    return {Vector3f{o.x - e.x, o.y - e.y, o.z - e.z}, Vector3f{o.x + e.x, o.y + e.y, o.z + e.z}};
}

std::vector<Vector3f> CylinderPrism(const PhysicsShape& shape) {
    constexpr int kSides = 16;
    const f64 turn = 6.283185307179586;
    const f64 wide = shape.radius * std::sqrt(3.141592653589793 / (0.5 * kSides * std::sin(turn / kSides)));
    const f32 half = shape.length * 0.5f;
    std::vector<Vector3f> points;
    points.reserve(kSides * 2);
    for (int k = 0; k < kSides; ++k) {
        const f64 a = turn * k / kSides;
        const f32 x = static_cast<f32>(wide * std::cos(a)), y = static_cast<f32>(wide * std::sin(a));
        points.push_back(Vector3f{x, y, half});
        points.push_back(Vector3f{x, y, -half});
    }
    return points;
}

Matrix44f CapsuleFrame(const Vector3f& a, const Vector3f& b, f32& length) {
    const f64 dx = static_cast<f64>(b.x) - a.x;
    const f64 dy = static_cast<f64>(b.y) - a.y;
    const f64 dz = static_cast<f64>(b.z) - a.z;
    const f64 d = std::sqrt(dx * dx + dy * dy + dz * dz);
    length = static_cast<f32>(d);
    const f64 z[3] = {d > 0.0 ? dx / d : 0.0, d > 0.0 ? dy / d : 0.0, d > 0.0 ? dz / d : 1.0};
    // Any axis far from +Z completes the frame; a capsule has no roll.
    const f64 helper[3] = {std::fabs(z[0]) < 0.9 ? 1.0 : 0.0, std::fabs(z[0]) < 0.9 ? 0.0 : 1.0, 0.0};
    f64 x[3] = {helper[1] * z[2] - helper[2] * z[1], helper[2] * z[0] - helper[0] * z[2],
                helper[0] * z[1] - helper[1] * z[0]};
    const f64 xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (f64& c : x) {
        c /= xl;
    }
    const f64 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Matrix44f m = Matrix44f::identity();
    for (int c = 0; c < 3; ++c) {
        m.data[0][c] = static_cast<f32>(x[c]);
        m.data[1][c] = static_cast<f32>(y[c]);
        m.data[2][c] = static_cast<f32>(z[c]);
    }
    m.data[3][0] = static_cast<f32>((static_cast<f64>(a.x) + b.x) * 0.5);
    m.data[3][1] = static_cast<f32>((static_cast<f64>(a.y) + b.y) * 0.5);
    m.data[3][2] = static_cast<f32>((static_cast<f64>(a.z) + b.z) * 0.5);
    return m;
}

bool CollidesCloth(const PhysicsShape& shape) {
    return shape.kind == PhysicsShapeKind::Capsule || shape.kind == PhysicsShapeKind::Sphere;
}

namespace {

/// @p collider made @p shape's: centred on its ends, as `PHCC` states one, and
/// as wide as the shape's matrix makes it across.
void TakeShape(ClothCollider& collider, const PhysicsBody& body, const PhysicsShape& shape) {
    collider.node = body.node;
    collider.kind = ClothColliderKind::Capsule;
    const Vector3f x = Row(shape.transform, 0), y = Row(shape.transform, 1);
    collider.radius = shape.radius * std::max(std::sqrt(x.x * x.x + x.y * x.y + x.z * x.z),
                                              std::sqrt(y.x * y.x + y.y * y.y + y.z * y.z));
    if (shape.kind == PhysicsShapeKind::Capsule) {
        const auto [a, b] = CapsuleEnds(shape);
        collider.transform = CapsuleFrame(a, b, collider.length);
    } else {
        collider.transform = Matrix44f::identity();
        for (int c = 0; c < 3; ++c) {
            collider.transform.data[3][c] = shape.transform.data[3][c];
        }
        collider.length = 0.0f;
    }
}

} // namespace

ClothCollider ColliderOfShape(const PhysicsBody& body, u32 shape) {
    ClothCollider collider;
    collider.body = body.id;
    collider.shape = shape;
    if (shape < body.shapes.size()) {
        TakeShape(collider, body, body.shapes[shape]);
    }
    return collider;
}

const ClothCollider* ShapeCollider(const PhysicsSet& physics, u32 body, u32 shape) {
    for (const ClothCollider& collider : physics.colliders) {
        if (body != 0 && collider.body == body && collider.shape == shape) {
            return &collider;
        }
    }
    return nullptr;
}

bool FollowShapes(PhysicsSet& physics) {
    bool changed = false;
    std::vector<u32> gone;
    for (ClothCollider& collider : physics.colliders) {
        if (collider.body == 0) {
            continue;
        }
        const PhysicsBody* body = physics.body(collider.body);
        if (body == nullptr || collider.shape >= body->shapes.size() || !CollidesCloth(body->shapes[collider.shape])) {
            gone.push_back(collider.id);
            continue;
        }
        ClothCollider followed = collider;
        TakeShape(followed, *body, body->shapes[collider.shape]);
        if (followed.node != collider.node || followed.radius != collider.radius ||
            followed.length != collider.length || std::memcmp(&followed.transform, &collider.transform, sizeof(Matrix44f)) != 0) {
            collider = followed;
            changed = true;
        }
    }
    if (gone.empty()) {
        return changed;
    }
    const auto isGone = [&](u32 id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
    std::erase_if(physics.colliders, [&](const ClothCollider& collider) { return isGone(collider.id); });
    for (Cloth& cloth : physics.cloths) {
        std::erase_if(cloth.colliders, isGone);
    }
    for (SoftBody& soft : physics.soft) {
        std::erase_if(soft.colliders, isGone);
    }
    return true;
}

void ShapeErased(PhysicsSet& physics, u32 body, u32 shape) {
    for (ClothCollider& collider : physics.colliders) {
        if (body == 0 || collider.body != body) {
            continue;
        }
        if (collider.shape == shape) {
            collider.shape = kInvalidIndex;
        } else if (collider.shape > shape && collider.shape != kInvalidIndex) {
            --collider.shape;
        }
    }
}

namespace {

template <class List>
auto FindById(List& list, u32 id) -> decltype(list.data()) {
    const auto it = std::find_if(list.begin(), list.end(), [id](const auto& r) { return r.id == id; });
    return it == list.end() ? nullptr : &*it;
}

} // namespace

const PhysicsBody* PhysicsSet::body(u32 id) const {
    return FindById(bodies, id);
}

PhysicsBody* PhysicsSet::body(u32 id) {
    return FindById(bodies, id);
}

const PhysicsJoint* PhysicsSet::joint(u32 id) const {
    return FindById(joints, id);
}

const ClothCollider* PhysicsSet::collider(u32 id) const {
    return FindById(colliders, id);
}

const Cloth* PhysicsSet::cloth(u32 id) const {
    return FindById(cloths, id);
}

Cloth* PhysicsSet::cloth(u32 id) {
    return FindById(cloths, id);
}

const PhysicsRig* PhysicsSet::rig(u32 id) const {
    return FindById(rigs, id);
}

const SoftBody* PhysicsSet::softBody(u32 id) const {
    return FindById(soft, id);
}

SoftBody* PhysicsSet::softBody(u32 id) {
    return FindById(soft, id);
}

bool PhysicsSet::hasId(u32 id) const {
    return body(id) != nullptr || joint(id) != nullptr || collider(id) != nullptr ||
           cloth(id) != nullptr || rig(id) != nullptr || softBody(id) != nullptr;
}

const PhysicsBody* PhysicsSet::firstBodyOn(u32 node) const {
    const auto it =
        std::find_if(bodies.begin(), bodies.end(), [node](const PhysicsBody& b) { return b.node == node; });
    return it == bodies.end() ? nullptr : &*it;
}

PhysicsHost PhysicsHostFor(Game game) {
    PhysicsHost host;
    switch (game) {
    case Game::StarCraft:
        // Per-map data really; this is the default the viewer's host uses.
        host.gravity = {0.0f, 0.0f, -8.8f};
        host.step = 1.0f / 60.0f;
        host.maxSubsteps = 3;
        host.velocityIterations = 8;
        host.positionIterations = 2;
        host.snapLinear = 0.25f;
        host.snapAngular = std::numbers::pi_v<f32> / 8.0f;
        host.releaseLinear = 8.0f;
        host.releaseAngular = 4.0f;
        break;
    case Game::Wow:
        host.gravity = {0.0f, 0.0f, -10.0f};
        host.maxSubsteps = 1;
        break;
    case Game::Diablo:
        host.gravity = {0.0f, 0.0f, -32.2f};
        host.maxSubsteps = 1;
        break;
    case Game::Warcraft:
        break;
    }
    return host;
}

// ============================================================================
// What a read of a `.wem` runs
// ============================================================================

using bakedetail::Hash;
using bakedetail::HashKeys;
using bakedetail::InsideHash;
using bakedetail::kEver;
using bakedetail::kStretchPad;
using bakedetail::OutsideHash;
using bakedetail::Stretch;
using bakedetail::StretchesOf;
using bakedetail::Within;
using bakedetail::WrittenHash;

namespace {

/// The stretches an older bake keyed on a node's channel, read off @p now's
/// keys: a key @p source does not hold is the bake's, with every key between
/// two of them, and a source key it left out is in the stretch nearest it.
template <class T>
std::vector<Stretch> ReadStretches(const SubTrack& now, const SubTrack* source, f32 duration) {
    const std::vector<CurveKey<T>> ours = DecodeKeys<T>(now);
    const std::vector<CurveKey<T>> theirs = source != nullptr ? DecodeKeys<T>(*source) : std::vector<CurveKey<T>>{};
    const auto held = [&](const CurveKey<T>& key) {
        return std::any_of(theirs.begin(), theirs.end(), [&](const CurveKey<T>& other) {
            return other.time == key.time && std::memcmp(&other.value, &key.value, sizeof(T)) == 0;
        });
    };
    std::vector<Stretch> out;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        if (held(ours[i])) {
            continue;
        }
        std::size_t last = i;
        while (last + 1 < ours.size() && !held(ours[last + 1])) {
            ++last;
        }
        Stretch stretch;
        stretch.from = i == 0 ? -kEver : ours[i].time - kStretchPad;
        stretch.to = last + 1 == ours.size() && ours[last].time >= duration - 0.001f ? kEver
                                                                                     : ours[last].time + kStretchPad;
        // An edge key leads the simulation's, where the clip's own go on before it.
        stretch.first = last == i ? stretch.from - 1.0f : ours[i == 0 ? i : i + 1].time;
        out.push_back(stretch);
        i = last;
    }
    for (const CurveKey<T>& key : theirs) {
        if (out.empty() || Within(out, key.time) ||
            std::any_of(ours.begin(), ours.end(), [&](const CurveKey<T>& other) { return other.time == key.time; })) {
            continue;
        }
        Stretch* nearest = &out.front();
        const auto apart = [&](const Stretch& s) { return std::max(s.from - key.time, key.time - s.to); };
        for (Stretch& s : out) {
            if (apart(s) < apart(*nearest)) {
                nearest = &s;
            }
        }
        nearest->from = std::min(nearest->from, key.time - kStretchPad);
        nearest->to = std::max(nearest->to, key.time + kStretchPad);
    }
    return out;
}

} // namespace

void MarkBakeStretches(Document& document) {
    std::vector<Stretch> stretches;
    for (Clip& clip : document.clips) {
        if (!clip.physics.has_value() || !clip.physics->baked.has_value() || clip.model >= document.models.size()) {
            continue;
        }
        PhysicsBake& record = *clip.physics->baked;
        if (record.stretchInside != 0) {
            continue;
        }
        // Keys edited since cannot be told from the bake's, channel by channel.
        const bool edited = WrittenHash(clip, record) != record.written;
        const AnimChannelTable& table = document.models[clip.model].animChannels;
        for (const u32 channel : record.channels) {
            const AnimChannel* declared = table.find(channel);
            if (declared == nullptr || declared->target.kind != TrackTarget::Kind::Node ||
                std::find(record.softChannels.begin(), record.softChannels.end(), channel) != record.softChannels.end() ||
                StretchesOf(record, channel, stretches).has_value()) {
                continue;
            }
            const bool turns =
                declared->target.channel == Channel::Rotation && declared->valueType == geom::AttrType::Quat;
            const bool moves =
                declared->target.channel == Channel::Translation && declared->valueType == geom::AttrType::F32x3;
            // One layer's keys, as `OwnAnimations` puts them back.
            const SubTrack* now = nullptr;
            const SubTrack* source = nullptr;
            u32 layers = 0;
            for (const SubTrackContainer& container : clip.containers) {
                if (const SubTrack* found = container.find(channel); found != nullptr && now == nullptr) {
                    now = found;
                }
            }
            for (const SubTrackContainer& container : record.source) {
                if (const SubTrack* found = container.find(channel); found != nullptr) {
                    source = found;
                    ++layers;
                }
            }
            // Every key of it taken away since: the bake's are gone, and what
            // it replaced is not put back under them. The track is the clip's
            // own from here on, with no stretch.
            const bool gone = edited && (now == nullptr || now->times.empty());
            if ((!turns && !moves) || layers > 1 || (!gone && (now == nullptr || now->times.empty())) ||
                (!gone && !now->wellSized(declared->valueType)) ||
                (source != nullptr && !source->wellSized(declared->valueType))) {
                continue;
            }
            stretches.clear();
            if (!gone) {
                stretches = turns ? ReadStretches<Quaternion>(*now, source, clip.duration)
                                  : ReadStretches<Vector3f>(*now, source, clip.duration);
                if (stretches.empty()) {
                    continue;
                }
            }
            record.stretchChannels.push_back(channel);
            record.stretchCounts.push_back(static_cast<u32>(stretches.size()));
            for (const Stretch& stretch : stretches) {
                record.stretchTimes.insert(record.stretchTimes.end(), {stretch.from, stretch.first, stretch.to});
            }
            // 0 is no track's keys: one with no stretch is never as the bake left it.
            record.stretchOutside.push_back(gone ? 0 : OutsideHash(clip, channel, stretches));
        }
        record.stretchInside = InsideHash(clip, record);
        // Edited before it was read: it goes on reading so.
        if (edited) {
            record.stretchInside = ~record.stretchInside != 0 ? ~record.stretchInside : 1;
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
