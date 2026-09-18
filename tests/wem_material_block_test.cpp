// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `MdxConverter::setMaterialBlock` and the default blocks
/// (EDIT_MODE_MATERIALS_DESIGN.md §6.1, §4.1): an editor writes a Warcraft III
/// material's native block, and the common view, the features and the channels
/// have to follow it exactly as a re-import of the saved file would.
///
/// Every value case reads the result where its consumer reads it — `toMdx`'s
/// model, at the field the renderer samples — because a green comparison of
/// the block with itself proves nothing about what the file says.

#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/reflect_bytes.h>
#include <whiteout/models/wem/validate.h>

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

mdx::Layer sdLayer(u32 texture, u32 version, mdx::Layer::FilterMode filter) {
    mdx::Layer layer;
    layer.filterMode = filter;
    layer.shader = mdx::Layer::ShaderType::SD;
    layer.textureAnimationId = 0xFFFFFFFF;
    if (version >= 900) {
        // The parser's in-memory shape for v900+: the map in the first
        // sub-texture, `textureId` zeroed.
        mdx::Layer::SubTexture sub;
        sub.textureId = texture;
        layer.subTextures.push_back(sub);
    } else {
        layer.textureId = texture;
    }
    return layer;
}

mdx::Layer hdLayer() {
    mdx::Layer layer;
    layer.filterMode = mdx::Layer::FilterMode::None;
    layer.shader = mdx::Layer::ShaderType::HD;
    layer.is_hd = true;
    layer.textureAnimationId = 0xFFFFFFFF;
    for (u32 s = 0; s < 6; ++s) {
        mdx::Layer::SubTexture sub;
        sub.textureId = s;
        sub.slot = static_cast<mdx::Layer::SlotType>(s);
        layer.subTextures.push_back(sub);
    }
    return layer;
}

/// One sequence, @p textures textures, one bone, one geoset on material 0,
/// and @p material as that material.
mdx::Model makeModel(u32 version, u32 textures, const mdx::Material& material) {
    mdx::Model model;
    model.version = version;
    model.modelName = "block";
    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);
    for (u32 t = 0; t < textures; ++t) {
        mdx::Texture texture;
        texture.fileName = "textures/t" + std::to_string(t) + ".blp";
        texture.flags = mdx::Texture::Flag::WrapWidth | mdx::Texture::Flag::WrapHeight;
        model.textures.push_back(texture);
    }
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

Document convert(const mdx::Model& model) {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(model);
    REQUIRE(result.ok());
    return std::move(*result.value);
}

Material& materialOf(Document& document, ProfileId profile) {
    ProfileMaterialSet* set = document.models[0].setFor(profile);
    REQUIRE(set != nullptr);
    REQUIRE_FALSE(set->materials.empty());
    return set->materials[0];
}

native::MdxMaterial blockOf(Document& document, ProfileId profile) {
    const native::MdxMaterial* block =
        std::get_if<native::MdxMaterial>(&materialOf(document, profile).Native());
    REQUIRE(block != nullptr);
    return *block;
}

MaterialBlockEdit editOf(ProfileId profile, native::MdxMaterial block,
                         std::vector<u32> remap = {}) {
    MaterialBlockEdit edit;
    edit.profile = profile;
    edit.material = 0;
    edit.block = std::move(block);
    edit.layerRemap = std::move(remap);
    return edit;
}

const AnimChannel* channelWith(const Document& document, TrackTarget::Kind kind, Channel channel) {
    for (const AnimChannel& candidate : document.models[0].animChannels.channels) {
        if (candidate.target.kind == kind && candidate.target.channel == channel) {
            return &candidate;
        }
    }
    return nullptr;
}

mdx::Model exportAs(const Document& document, ProfileId profile, u32 version) {
    MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, profile, version);
    REQUIRE(exported.ok());
    return std::move(*exported.value);
}

} // namespace

