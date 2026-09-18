// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `SameReflected` / `ReflectBytes`: the one value comparison an editor has for
/// WEM's structs. Undo checks, "did this gesture change anything" and a
/// preview's rebuild key all stand on it, so the property pinned here is that
/// it sees EVERY part of a value: a material's feature payloads, its body's
/// slots, its native block's layer fields and its sync state, each on its own.

#include <cstring>
#include <string>
#include <variant>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/reflect_bytes.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

template <class T>
mdx::Track<T> makeTrack(std::vector<u32> times, std::vector<T> values) {
    mdx::Track<T> track;
    track.isUsed = true;
    track.interpolationType = mdx::InterpolationType::Linear;
    track.keyCount = times.size();
    track.timestamps = std::move(times);
    track.keys_data = std::move(values);
    return track;
}

/// A v1200 model with both families: material 0 is one HD layer over six
/// slots with a fresnel rim, material 1 one SD layer with a keyed alpha and a
/// keyed TXAN. Everything `ReflectBytes` has to see is then present at once:
/// features of two kinds, a PBR body, native blocks, channels and a clip.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 1200;
    model.modelName = "reflect";

    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);

    for (u32 t = 0; t < 6; ++t) {
        mdx::Texture texture;
        texture.fileName = "textures/t" + std::to_string(t) + ".dds";
        texture.flags = mdx::Texture::Flag::WrapWidth | mdx::Texture::Flag::WrapHeight;
        model.textures.push_back(texture);
    }

    mdx::Layer hd;
    hd.filterMode = mdx::Layer::FilterMode::None;
    hd.shader = mdx::Layer::ShaderType::HD;
    hd.is_hd = true;
    hd.alpha = 1.0f;
    hd.emissiveGain = 1.0f;
    hd.fresnelColor = Vector3f{1.0f, 0.5f, 0.25f};
    hd.fresnelOpacity = 0.5f;
    hd.textureAnimationId = 0xFFFFFFFF;
    for (u32 s = 0; s < 6; ++s) {
        mdx::Layer::SubTexture sub;
        sub.textureId = s;
        sub.slot = static_cast<mdx::Layer::SlotType>(s);
        hd.subTextures.push_back(sub);
    }
    mdx::Material hdMaterial;
    hdMaterial.layers.push_back(hd);
    model.materials.push_back(hdMaterial);

    mdx::Layer sd;
    sd.filterMode = mdx::Layer::FilterMode::Blend;
    sd.shader = mdx::Layer::ShaderType::SD;
    sd.alpha = 1.0f;
    sd.textureAnimationId = 0;
    mdx::Layer::SubTexture colour;
    colour.textureId = 0;
    sd.subTextures.push_back(colour);
    sd.alphaTracks = makeTrack<f32>({0, 1000}, {1.0f, 0.5f});
    mdx::Material sdMaterial;
    sdMaterial.layers.push_back(sd);
    model.materials.push_back(sdMaterial);

    mdx::TextureAnimation scroll;
    scroll.translationTracks =
        makeTrack<Vector3f>({0, 1000}, {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}});
    model.textureAnimations.push_back(scroll);

    mdx::Bone root;
    root.node.name = "root";
    root.node.objectId = 0;
    root.node.parentId = mdx::Node::NO_PARENT;
    model.bones.push_back(root);
    model.pivotPoints = {Vector3f{0, 0, 0}};

    for (u32 m = 0; m < 2; ++m) {
        mdx::Geoset geoset;
        geoset.lodName = "g" + std::to_string(m);
        geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}};
        geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1}};
        geoset.textureCoordinateSets.push_back({Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}});
        geoset.faces = {0, 1, 2};
        geoset.materialId = m;
        model.geosets.push_back(geoset);
    }
    return model;
}

Document convert() {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(makeModel());
    REQUIRE(result.ok());
    return std::move(*result.value);
}

const Material& materialOf(const Document& document, ProfileId profile, u32 slot) {
    const Material* material = Resolve(document.models[0], slot, profile);
    REQUIRE(material != nullptr);
    return *material;
}

} // namespace

