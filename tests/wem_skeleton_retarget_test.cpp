// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM v3 — `RetargetSkeleton`, the operation that restates a rig in another
/// convention (§10.5).
///
/// The whole contract is one identity: `inverseBind(b) * world(b, t)` — the
/// matrix a skinning palette applies — is the same before and after. Everything
/// here is therefore an evaluator plus that comparison; asserting on pivots or
/// on key counts would pass for a rig that renders wrong.
///
/// The evaluator is deliberately written out rather than reused from the
/// library: a retarget checked against the same composition it performs proves
/// only self-consistency.

#include <cmath>
#include <vector>

#include <catch2/catch_all.hpp>

#include <whiteout/models/wem/retarget.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

Vector3f RowMultiply(const Matrix44f& m, const Vector3f& v) {
    return Vector3f{v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0],
                    v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1],
                    v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2]};
}

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

const AnimChannel* FindChannel(const AnimChannelTable& table, u32 node, Channel channel) {
    for (const AnimChannel& entry : table.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            return &entry;
        }
    }
    return nullptr;
}

/// Linear between keys, clamped outside, shortest-arc for a quaternion — the
/// least a consumer of a `SubTrack` can do.
void SampleTrack(const SubTrack& track, u32 components, f32 time, f32* out) {
    const std::size_t keys = track.times.size();
    if (keys == 0) {
        return;
    }
    const u32 stride = ValuesPerKey(track.interp) * components;
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    std::size_t after = 0;
    while (after < keys && track.times[after] <= time) {
        ++after;
    }
    if (after == 0) {
        for (u32 c = 0; c < components; ++c) {
            out[c] = values[c];
        }
        return;
    }
    const std::size_t before = after - 1;
    if (after >= keys || track.interp == Interpolation::Step) {
        for (u32 c = 0; c < components; ++c) {
            out[c] = values[before * stride + c];
        }
        return;
    }
    const f32 span = track.times[after] - track.times[before];
    const f32 alpha = span > 0.0f ? (time - track.times[before]) / span : 0.0f;
    const f32* a = values + before * stride;
    const f32* b = values + after * stride;
    if (components == 4) {
        const f32 dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
        f32 length = 0.0f;
        for (u32 c = 0; c < 4; ++c) {
            out[c] = a[c] + alpha * (sign * b[c] - a[c]);
            length += out[c] * out[c];
        }
        length = std::sqrt(length);
        for (u32 c = 0; c < 4 && length > 0.0f; ++c) {
            out[c] /= length;
        }
        return;
    }
    for (u32 c = 0; c < components; ++c) {
        out[c] = a[c] + alpha * (b[c] - a[c]);
    }
}

Transform NodeTrsAt(const Model& model, const SubTrackContainer& container, u32 node, f32 time,
                    const Transform& rest) {
    Transform out = rest;
    const Channel channels[3] = {Channel::Translation, Channel::Rotation, Channel::Scale};
    for (int c = 0; c < 3; ++c) {
        const AnimChannel* channel = FindChannel(model.animChannels, node, channels[c]);
        if (channel == nullptr) {
            continue;
        }
        const u32 components = geom::AttrTypeComponents(channel->valueType);
        f32 buffer[4] = {0, 0, 0, 1};
        if (const SubTrack* track = container.find(channel->id)) {
            SampleTrack(*track, components, time, buffer);
        } else if (channel->hasInitValue()) {
            const f32* init = reinterpret_cast<const f32*>(channel->initValue.data());
            for (u32 i = 0; i < components && i < 4; ++i) {
                buffer[i] = init[i];
            }
        } else {
            continue;
        }
        if (channels[c] == Channel::Translation) {
            out.translation = Vector3f{buffer[0], buffer[1], buffer[2]};
        } else if (channels[c] == Channel::Rotation) {
            out.rotation = Quaternion{buffer[0], buffer[1], buffer[2], buffer[3]};
        } else {
            out.scale = components == 1 ? Vector3f{buffer[0], buffer[0], buffer[0]}
                                        : Vector3f{buffer[0], buffer[1], buffer[2]};
        }
    }
    return out;
}