TEST_CASE("wem material block: a layer of the other family is refused and nothing moves",
          "[wem][materials][edit]") {
    mdx::Material material;
    material.layers.push_back(sdLayer(0, 1100, mdx::Layer::FilterMode::None));
    Document document = convert(makeModel(1100, 2, material));
    const Document before = document;

    native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
    block.layers[0].shaderType = native::MdxShaderType::HD;
    MdxConverter converter;
    const MaterialBlockResult result =
        converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block));
    CHECK_FALSE(result.ok);
    CHECK(result.diagnostics.countOf(DiagCode::NativeKindProfileMismatch) == 1u);
    CHECK(SameReflected(document, before));

    // And an empty stack: it would draw untextured white, not nothing.
    block = blockOf(document, ProfileId::Wc3Classic);
    block.layers.clear();
    CHECK_FALSE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block)).ok);
    CHECK(SameReflected(document, before));
}

TEST_CASE("wem material block: a Reforged model takes all four shaders, a classic one SD alone",
          "[wem][materials][edit]") {
    mdx::Material material;
    material.layers.push_back(hdLayer());
    Document document = convert(makeModel(1100, 6, material));
    MdxConverter converter;

    // HD -> SD in the Reforged set: the colour map alone. `isHd` is left as it
    // was on purpose: the shader is what says it, and the entry reads that.
    native::MdxMaterial block = blockOf(document, ProfileId::Wc3Reforged);
    REQUIRE(block.layers[0].isHd);
    block.layers[0].shaderType = native::MdxShaderType::SD;
    block.layers[0].subTextures.resize(1);
    REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Reforged, block)).ok);
    const native::MdxMaterial after = blockOf(document, ProfileId::Wc3Reforged);
    REQUIRE(after.layers.size() == 1);
    CHECK(after.layers[0].shaderType == native::MdxShaderType::SD);
    CHECK_FALSE(after.layers[0].isHd);
    CHECK(materialOf(document, ProfileId::Wc3Reforged).Common().kind() == MaterialKind::Composite);
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::UnsupportedMaterialKind) ==
          0u);
    // Where the renderer reads it: one SD layer over the colour map.
    const mdx::Model exported = exportAs(document, ProfileId::Wc3Reforged, 1100);
    REQUIRE(exported.materials[0].layers.size() == 1);
    CHECK(exported.materials[0].layers[0].shader == mdx::Layer::ShaderType::SD);
    CHECK_FALSE(exported.materials[0].layers[0].is_hd);
    REQUIRE(exported.materials[0].layers[0].subTextures.size() == 1);
    CHECK(exported.materials[0].layers[0].subTextures[0].textureId == 0u);

    // SD on HD as well.
    block = blockOf(document, ProfileId::Wc3Reforged);
    block.layers[0].shaderType = native::MdxShaderType::SDOnHD;
    REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Reforged, block)).ok);
    CHECK(blockOf(document, ProfileId::Wc3Reforged).layers[0].shaderType ==
          native::MdxShaderType::SDOnHD);

    // A classic set refuses SD on HD, and nothing moves.
    mdx::Material sd;
    sd.layers.push_back(sdLayer(0, 1100, mdx::Layer::FilterMode::None));
    Document classicDocument = convert(makeModel(1100, 2, sd));
    const Document before = classicDocument;
    native::MdxMaterial classic = blockOf(classicDocument, ProfileId::Wc3Classic);
    classic.layers[0].shaderType = native::MdxShaderType::SDOnHD;
    const MaterialBlockResult refused =
        converter.setMaterialBlock(classicDocument, editOf(ProfileId::Wc3Classic, classic));
    CHECK_FALSE(refused.ok);
    CHECK(refused.diagnostics.countOf(DiagCode::NativeKindProfileMismatch) == 1u);
    CHECK(SameReflected(classicDocument, before));
}

