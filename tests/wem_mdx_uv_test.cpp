// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// A layer's fixed UV transform and its constant rate on the way to an `.mdx`.
///
/// Only a material with no MDX block carries either (a set `DeriveProfile`
/// made), and a `TextureAnimation` is keys and nothing else, so `toMdx` has to
/// say both as keys. Each case below is one way that can come out wrong: a
/// fixed transform that plays in one sequence only, a keyed track that costs
/// the layer its tiling, two maps of one drawn layer writing over each other.

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/mdx_uv.h>
#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/retarget.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

constexpr u32 kNoGlobalSequence = mdx::Track<f32>::kNoGlobalSequence;

/// Two sequences over one timeline, one textured layer, one geoset.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 800;
    model.modelName = "uv";

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);

    mdx::Sequence walk;
    walk.name = "Walk";
    walk.intervalStart = 2000;
    walk.intervalEnd = 3000;
    model.sequences.push_back(walk);

    mdx::Texture texture;
    texture.fileName = "textures/body.blp";
    model.textures.push_back(texture);
    mdx::Texture glow;
    glow.fileName = "textures/glow.blp";
    model.textures.push_back(glow);

    mdx::Material material;
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.textureId = 0;
    layer.textureAnimationId = 0xFFFFFFFF;
    material.layers.push_back(layer);
    model.materials.push_back(material);

    mdx::Bone root;
    root.node.name = "root";
    root.node.objectId = 0;
    root.node.parentId = mdx::Node::NO_PARENT;
    model.bones.push_back(root);
    model.pivotPoints = {Vector3f{0, 0, 0}};

    mdx::Geoset geoset;
    geoset.lodName = "body";
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}};
    geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1}};
    geoset.textureCoordinateSets.push_back({Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}});
    geoset.faces = {0, 1, 2};
    geoset.materialId = 0;
    model.geosets.push_back(geoset);
    return model;
}

/// `makeModel` as a document whose Classic material has no MDX block: the
/// shape `DeriveProfile` leaves, where the common view is the truth.
Document blocklessDocument() {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(makeModel());
    REQUIRE(result.ok());
    Document document = std::move(*result.value);
    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1u);
    set->materials[0].ClearNative();
    return document;
}

TextureInput& inputOf(Document& document, ProfileId profile, u32 ordinal) {
    ProfileMaterialSet* set = document.models[0].setFor(profile);
    REQUIRE(set != nullptr);
    TextureInput* input = set->materials[0].InitCommon().inputAt(ordinal);
    REQUIRE(input != nullptr);
    return *input;
}

/// A plain tiling: `uv' = (su * u, sv * v)`.
void tile(TextureInput& input, f32 su, f32 sv) {
    input.uvTransform = Matrix3x2f{};
    input.uvTransform.m[0][0] = su;
    input.uvTransform.m[1][1] = sv;
}

/// A `UvAnimation` feature on @p ordinal of the first material of @p profile.
MaterialFeature& uvFeature(Document& document, ProfileId profile, u32 ordinal) {
    ProfileMaterialSet* set = document.models[0].setFor(profile);
    REQUIRE(set != nullptr);
    CommonMaterial& common = set->materials[0].InitCommon();
    for (MaterialFeature& feature : common.features) {
        if (feature.kind() == FeatureKind::UvAnimation && feature.layer == ordinal) {
            return feature;
        }
    }
    MaterialFeature feature;
    feature.id = NextFeatureId(common.features);
    feature.layer = ordinal;
    feature.payload = UvAnimationFeature{};
    common.features.push_back(feature);
    return common.features.back();
}

/// Keys @p kind of the feature on @p ordinal in `clip`, as @p type.
u32 keyUv(Document& document, ProfileId profile, u32 ordinal, Channel kind, geom::AttrType type,
          u32 clip, std::vector<f32> times, std::vector<f32> values) {
    Model& model = document.models[0];
    AnimChannel channel;
    channel.id = (std::max)(1u, model.animChannels.nextFreeId());
    channel.target.kind = TrackTarget::Kind::MaterialFeature;
    channel.target.material.profile = profile;
    channel.target.material.slot = 0;
    channel.target.material.look = 0;
    channel.target.sub = uvFeature(document, profile, ordinal).id;
    channel.target.channel = kind;
    channel.valueType = type;
    model.animChannels.add(channel);

    SubTrack track;
    track.channel = channel.id;
    track.interp = Interpolation::Linear;
    track.times = std::move(times);
    track.values.resize(values.size() * sizeof(f32));
    std::memcpy(track.values.data(), values.data(), track.values.size());
    REQUIRE(clip < document.clips.size());
    REQUIRE_FALSE(document.clips[clip].containers.empty());
    document.clips[clip].containers[0].subTracks.push_back(std::move(track));
    return channel.id;
}