/// `T(-p) * S * R * T(p + t)` — what a pivot rig composes.
Matrix44f PivotComposition(const Transform& trs, const Vector3f& pivot) {
    Matrix44f out = ToMatrix(Transform{Vector3f{0, 0, 0}, trs.rotation, trs.scale});
    const Vector3f moved = RowMultiply(out, Vector3f{-pivot.x, -pivot.y, -pivot.z});
    out.data[3][0] = moved.x + pivot.x + trs.translation.x;
    out.data[3][1] = moved.y + pivot.y + trs.translation.y;
    out.data[3][2] = moved.z + pivot.z + trs.translation.z;
    return out;
}

/// Every node's skinning matrix under the tree's own convention.
std::vector<Matrix44f> SkinAt(const Model& model, const SubTrackContainer& container, f32 time) {
    const bool pivot = model.nodes.rig == RigConvention::PivotRelative;
    const u32 count = model.nodes.size();
    std::vector<Matrix44f> local(count), world(count), out(count);
    for (u32 n = 0; n < count; ++n) {
        const Node& node = model.nodes.nodes[n];
        // The one difference that IS the convention: an un-keyed channel rests
        // at the identity in a pivot rig and at the node's own local otherwise.
        const Transform rest = pivot ? Transform::identity() : node.local;
        const Transform trs = NodeTrsAt(model, container, n, time, rest);
        local[n] = pivot ? PivotComposition(trs, node.pivot) : ToMatrix(trs);
    }
    for (u32 n = 0; n < count; ++n) {
        Matrix44f chain = Matrix44f::identity();
        for (u32 walk = n, guard = 0; walk < count && guard <= count; ++guard) {
            chain = chain * local[walk];
            const u32 parent = model.nodes.nodes[walk].parent;
            if (parent == walk || parent >= count) {
                break;
            }
            walk = parent;
        }
        world[n] = chain;
        out[n] = pivot ? chain : model.nodes.inverseBindMatrix(n) * chain;
    }
    return out;
}

Node MakeBone(const char* name, u32 parent) {
    Node node;
    node.name = name;
    node.kind = NodeKind::Bone;
    node.parent = parent;
    node.resetPayloadForKind();
    return node;
}

u32 DeclareChannel(Model& model, u32 node, Channel channel, geom::AttrType type) {
    AnimChannel entry;
    entry.id = model.animChannels.nextFreeId();
    entry.target.kind = TrackTarget::Kind::Node;
    entry.target.node = node;
    entry.target.channel = channel;
    entry.valueType = type;
    return model.animChannels.add(entry);
}

void AddTrack(SubTrackContainer& container, u32 channel, const std::vector<f32>& times,
              const std::vector<f32>& values) {
    SubTrack track;
    track.channel = channel;
    track.interp = Interpolation::Linear;
    track.times = times;
    track.values.resize(values.size() * sizeof(f32));
    std::memcpy(track.values.data(), values.data(), values.size() * sizeof(f32));
    container.subTracks.push_back(std::move(track));
}