TEST_CASE("wem material block: the colour map lands where the renderer reads it",
          "[wem][materials][edit]") {
    MdxConverter converter;
    SECTION("v1100: the first sub-texture") {
        mdx::Material material;
        material.layers.push_back(sdLayer(0, 1100, mdx::Layer::FilterMode::None));
        Document document = convert(makeModel(1100, 2, material));
        native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
        REQUIRE(block.layers[0].subTextures.size() == 1u);
        block.layers[0].subTextures[0].textureId = 1;
        block.layers[0].alpha = 0.5f;

        const MaterialBlockResult result =
            converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block));
        REQUIRE(result.ok);
        const Material& edited = materialOf(document, ProfileId::Wc3Classic);
        CHECK(edited.sync() == NativeSync::InSync);
        CHECK(edited.name == document.models[0].materialSlots[0]);
        // The common follows, through the import's own projection.
        const TextureInput* input = edited.Common().inputAt(0);
        REQUIRE(input != nullptr);
        CHECK(input->texture == 1u);

        const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 1100);
        REQUIRE(out.materials[0].layers.size() == 1u);
        REQUIRE_FALSE(out.materials[0].layers[0].subTextures.empty());
        CHECK(out.materials[0].layers[0].subTextures[0].textureId == 1u);
        CHECK(out.materials[0].layers[0].alpha == Catch::Approx(0.5f));
    }
    SECTION("v1100: textureId is a leftover, and writing it changes nothing") {
        // The trap the form has to avoid: the parser zeroes `textureId` on a
        // v900+ layer, and nothing reads it again.
        mdx::Material material;
        material.layers.push_back(sdLayer(0, 1100, mdx::Layer::FilterMode::None));
        Document document = convert(makeModel(1100, 2, material));
        native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
        block.layers[0].textureId = 1;
        REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block)).ok);
        CHECK(materialOf(document, ProfileId::Wc3Classic).Common().inputAt(0)->texture == 0u);
        const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 1100);
        CHECK(out.materials[0].layers[0].subTextures[0].textureId == 0u);
    }
    SECTION("v800: textureId") {
        mdx::Material material;
        material.layers.push_back(sdLayer(0, 800, mdx::Layer::FilterMode::None));
        Document document = convert(makeModel(800, 2, material));
        native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
        REQUIRE(block.layers[0].subTextures.empty());
        block.layers[0].textureId = 1;
        REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block)).ok);
        CHECK(materialOf(document, ProfileId::Wc3Classic).Common().inputAt(0)->texture == 1u);
        const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 800);
        CHECK(out.materials[0].layers[0].textureId == 1u);
    }
}