/// The key of @p track at @p time, or null.
template <class T>
const T* keyAt(const mdx::Track<T>& track, u32 time) {
    for (std::size_t k = 0; k < track.timestamps.size() && k < track.keys_data.size(); ++k) {
        if (track.timestamps[k] == time) {
            return &track.keys_data[k];
        }
    }
    return nullptr;
}

const mdx::TextureAnimation& onlyTextureAnimation(const mdx::Model& exported) {
    REQUIRE(exported.textureAnimations.size() == 1u);
    REQUIRE_FALSE(exported.materials.empty());
    REQUIRE_FALSE(exported.materials[0].layers.empty());
    REQUIRE(exported.materials[0].layers[0].textureAnimationId == 0u);
    return exported.textureAnimations[0];
}

} // namespace

TEST_CASE("wem mdx uv an unkeyed fixed transform plays in every sequence", "[wem][mdx][uv]") {
    // Warcraft III gives a sequence only the keys inside its own window, and a
    // texture animation with none plays the identity. A fixed transform written
    // as a key at time 0 of the animations' clock therefore tiles in whichever
    // sequence holds 0 and nowhere else; on a global sequence it plays under
    // all of them.
    Document document = blocklessDocument();
    tile(inputOf(document, ProfileId::Wc3Classic, 0), 2.0f, 2.0f);

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);

    // R * S * (uv + T - 0.5) + 0.5 = 2 * uv needs T = (0.25, 0.25).
    REQUIRE(animation.scalingTracks.isUsed);
    REQUIRE(animation.scalingTracks.keys_data.size() == 1u);
    CHECK(animation.scalingTracks.keys_data[0].x == Catch::Approx(2.0f));
    CHECK(animation.scalingTracks.keys_data[0].y == Catch::Approx(2.0f));
    REQUIRE(animation.translationTracks.isUsed);
    REQUIRE(animation.translationTracks.keys_data.size() == 1u);
    CHECK(animation.translationTracks.keys_data[0].x == Catch::Approx(0.25f));
    CHECK(animation.translationTracks.keys_data[0].y == Catch::Approx(0.25f));
    CHECK_FALSE(animation.rotationTracks.isUsed);

    CHECK(animation.scalingTracks.globalSequenceId != kNoGlobalSequence);
    CHECK(animation.translationTracks.globalSequenceId != kNoGlobalSequence);
    CHECK(animation.scalingTracks.globalSequenceId < exported->globalSequences.size());
    CHECK(animation.translationTracks.globalSequenceId < exported->globalSequences.size());
}

TEST_CASE("wem mdx uv a keyed track leaves the layer its fixed transform", "[wem][mdx][uv]") {
    // Translation keyed in `Stand` only, over a 2 x 2 tiling. The tiling is not
    // the keyed track's to drop, and `Walk`, which does not key the track, has
    // to go on showing the layer where it stood: one key at its start.
    Document document = blocklessDocument();
    tile(inputOf(document, ProfileId::Wc3Classic, 0), 2.0f, 2.0f);
    keyUv(document, ProfileId::Wc3Classic, 0, Channel::UvTranslate, geom::AttrType::F32x3, 0,
          {0.0f, 1.0f}, {0.25f, 0.25f, 0.0f, 1.25f, 0.25f, 0.0f});

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    REQUIRE(exported->sequences.size() == 2u);
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);

    const mdx::Track<Vector3f>& translation = animation.translationTracks;
    REQUIRE(translation.isUsed);
    CHECK(translation.globalSequenceId == kNoGlobalSequence);
    REQUIRE(keyAt(translation, 0) != nullptr);
    REQUIRE(keyAt(translation, 1000) != nullptr);
    CHECK(keyAt(translation, 1000)->x == Catch::Approx(1.25f));

    const u32 walkStart = exported->sequences[1].intervalStart;
    const Vector3f* held = keyAt(translation, walkStart);
    REQUIRE(held != nullptr);
    CHECK(held->x == Catch::Approx(0.25f));
    CHECK(held->y == Catch::Approx(0.25f));

    const mdx::Track<Vector3f>& scaling = animation.scalingTracks;
    REQUIRE(scaling.isUsed);
    REQUIRE(scaling.keys_data.size() == 1u);
    CHECK(scaling.keys_data[0].x == Catch::Approx(2.0f));
    CHECK(scaling.keys_data[0].y == Catch::Approx(2.0f));
    CHECK(scaling.globalSequenceId != kNoGlobalSequence);
    CHECK_FALSE(animation.rotationTracks.isUsed);
}