/// A three-bone explicit-bind rig whose `IREF` is deliberately NOT the inverse
/// of its own rest chain — which is the situation `.m3` actually ships, and the
/// reason a pivot rig has to gain keys it did not have.
Document MakeExplicitRig(const Vector3f& middleScale) {
    Document document;
    document.models.emplace_back();
    Model& model = document.models[0];
    model.name = "rig";

    PoseSchema schema;
    schema.name = "iref";
    schema.space = PoseSpace::Model;
    schema.inverse = true;
    schema.storage = PoseStorage::Matrix;
    model.nodes.poseSchema.push_back(schema);
    model.nodes.authoritativePose = 0;
    model.nodes.rig = RigConvention::ExplicitBind;

    const Vector3f positions[3] = {{0, 0, 0}, {0, 0, 2}, {0, 1, 3}};
    const Vector3f scales[3] = {{1, 1, 1}, middleScale, {1, 1, 1}};
    for (u32 n = 0; n < 3; ++n) {
        Node bone = MakeBone(n == 0 ? "root" : (n == 1 ? "spine" : "head"),
                             n == 0 ? kInvalidNode : n - 1);
        bone.local.translation = n == 0 ? positions[0]
                                        : Vector3f{positions[n].x - positions[n - 1].x,
                                                   positions[n].y - positions[n - 1].y,
                                                   positions[n].z - positions[n - 1].z};
        bone.local.scale = scales[n];
        // A bind frame with a rotation, so the conjugation is not a translation.
        const Quaternion tilt =
            Quaternion{0.0f, 0.2588f, 0.0f, 0.9659f}.normalized(); // 30 degrees about Y
        Transform bind;
        bind.translation = positions[n];
        bind.rotation = n == 1 ? tilt : Quaternion{0, 0, 0, 1};
        bind.scale = Vector3f{1, 1, 1};
        bone.poseMatrices.push_back(Matrix44f::inverse(ToMatrix(bind)));
        bone.poses.push_back(FromMatrix(bone.poseMatrices[0]));
        model.nodes.add(std::move(bone));
    }

    Clip clip;
    clip.name = "wave";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.emplace_back();
    SubTrackContainer& container = clip.containers[0];

    const u32 rotation = DeclareChannel(model, 1, Channel::Rotation, geom::AttrType::Quat);
    const u32 translation = DeclareChannel(model, 2, Channel::Translation, geom::AttrType::F32x3);
    AddTrack(container, rotation, {0.0f, 0.5f, 1.0f},
             {0, 0, 0, 1, 0.3827f, 0, 0, 0.9239f, 0, 0, 0, 1});
    AddTrack(container, translation, {0.0f, 1.0f}, {0, 1, 1, 0.5f, 1.25f, 1});
    document.clips.push_back(std::move(clip));
    return document;
}

/// A four-node pivot rig: absolute pivots, an identity rest, and offset tracks.
Document MakePivotRig() {
    Document document;
    document.models.emplace_back();
    Model& model = document.models[0];
    model.name = "rig";
    model.nodes.poseSchema.push_back(PoseSchema{});
    model.nodes.authoritativePose = 0;
    model.nodes.rig = RigConvention::PivotRelative;

    const Vector3f pivots[4] = {{0, 0, 0}, {0, 0, 2}, {0.5f, 0, 3}, {-0.5f, 0, 3}};
    const u32 parents[4] = {kInvalidNode, 0, 1, 1};
    for (u32 n = 0; n < 4; ++n) {
        Node bone = MakeBone(("bone" + std::to_string(n)).c_str(), parents[n]);
        bone.pivot = pivots[n];
        const Vector3f parent =
            parents[n] == kInvalidNode ? Vector3f{0, 0, 0} : pivots[parents[n]];
        bone.local.translation =
            Vector3f{pivots[n].x - parent.x, pivots[n].y - parent.y, pivots[n].z - parent.z};
        bone.poses.push_back(bone.local);
        model.nodes.add(std::move(bone));
    }

    Clip clip;
    clip.name = "swing";
    clip.model = 0;
    clip.duration = 1.0f;
    clip.containers.emplace_back();
    SubTrackContainer& container = clip.containers[0];
    const u32 rotation = DeclareChannel(model, 1, Channel::Rotation, geom::AttrType::Quat);
    const u32 translation = DeclareChannel(model, 2, Channel::Translation, geom::AttrType::F32x3);
    const u32 scale = DeclareChannel(model, 3, Channel::Scale, geom::AttrType::F32x3);
    AddTrack(container, rotation, {0.0f, 1.0f}, {0, 0, 0, 1, 0.3827f, 0, 0, 0.9239f});
    AddTrack(container, translation, {0.0f, 1.0f}, {0, 0, 0, 0.25f, -0.5f, 0.75f});
    AddTrack(container, scale, {0.0f, 1.0f}, {1, 1, 1, 1.5f, 0.5f, 2.0f});
    document.clips.push_back(std::move(clip));
    return document;
}

