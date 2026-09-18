// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// `RemoveTexture` over the §7.4 referencer table (EDIT_MODE_MATERIALS_DESIGN.md
/// §5.4, G12). The property that matters is not the indices — they are
/// supposed to move — but the FILES: after a removal, every reference in the
/// exported model names the file it named before, except the ones that named
/// the removed texture, which name the replacement's.
///
/// Two rows are traps and each has a case: the native block renumbers too, and
/// a ribbon's `KRTX` is a `TextureIndex` that is not a texture.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/mdx/structures.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/reflect_bytes.h>
#include <whiteout/models/wem/validate.h>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

mdx::Track<u32> keys(std::vector<u32> values) {
    mdx::Track<u32> track;
    track.isUsed = true;
    track.interpolationType = mdx::InterpolationType::None;
    track.keyCount = values.size();
    for (std::size_t i = 0; i < values.size(); ++i) {
        track.timestamps.push_back(static_cast<u32>(i * 100));
    }
    track.keys_data = std::move(values);
    return track;
}

mdx::Layer sdLayer(u32 texture, mdx::Layer::FilterMode filter = mdx::Layer::FilterMode::None) {
    mdx::Layer layer;
    layer.filterMode = filter;
    layer.shader = mdx::Layer::ShaderType::SD;
    layer.textureAnimationId = 0xFFFFFFFF;
    mdx::Layer::SubTexture sub;
    sub.textureId = texture;
    layer.subTextures.push_back(sub);
    return layer;
}

mdx::Geoset geosetOn(u32 material) {
    mdx::Geoset geoset;
    geoset.lodName = "g" + std::to_string(material);
    geoset.vertexPositions = {Vector3f{0, 0, 0}, Vector3f{1, 0, 0}, Vector3f{1, 1, 0}};
    geoset.vertexNormals = {Vector3f{0, 0, 1}, Vector3f{0, 0, 1}, Vector3f{0, 0, 1}};
    geoset.textureCoordinateSets.push_back({Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}});
    geoset.faces = {0, 1, 2};
    geoset.materialId = material;
    return geoset;
}

/// Six textures, and every row of the table naming some of them: classic
/// layers through their first sub-texture, one of them flipbooking over 0..3;
/// an HD layer over all six slots; a particle emitter on 2; and a ribbon whose
/// `KRTX` keys 0..3 — which must NOT move.
mdx::Model makeModel() {
    mdx::Model model;
    model.version = 1200;
    model.modelName = "textures";
    mdx::Sequence stand;
    stand.name = "Stand";
    stand.intervalStart = 0;
    stand.intervalEnd = 1000;
    model.sequences.push_back(stand);
    for (u32 t = 0; t < 6; ++t) {
        mdx::Texture texture;
        texture.fileName = "textures/t" + std::to_string(t) + ".blp";
        texture.flags = mdx::Texture::Flag::WrapWidth | mdx::Texture::Flag::WrapHeight;
        model.textures.push_back(texture);
    }

    mdx::Material flipbook;
    flipbook.layers.push_back(sdLayer(1));
    flipbook.layers[0].textureIdTracks = keys({0, 1, 2, 3});
    model.materials.push_back(flipbook);

    mdx::Material twoLayers;
    twoLayers.layers.push_back(sdLayer(3));
    twoLayers.layers.push_back(sdLayer(0, mdx::Layer::FilterMode::Additive));
    model.materials.push_back(twoLayers);

    mdx::Material hd;
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
    hd.layers.push_back(layer);
    model.materials.push_back(hd);

    mdx::Bone root;
    root.node.name = "root";
    root.node.objectId = 0;
    root.node.parentId = mdx::Node::NO_PARENT;
    model.bones.push_back(root);

    mdx::ParticleEmitter2 particles;
    particles.node.name = "particles";
    particles.node.objectId = 1;
    particles.node.parentId = 0;
    particles.textureId = 2;
    model.particleEmitters2.push_back(particles);

    mdx::RibbonEmitter ribbon;
    ribbon.node.name = "ribbon";
    ribbon.node.objectId = 2;
    ribbon.node.parentId = 0;
    ribbon.rows = 2;
    ribbon.columns = 2;
    ribbon.materialId = 0;
    ribbon.textureSlotTracks = keys({0, 1, 2, 3});
    model.ribbonEmitters.push_back(ribbon);

    model.pivotPoints = {Vector3f{0, 0, 0}, Vector3f{0, 0, 0}, Vector3f{0, 0, 0}};
    for (u32 m = 0; m < 3; ++m) {
        model.geosets.push_back(geosetOn(m));
    }
    return model;
}

Document convert() {
    MdxConverter converter;
    Result<Document> result = converter.fromMdx(makeModel());
    REQUIRE(result.ok());
    return std::move(*result.value);
}

/// Every texture reference an exported model makes, as the FILE it names, in a
/// fixed order: each layer's maps, each flipbook key, each particle emitter.
std::vector<std::string> referencedFiles(const mdx::Model& model) {
    std::vector<std::string> files;
    const auto file = [&](u32 index) {
        return index < model.textures.size() ? model.textures[index].fileName : std::string("?");
    };
    for (const mdx::Material& material : model.materials) {
        for (const mdx::Layer& layer : material.layers) {
            if (layer.subTextures.empty()) {
                files.push_back(file(layer.textureId));
            }
            for (const mdx::Layer::SubTexture& sub : layer.subTextures) {
                files.push_back(file(sub.textureId));
            }
            for (const u32 key : layer.textureIdTracks.keys_data) {
                files.push_back(file(key));
            }
        }
    }
    for (const mdx::ParticleEmitter2& emitter : model.particleEmitters2) {
        files.push_back(file(emitter.textureId));
    }
    return files;
}