TEST_CASE("wem mdx uv one map owns an HD layer's texture animation", "[wem][mdx][uv]") {
    // A Reforged material's maps are ordinals of their own and all of them
    // become one HD layer, which has one texture matrix. The first map with a
    // transform takes it; a later one must not write over it.
    MdxConverter converter;
    Result<Document> source = converter.fromMdx(makeModel());
    REQUIRE(source.ok());
    Document document = std::move(*source.value);
    // Through the common material, as a derive from another game goes: the
    // two Warcraft III profiles would otherwise share the MDX block.
    RetargetOptions options;
    options.keepSharedNative = false;
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Wc3Reforged, options).ok);

    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Reforged);
    REQUIRE(set != nullptr);
    REQUIRE(set->materials.size() == 1u);
    REQUIRE_FALSE(set->materials[0].hasNative());
    PbrDeferredBody* body = set->materials[0].InitCommon().pbr();
    REQUIRE(body != nullptr);
    REQUIRE_FALSE(body->slots.empty());
    REQUIRE(body->slots[0].first == PbrSlot::BaseColor);
    tile(body->slots[0].second, 2.0f, 2.0f);
    TextureInput glow;
    glow.texture = 1;
    tile(glow, 3.0f, 3.0f);
    body->set(PbrSlot::Emissive, glow);

    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Reforged);
    REQUIRE(exported.ok());
    REQUIRE(exported->materials[0].layers.size() == 1u);
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    REQUIRE(animation.scalingTracks.isUsed);
    REQUIRE(animation.scalingTracks.keys_data.size() == 1u);
    CHECK(animation.scalingTracks.keys_data[0].x == Catch::Approx(2.0f));
    CHECK(animation.scalingTracks.keys_data[0].y == Catch::Approx(2.0f));
}