TEST_CASE("wem material block: feature ids survive, and a keyed fresnel at zero keeps its track",
          "[wem][materials][edit]") {
    mdx::Material material;
    mdx::Layer layer = hdLayer();
    layer.fresnelOpacity = 0.5f;
    layer.fresnelColor = Vector3f{1.0f, 0.5f, 0.0f};
    layer.fresnelAlphaTracks = makeTrack<f32>({0, 1000}, {0.5f, 1.0f});
    material.layers.push_back(layer);
    Document document = convert(makeModel(1200, 6, material));

    const std::vector<MaterialFeature> features =
        materialOf(document, ProfileId::Wc3Reforged).Common().features;
    REQUIRE(features.size() == 1u);
    REQUIRE(features[0].kind() == FeatureKind::Fresnel);
    const u32 id = features[0].id;
    const AnimChannel* channel =
        channelWith(document, TrackTarget::Kind::MaterialFeature, Channel::Alpha);
    REQUIRE(channel != nullptr);
    REQUIRE(channel->target.sub == id);

    MdxConverter converter;
    // A field edit elsewhere on the layer: the fresnel is re-derived and keeps
    // the id its sub-tracks join on.
    native::MdxMaterial block = blockOf(document, ProfileId::Wc3Reforged);
    block.layers[0].alpha = 0.75f;
    REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Reforged, block)).ok);
    REQUIRE(materialOf(document, ProfileId::Wc3Reforged).Common().features.size() == 1u);
    CHECK(materialOf(document, ProfileId::Wc3Reforged).Common().features[0].id == id);

    // The static strength typed to zero: the import alone would make no feature
    // and the track would lose its target.
    block = blockOf(document, ProfileId::Wc3Reforged);
    block.layers[0].fresnelOpacity = 0.0f;
    const MaterialBlockResult result =
        converter.setMaterialBlock(document, editOf(ProfileId::Wc3Reforged, block));
    REQUIRE(result.ok);
    CHECK(result.channelsInvalidated == 0u);
    const std::vector<MaterialFeature>& kept =
        materialOf(document, ProfileId::Wc3Reforged).Common().features;
    REQUIRE(kept.size() == 1u);
    CHECK(kept[0].id == id);
    REQUIRE(kept[0].kind() == FeatureKind::Fresnel);
    CHECK(std::get<FresnelFeature>(kept[0].payload).outMax == 0.0f);
    channel = channelWith(document, TrackTarget::Kind::MaterialFeature, Channel::Alpha);
    REQUIRE(channel != nullptr);
    CHECK(channel->target.material.slot == 0u);

    const mdx::Model out = exportAs(document, ProfileId::Wc3Reforged, 1200);
    CHECK(out.materials[0].layers[0].fresnelAlphaTracks.isUsed);
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::IndexOutOfRange) == 0u);
}

TEST_CASE("wem material block: two keyed fresnels swap with their layers",
          "[wem][materials][edit]") {
    // The import numbers features by position, so after a swap the fresh
    // fresnel on layer 0 would take id 0 — the id of the one that WAS on
    // layer 0. Only carrying ids by layer keeps each track on its rim.
    mdx::Material material;
    for (u32 l = 0; l < 2; ++l) {
        mdx::Layer layer = hdLayer();
        layer.filterMode = l == 0 ? mdx::Layer::FilterMode::None : mdx::Layer::FilterMode::Additive;
        layer.fresnelOpacity = l == 0 ? 0.25f : 0.75f;
        material.layers.push_back(layer);
    }
    material.layers[1].fresnelAlphaTracks = makeTrack<f32>({0, 1000}, {0.75f, 0.1f});
    Document document = convert(makeModel(1200, 6, material));
    const AnimChannel* rim = channelWith(document, TrackTarget::Kind::MaterialFeature, Channel::Alpha);
    REQUIRE(rim != nullptr);
    const u32 id = rim->target.sub;

    MdxConverter converter;
    native::MdxMaterial block = blockOf(document, ProfileId::Wc3Reforged);
    REQUIRE(block.layers.size() == 2u);
    std::swap(block.layers[0], block.layers[1]);
    REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Reforged, block, {1, 0})).ok);

    // The keyed rim's feature is now on layer 0, under the id its track joins.
    const std::vector<MaterialFeature>& features =
        materialOf(document, ProfileId::Wc3Reforged).Common().features;
    const MaterialFeature* keyed = nullptr;
    for (const MaterialFeature& feature : features) {
        if (feature.id == id) {
            keyed = &feature;
        }
    }
    REQUIRE(keyed != nullptr);
    CHECK(keyed->layer == 0u);
    CHECK(std::get<FresnelFeature>(keyed->payload).outMax == Catch::Approx(0.75f));

    const mdx::Model out = exportAs(document, ProfileId::Wc3Reforged, 1200);
    REQUIRE(out.materials[0].layers.size() == 2u);
    CHECK(out.materials[0].layers[0].fresnelAlphaTracks.isUsed);
    CHECK_FALSE(out.materials[0].layers[1].fresnelAlphaTracks.isUsed);
}