TEST_CASE("wem reflect bytes: the same value is the same bytes", "[wem][reflect]") {
    const Document a = convert();
    const Document b = convert();
    CHECK(SameReflected(a, b));
    CHECK(ReflectBytes(a) == ReflectBytes(a));
    CHECK_FALSE(ReflectBytes(a).empty());

    // The fixture carries what the other cases change, so none of them is
    // comparing two empty things.
    const Material& hd = materialOf(a, ProfileId::Wc3Reforged, 0);
    CHECK(hd.hasNative());
    CHECK(hd.sync() == NativeSync::InSync);
    CHECK(hd.Common().kind() == MaterialKind::PBRDeferred);
    REQUIRE(hd.Common().features.size() == 1u);
    CHECK(hd.Common().features[0].kind() == FeatureKind::Fresnel);
    const Material& sd = materialOf(a, ProfileId::Wc3Classic, 1);
    REQUIRE_FALSE(sd.Common().features.empty());
    CHECK_FALSE(a.clips.empty());
    CHECK(SameReflected(hd, materialOf(b, ProfileId::Wc3Reforged, 0)));
}

TEST_CASE("wem reflect bytes: each part of a material is seen on its own", "[wem][reflect]") {
    const Document document = convert();
    const Material& original = materialOf(document, ProfileId::Wc3Reforged, 0);
    Material copy = original;
    REQUIRE(SameReflected(copy, original));

    SECTION("a feature payload") {
        // Through InitCommon, so the sync state stays and only the payload moves.
        std::get<FresnelFeature>(copy.InitCommon().features[0].payload).outMax = 0.75f;
        CHECK(copy.sync() == original.sync());
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("a feature id") {
        copy.InitCommon().features[0].id += 1;
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("a body slot") {
        auto& body = std::get<PbrDeferredBody>(copy.InitCommon().body);
        REQUIRE(body.slots.size() > 1u);
        body.slots[1].second.texture = 5;
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("a native layer field") {
        native::MdxMaterial block = std::get<native::MdxMaterial>(copy.Native());
        REQUIRE(block.layers[0].subTextures.size() == 6u);
        block.layers[0].subTextures[2].textureId = 4;
        copy.SetNativeInSync(block);
        CHECK(copy.sync() == NativeSync::InSync);
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("a native header field") {
        native::MdxMaterial block = std::get<native::MdxMaterial>(copy.Native());
        block.priorityPlane = 3;
        copy.SetNativeInSync(block);
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("the sync state alone") {
        // Asking for the editable view changes no value but the state, and the
        // state is what decides which half an exporter writes.
        copy.MutableCommon();
        CHECK(copy.sync() == NativeSync::CommonEdited);
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("the name") {
        copy.name += "x";
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("put back, the same again") {
        copy.InitCommon().features[0].id += 1;
        REQUIRE_FALSE(SameReflected(copy, original));
        copy = original;
        CHECK(SameReflected(copy, original));
    }
}

TEST_CASE("wem reflect bytes: a texture, a clip and a document", "[wem][reflect]") {
    const Document document = convert();

    SECTION("a texture's wrap word, replaceable id and path") {
        const TextureRef& original = document.textures[0];
        TextureRef flags = original;
        flags.flags ^= 0x1;
        CHECK_FALSE(SameReflected(flags, original));
        TextureRef replaceable = original;
        replaceable.replaceableId = 1;
        CHECK_FALSE(SameReflected(replaceable, original));
        TextureRef path = original;
        path.path += "x";
        CHECK_FALSE(SameReflected(path, original));
        CHECK(SameReflected(TextureRef(original), original));
    }
    SECTION("one byte of one key value in a clip") {
        REQUIRE_FALSE(document.clips.empty());
        const Clip& original = document.clips[0];
        Clip copy = original;
        SubTrack* track = nullptr;
        for (SubTrackContainer& container : copy.containers) {
            for (SubTrack& candidate : container.subTracks) {
                if (!candidate.values.empty()) {
                    track = &candidate;
                }
            }
        }
        REQUIRE(track != nullptr);
        track->values.back() ^= 0x01;
        CHECK_FALSE(SameReflected(copy, original));
    }
    SECTION("a slot name, and a texture, in a whole document") {
        Document renamed = document;
        renamed.models[0].materialSlots[1] = "renamed";
        CHECK_FALSE(SameReflected(renamed, document));
        Document clamped = document;
        clamped.textures[3].flags = 0;
        CHECK_FALSE(SameReflected(clamped, document));
    }
}