TEST_CASE("wem mdx uv a constant rate is keys on a global sequence", "[wem][mdx][uv]") {
    // Half a tile a second along U and half a turn a second: the scroll's
    // period is the two seconds one tile takes, and the turn shares it.
    Document document = blocklessDocument();
    UvAnimationFeature rate;
    rate.scrollRate = Vector2f{0.5f, 0.0f};
    rate.rotateRate = 3.14159265f;
    uvFeature(document, ProfileId::Wc3Classic, 0).payload = rate;

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);

    const mdx::Track<Vector3f>& translation = animation.translationTracks;
    REQUIRE(translation.isUsed);
    REQUIRE(translation.globalSequenceId < exported->globalSequences.size());
    CHECK(exported->globalSequences[translation.globalSequenceId] == 2000u);
    CHECK(translation.interpolationType == mdx::InterpolationType::Linear);
    REQUIRE(translation.timestamps == std::vector<u32>{0u, 2000u});
    REQUIRE(translation.keys_data.size() == 2u);
    CHECK(translation.keys_data[0].x == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(translation.keys_data[0].y == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(translation.keys_data[1].x == Catch::Approx(1.0f));
    CHECK(translation.keys_data[1].y == Catch::Approx(0.0f).margin(1e-6f));

    const mdx::Track<Quaternion>& rotation = animation.rotationTracks;
    REQUIRE(rotation.isUsed);
    CHECK(rotation.globalSequenceId == translation.globalSequenceId);
    CHECK(rotation.interpolationType == mdx::InterpolationType::Linear);
    REQUIRE(rotation.timestamps == std::vector<u32>{0u, 500u, 1000u, 1500u, 2000u});
    REQUIRE(rotation.keys_data.size() == 5u);
    const f32 quarter = 0.70710678f;
    const f32 z[5] = {0.0f, quarter, 1.0f, quarter, 0.0f};
    const f32 w[5] = {1.0f, quarter, 0.0f, -quarter, -1.0f};
    for (std::size_t k = 0; k < 5; ++k) {
        CHECK(rotation.keys_data[k].x == 0.0f);
        CHECK(rotation.keys_data[k].y == 0.0f);
        CHECK(rotation.keys_data[k].z == Catch::Approx(z[k]).margin(1e-5f));
        CHECK(rotation.keys_data[k].w == Catch::Approx(w[k]).margin(1e-5f));
    }
    CHECK_FALSE(animation.scalingTracks.isUsed);
}

// ============================================================================
// mdx_uv: the arithmetic itself
// ============================================================================

TEST_CASE("wem mdx uv a fixed transform reads as the three values that play it",
          "[wem][mdx][uv]") {
    mdx_uv::UvValues values;
    values.translation = Vector2f{0.3f, -0.2f};
    values.angle = 0.7f;
    values.scale = Vector2f{2.0f, 3.0f};
    const Matrix3x2f matrix = mdx_uv::MatrixOf(values);

    // uv' = R * S * (uv + T - 0.5) + 0.5, at one point by hand.
    const Vector2f uv{0.9f, 0.1f};
    const f32 x = (uv.x + values.translation.x - 0.5f) * values.scale.x;
    const f32 y = (uv.y + values.translation.y - 0.5f) * values.scale.y;
    const Vector2f played = matrix.apply(uv);
    CHECK(played.x == Catch::Approx(std::cos(0.7f) * x - std::sin(0.7f) * y + 0.5f));
    CHECK(played.y == Catch::Approx(std::sin(0.7f) * x + std::cos(0.7f) * y + 0.5f));

    const std::optional<mdx_uv::UvValues> back = mdx_uv::FixedValues(matrix);
    REQUIRE(back.has_value());
    CHECK(back->translation.x == Catch::Approx(0.3f));
    CHECK(back->translation.y == Catch::Approx(-0.2f));
    CHECK(back->angle == Catch::Approx(0.7f));
    CHECK(back->scale.x == Catch::Approx(2.0f));
    CHECK(back->scale.y == Catch::Approx(3.0f));

    const std::optional<mdx_uv::UvValues> identity = mdx_uv::FixedValues(Matrix3x2f{});
    REQUIRE(identity.has_value());
    CHECK(mdx_uv::AtRest(Channel::UvTranslate, *identity));
    CHECK(mdx_uv::AtRest(Channel::UvRotate, *identity));
    CHECK(mdx_uv::AtRest(Channel::UvScale, *identity));
    CHECK_FALSE(mdx_uv::AtRest(Channel::UvScale, *back));

    Matrix3x2f shear;
    shear.m[0][1] = 0.5f;
    CHECK_FALSE(mdx_uv::FixedValues(shear).has_value());
}

TEST_CASE("wem mdx uv a key is read and written in its channel's own type", "[wem][mdx][uv]") {
    const auto bytes = [](std::initializer_list<f32> floats) {
        std::vector<u8> out(floats.size() * sizeof(f32));
        std::memcpy(out.data(), floats.begin(), out.size());
        return out;
    };

    SECTION("the way a TextureAnimation keys it") {
        const std::vector<u8> translation = bytes({0.25f, -0.5f, 0.0f});
        const Vector2f t =
            mdx_uv::DecodeKey(Channel::UvTranslate, geom::AttrType::F32x3, translation.data());
        CHECK(t.x == 0.25f);
        CHECK(t.y == -0.5f);
        CHECK(mdx_uv::EncodeKey(Channel::UvTranslate, geom::AttrType::F32x3, t) == translation);

        const Quaternion turn = mdx_uv::TurnAboutZ(1.2f);
        const std::vector<u8> rotation = bytes({turn.x, turn.y, turn.z, turn.w});
        const Vector2f r =
            mdx_uv::DecodeKey(Channel::UvRotate, geom::AttrType::Quat, rotation.data());
        CHECK(r.x == Catch::Approx(1.2f));
        const std::vector<u8> again = mdx_uv::EncodeKey(Channel::UvRotate, geom::AttrType::Quat, r);
        REQUIRE(again.size() == sizeof(Quaternion));
        Quaternion written;
        std::memcpy(&written, again.data(), sizeof(written));
        CHECK(written.x == 0.0f);
        CHECK(written.y == 0.0f);
        CHECK(written.z == Catch::Approx(turn.z));
        CHECK(written.w == Catch::Approx(turn.w));

        // A whole turn is not no turn: its key is the identity negated, which
        // reads as a whole turn one way or the other.
        const Quaternion whole = mdx_uv::TurnAboutZ(6.2831853f);
        const std::vector<u8> wholeBytes = bytes({whole.x, whole.y, whole.z, whole.w});
        CHECK(std::fabs(mdx_uv::DecodeKey(Channel::UvRotate, geom::AttrType::Quat,
                                          wholeBytes.data())
                            .x) == Catch::Approx(6.2831853f).margin(1e-4f));

        const std::vector<u8> scale = bytes({2.0f, 3.0f, 1.0f});
        const Vector2f k = mdx_uv::DecodeKey(Channel::UvScale, geom::AttrType::F32x3, scale.data());
        CHECK(k.x == 2.0f);
        CHECK(k.y == 3.0f);
        CHECK(mdx_uv::EncodeKey(Channel::UvScale, geom::AttrType::F32x3, k) == scale);
    }
    SECTION("the way an .m3 layer keys it") {
        // The offset is the translation negated; the angle is the euler z; the
        // tiling is the scale. Each goes back to the bytes it came from.
        const std::vector<u8> offset = bytes({0.25f, -0.5f});
        const Vector2f t =
            mdx_uv::DecodeKey(Channel::UvTranslate, geom::AttrType::F32x2, offset.data());
        CHECK(t.x == -0.25f);
        CHECK(t.y == 0.5f);
        CHECK(mdx_uv::EncodeKey(Channel::UvTranslate, geom::AttrType::F32x2, t) == offset);

        const std::vector<u8> angle = bytes({0.0f, 0.0f, 1.2f});
        const Vector2f r =
            mdx_uv::DecodeKey(Channel::UvRotate, geom::AttrType::F32x3, angle.data());
        CHECK(r.x == 1.2f);
        CHECK(mdx_uv::EncodeKey(Channel::UvRotate, geom::AttrType::F32x3, r) == angle);

        const std::vector<u8> tiling = bytes({2.0f, 3.0f});
        const Vector2f k = mdx_uv::DecodeKey(Channel::UvScale, geom::AttrType::F32x2, tiling.data());
        CHECK(k.x == 2.0f);
        CHECK(k.y == 3.0f);
        CHECK(mdx_uv::EncodeKey(Channel::UvScale, geom::AttrType::F32x2, k) == tiling);
    }
}

// ============================================================================
// The rest, and what the export fills
// ============================================================================

namespace {

TrackTarget uvTarget(Document& document, ProfileId profile, u32 ordinal, Channel channel) {
    TrackTarget target;
    target.kind = TrackTarget::Kind::MaterialFeature;
    target.material.profile = profile;
    target.material.slot = 0;
    target.material.look = 0;
    target.sub = uvFeature(document, profile, ordinal).id;
    target.channel = channel;
    return target;
}

template <class T>
T restAs(const TrackRests& rests) {
    REQUIRE(rests.unkeyed.size() == sizeof(T));
    T value;
    std::memcpy(&value, rests.unkeyed.data(), sizeof(T));
    return value;
}

u32 countOf(const Diagnostics& diagnostics, const std::string& fragment) {
    u32 count = 0;
    for (const Diagnostic& entry : diagnostics.all()) {
        count += entry.message.find(fragment) != std::string::npos;
    }
    return count;
}

} // namespace

TEST_CASE("wem mdx uv an unkeyed track rests at the layer's fixed transform", "[wem][mdx][uv]") {
    Document document = blocklessDocument();
    tile(inputOf(document, ProfileId::Wc3Classic, 0), 2.0f, 2.0f);
    const ProfileId profile = ProfileId::Wc3Classic;

    const TrackRests scale = RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvScale),
                                     geom::AttrType::F32x3, Game::Warcraft);
    // One rest: an animation that does not key the track shows the layer where
    // it stood whether or not another animation keys it.
    CHECK_FALSE(scale.differ());
    CHECK(restAs<Vector3f>(scale).x == Catch::Approx(2.0f));
    CHECK(restAs<Vector3f>(scale).y == Catch::Approx(2.0f));
    CHECK(restAs<Vector3f>(scale).z == Catch::Approx(1.0f));

    const TrackRests translation =
        RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvTranslate),
                geom::AttrType::F32x3, Game::Warcraft);
    CHECK_FALSE(translation.differ());
    CHECK(restAs<Vector3f>(translation).x == Catch::Approx(0.25f));
    CHECK(restAs<Vector3f>(translation).y == Catch::Approx(0.25f));

    const TrackRests rotation =
        RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvRotate),
                geom::AttrType::Quat, Game::Warcraft);
    CHECK(restAs<Quaternion>(rotation).z == Catch::Approx(0.0f).margin(1e-6f));
    CHECK(restAs<Quaternion>(rotation).w == Catch::Approx(1.0f));

    SECTION("in the type asked for") {
        // An `.m3` offset is the translation negated.
        const TrackRests offset =
            RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvTranslate),
                    geom::AttrType::F32x2, Game::Warcraft);
        CHECK(restAs<Vector2f>(offset).x == Catch::Approx(-0.25f));
        CHECK(restAs<Vector2f>(offset).y == Catch::Approx(-0.25f));
    }
    SECTION("under Warcraft III's storage only") {
        // The reading of the transform as three values is MDX's.
        const TrackRests other =
            RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvScale),
                    geom::AttrType::F32x3, Game::StarCraft);
        CHECK(restAs<Vector3f>(other).x == Catch::Approx(1.0f));
    }
    SECTION("a shear has no MDX form and rests at the identity") {
        inputOf(document, profile, 0).uvTransform.m[0][1] = 0.5f;
        const TrackRests sheared =
            RestsOf(document, 0, uvTarget(document, profile, 0, Channel::UvScale),
                    geom::AttrType::F32x3, Game::Warcraft);
        CHECK(restAs<Vector3f>(sheared).x == Catch::Approx(1.0f));
        CHECK(restAs<Vector3f>(sheared).y == Catch::Approx(1.0f));
    }
}