TEST_CASE("wem material block: a swap moves the alpha track with its layer",
          "[wem][materials][edit]") {
    mdx::Material material;
    material.layers.push_back(sdLayer(0, 800, mdx::Layer::FilterMode::None));
    material.layers.push_back(sdLayer(1, 800, mdx::Layer::FilterMode::Blend));
    material.layers[1].alphaTracks = makeTrack<f32>({0, 1000}, {1.0f, 0.25f});
    Document document = convert(makeModel(800, 2, material));
    const AnimChannel* alpha =
        channelWith(document, TrackTarget::Kind::MaterialLayer, Channel::Alpha);
    REQUIRE(alpha != nullptr);
    REQUIRE(alpha->target.sub == 1u);

    MdxConverter converter;
    SECTION("swapped: the track follows its layer to position 0") {
        native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
        std::swap(block.layers[0], block.layers[1]);
        const MaterialBlockResult result =
            converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block, {1, 0}));
        REQUIRE(result.ok);
        CHECK(result.channelsRemapped == 1u);
        alpha = channelWith(document, TrackTarget::Kind::MaterialLayer, Channel::Alpha);
        CHECK(alpha->target.sub == 0u);
        const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 800);
        REQUIRE(out.materials[0].layers.size() == 2u);
        CHECK(out.materials[0].layers[0].textureId == 1u);
        CHECK(out.materials[0].layers[0].alphaTracks.isUsed);
        CHECK_FALSE(out.materials[0].layers[1].alphaTracks.isUsed);
    }
    SECTION("the keyed layer removed: its channel is invalidated, not repointed") {
        native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
        block.layers.pop_back();
        const MaterialBlockResult result = converter.setMaterialBlock(
            document, editOf(ProfileId::Wc3Classic, block, {0, kInvalidIndex}));
        REQUIRE(result.ok);
        CHECK(result.channelsInvalidated == 1u);
        alpha = channelWith(document, TrackTarget::Kind::MaterialLayer, Channel::Alpha);
        REQUIRE(alpha != nullptr);
        CHECK(alpha->target.material.slot == kInvalidIndex);
        const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 800);
        REQUIRE(out.materials[0].layers.size() == 1u);
        CHECK_FALSE(out.materials[0].layers[0].alphaTracks.isUsed);
    }
}

TEST_CASE("wem material block: a UV animation stays on its layer through a swap",
          "[wem][materials][edit][uv]") {
    mdx::Material material;
    material.layers.push_back(sdLayer(0, 800, mdx::Layer::FilterMode::None));
    material.layers.push_back(sdLayer(1, 800, mdx::Layer::FilterMode::Additive));
    material.layers[0].textureAnimationId = 0;
    material.layers[1].textureAnimationId = 1;
    mdx::Model model = makeModel(800, 2, material);
    const auto scroll = [](Vector3f to) {
        mdx::TextureAnimation animation;
        animation.translationTracks =
            makeTrack<Vector3f>({0, 1000}, {Vector3f{0, 0, 0}, to});
        return animation;
    };
    model.textureAnimations = {scroll(Vector3f{1, 0, 0}), scroll(Vector3f{0, 2, 0})};
    Document document = convert(model);
    const std::vector<MaterialFeature> before =
        materialOf(document, ProfileId::Wc3Classic).Common().features;

    MdxConverter converter;
    native::MdxMaterial block = blockOf(document, ProfileId::Wc3Classic);
    std::swap(block.layers[0], block.layers[1]);
    REQUIRE(converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, block, {1, 0})).ok);

    // Same ids, layers exchanged: the sub-tracks join the id and follow.
    const std::vector<MaterialFeature>& after =
        materialOf(document, ProfileId::Wc3Classic).Common().features;
    u32 uvFeatures = 0;
    for (const MaterialFeature& feature : after) {
        if (feature.kind() != FeatureKind::UvAnimation) {
            continue;
        }
        ++uvFeatures;
        for (const MaterialFeature& old : before) {
            if (old.id == feature.id) {
                CHECK(feature.layer == 1u - old.layer);
            }
        }
    }
    CHECK(uvFeatures == 2u);

    const mdx::Model out = exportAs(document, ProfileId::Wc3Classic, 800);
    REQUIRE(out.materials[0].layers.size() == 2u);
    const auto scrollOf = [&](u32 layer) {
        const u32 id = out.materials[0].layers[layer].textureAnimationId;
        REQUIRE(id < out.textureAnimations.size());
        return out.textureAnimations[id].translationTracks.keys_data.back();
    };
    // The layer now first is the one that scrolled along V.
    CHECK(scrollOf(0).y == Catch::Approx(2.0f));
    CHECK(scrollOf(1).x == Catch::Approx(1.0f));
}