mdx::Model exportAs(const Document& document, ProfileId profile) {
    MdxConverter converter;
    Result<mdx::Model> exported = converter.toMdx(document, profile, 1200);
    REQUIRE(exported.ok());
    return std::move(*exported.value);
}

std::vector<u32> ribbonKeys(const mdx::Model& model) {
    REQUIRE(model.ribbonEmitters.size() == 1u);
    return model.ribbonEmitters[0].textureSlotTracks.keys_data;
}

} // namespace

TEST_CASE("wem remove texture: every reference names the same file, and a ribbon's cells stay",
          "[wem][materials][textures]") {
    Document document = convert();
    const std::vector<std::string> classicBefore =
        referencedFiles(exportAs(document, ProfileId::Wc3Classic));
    const std::vector<std::string> hdBefore =
        referencedFiles(exportAs(document, ProfileId::Wc3Reforged));
    REQUIRE(ribbonKeys(exportAs(document, ProfileId::Wc3Classic)) == std::vector<u32>{0, 1, 2, 3});

    const RemovalResult result = RemoveTexture(document, 1, 3);
    REQUIRE(result.removed);
    CHECK(document.textures.size() == 5u);
    CHECK(result.remap == std::vector<u32>{0, kInvalidIndex, 1, 2, 3, 4});
    CHECK(Validate(document, ValidateLevel::Profile).countOf(DiagCode::IndexOutOfRange) == 0u);

    // What named t1 now names t3, whatever index that is now; everything else
    // names what it did.
    const auto expected = [](std::vector<std::string> files) {
        for (std::string& file : files) {
            if (file == "textures/t1.blp") {
                file = "textures/t3.blp";
            }
        }
        return files;
    };
    const mdx::Model classic = exportAs(document, ProfileId::Wc3Classic);
    CHECK(referencedFiles(classic) == expected(classicBefore));
    CHECK(referencedFiles(exportAs(document, ProfileId::Wc3Reforged)) == expected(hdBefore));

    // A ribbon's KRTX is a cell of its own 2x2 grid, not a texture.
    CHECK(ribbonKeys(classic) == std::vector<u32>{0, 1, 2, 3});
}

TEST_CASE("wem texture referencers are counted once each, by what they are",
          "[wem][materials][textures]") {
    const Document document = convert();
    // A v1200 file: the classic set holds its SD materials, and the Reforged
    // set holds them too (an HD model uses SD) beside the HD one. Each set's
    // material is a referencer of its own, and so is each set's flipbook track.
    //
    // t1: the flipbook layer's map in each set, and the HD layer's normal slot;
    // the keys 0..3 name it once per set. A common input mirrors its block and
    // is not counted again.
    const TextureReferencerCount one = CountTextureReferencers(document, 1);
    CHECK(one.materialLayers == 3u);
    CHECK(one.payloadLinks == 0u);
    CHECK(one.flipbookKeys == 2u);
    // t2: the HD ORM slot, the particle emitter, one key per set. The ribbon's
    // KRTX keys 0..3 too, and is not a texture.
    const TextureReferencerCount two = CountTextureReferencers(document, 2);
    CHECK(two.materialLayers == 1u);
    CHECK(two.payloadLinks == 1u);
    CHECK(two.flipbookKeys == 2u);
}

TEST_CASE("wem remove texture: refused while used and unnamed, immediate when unused",
          "[wem][materials][textures]") {
    Document document = convert();
    const Document before = document;

    const RemovalResult refused = RemoveTexture(document, 1);
    CHECK_FALSE(refused.removed);
    CHECK(SameReflected(document, before));
    CHECK_FALSE(RemoveTexture(document, 1, 1).removed);
    CHECK(SameReflected(document, before));

    TextureRef spare;
    spare.path = "textures/spare.blp";
    document.textures.push_back(spare);
    const RemovalResult unused = RemoveTexture(document, 6);
    CHECK(unused.removed);
    CHECK(SameReflected(document, before));
}

TEST_CASE("wem remove texture: a native kind with no rows in the table is refused",
          "[wem][materials][textures]") {
    Document document = convert();
    // An M2 block in a Warcraft III set is not a document anyone makes; it
    // stands for any kind whose texture references were never audited.
    document.models[0].profileSets[0].materials[0].SetNativeInSync(native::M2Material{});
    const Document before = document;
    const RemovalResult result = RemoveTexture(document, 5);
    CHECK_FALSE(result.removed);
    CHECK(result.diagnostics.countOf(DiagCode::OperationUnsupported) == 1u);
    CHECK(SameReflected(document, before));
}

TEST_CASE("wem check texture referencers names each row out of range",
          "[wem][materials][textures]") {
    Document document = convert();
    Diagnostics clean;
    CheckTextureReferencers(document, clean);
    CHECK(clean.countOf(DiagCode::IndexOutOfRange) == 0u);

    // Drop the last texture without the operation: the HD slot and nothing
    // else named it.
    document.textures.pop_back();
    Diagnostics broken;
    CheckTextureReferencers(document, broken);
    CHECK(broken.countOf(DiagCode::IndexOutOfRange) >= 1u);
}