TEST_CASE("wem mdx uv a layer of an .mdx is not moved", "[wem][mdx][uv]") {
    // A material with an MDX block has no fixed transform, so nothing above
    // may reach a native file: a track keyed in one sequence gets no key in
    // the other, and its rest is the identity.
    mdx::Model source = makeModel();
    mdx::TextureAnimation scroll;
    scroll.translationTracks.isUsed = true;
    scroll.translationTracks.interpolationType = mdx::InterpolationType::Linear;
    scroll.translationTracks.timestamps = {0, 1000};
    scroll.translationTracks.keys_data = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}};
    scroll.translationTracks.keyCount = 2;
    source.textureAnimations.push_back(scroll);
    source.materials[0].layers[0].textureAnimationId = 0;

    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);

    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    CHECK(animation.translationTracks.timestamps == std::vector<u32>{0u, 1000u});
    CHECK(animation.translationTracks.globalSequenceId == kNoGlobalSequence);
    CHECK_FALSE(animation.scalingTracks.isUsed);
    CHECK_FALSE(animation.rotationTracks.isUsed);
    CHECK(exported->globalSequences.empty());

    const ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    REQUIRE_FALSE(set->materials[0].Common().features.empty());
    TrackTarget target;
    target.kind = TrackTarget::Kind::MaterialFeature;
    target.material.profile = ProfileId::Wc3Classic;
    target.material.slot = 0;
    target.sub = set->materials[0].Common().features[0].id;
    target.channel = Channel::UvTranslate;
    const TrackRests rests = RestsOf(document, 0, target, geom::AttrType::F32x3, Game::Warcraft);
    CHECK(restAs<Vector3f>(rests).x == 0.0f);
    CHECK(restAs<Vector3f>(rests).y == 0.0f);
}