TEST_CASE("wem material block: the default blocks", "[wem][materials][edit]") {
    mdx::Material material;
    material.layers.push_back(sdLayer(0, 1200, mdx::Layer::FilterMode::None));
    Document document = convert(makeModel(1200, 2, material));
    const std::size_t textures = document.textures.size();
    MdxConverter converter;

    SECTION("Reforged: six slots, the neutrals interned once however often") {
        const MaterialBlockDraft first =
            converter.defaultMaterialBlock(document, ProfileId::Wc3Reforged, 1, 1200);
        CHECK(first.texturesAppended == 5u);
        CHECK(document.textures.size() == textures + 5u);
        REQUIRE(first.block.layers.size() == 1u);
        const native::MdxLayer& layer = first.block.layers[0];
        CHECK(layer.isHd);
        CHECK(layer.shaderType == native::MdxShaderType::HD);
        REQUIRE(layer.subTextures.size() == 6u);
        CHECK(layer.subTextures[0].textureId == 1u);
        const TextureRef& team = document.textures[layer.subTextures[4].textureId];
        CHECK(team.replaceableId == 1u);
        CHECK(team.path.empty());
        CHECK(document.textures[layer.subTextures[1].textureId].path == "Textures/normal.blp");
        CHECK(document.textures[layer.subTextures[1].textureId].flags == 3u);

        const MaterialBlockDraft second =
            converter.defaultMaterialBlock(document, ProfileId::Wc3Reforged, 0, 1200);
        CHECK(second.texturesAppended == 0u);
        CHECK(document.textures.size() == textures + 5u);
        for (u32 s = 1; s < 6; ++s) {
            CHECK(second.block.layers[0].subTextures[s].textureId ==
                  layer.subTextures[s].textureId);
        }
    }
    SECTION("Classic: the colour map where the version keeps it") {
        const MaterialBlockDraft modern =
            converter.defaultMaterialBlock(document, ProfileId::Wc3Classic, 1, 1100);
        CHECK(modern.texturesAppended == 0u);
        REQUIRE(modern.block.layers[0].subTextures.size() == 1u);
        CHECK(modern.block.layers[0].subTextures[0].textureId == 1u);
        CHECK(modern.block.layers[0].shaderType == native::MdxShaderType::SD);
        CHECK_FALSE(modern.block.layers[0].isHd);
        CHECK(modern.block.layers[0].alpha == 1.0f);

        const MaterialBlockDraft legacy =
            converter.defaultMaterialBlock(document, ProfileId::Wc3Classic, 1, 800);
        CHECK(legacy.block.layers[0].subTextures.empty());
        CHECK(legacy.block.layers[0].textureId == 1u);
    }
    SECTION("a default block goes through setMaterialBlock as it is") {
        const MaterialBlockDraft draft =
            converter.defaultMaterialBlock(document, ProfileId::Wc3Classic, 1, 1200);
        const MaterialBlockResult result =
            converter.setMaterialBlock(document, editOf(ProfileId::Wc3Classic, draft.block));
        REQUIRE(result.ok);
        CHECK(materialOf(document, ProfileId::Wc3Classic).Common().inputAt(0)->texture == 1u);
    }
}