/// The worst distance, in units, between the two documents' skinning matrices,
/// probed at the origin and one unit along each axis so a pure rotation about
/// the probe cannot hide.
f32 WorstSkinError(const Document& before, const Document& after, const std::vector<f32>& times) {
    const Model& source = before.models[0];
    const Model& target = after.models[0];
    f32 worst = 0.0f;
    for (std::size_t c = 0; c < before.clips.size(); ++c) {
        for (std::size_t k = 0; k < before.clips[c].containers.size(); ++k) {
            for (const f32 time : times) {
                const std::vector<Matrix44f> want =
                    SkinAt(source, before.clips[c].containers[k], time);
                const std::vector<Matrix44f> got =
                    SkinAt(target, after.clips[c].containers[k], time);
                // The retarget may insert helpers, so the source's nth bone is
                // the nth node of the target that is not one.
                std::vector<u32> map;
                for (u32 n = 0; n < target.nodes.size(); ++n) {
                    const std::string& name = target.nodes.nodes[n].name;
                    const bool helper = name.size() >= 8 &&
                                        name.compare(name.size() - 8, 8, "_stretch") == 0;
                    if (!helper) {
                        map.push_back(n);
                    }
                }
                REQUIRE(map.size() == source.nodes.size());
                const Vector3f probes[4] = {
                    {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
                for (u32 n = 0; n < want.size(); ++n) {
                    for (const Vector3f& probe : probes) {
                        worst = std::fmax(worst, Distance(RowMultiply(want[n], probe),
                                                          RowMultiply(got[map[n]], probe)));
                    }
                }
            }
        }
    }
    return worst;
}

const std::vector<f32> kProbeTimes = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

} // namespace

TEST_CASE("skeleton retarget to a pivot rig keeps the skinning matrix") {
    // A uniform middle scale: the conjugated frames stay scale-rotations, so no
    // node has to split and the rewrite is exact at every key.
    const Document before = MakeExplicitRig(Vector3f{1.5f, 1.5f, 1.5f});
    Document after = before;

    const SkeletonRetargetResult result = RetargetSkeleton(after, ProfileId::Wc3Classic);
    REQUIRE(result.ok);
    CHECK(after.models[0].nodes.rig == RigConvention::PivotRelative);
    CHECK(result.shearedNodes == 0);
    CHECK(result.nodesInserted == 0);
    CHECK(after.models[0].nodes.size() == before.models[0].nodes.size());

    CHECK(WorstSkinError(before, after, kProbeTimes) < 1e-3f);
}

TEST_CASE("skeleton retarget puts a bone's bind position in the pivot") {
    const Document before = MakeExplicitRig(Vector3f{1.5f, 1.5f, 1.5f});
    Document after = before;
    REQUIRE(RetargetSkeleton(after, ProfileId::Wc3Classic).ok);

    // Every source bone binds at a known place; a pivot rig has to say so, and
    // `.m3 -> .mdx` used to write a skeleton of zeroes here.
    const Vector3f expected[3] = {{0, 0, 0}, {0, 0, 2}, {0, 1, 3}};
    for (u32 n = 0; n < 3; ++n) {
        const Vector3f pivot = after.models[0].nodes.nodes[n].pivot;
        CHECK(pivot.x == Catch::Approx(expected[n].x).margin(1e-4f));
        CHECK(pivot.y == Catch::Approx(expected[n].y).margin(1e-4f));
        CHECK(pivot.z == Catch::Approx(expected[n].z).margin(1e-4f));
    }
}

TEST_CASE("skeleton retarget splits a bone whose bind frame shears") {
    // A non-uniform scale under a rotated bind frame is shear the pivot node
    // cannot hold — the 7.4% of SC2 bones the split exists for.
    const Document before = MakeExplicitRig(Vector3f{2.0f, 1.0f, 0.5f});
    Document after = before;

    const SkeletonRetargetResult result = RetargetSkeleton(after, ProfileId::Wc3Classic);
    REQUIRE(result.ok);
    CHECK(result.shearedNodes > 0);
    CHECK(result.nodesInserted == result.shearedNodes);
    CHECK(after.models[0].nodes.size() ==
          before.models[0].nodes.size() + result.nodesInserted);
    CHECK(after.models[0].nodes.parentsPrecedeChildren());
    CHECK(result.diagnostics.countOf(DiagCode::BoneShearSplit) == 1);

    CHECK(WorstSkinError(before, after, kProbeTimes) < 1e-3f);
}

TEST_CASE("skeleton retarget without the split is only approximate") {
    const Document before = MakeExplicitRig(Vector3f{2.0f, 1.0f, 0.5f});
    Document projected = before;
    SkeletonRetargetOptions options;
    options.splitShearedNodes = false;

    const SkeletonRetargetResult result = RetargetSkeleton(projected, ProfileId::Wc3Classic, options);
    REQUIRE(result.ok);
    CHECK(result.nodesInserted == 0);
    CHECK(projected.models[0].nodes.size() == before.models[0].nodes.size());
    CHECK(result.diagnostics.countOf(DiagCode::BoneShearProjected) == 1);

    // The point of the option is that it costs accuracy, so the test says so
    // rather than pretending the two are interchangeable.
    CHECK(WorstSkinError(before, projected, kProbeTimes) > 1e-3f);
}

TEST_CASE("skeleton retarget to an explicit bind keeps the skinning matrix") {
    const Document before = MakePivotRig();
    Document after = before;

    const SkeletonRetargetResult result = RetargetSkeleton(after, ProfileId::Sc2);
    REQUIRE(result.ok);
    CHECK(after.models[0].nodes.rig == RigConvention::ExplicitBind);
    // No shear can arise in this direction, so no node is ever added.
    CHECK(result.nodesInserted == 0);
    CHECK(after.models[0].nodes.size() == before.models[0].nodes.size());

    CHECK(WorstSkinError(before, after, kProbeTimes) < 1e-4f);
}

TEST_CASE("an explicit bind states the pivot rig's bind as T(-pivot)") {
    const Document before = MakePivotRig();
    Document after = before;
    REQUIRE(RetargetSkeleton(after, ProfileId::Sc2).ok);

    const NodeTree& tree = after.models[0].nodes;
    REQUIRE(tree.poseSchema.size() == 1);
    CHECK(tree.poseSchema[0].inverse);
    CHECK(tree.poseSchema[0].storage == PoseStorage::Matrix);
    for (u32 n = 0; n < tree.size(); ++n) {
        // `.m3` leaves no pivot; the offset has moved into the tracks.
        CHECK(tree.nodes[n].pivot.x == Catch::Approx(0.0f).margin(1e-6f));
        const Vector3f pivot = before.models[0].nodes.nodes[n].pivot;
        const Matrix44f bind = tree.inverseBindMatrix(n);
        CHECK(bind.data[3][0] == Catch::Approx(-pivot.x).margin(1e-4f));
        CHECK(bind.data[3][1] == Catch::Approx(-pivot.y).margin(1e-4f));
        CHECK(bind.data[3][2] == Catch::Approx(-pivot.z).margin(1e-4f));
    }
}

TEST_CASE("skeleton retarget round trips through both conventions") {
    const Document before = MakePivotRig();
    Document round = before;
    REQUIRE(RetargetSkeleton(round, ProfileId::Sc2).ok);
    REQUIRE(RetargetSkeleton(round, ProfileId::Wc3Classic).ok);
    CHECK(round.models[0].nodes.rig == RigConvention::PivotRelative);
    CHECK(WorstSkinError(before, round, kProbeTimes) < 1e-3f);
}

TEST_CASE("skeleton retarget leaves a rig already in the target convention alone") {
    Document document = MakePivotRig();
    const std::size_t nodes = document.models[0].nodes.size();
    const std::size_t channels = document.models[0].animChannels.channels.size();

    const SkeletonRetargetResult result = RetargetSkeleton(document, ProfileId::Wc3Classic);
    CHECK(result.ok);
    CHECK(result.nodesInserted == 0);
    CHECK(result.nodesRewritten == 0);
    CHECK(document.models[0].nodes.size() == nodes);
    CHECK(document.models[0].animChannels.channels.size() == channels);
    // A no-op is a success WITH a report, the same contract `DeriveProfile` has.
    CHECK(result.diagnostics.countOf(DiagCode::RigConventionChanged) == 1);
}

TEST_CASE("the node convention survives a save and load") {
    Document document = MakePivotRig();
    REQUIRE(RetargetSkeleton(document, ProfileId::Sc2).ok);
    CHECK(document.models[0].nodes.rig == RigConvention::ExplicitBind);

    // A tree that never recorded one is read back from what its schema implies —
    // an inverse or matrix pose exists only to carry an explicit bind.
    NodeTree pivotLike;
    pivotLike.poseSchema.push_back(PoseSchema{});
    CHECK(pivotLike.inferredRig() == RigConvention::PivotRelative);
    CHECK(document.models[0].nodes.inferredRig() == RigConvention::ExplicitBind);
}