TEST_CASE("wem mdx uv a StarCraft II offset keyed over a tiling keeps the tiling",
          "[wem][mdx][uv]") {
    // The commonest converted shape: an `.m3` layer scrolls by keying its
    // offset and tiles by a static scale.
    Document document = blocklessDocument();
    tile(inputOf(document, ProfileId::Wc3Classic, 0), 2.0f, 2.0f);
    keyUv(document, ProfileId::Wc3Classic, 0, Channel::UvTranslate, geom::AttrType::F32x2, 0,
          {0.0f, 1.0f}, {0.0f, 0.0f, -1.0f, 0.0f});

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    REQUIRE(keyAt(animation.translationTracks, 1000) != nullptr);
    CHECK(keyAt(animation.translationTracks, 1000)->x == Catch::Approx(1.0f));
    REQUIRE(animation.scalingTracks.isUsed);
    CHECK(animation.scalingTracks.keys_data[0].x == Catch::Approx(2.0f));
    // The other sequence shows the layer where its fixed transform puts it.
    const Vector3f* held = keyAt(animation.translationTracks, exported->sequences[1].intervalStart);
    REQUIRE(held != nullptr);
    CHECK(held->x == Catch::Approx(0.25f));
}

TEST_CASE("wem mdx uv a track keyed on a global loop needs no held key", "[wem][mdx][uv]") {
    // A global sequence plays under every animation, so nothing is unkeyed.
    mdx::Model source = makeModel();
    source.globalSequences = {1000};
    mdx::TextureAnimation scroll;
    scroll.translationTracks.isUsed = true;
    scroll.translationTracks.interpolationType = mdx::InterpolationType::Linear;
    scroll.translationTracks.globalSequenceId = 0;
    scroll.translationTracks.timestamps = {0, 1000};
    scroll.translationTracks.keys_data = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}};
    scroll.translationTracks.keyCount = 2;
    source.textureAnimations.push_back(scroll);
    source.materials[0].layers[0].textureAnimationId = 0;

    MdxConverter converter;
    Result<Document> imported = converter.fromMdx(source);
    REQUIRE(imported.ok());
    Document document = std::move(*imported.value);
    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Classic);
    REQUIRE(set != nullptr);
    set->materials[0].ClearNative();
    tile(inputOf(document, ProfileId::Wc3Classic, 0), 2.0f, 2.0f);

    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    CHECK(animation.translationTracks.timestamps == std::vector<u32>{0u, 1000u});
    CHECK(animation.translationTracks.globalSequenceId == 0u);
    // The tiling rides the clock the file already has.
    REQUIRE(animation.scalingTracks.isUsed);
    CHECK(animation.scalingTracks.globalSequenceId == 0u);
    CHECK(exported->globalSequences.size() == 1u);
}

TEST_CASE("wem mdx uv a rate under a keyed track is dropped aloud", "[wem][mdx][uv]") {
    Document document = blocklessDocument();
    UvAnimationFeature rate;
    rate.scrollRate = Vector2f{0.5f, 0.0f};
    rate.rotateRate = 3.14159265f;
    uvFeature(document, ProfileId::Wc3Classic, 0).payload = rate;
    keyUv(document, ProfileId::Wc3Classic, 0, Channel::UvTranslate, geom::AttrType::F32x3, 0,
          {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f});

    MdxConverter converter;
    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Classic);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    // The keys stand; the scroll they replace is named, and the turn, which
    // nothing keys, is still written.
    CHECK(animation.translationTracks.globalSequenceId == kNoGlobalSequence);
    REQUIRE(keyAt(animation.translationTracks, 1000) != nullptr);
    CHECK(keyAt(animation.translationTracks, 1000)->x == Catch::Approx(0.5f));
    CHECK(countOf(exported.diagnostics, "scroll RATE is dropped") == 1u);
    REQUIRE(animation.rotationTracks.isUsed);
    CHECK(animation.rotationTracks.keys_data.size() == 5u);
    CHECK(countOf(exported.diagnostics, "turn RATE is dropped") == 0u);
}

TEST_CASE("wem mdx uv two keyed maps of one HD layer: the first map keeps it", "[wem][mdx][uv]") {
    MdxConverter converter;
    Result<Document> source = converter.fromMdx(makeModel());
    REQUIRE(source.ok());
    Document document = std::move(*source.value);
    RetargetOptions options;
    options.keepSharedNative = false;
    REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Wc3Reforged, options).ok);
    ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Reforged);
    REQUIRE(set != nullptr);
    PbrDeferredBody* body = set->materials[0].InitCommon().pbr();
    REQUIRE(body != nullptr);
    TextureInput glow;
    glow.texture = 1;
    body->set(PbrSlot::Emissive, glow);
    u32 emissive = kInvalidIndex;
    for (u32 i = 0; i < body->slots.size(); ++i) {
        if (body->slots[i].first == PbrSlot::Emissive) {
            emissive = i;
        }
    }
    REQUIRE(emissive != kInvalidIndex);
    REQUIRE(emissive != 0u);

    // The later map's channel comes first in the table, and still loses.
    keyUv(document, ProfileId::Wc3Reforged, emissive, Channel::UvTranslate, geom::AttrType::F32x3,
          0, {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f, 7.0f, 0.0f});
    keyUv(document, ProfileId::Wc3Reforged, 0, Channel::UvTranslate, geom::AttrType::F32x3, 0,
          {0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f});

    const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Reforged);
    REQUIRE(exported.ok());
    const mdx::TextureAnimation& animation = onlyTextureAnimation(*exported);
    REQUIRE(keyAt(animation.translationTracks, 1000) != nullptr);
    CHECK(keyAt(animation.translationTracks, 1000)->x == Catch::Approx(3.0f));
    CHECK(keyAt(animation.translationTracks, 1000)->y == Catch::Approx(0.0f));
    CHECK(countOf(exported.diagnostics, "another map of this layer") == 1u);
}

// ============================================================================
// The layer map
// ============================================================================

TEST_CASE("wem mdx uv the layer map is the export's own numbering", "[wem][mdx][uv]") {
    MdxConverter converter;
    Result<Document> source = converter.fromMdx(makeModel());
    REQUIRE(source.ok());
    Document document = std::move(*source.value);

    SECTION("a material with a block: the identity") {
        const MdxLayerMap map = MdxLayerMapOf(document, 0, ProfileId::Wc3Classic);
        REQUIRE(map.layerOfOrdinal.size() == 1u);
        CHECK(map.layerOfOrdinal[0] == std::vector<u32>{0u});
        CHECK(map.layerCount == std::vector<u32>{1u});
    }
    SECTION("a Reforged material's maps all become its one HD layer") {
        RetargetOptions options;
        options.keepSharedNative = false;
        REQUIRE(DeriveProfile(document, ProfileId::Wc3Classic, ProfileId::Wc3Reforged, options).ok);
        ProfileMaterialSet* set = document.models[0].setFor(ProfileId::Wc3Reforged);
        REQUIRE(set != nullptr);
        PbrDeferredBody* body = set->materials[0].InitCommon().pbr();
        REQUIRE(body != nullptr);
        TextureInput glow;
        glow.texture = 1;
        body->set(PbrSlot::Emissive, glow);

        const MdxLayerMap map = MdxLayerMapOf(document, 0, ProfileId::Wc3Reforged);
        REQUIRE(map.layerOfOrdinal.size() == 1u);
        REQUIRE(map.layerOfOrdinal[0].size() == body->slots.size());
        for (const u32 layer : map.layerOfOrdinal[0]) {
            CHECK(layer == 0u);
        }
        const Result<mdx::Model> exported = converter.toMdx(document, ProfileId::Wc3Reforged);
        REQUIRE(exported.ok());
        CHECK(map.layerCount[0] == exported->materials[0].layers.size());
    }
    SECTION("a profile the model has no set for") {
        const MdxLayerMap map = MdxLayerMapOf(document, 0, ProfileId::Wc3Reforged);
        REQUIRE(map.layerOfOrdinal.size() == 1u);
        CHECK(map.layerOfOrdinal[0].empty());
        CHECK(map.layerCount == std::vector<u32>{0u});
    }
}
