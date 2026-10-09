// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// EDIT_MODE_OPTIMIZE_DESIGN.md §6: `OptimizeDocument` draws and animates what
/// it was given. Every fixture is an `.mdx` through `fromMdx`, the path Edit
/// Mode takes, and every one is checked the same three ways: each vertex posed
/// by every clip lands where the source's did, each section still wears the
/// source's material, and the document validates and writes back.
///
/// A green that changed nothing proves nothing, so each case also says what
/// the pass should have done, and the corpus sweep counts it.

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/pose.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/materials/draw_order.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/native/mdx_native.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/optimize.h>
#include <whiteout/models/wem/reflect_bytes.h>
#include <whiteout/models/wem/rigging/tpose.h>
#include <whiteout/models/wem/skinning/deform.h>
#include <whiteout/models/wem/validate.h>

#include "wem_corpus_files.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace whiteout;
using namespace whiteout::models::wem;

namespace {

using NodeType = mdx::Node::NodeType;
constexpr u32 kNoParent = mdx::Node::NO_PARENT;

/// An `.mdx` built one record at a time. Nodes are numbered in the order they
/// are added, so a case adds its bones first, as every shipped file does.
struct Rig {
    mdx::Model model;
    u32 nextObject = 0;

    explicit Rig(u32 version = 800) {
        model.version = version;
        model.modelName = "rig";
        mdx::Sequence stand;
        stand.name = "Stand";
        stand.intervalStart = 0;
        stand.intervalEnd = 1000;
        model.sequences.push_back(stand);
    }

    u32 texture(const std::string& path) {
        mdx::Texture texture;
        texture.fileName = path;
        model.textures.push_back(texture);
        return static_cast<u32>(model.textures.size() - 1);
    }

    u32 material(u32 texture, mdx::Layer::FilterMode filter = mdx::Layer::FilterMode::None) {
        mdx::Material material;
        mdx::Layer layer;
        layer.textureId = texture;
        layer.filterMode = filter;
        material.layers.push_back(layer);
        model.materials.push_back(material);
        return static_cast<u32>(model.materials.size() - 1);
    }

    mdx::Node node(const std::string& name, u32 parent, NodeType type, const Vector3f& pivot) {
        mdx::Node node;
        node.name = name;
        node.objectId = nextObject++;
        node.parentId = parent;
        node.type = type;
        model.pivotPoints.push_back(pivot);
        return node;
    }

    u32 bone(const std::string& name, u32 parent, const Vector3f& pivot, bool keyed = false) {
        mdx::Bone bone;
        bone.node = node(name, parent, NodeType::Bone, pivot);
        if (keyed) {
            turn(bone.node);
        }
        model.bones.push_back(bone);
        return bone.node.objectId;
    }

    u32 helper(const std::string& name, u32 parent, const Vector3f& pivot, bool keyed = false) {
        mdx::Helper helper;
        helper.node = node(name, parent, NodeType::Helper, pivot);
        if (keyed) {
            turn(helper.node);
        }
        model.helpers.push_back(helper);
        return helper.node.objectId;
    }

    u32 attachment(const std::string& name, u32 parent, const Vector3f& pivot) {
        mdx::Attachment attachment;
        attachment.node = node(name, parent, NodeType::Attachment, pivot);
        model.attachments.push_back(attachment);
        return attachment.node.objectId;
    }

    /// A quarter turn about z across Stand, so a keyed bone visibly moves.
    static void turn(mdx::Node& node) {
        node.rotationTracks.isUsed = true;
        node.rotationTracks.interpolationType = mdx::InterpolationType::Linear;
        node.rotationTracks.keyCount = 2;
        node.rotationTracks.timestamps = {0, 1000};
        node.rotationTracks.keys_data = {Quaternion{0, 0, 0, 1},
                                         Quaternion{0, 0, 0.70710678f, 0.70710678f}};
    }

    mdx::Bone& boneRecord(u32 objectId) {
        for (mdx::Bone& bone : model.bones) {
            if (bone.node.objectId == objectId) {
                return bone;
            }
        }
        FAIL("no bone " << objectId);
        return model.bones.front();
    }

    /// @p quads unit quads at @p at, every vertex in one matrix group of
    /// @p bones (object ids).
    u32 geoset(u32 material, const std::vector<u32>& bones, const Vector3f& at, u32 quads = 1,
               bool secondUvSet = false) {
        mdx::Geoset geoset;
        geoset.lodName = "geoset_" + std::to_string(model.geosets.size());
        geoset.materialId = material;
        std::vector<Vector2f> uvs;
        for (u32 q = 0; q < quads; ++q) {
            const f32 x = at.x + static_cast<f32>(q % 100) * 1.5f;
            const f32 y = at.y + static_cast<f32>(q / 100) * 1.5f;
            const u16 base = static_cast<u16>(geoset.vertexPositions.size());
            geoset.vertexPositions.insert(geoset.vertexPositions.end(),
                                          {Vector3f{x, y, at.z}, Vector3f{x + 1, y, at.z},
                                           Vector3f{x + 1, y + 1, at.z}, Vector3f{x, y + 1, at.z}});
            for (u32 v = 0; v < 4; ++v) {
                geoset.vertexNormals.push_back(Vector3f{0, 0, 1});
                geoset.vertexGroups.push_back(0);
            }
            uvs.insert(uvs.end(), {Vector2f{0, 0}, Vector2f{1, 0}, Vector2f{1, 1}, Vector2f{0, 1}});
            geoset.faces.insert(geoset.faces.end(),
                                {base, static_cast<u16>(base + 1), static_cast<u16>(base + 2), base,
                                 static_cast<u16>(base + 2), static_cast<u16>(base + 3)});
        }
        geoset.textureCoordinateSets.push_back(uvs);
        if (secondUvSet) {
            geoset.textureCoordinateSets.push_back(uvs);
        }
        geoset.matrixGroups = {static_cast<u32>(bones.size())};
        geoset.matrixIndices = bones;
        model.geosets.push_back(geoset);
        return static_cast<u32>(model.geosets.size() - 1);
    }

    /// A keyed alpha on @p geoset: 0 at the start of Stand, 1 at @p end.
    u32 fade(u32 geoset, u32 end = 1000) {
        mdx::GeosetAnimation animation;
        animation.geosetId = geoset;
        animation.alphaTracks.isUsed = true;
        animation.alphaTracks.interpolationType = mdx::InterpolationType::Linear;
        animation.alphaTracks.keyCount = 2;
        animation.alphaTracks.timestamps = {0, end};
        animation.alphaTracks.keys_data = {0.0f, 1.0f};
        model.geosetAnimations.push_back(animation);
        return static_cast<u32>(model.geosetAnimations.size() - 1);
    }

    void gate(u32 bone, u32 geoset, u32 record) {
        boneRecord(bone).geosetId = geoset;
        boneRecord(bone).geosetAnimationId = record;
    }

    Document import() const {
        const MdxConverter converter;
        Result<Document> document = converter.fromMdx(model);
        REQUIRE(document.ok());
        return std::move(*document.value);
    }
};

u32 nodeNamed(const Model& model, const std::string& name) {
    for (u32 n = 0; n < model.nodes.size(); ++n) {
        if (model.nodes.nodes[n].name == name) {
            return n;
        }
    }
    return kInvalidNode;
}

/// The times a clip is read at: its ends, a point off every key, and each key.
std::vector<f32> sampleTimes(const ClipPose& pose, const Clip& clip, std::size_t cap) {
    std::vector<f32> times = {0.0f, clip.duration * 0.37f, clip.duration};
    for (const f32 t : pose.keyTimes()) {
        times.push_back(t);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() > cap) {
        std::vector<f32> thinned;
        for (std::size_t i = 0; i < cap; ++i) {
            thinned.push_back(times[i * times.size() / cap]);
        }
        times = std::move(thinned);
    }
    return times;
}

/// A material as it draws: its texture indices replaced by the texture they
/// name, compared by content across both documents, and its name dropped where
/// the MDX block is what is written (`fromMdx` names a material for its slot).
struct DrawnMaterials {
    std::map<std::vector<u8>, u32> textures;

    u32 textureId(const Document& document, u32 index) {
        if (index >= document.textures.size()) {
            return index;
        }
        return textures.emplace(ReflectBytes(document.textures[index]), 0x10000u + textures.size())
            .first->second;
    }

    std::vector<u8> of(const Document& document, const Material& material) {
        return ReflectBytes(normalised(document, material));
    }

    Material normalised(const Document& document, const Material& material) {
        Material copy = material;
        if (copy.NativeIsAuthoritative()) {
            copy.name.clear();
        }
        CommonMaterial& common = copy.InitCommon();
        for (u32 ordinal = 0; ordinal < common.ordinalCount(); ++ordinal) {
            if (TextureInput* input = common.inputAt(ordinal)) {
                input->texture = textureId(document, input->texture);
            }
        }
        if (const auto* block = std::get_if<native::MdxMaterial>(&copy.Native())) {
            native::MdxMaterial edited = *block;
            for (native::MdxLayer& layer : edited.layers) {
                // The layer's own field names a texture only where it has no
                // sub-textures (`forEachTextureReferencer`): beside them it is
                // a number the file carries and nothing reads.
                if (layer.subTextures.empty()) {
                    layer.textureId = textureId(document, layer.textureId);
                }
                for (native::MdxSubTexture& sub : layer.subTextures) {
                    sub.textureId = textureId(document, sub.textureId);
                }
            }
            copy.SetNativeAuthoritative(std::move(edited));
        }
        return copy;
    }
};

/// Every leaf `reflect()` names, by its path: what `ReflectBytes` compares, kept
/// apart so a mismatch can say which field it is.
class ReflectLeaves {
public:
    static constexpr bool kReading = false;

    std::vector<std::pair<std::string, std::vector<u8>>> leaves;

    template <class T>
    void field(const char* name, T& value) {
        const std::size_t mark = path_.size();
        path_ += '.';
        path_ += name;
        visit(value);
        path_.resize(mark);
    }
    template <class T>
    void optional(const char* name, std::optional<T>& value) {
        if (value.has_value()) {
            field(name, *value);
        } else {
            leaves.emplace_back(path_ + '.' + name, std::vector<u8>{});
        }
    }
    template <class T>
    void inlineList(const char* name, std::vector<T>& value) {
        field(name, value);
    }
    template <class C>
    void count(const char* name, const C& container) {
        u64 size = container.size();
        field(name, size);
    }
    template <class T>
    void chunk(const char* name, T& value) {
        field(name, value);
    }
    template <class Alt, class Variant>
    void chunkAlternative(const char* name, Variant& value, u32&) {
        if (std::holds_alternative<Alt>(value)) {
            field(name, std::get<Alt>(value));
        }
    }
    ReflectLeaves& since(u32) {
        return *this;
    }
    template <class T, class F>
    void fieldOr(const char* name, T& value, F&&) {
        field(name, value);
    }

private:
    template <class T>
    void visit(T& value) {
        constexpr FieldKind kind = ClassifyField<T>();
        if constexpr (kind == FieldKind::Reflected) {
            value.reflect(*this);
        } else if constexpr (kind == FieldKind::Vector) {
            using Element = typename T::value_type;
            if constexpr (ClassifyField<Element>() == FieldKind::Raw) {
                leaves.emplace_back(path_, ReflectBytes(value));
            } else {
                for (std::size_t i = 0; i < value.size(); ++i) {
                    const std::size_t mark = path_.size();
                    path_ += '[' + std::to_string(i) + ']';
                    visit(value[i]);
                    path_.resize(mark);
                }
                leaves.emplace_back(path_ + ".size", ReflectBytes(static_cast<u64>(value.size())));
            }
        } else {
            leaves.emplace_back(path_, ReflectBytes(value));
        }
    }

    std::string path_;
};

/// The first reflected field @p a and @p b differ in, or empty.
template <class T>
std::string firstDifference(const T& a, const T& b) {
    ReflectLeaves left;
    ReflectLeaves right;
    left.field("", const_cast<T&>(a));
    right.field("", const_cast<T&>(b));
    for (std::size_t i = 0; i < left.leaves.size() && i < right.leaves.size(); ++i) {
        if (left.leaves[i] != right.leaves[i]) {
            return left.leaves[i].first == right.leaves[i].first
                       ? left.leaves[i].first
                       : left.leaves[i].first + " / " + right.leaves[i].first;
        }
    }
    return left.leaves.size() != right.leaves.size() ? "a different number of fields" : "";
}

struct Equivalence {
    u32 vertices = 0;
    u32 misplaced = 0;       ///< A posed vertex away from the source's.
    u32 restMoved = 0;       ///< A rest position that is not the source's bits.
    u32 countMismatch = 0;   ///< A mesh whose vertex count is not its origins'.
    u32 materialChanged = 0; ///< A section that wears another material.
    std::string materialNote; ///< The first of them: which mesh, and which field.
    /// Non-finite in the source and here alike: a source whose tracks pose to
    /// NaN, kept as broken as it came. One side alone is a misplaced vertex.
    u32 bothNonFinite = 0;
    f32 worst = 0.0f;
};

bool finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// @p after against @p before, model 0, through @p report's origins.
Equivalence compare(const Document& before, const Document& after, const OptimizeReport& report,
                    std::size_t samplesPerClip = 12) {
    Equivalence out;
    const Model& a = before.models[0];
    const Model& b = after.models[0];
    std::vector<std::vector<u32>> origins =
        report.models.empty() ? std::vector<std::vector<u32>>{} : report.models[0].meshOrigins;
    if (origins.empty()) {
        for (u32 m = 0; m < b.meshes.size(); ++m) {
            origins.push_back({m});
        }
    }
    REQUIRE(origins.size() == b.meshes.size());

    f32 extent = 1.0f;
    for (const Mesh& mesh : a.meshes) {
        for (const Vector3f& p :
             mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex)) {
            extent = std::max({extent, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)});
        }
    }
    const f32 tolerance = 1e-4f * extent;
    DrawnMaterials drawn;

    // Rest positions and materials: exact.
    for (u32 k = 0; k < b.meshes.size(); ++k) {
        const auto merged =
            b.meshes[k].attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        std::size_t at = 0;
        for (const u32 o : origins[k]) {
            const auto source =
                a.meshes[o].attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            for (std::size_t v = 0; v < source.size(); ++v, ++at) {
                if (at >= merged.size()) {
                    break;
                }
                out.restMoved += std::memcmp(&source[v], &merged[at], sizeof(Vector3f)) != 0 ? 1 : 0;
            }
            for (const MeshSection& section : a.meshes[o].sections) {
                for (const ProfileId profile : before.profiles) {
                    const Material* was = Resolve(a, section.materialSlot, profile);
                    const Material* now = Resolve(b, b.meshes[k].sections[0].materialSlot, profile);
                    if ((was == nullptr) != (now == nullptr) ||
                        (was != nullptr && drawn.of(before, *was) != drawn.of(after, *now))) {
                        if (out.materialChanged++ == 0) {
                            out.materialNote =
                                "mesh " + std::to_string(o) + " -> " + std::to_string(k) + " in " +
                                Profile(profile).name + ": " +
                                (was == nullptr || now == nullptr
                                     ? "a material on one side only"
                                     : firstDifference(drawn.normalised(before, *was),
                                                       drawn.normalised(after, *now)));
                        }
                    }
                }
            }
        }
        out.countMismatch += at != merged.size() ? 1 : 0;
    }

    // Every clip, posed.
    std::vector<Matrix44f> paletteA;
    std::vector<Matrix44f> paletteB;
    for (u32 c = 0; c < before.clips.size(); ++c) {
        if (before.clips[c].model != 0) {
            continue;
        }
        const ClipPose poseA(before, 0, c);
        const ClipPose poseB(after, 0, c);
        if (poseA.empty() || poseB.empty()) {
            continue;
        }
        for (const f32 t : sampleTimes(poseA, before.clips[c], samplesPerClip)) {
            poseA.skinningAt(t, paletteA);
            poseB.skinningAt(t, paletteB);
            for (u32 k = 0; k < b.meshes.size(); ++k) {
                const std::vector<Vector3f> posed = skinning::DeformMesh(b.meshes[k], paletteB);
                std::size_t at = 0;
                for (const u32 o : origins[k]) {
                    const std::vector<Vector3f> source = skinning::DeformMesh(a.meshes[o], paletteA);
                    for (std::size_t v = 0; v < source.size() && at < posed.size(); ++v, ++at) {
                        ++out.vertices;
                        const bool wasFinite = finite(source[v]);
                        const bool isFinite = finite(posed[at]);
                        if (!wasFinite || !isFinite) {
                            out.bothNonFinite += !wasFinite && !isFinite ? 1u : 0u;
                            out.misplaced += wasFinite != isFinite ? 1u : 0u;
                            continue;
                        }
                        const f32 d = std::max({std::fabs(source[v].x - posed[at].x),
                                                std::fabs(source[v].y - posed[at].y),
                                                std::fabs(source[v].z - posed[at].z)});
                        out.misplaced += d <= tolerance ? 0u : 1u;
                        out.worst = std::max(out.worst, d);
                    }
                }
            }
        }
    }
    return out;
}

void requireEquivalent(const Document& before, const Document& after, const OptimizeReport& report) {
    const Equivalence same = compare(before, after, report);
    INFO("worst posed distance " << same.worst);
    CHECK(same.countMismatch == 0);
    CHECK(same.restMoved == 0);
    CHECK(same.materialChanged == 0);
    CHECK(same.vertices > 0);
    CHECK(same.bothNonFinite == 0);
    CHECK(same.misplaced == 0);
}

void requireWritable(const Document& document) {
    const Diagnostics report = Validate(document, ValidateLevel::Profile);
    INFO(report.formatHistogram());
    CHECK_FALSE(report.hasErrors());
    const MdxConverter converter;
    for (const ProfileId profile : document.profiles) {
        CHECK(converter.toMdx(document, profile, MdxFileVersion(profile)).ok());
    }
}

/// Runs the pass on a copy and checks the three things every case checks.
struct Optimized {
    Document before;
    Document after;
    OptimizeReport report;
};

Optimized optimize(const Rig& rig, const OptimizeOptions& options = {}) {
    Optimized out;
    out.before = rig.import();
    out.after = out.before;
    out.report = OptimizeDocument(out.after, options);
    CHECK_FALSE(out.report.diagnostics.hasErrors());
    requireEquivalent(out.before, out.after, out.report);
    requireWritable(out.after);
    return out;
}

/// The weight vertex @p v of @p mesh carries on nodes that are not bones.
f32 nonBoneWeight(const Model& model, const Mesh& mesh, u32 v) {
    f32 weight = 0.0f;
    for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
        if (influence.bone < model.nodes.size() &&
            model.nodes.nodes[influence.bone].kind != NodeKind::Bone) {
            weight += influence.weight;
        }
    }
    return weight;
}

f32 nonBoneWeight(const Model& model) {
    f32 weight = 0.0f;
    for (const Mesh& mesh : model.meshes) {
        for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
            weight += nonBoneWeight(model, mesh, v);
        }
    }
    return weight;
}

/// The vertices of @p after that carry more weight on non-bones than the
/// source vertex they came from: the pass must never make one. Per vertex,
/// because a count over the model lets one promotion hide one gain.
u32 verticesGainingNonBoneWeight(const Document& before, const Document& after,
                                 const OptimizeReport& report) {
    const Model& a = before.models[0];
    const Model& b = after.models[0];
    const bool merged = !report.models.empty() && !report.models[0].meshOrigins.empty();
    u32 gained = 0;
    for (u32 k = 0; k < b.meshes.size(); ++k) {
        const std::vector<u32> origins = merged ? report.models[0].meshOrigins[k] : std::vector<u32>{k};
        u32 at = 0;
        for (const u32 o : origins) {
            for (u32 v = 0; v < a.meshes[o].skin.vertexCount() && at < b.meshes[k].skin.vertexCount();
                 ++v, ++at) {
                gained += nonBoneWeight(b, b.meshes[k], at) > nonBoneWeight(a, a.meshes[o], v) + 1e-5f
                              ? 1u
                              : 0u;
            }
        }
    }
    return gained;
}

/// Whether @p material's first layer shades as Reforged HD, by its `.mdx`
/// block: the layer the game asks while it is drawn at all.
bool firstLayerShadesHd(const Material* material) {
    const auto* block = material != nullptr ? std::get_if<native::MdxMaterial>(&material->Native()) : nullptr;
    if (block == nullptr || block->layers.empty()) {
        return false;
    }
    const native::MdxLayer& layer = block->layers.front();
    const bool named = block->shader == "Shader_HD_DefaultUnit" || block->shader == "Shader_HD_Crystal";
    return layer.isHd || layer.shaderType == native::MdxShaderType::HD ||
           layer.shaderType == native::MdxShaderType::Crystal || (block->sourceVersion < 1100 && named);
}

/// Per mesh of @p document's first model, whether its material's first layer
/// is HD and its alpha -- the geoset's, or that layer's -- is ever played drawn
/// and less than full. The game sorts such a geoset by its centroid for as
/// long as that lasts (§3.3). Played, not read off the keys: `Animator` samples
/// every clip at the alpha tracks' key times and between them, so this is an
/// answer of its own to part of the question `DrawsOrderFree` settles from the
/// keys. Only part: a later layer is asked once the first is hidden, which
/// the fixtures cover and this does not.
std::vector<u8> hdMeshesThatFade(const Document& document) {
    const Model& model = document.models[0];
    const std::vector<AnimChannel>& channels = model.animChannels.channels;
    std::vector<u8> fades(model.meshes.size(), 0);
    std::vector<std::vector<u32>> alphas(model.meshes.size());
    bool any = false;
    for (u32 m = 0; m < model.meshes.size(); ++m) {
        if (model.meshes[m].sections.empty()) {
            continue;
        }
        const u32 slot = model.meshes[m].sections[0].materialSlot;
        bool hd = false;
        for (const ProfileId profile : document.profiles) {
            hd = hd || firstLayerShadesHd(Resolve(model, slot, profile));
        }
        for (u32 c = 0; hd && c < channels.size(); ++c) {
            const TrackTarget& target = channels[c].target;
            const bool own = target.kind == TrackTarget::Kind::Section && target.mesh == m;
            // A layer track names its layer's place in the stack: 0 is the first.
            const bool layer = target.kind == TrackTarget::Kind::MaterialLayer &&
                               target.material.slot == slot && target.sub == 0;
            if ((own || layer) && target.channel == Channel::Alpha &&
                channels[c].valueType == geom::AttrType::F32) {
                alphas[m].push_back(c);
                any = true;
            }
        }
    }
    if (!any) {
        return fades;
    }
    const Animator animator(document, 0);
    Pose pose;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model != 0) {
            continue;
        }
        std::vector<f32> times = {0.0f, clip.duration};
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || channel->target.channel != Channel::Alpha) {
                    continue;
                }
                for (std::size_t k = 0; k < track.times.size(); ++k) {
                    times.push_back(track.times[k]);
                    if (k + 1 < track.times.size()) {
                        times.push_back(0.5f * (track.times[k] + track.times[k + 1]));
                    }
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        for (const f32 t : times) {
            if (t < 0.0f || t > clip.duration) {
                continue;
            }
            Mix mix;
            mix.plays.push_back(Play{c, t, 1.0f, false});
            mix.globals = false;
            animator.sample(mix, pose);
            for (u32 m = 0; m < alphas.size(); ++m) {
                for (const u32 channel : alphas[m]) {
                    if (pose.channelValues[channel].size() != sizeof(f32)) {
                        continue;
                    }
                    f32 alpha = 1.0f;
                    std::memcpy(&alpha, pose.channelValues[channel].data(), sizeof(f32));
                    fades[m] = fades[m] != 0 || (alpha > 0.5f / 255.0f && alpha < 1.0f) ? 1 : 0;
                }
            }
        }
    }
    return fades;
}

/// Whether one of the pass's lines holds @p text.
bool says(const OptimizeReport& report, const std::string& text) {
    for (const Diagnostic& entry : report.diagnostics.all()) {
        if (entry.message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Whether `fromMdx` kept a `FAFX` list for @p model.
bool namesFaceFxActor(const Model& model) {
    for (const ProfileMaterialSet& set : model.profileSets) {
        if (set.native.value("faceFxCount", 0) > 0) {
            return true;
        }
    }
    return false;
}

/// Each node's name with its parent's, in no order: what an animation that
/// binds bones by name sees of the tree.
std::multiset<std::pair<std::string, std::string>> namedTree(const Model& model) {
    std::multiset<std::pair<std::string, std::string>> out;
    for (const Node& node : model.nodes.nodes) {
        out.emplace(node.name, node.parent < model.nodes.size() ? model.nodes.nodes[node.parent].name
                                                                : std::string());
    }
    return out;
}

bool noChannelInvalidated(const Model& model) {
    for (const AnimChannel& channel : model.animChannels.channels) {
        if ((channel.target.kind == TrackTarget::Kind::Section &&
             channel.target.mesh >= model.meshes.size()) ||
            (channel.target.kind == TrackTarget::Kind::Node &&
             channel.target.node >= model.nodes.size())) {
            return false;
        }
    }
    return true;
}

} // namespace

// ============================================================================
// Meshes
// ============================================================================

TEST_CASE("optimize merges geosets that draw alike", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(skin, {root}, {0, 0, 0});
    rig.geoset(cloth, {root}, {4, 0, 0});
    rig.geoset(skin, {root}, {8, 0, 0});
    rig.geoset(skin, {root}, {12, 0, 0});

    const Optimized run = optimize(rig);
    CHECK(run.report.meshesMerged == 2);
    REQUIRE(run.after.models[0].meshes.size() == 2);
    // The keeper is the first of them, so it draws where geoset 0 did.
    CHECK(run.report.models[0].meshOrigins == std::vector<std::vector<u32>>{{0, 2, 3}, {1}});
}

TEST_CASE("optimize keeps apart geosets that animate differently", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(skin, {root}, {0, 0, 0});
    const u32 fading = rig.geoset(skin, {root}, {4, 0, 0});
    const u32 twin = rig.geoset(skin, {root}, {8, 0, 0});
    const u32 later = rig.geoset(skin, {root}, {12, 0, 0});
    rig.fade(fading);
    rig.fade(twin);
    rig.fade(later, 500); // the same fade, keyed at other times

    const Optimized run = optimize(rig);
    CHECK(run.report.meshesMerged == 1);
    CHECK(run.report.models[0].meshOrigins ==
          std::vector<std::vector<u32>>{{0}, {fading, twin}, {later}});
    // The absorbed geoset's fade went with it rather than be left invalidated,
    // and the merged mesh still fades.
    const Model& model = run.after.models[0];
    CHECK(noChannelInvalidated(model));
    u32 fades = 0;
    for (const AnimChannel& channel : model.animChannels.channels) {
        fades += channel.target.kind == TrackTarget::Kind::Section ? 1u : 0u;
    }
    CHECK(fades == 2);
}

TEST_CASE("optimize merges geosets once a track that says nothing is gone", "[wem][optimize]") {
    // Both geosets carry a geoset animation; one keys its alpha at 1 throughout,
    // which is what the other shows without a track. The key step drops the
    // sub-track, and the declaration it leaves has to go with it, or the two
    // still differ by a channel that plays nothing.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    const u32 keyed = rig.fade(rig.geoset(skin, {root}, {0, 0, 0}));
    rig.model.geosetAnimations[keyed].alphaTracks.keys_data = {1.0f, 1.0f};
    mdx::GeosetAnimation still;
    still.geosetId = rig.geoset(skin, {root}, {4, 0, 0});
    rig.model.geosetAnimations.push_back(still);

    const Optimized run = optimize(rig);
    CHECK(run.report.channelsDropped == 1);
    CHECK(run.report.meshesMerged == 1);
    for (const AnimChannel& channel : run.after.models[0].animChannels.channels) {
        CHECK(channel.target.kind != TrackTarget::Kind::Section);
    }
    // Without the key step the track is still there, and they stay apart.
    OptimizeOptions keysKept;
    keysKept.reduceKeys = false;
    CHECK(optimize(rig, keysKept).report.meshesMerged == 0);
}

TEST_CASE("optimize leaves translucent geosets apart unless asked", "[wem][optimize]") {
    Rig rig;
    const u32 glow = rig.material(rig.texture("textures/glow.blp"), mdx::Layer::FilterMode::Additive);
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(glow, {root}, {0, 0, 0});
    rig.geoset(glow, {root}, {4, 0, 0});

    CHECK(optimize(rig).report.meshesMerged == 0);
    OptimizeOptions blended;
    blended.mergeBlended = true;
    CHECK(optimize(rig, blended).report.meshesMerged == 1);
}

namespace {

/// A material of one HD layer, as a Reforged file states it from v1100.
u32 hdMaterial(Rig& rig, u32 texture) {
    mdx::Material material;
    mdx::Layer layer;
    layer.textureId = texture;
    layer.shader = mdx::Layer::ShaderType::HD;
    material.layers.push_back(layer);
    rig.model.materials.push_back(material);
    return static_cast<u32>(rig.model.materials.size() - 1);
}

} // namespace

TEST_CASE("optimize leaves apart HD geosets that fade", "[wem][optimize]") {
    // The game sorts an HD geoset with the translucent ones while it is less
    // than fully drawn, whatever its filter says, so two that fade alike sort
    // apart, each by its own centroid.
    Rig fading(1200);
    const u32 skin = hdMaterial(fading, fading.texture("textures/skin.dds"));
    const u32 root = fading.bone("root", kNoParent, {0, 0, 0}, true);
    fading.fade(fading.geoset(skin, {root}, {0, 0, 0}));
    fading.fade(fading.geoset(skin, {root}, {4, 0, 0}));
    CHECK(optimize(fading).report.meshesMerged == 0);
    OptimizeOptions blended;
    blended.mergeBlended = true;
    CHECK(optimize(fading, blended).report.meshesMerged == 1);

    // Shown or hidden with nothing between is never less than fully drawn.
    Rig blinking(1200);
    const u32 plate = hdMaterial(blinking, blinking.texture("textures/skin.dds"));
    const u32 base = blinking.bone("root", kNoParent, {0, 0, 0}, true);
    for (const f32 x : {0.0f, 4.0f}) {
        const u32 record = blinking.fade(blinking.geoset(plate, {base}, {x, 0, 0}));
        blinking.model.geosetAnimations[record].alphaTracks.interpolationType =
            mdx::InterpolationType::None;
    }
    CHECK(optimize(blinking).report.meshesMerged == 1);

    // And an SD geoset is opaque by its filter alone: the fade changes nothing
    // about where it is drawn ("keeps apart geosets that animate differently").
}

TEST_CASE("optimize asks the layer the game would", "[wem][optimize]") {
    // The first layer is opaque and can be keyed out, and the game then asks
    // the second, which blends.
    Rig rig;
    mdx::Material material;
    mdx::Layer under;
    under.textureId = rig.texture("textures/skin.blp");
    under.alphaTracks.isUsed = true;
    under.alphaTracks.interpolationType = mdx::InterpolationType::Linear;
    under.alphaTracks.keyCount = 2;
    under.alphaTracks.timestamps = {0, 1000};
    under.alphaTracks.keys_data = {1.0f, 0.0f};
    mdx::Layer over;
    over.textureId = rig.texture("textures/glow.blp");
    over.filterMode = mdx::Layer::FilterMode::Blend;
    material.layers = {under, over};
    rig.model.materials.push_back(material);
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(0, {root}, {0, 0, 0});
    rig.geoset(0, {root}, {4, 0, 0});
    CHECK(optimize(rig).report.meshesMerged == 0);

    // Never keyed out, the first layer is the one asked, and it is opaque: the
    // second blends with the geoset's own pixels.
    Rig steady = rig;
    steady.model.materials[0].layers[0].alphaTracks = {};
    CHECK(optimize(steady).report.meshesMerged == 1);

    // An alpha-tested first layer leaves holes, and through them the second
    // blends with the scene as it stands when the geoset is drawn.
    Rig cutOut = steady;
    cutOut.model.materials[0].layers[0].filterMode = mdx::Layer::FilterMode::Transparent;
    CHECK(optimize(cutOut).report.meshesMerged == 0);
}

TEST_CASE("optimize leaves apart geosets drawn without the depth buffer", "[wem][optimize]") {
    // A geoset that does not write depth is drawn over by whatever comes
    // after it, and one that does not test it over whatever came before.
    for (const mdx::Layer::ShadingFlag flag :
         {mdx::Layer::ShadingFlag::NoDepthSet, mdx::Layer::ShadingFlag::NoDepthTest}) {
        Rig rig;
        const u32 skin = rig.material(rig.texture("textures/skin.blp"));
        rig.model.materials[skin].layers[0].shadingFlags = flag;
        const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
        rig.geoset(skin, {root}, {0, 0, 0});
        rig.geoset(skin, {root}, {4, 0, 0});
        CHECK(optimize(rig).report.meshesMerged == 0);
    }
}

TEST_CASE("optimize keeps apart geosets whose vertex layouts differ", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(skin, {root}, {0, 0, 0});
    // A second UV set the first would be zero-filled for.
    rig.geoset(skin, {root}, {4, 0, 0}, 1, true);

    CHECK(optimize(rig).report.meshesMerged == 0);
}

TEST_CASE("optimize keeps an overlay drawn after what it lies on", "[wem][optimize]") {
    // Geoset 2 duplicates geoset 1's faces in geoset 0's material: merged into
    // 0 it would draw before 1, and 1 would win the tie at equal depth.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(skin, {root}, {0, 0, 0});
    rig.geoset(cloth, {root}, {4, 0, 0});
    rig.geoset(skin, {root}, {4, 0, 0});
    rig.geoset(skin, {root}, {8, 0, 0});

    const Optimized run = optimize(rig);
    // 3 joins 0; 2 stays behind 1.
    CHECK(run.report.models[0].meshOrigins == std::vector<std::vector<u32>>{{0, 3}, {1}, {2}});
}

TEST_CASE("optimize stops a merge at the geoset vertex limit", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    // 36,000 vertices each: two are past the 65,535 a geoset indexes.
    rig.geoset(skin, {root}, {0, 0, 0}, 9000);
    rig.geoset(skin, {root}, {0, 0, 50}, 9000);
    rig.geoset(skin, {root}, {0, 0, 100}, 10);

    const Optimized run = optimize(rig);
    CHECK(run.report.meshesMerged == 1);
    for (const Mesh& mesh : run.after.models[0].meshes) {
        CHECK(mesh.vertexCount() <= 0xFFFFu);
    }
}

TEST_CASE("optimize drops an empty geoset unless a bone is gated on it", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    const u32 gated = rig.bone("gated", root, {0, 0, 5}, true);
    rig.attachment("Weapon Ref", gated, {0, 0, 6});
    rig.geoset(skin, {root}, {0, 0, 0});
    const u32 empty = rig.geoset(cloth, {root}, {4, 0, 0});
    const u32 gate = rig.geoset(cloth, {root}, {8, 0, 0});
    rig.model.geosets[empty].faces.clear();
    rig.model.geosets[gate].faces.clear();
    rig.gate(gated, gate, rig.fade(gate));

    Document document = rig.import();
    // An importer may drop a geoset with no face itself; say which case ran.
    const std::size_t imported = document.models[0].meshes.size();
    const OptimizeReport report = OptimizeDocument(document);
    CHECK_FALSE(report.diagnostics.hasErrors());
    requireWritable(document);
    const Model& model = document.models[0];
    const u32 kept = nodeNamed(model, "gated");
    REQUIRE(kept != kInvalidNode);
    const auto* bone = std::get_if<BonePayload>(&model.nodes.nodes[kept].payload);
    REQUIRE(bone != nullptr);
    CHECK(bone->gateMesh < model.meshes.size());
    if (imported == 3) {
        CHECK(report.meshesRemoved == 1);
        CHECK(model.meshes.size() == 2);
    }
}

// ============================================================================
// Materials
// ============================================================================

TEST_CASE("optimize merges duplicate textures and materials, and drops unused ones",
          "[wem][optimize]") {
    Rig rig;
    const u32 body = rig.texture("textures/body.blp");
    const u32 twin = rig.texture("textures/body.blp");
    rig.texture("textures/unused.blp");
    const u32 first = rig.material(body);
    const u32 second = rig.material(twin); // the same once the textures are one
    rig.material(rig.texture("textures/orphan.blp")); // no geoset wears it
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    rig.geoset(first, {root}, {0, 0, 0});
    rig.geoset(second, {root}, {4, 0, 0});

    const Optimized run = optimize(rig);
    CHECK(run.report.texturesMerged == 1);
    CHECK(run.report.texturesRemoved == 2);
    CHECK(run.report.slotsMerged == 1);
    CHECK(run.report.slotsRemoved == 1);
    CHECK(run.report.meshesMerged == 1);
    CHECK(run.after.textures.size() == 1);
    CHECK(run.after.models[0].materialSlots.size() == 1);
}

// ============================================================================
// Nodes
// ============================================================================

namespace {

/// root ─ spine* ─┬─ inert          (weights a geoset)
///                ├─ Bone_Head      (the game looks it up)
///                ├─ tail*          (keyed, holds nothing)
///                └─ helper ─ hand* (weights a geoset)
/// `*` keyed.
Rig Skeleton() {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0});
    const u32 spine = rig.bone("spine", root, {0, 0, 10}, true);
    const u32 inert = rig.bone("inert", spine, {0, 0, 20});
    const u32 head = rig.bone("Bone_Head", spine, {0, 0, 30});
    rig.bone("tail", spine, {0, 5, 10}, true);
    // The hand's parent is a helper numbered after it, as bones-first
    // numbering makes of a real rig.
    const u32 hand = rig.bone("hand", 6, {5, 0, 15}, true);
    const u32 helper = rig.helper("helper", spine, {3, 0, 15});
    REQUIRE(helper == 6);
    rig.geoset(skin, {root}, {0, 0, 0});
    rig.geoset(cloth, {inert}, {0, 0, 20});
    rig.geoset(skin, {head}, {0, 0, 30});
    rig.geoset(cloth, {hand}, {5, 0, 15});
    return rig;
}

OptimizeOptions nodesOnly() {
    OptimizeOptions options;
    options.mergeMeshes = false;
    options.mergeMaterials = false;
    return options;
}

} // namespace

TEST_CASE("optimize replaces unkeyed bones and helpers by their parents", "[wem][optimize]") {
    const Optimized run = optimize(Skeleton(), nodesOnly());
    const Model& model = run.after.models[0];
    // inert and helper pass through; tail is a dead leaf.
    CHECK(run.report.nodesRemoved == 3);
    CHECK(nodeNamed(model, "inert") == kInvalidNode);
    CHECK(nodeNamed(model, "helper") == kInvalidNode);
    CHECK(nodeNamed(model, "tail") == kInvalidNode);
    // The root keeps its weights (a root's have nowhere to go) and Bone_Head
    // is the game's.
    CHECK(nodeNamed(model, "root") != kInvalidNode);
    CHECK(nodeNamed(model, "Bone_Head") != kInvalidNode);
    const u32 spine = nodeNamed(model, "spine");
    const u32 hand = nodeNamed(model, "hand");
    REQUIRE(spine != kInvalidNode);
    REQUIRE(hand != kInvalidNode);
    CHECK(model.nodes.nodes[hand].parent == spine);
    for (const geom::Influence& influence : model.meshes[1].skin.influences) {
        CHECK(influence.bone == spine);
    }
    CHECK(noChannelInvalidated(model));
    CHECK(run.report.models[0].nodeRemap.size() == run.before.models[0].nodes.size());
}

TEST_CASE("optimize reduces nodes only where the model is the whole animation",
          "[wem][optimize]") {
    Document document = Skeleton().import();
    // A World of Warcraft model binds `.anim` files by bone.
    document.defaultProfile = ProfileId::Wow;
    Document every = document;
    CHECK(OptimizeDocument(document, nodesOnly()).nodesRemoved == 0);
    OptimizeOptions anyGame = nodesOnly();
    anyGame.reduceNodesOfEveryGame = true;
    CHECK(OptimizeDocument(every, anyGame).nodesRemoved == 3);
}

TEST_CASE("optimize leaves the nodes of a model an actor binds", "[wem][optimize]") {
    // The skeleton that loses three nodes, as a model that names a FaceFX
    // actor whose bones nobody gave: the game moves some of them by name, and
    // plays nothing once one is missing.
    Rig rig = Skeleton();
    rig.model.faceEffects.push_back({"Node", "Units/Human/Footman/Footman.facefx"});

    const Optimized run = optimize(rig);
    const Model& model = run.after.models[0];
    CHECK(FaceFxActorPath(model) == "Units/Human/Footman/Footman.facefx");
    CHECK(run.report.nodesRemoved == 0);
    CHECK(run.report.models[0].nodesKeptForActor);
    CHECK(namedTree(model) == namedTree(run.before.models[0]));
    // The rest of the pass does not read the skeleton, and still runs.
    CHECK(run.report.meshesMerged == 2);
    CHECK(says(run.report, "names the FaceFX actor"));
}

TEST_CASE("optimize keeps what an actor binds, its frame and the weights under it",
          "[wem][optimize]") {
    // root ─ spine* ─┬─ face ─┬─ jaw ─ chin   (jaw bound; both weight a geoset)
    //                │        ├─ lip          (bound, and holds nothing)
    //                │        └─ cheek        (weights a geoset)
    //                └─ idle                  (weights a geoset)
    // `*` keyed; everything else is inert in the file.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0});
    const u32 spine = rig.bone("spine", root, {0, 0, 10}, true);
    const u32 face = rig.bone("face", spine, {0, 0, 20});
    const u32 jaw = rig.bone("jaw", face, {0, 2, 20});
    const u32 chin = rig.bone("chin", jaw, {0, 3, 19});
    rig.bone("lip", face, {0, 2, 21});
    const u32 cheek = rig.bone("cheek", face, {2, 1, 21});
    const u32 idle = rig.bone("idle", spine, {0, 0, 12});
    rig.geoset(skin, {root}, {0, 0, 0});
    rig.geoset(skin, {jaw}, {0, 2, 20});
    rig.geoset(skin, {chin}, {0, 3, 19});
    rig.geoset(skin, {cheek}, {2, 1, 21});
    rig.geoset(skin, {idle}, {0, 0, 12});
    rig.model.faceEffects.push_back({"Node", "face.facefx"});

    OptimizeOptions options = nodesOnly();
    options.boundBones = [](const Document&, u32) {
        return std::optional<std::vector<std::string>>(
            std::vector<std::string>{"jaw", "lip", "tongue"});
    };
    const Optimized run = optimize(rig, options);
    const Model& model = run.after.models[0];
    // Bound: the jaw, and the lip a dead leaf would otherwise be.
    CHECK(nodeNamed(model, "jaw") != kInvalidNode);
    CHECK(nodeNamed(model, "lip") != kInvalidNode);
    // The frame both are moved in.
    const u32 frame = nodeNamed(model, "face");
    REQUIRE(frame != kInvalidNode);
    CHECK(model.nodes.nodes[nodeNamed(model, "jaw")].parent == frame);
    CHECK(model.nodes.nodes[nodeNamed(model, "lip")].parent == frame);
    // The actor writes the jaw and not the chin, so the chin's vertices stay
    // the chin's.
    const u32 chinNow = nodeNamed(model, "chin");
    REQUIRE(chinNow != kInvalidNode);
    for (const geom::Influence& influence : model.meshes[2].skin.influences) {
        CHECK(influence.bone == chinNow);
    }
    // Nothing binds the cheek or its parent, so it folds as any inert bone.
    CHECK(nodeNamed(model, "cheek") == kInvalidNode);
    CHECK(nodeNamed(model, "idle") == kInvalidNode);
    CHECK(run.report.nodesRemoved == 2);
    CHECK(run.report.models[0].boundNodes == 2);
    CHECK_FALSE(run.report.models[0].nodesKeptForActor);
    // The actor's third bone is not in the model: the game plays no FaceFX on
    // it as it stands, which is worth a line.
    CHECK(says(run.report, "binds 1 bone(s)"));
}

TEST_CASE("optimize keeps the nodes its caller names", "[wem][optimize]") {
    OptimizeOptions options = nodesOnly();
    options.keepNodes = [](const Document& document, u32 model) {
        return std::vector<u32>{nodeNamed(document.models[model], "inert"),
                                nodeNamed(document.models[model], "tail"), kInvalidNode};
    };
    const Optimized run = optimize(Skeleton(), options);
    const Model& model = run.after.models[0];
    // A pass-through and a dead leaf, both kept; the helper still goes.
    CHECK(nodeNamed(model, "inert") != kInvalidNode);
    CHECK(nodeNamed(model, "tail") != kInvalidNode);
    CHECK(nodeNamed(model, "helper") == kInvalidNode);
    CHECK(run.report.nodesRemoved == 1);
}

TEST_CASE("an End keeps the joints the solve reads", "[wem][optimize]") {
    // The hand's first joint child is a metacarpal numbered after a finger that
    // hangs under a pass-through helper. Children are in index order, so the
    // helper, reduced, would hand the finger to the hand ahead of it.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("pelvis_bind_jnt", kNoParent, {0, 0, 60});
    const u32 upper = rig.bone("L_upr_arm_bind_jnt", root, {0, 10, 60}, true);
    const u32 lower = rig.bone("L_lwr_arm_bind_jnt", upper, {10, 24, 52}, true);
    const u32 hand = rig.bone("bone_hand_left", lower, {18, 34, 44}, true);
    const u32 finger = rig.bone("L_finger_01", 6, {22, 40, 42}, true);
    rig.bone("L_meta_01", hand, {20, 36, 44});
    REQUIRE(rig.helper("L_finger_root", hand, {19, 37, 43}) == 6);
    // An unweighted finger tip: the roll is read along the fingers, so it
    // stays with the rest of them.
    rig.bone("L_finger_02", finger, {24, 42, 41});
    rig.geoset(skin, {root}, {0, 0, 60});
    rig.geoset(skin, {finger}, {22, 40, 42});
    const Document before = rig.import();
    const std::vector<u32> kept = TPoseNodes(before, 0);
    for (const char* name : {"L_upr_arm_bind_jnt", "L_lwr_arm_bind_jnt", "bone_hand_left", "L_meta_01",
                             "L_finger_root", "L_finger_01", "L_finger_02"}) {
        INFO(name);
        CHECK(std::find(kept.begin(), kept.end(), nodeNamed(before.models[0], name)) != kept.end());
    }
    CHECK(std::find(kept.begin(), kept.end(), nodeNamed(before.models[0], "pelvis_bind_jnt")) == kept.end());

    const auto firstJoint = [](const Document& document) {
        const NodeTree& tree = document.models[0].nodes;
        for (const u32 child : tree.children(nodeNamed(document.models[0], "bone_hand_left"))) {
            const NodeKind kind = tree.nodes[child].kind;
            if (kind == NodeKind::Bone || kind == NodeKind::Helper) {
                return tree.nodes[child].name;
            }
        }
        return std::string();
    };
    Document plain = before;
    OptimizeDocument(plain, nodesOnly());
    CHECK(firstJoint(plain) == "L_finger_01");
    CHECK(nodeNamed(plain.models[0], "L_finger_02") == kInvalidNode);
    Document imported = before;
    OptimizeOptions options = nodesOnly();
    options.keepNodes = TPoseNodes;
    OptimizeDocument(imported, options);
    CHECK(firstJoint(imported) == "L_meta_01");
    CHECK(nodeNamed(imported.models[0], "L_finger_02") != kInvalidNode);
}

TEST_CASE("optimize keeps a bone whose gate its attachments inherit", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    const u32 gated = rig.bone("gated", root, {0, 0, 10});
    const u32 leaf = rig.bone("gated_leaf", root, {5, 0, 0});
    const u32 relay = rig.bone("relay", gated, {0, 0, 20});
    rig.attachment("Weapon Ref", gated, {0, 0, 12});
    rig.attachment("Hand Ref", relay, {0, 0, 22});
    rig.geoset(skin, {leaf}, {5, 0, 0});
    const u32 veil = rig.geoset(cloth, {root}, {0, 0, 10});
    const u32 record = rig.fade(veil);
    rig.gate(gated, veil, record);
    rig.gate(leaf, veil, record);
    rig.gate(relay, veil, record);

    const Optimized run = optimize(rig, nodesOnly());
    const Model& model = run.after.models[0];
    // Its attachment would follow root's gate (none) instead of the veil's.
    CHECK(nodeNamed(model, "gated") != kInvalidNode);
    // No child: the gate hides nothing but the bone.
    CHECK(nodeNamed(model, "gated_leaf") == kInvalidNode);
    // Its attachment's nearest bone becomes `gated`, which gates the same.
    CHECK(nodeNamed(model, "relay") == kInvalidNode);
    const u32 hand = nodeNamed(model, "Hand Ref");
    REQUIRE(hand != kInvalidNode);
    CHECK(model.nodes.nodes[hand].parent == nodeNamed(model, "gated"));
    CHECK(run.report.nodesRemoved == 2);
}

TEST_CASE("optimize makes a helper a bone when it takes a bone's weights", "[wem][optimize]") {
    // spine* ─ wrist* (helper) ─┬─ inert (weights a geoset)
    //                           └─ Hand Ref
    // spine is gated on the veil, so Hand Ref follows the veil through it.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 cloth = rig.material(rig.texture("textures/cloth.blp"));
    const u32 spine = rig.bone("spine", kNoParent, {0, 0, 0}, true);
    const u32 inert = rig.bone("inert", 2, {0, 0, 20});
    const u32 wrist = rig.helper("wrist", spine, {0, 0, 10}, true);
    REQUIRE(wrist == 2);
    rig.attachment("Hand Ref", wrist, {0, 0, 12});
    rig.geoset(skin, {inert}, {0, 0, 20});
    const u32 veil = rig.geoset(cloth, {spine}, {0, 0, 0});
    rig.gate(spine, veil, rig.fade(veil));

    const Optimized run = optimize(rig, nodesOnly());
    const Model& model = run.after.models[0];
    CHECK(run.report.nodesRemoved == 1);
    CHECK(run.report.helpersPromoted == 1);
    const u32 promoted = nodeNamed(model, "wrist");
    REQUIRE(promoted != kInvalidNode);
    REQUIRE(model.nodes.nodes[promoted].kind == NodeKind::Bone);
    for (const geom::Influence& influence : model.meshes[0].skin.influences) {
        CHECK(influence.bone == promoted);
    }
    // Hand Ref's nearest bone is the wrist now, and it gates what spine did.
    const u32 spineNow = nodeNamed(model, "spine");
    CHECK(std::get<BonePayload>(model.nodes.nodes[promoted].payload).gateMesh ==
          std::get<BonePayload>(model.nodes.nodes[spineNow].payload).gateMesh);
    // No influence names a node that is not a bone.
    CHECK(Validate(run.after, ValidateLevel::Profile).countOf(DiagCode::DanglingNodeReference) == 0);

    const MdxConverter converter;
    const Result<mdx::Model> written = converter.toMdx(run.after, ProfileId::Wc3Classic, 800);
    REQUIRE(written.ok());
    bool asBone = false;
    for (const mdx::Bone& bone : written->bones) {
        asBone = asBone || (bone.node.name == "wrist" &&
                            (static_cast<u32>(bone.node.flags) &
                             static_cast<u32>(mdx::Node::NodeFlag::Bone)) != 0);
    }
    CHECK(asBone);
    CHECK(written->helpers.empty());
}

TEST_CASE("optimize keeps a bone whose parent cannot carry its weights", "[wem][optimize]") {
    // spine* ─ Weapon Ref ─┬─ carried        (weights a geoset)
    //                      └─ relay ─ tip*   (tip weights a geoset)
    // An attachment is no bone and cannot be made one, so `carried` stays;
    // `relay` holds no weight and still goes.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 spine = rig.bone("spine", kNoParent, {0, 0, 0}, true);
    const u32 carried = rig.bone("carried", 4, {0, 0, 20});
    const u32 relay = rig.bone("relay", 4, {0, 5, 20});
    const u32 tip = rig.bone("tip", relay, {0, 5, 30}, true);
    REQUIRE(rig.attachment("Weapon Ref", spine, {0, 0, 10}) == 4);
    rig.geoset(skin, {spine}, {0, 0, 0});
    rig.geoset(skin, {carried}, {0, 0, 20});
    rig.geoset(skin, {tip}, {0, 5, 30});

    const Optimized run = optimize(rig, nodesOnly());
    const Model& model = run.after.models[0];
    CHECK(nodeNamed(model, "carried") != kInvalidNode);
    CHECK(nodeNamed(model, "relay") == kInvalidNode);
    CHECK(run.report.nodesRemoved == 1);
    CHECK(run.report.helpersPromoted == 0);
    CHECK(nonBoneWeight(model) == 0.0f);
    const u32 held = nodeNamed(model, "tip");
    REQUIRE(held != kInvalidNode);
    CHECK(model.nodes.nodes[held].parent == nodeNamed(model, "Weapon Ref"));
}

TEST_CASE("optimize keeps every node something names", "[wem][optimize]") {
    // One bone per place a node index is stored outside the tree. Each is
    // inert, weighs nothing and has no child -- a dead leaf by the file -- and
    // each has to stay, because the renumbering would clear what names it.
    const std::vector<std::string> named = {
        "gate",        "stage_driven", "stage_target",  "stage_source", "stage_up",
        "body",        "collider",     "cloth_bone",    "helper",       "field",
        "whole_gate",  "pieces_gate",  "fracture_bone", "skin_node",    "link",
        "event",       "centre"};
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 root = rig.bone("root", kNoParent, {0, 0, 0}, true);
    // Nothing names this one: it goes, and every index after it moves.
    rig.bone("spare", root, {0, 0, 1});
    for (std::size_t i = 0; i < named.size(); ++i) {
        rig.bone(named[i], root, {static_cast<f32>(i), 0, 2});
    }
    rig.geoset(skin, {root}, {0, 0, 0});
    Document document = rig.import();
    Model& model = document.models[0];
    const auto at = [&](const char* name) { return nodeNamed(model, name); };

    model.meshes[0].sections[0].native.set(kSectionVisibilityNode, static_cast<i64>(at("gate")));
    PoseStage stage;
    stage.id = 1;
    stage.driven = {at("stage_driven")};
    stage.targets = {at("stage_target")};
    StageSource source;
    source.node = at("stage_source");
    stage.sources.push_back(source);
    stage.upNode = at("stage_up");
    model.poseStages.push_back(stage);
    PhysicsBody body;
    body.id = model.physics.nextId++;
    body.node = at("body");
    model.physics.bodies.push_back(body);
    ClothCollider collider;
    collider.id = model.physics.nextId++;
    collider.node = at("collider");
    model.physics.colliders.push_back(collider);
    Cloth cloth;
    cloth.id = model.physics.nextId++;
    cloth.recipe.emplace();
    cloth.recipe->bones = {at("cloth_bone")};
    model.physics.cloths.push_back(cloth);
    PhysicsRig pieces;
    pieces.id = model.physics.nextId++;
    pieces.fracture.emplace();
    pieces.fracture->helper = at("helper");
    pieces.fracture->field = at("field");
    pieces.fracture->wholeGate = at("whole_gate");
    pieces.fracture->piecesGate = at("pieces_gate");
    pieces.fracture->bones = {at("fracture_bone")};
    pieces.fracture->skinNodes = {at("skin_node")};
    model.physics.rigs.push_back(pieces);
    model.nodes.nodes[at("root")].skin.mirror = at("link");
    REQUIRE_FALSE(document.clips.empty());
    ClipEvent event;
    event.node = at("event");
    document.clips[0].events.push_back(event);
    document.clips[0].physics.emplace();
    BakeWorldForce blast;
    blast.centreNode = at("centre");
    document.clips[0].physics->world.push_back(blast);

    // The walk the renumbering and the pass share reaches every row.
    std::set<NodeReference> rows;
    const Model& held = model;
    ForEachNodeReference(held.nodes, std::span<const Mesh>(held.meshes), &held.animChannels,
                         &held.poseStages, &held.physics, std::span<const Clip>(document.clips),
                         [&](NodeReference row, const u32&, u32, u32) { rows.insert(row); });
    for (int row = 0; row <= static_cast<int>(NodeReference::ClipWorldCentre); ++row) {
        if (static_cast<NodeReference>(row) == NodeReference::RigidNode) {
            continue; // No section of this model is rigid.
        }
        INFO("row " << row);
        CHECK(rows.count(static_cast<NodeReference>(row)) == 1);
    }

    const OptimizeReport report = OptimizeDocument(document, nodesOnly());
    CHECK(report.nodesRemoved == 1);
    CHECK(at("spare") == kInvalidNode);
    for (const std::string& name : named) {
        INFO(name);
        CHECK(nodeNamed(model, name) != kInvalidNode);
    }
    // And each reference followed its node to its new index.
    CHECK(model.meshes[0].sections[0].native.value(kSectionVisibilityNode, -1) == at("gate"));
    REQUIRE(model.poseStages.size() == 1);
    CHECK(model.poseStages[0].driven == std::vector<u32>{at("stage_driven")});
    CHECK(model.poseStages[0].targets == std::vector<u32>{at("stage_target")});
    REQUIRE(model.poseStages[0].sources.size() == 1);
    CHECK(model.poseStages[0].sources[0].node == at("stage_source"));
    CHECK(model.poseStages[0].upNode == at("stage_up"));
    REQUIRE(model.physics.bodies.size() == 1);
    CHECK(model.physics.bodies[0].node == at("body"));
    REQUIRE(model.physics.colliders.size() == 1);
    CHECK(model.physics.colliders[0].node == at("collider"));
    CHECK(model.physics.cloths[0].recipe->bones == std::vector<u32>{at("cloth_bone")});
    const FractureRecipe& fracture = *model.physics.rigs[0].fracture;
    CHECK(fracture.helper == at("helper"));
    CHECK(fracture.field == at("field"));
    CHECK(fracture.wholeGate == at("whole_gate"));
    CHECK(fracture.piecesGate == at("pieces_gate"));
    CHECK(fracture.bones == std::vector<u32>{at("fracture_bone")});
    CHECK(fracture.skinNodes == std::vector<u32>{at("skin_node")});
    CHECK(model.nodes.nodes[at("root")].skin.mirror == at("link"));
    CHECK(document.clips[0].events.back().node == at("event"));
    CHECK(document.clips[0].physics->world[0].centreNode == at("centre"));
}

TEST_CASE("optimize folds a bone only into a parent its profiles have", "[wem][optimize]") {
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    const u32 spine = rig.bone("spine", kNoParent, {0, 0, 0}, true);
    const u32 inert = rig.bone("inert", spine, {0, 0, 10});
    rig.geoset(skin, {spine}, {0, 0, 0});
    rig.geoset(skin, {inert}, {0, 0, 10});
    const Document imported = rig.import();
    REQUIRE(imported.profiles.size() == 1);
    const ProfileId held = imported.profiles[0];

    // The parent is not written in the one profile that holds the bone.
    Document apart = imported;
    apart.models[0].nodes.nodes[nodeNamed(apart.models[0], "spine")].profiles = kNoProfiles;
    CHECK(OptimizeDocument(apart, nodesOnly()).nodesRemoved == 0);

    // The bone is in fewer profiles than its parent, which is no obstacle.
    Document within = imported;
    within.models[0].nodes.nodes[nodeNamed(within.models[0], "inert")].profiles = ProfileBit(held);
    CHECK(OptimizeDocument(within, nodesOnly()).nodesRemoved == 1);
}

TEST_CASE("optimize keeps a model's last bone", "[wem][optimize]") {
    // The bone weights nothing and holds nothing: a dead leaf, but the only
    // bone a geoset could bind to.
    Rig rig;
    const u32 skin = rig.material(rig.texture("textures/skin.blp"));
    rig.bone("root", kNoParent, {0, 0, 0});
    rig.bone("spare", 0, {0, 0, 5});
    rig.geoset(skin, {}, {0, 0, 0});

    Document document = rig.import();
    const OptimizeReport report = OptimizeDocument(document, nodesOnly());
    CHECK(report.nodesRemoved == 1);
    CHECK(document.models[0].nodes.size() == 1);
}

TEST_CASE("optimize keeps a classic matrix group writable", "[wem][optimize]") {
    // {inert, spine, other} is one group; moving inert's third to spine would
    // make it {spine 2/3, other 1/3}, which no classic group spells.
    Rig shared;
    const u32 skin = shared.material(shared.texture("textures/skin.blp"));
    const u32 spine = shared.bone("spine", kNoParent, {0, 0, 0}, true);
    const u32 inert = shared.bone("inert", spine, {0, 0, 10});
    const u32 other = shared.bone("other", spine, {5, 0, 0}, true);
    shared.geoset(skin, {inert, spine, other}, {0, 0, 5});
    CHECK(optimize(shared, nodesOnly()).report.nodesRemoved == 0);

    // {inert, other} becomes {spine, other}: still one group of two.
    Rig apart;
    const u32 skin2 = apart.material(apart.texture("textures/skin.blp"));
    const u32 spine2 = apart.bone("spine", kNoParent, {0, 0, 0}, true);
    const u32 inert2 = apart.bone("inert", spine2, {0, 0, 10});
    const u32 other2 = apart.bone("other", spine2, {5, 0, 0}, true);
    apart.geoset(skin2, {inert2, other2}, {0, 0, 5});
    CHECK(optimize(apart, nodesOnly()).report.nodesRemoved == 1);
}

TEST_CASE("optimize knows the bone names Warcraft III resolves", "[wem][optimize]") {
    CHECK(IsWarcraftEngineBoneName("Bone_Head"));
    CHECK(IsWarcraftEngineBoneName("bone_chest"));
    CHECK(IsWarcraftEngineBoneName("BONE_TURRET"));
    CHECK(IsWarcraftEngineBoneName("Bone_Hand Left"));
    CHECK(IsWarcraftEngineBoneName("\"bone_foot\",right"));
    CHECK_FALSE(IsWarcraftEngineBoneName("Bone_Head_Ref"));
    CHECK_FALSE(IsWarcraftEngineBoneName("head"));
    CHECK_FALSE(IsWarcraftEngineBoneName(""));
}

// ============================================================================
// The corpus
// ============================================================================

/// What the sweep says of one file, step by step: `WEM_OPTIMIZE_PROBE` names it.
TEST_CASE("optimize probe: one file, each step alone", "[.][wem][optimize][probe]") {
    const char* path = std::getenv("WEM_OPTIMIZE_PROBE");
    if (path == nullptr) {
        SKIP("WEM_OPTIMIZE_PROBE names no file");
    }
    const auto bytes = test::readCorpusFile(path);
    const MdxConverter converter;
    Result<Document> imported =
        converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(imported.ok());
    const Document& before = *imported;
    // Where the two readings of the draw-order rule part, what each was given.
    const std::vector<u8> fading = hdMeshesThatFade(before);
    for (u32 mesh = 0; mesh < before.models[0].meshes.size(); ++mesh) {
        if (fading[mesh] == 0 || !DrawsOrderFree(before, 0, mesh)) {
            continue;
        }
        const Model& model = before.models[0];
        const u32 slot = model.meshes[mesh].sections[0].materialSlot;
        std::cout << "[probe] mesh " << mesh << " fades and is called order-free; slot " << slot << "\n";
        for (const ProfileId profile : before.profiles) {
            const Material* material = Resolve(model, slot, profile);
            const auto* block =
                material != nullptr ? std::get_if<native::MdxMaterial>(&material->Native()) : nullptr;
            if (block == nullptr) {
                continue;
            }
            std::cout << "    " << Profile(profile).name << ": v" << block->sourceVersion << " shader '"
                      << block->shader << "'\n";
            for (const native::MdxLayer& layer : block->layers) {
                std::cout << "      layer filter " << static_cast<u32>(layer.filterMode) << " flags "
                          << static_cast<u32>(layer.shadingFlags) << " alpha " << layer.alpha
                          << " shader " << static_cast<u32>(layer.shaderType) << " hd " << layer.isHd
                          << "\n";
            }
        }
        for (const AnimChannel& channel : model.animChannels.channels) {
            const TrackTarget& target = channel.target;
            const bool own = target.kind == TrackTarget::Kind::Section && target.mesh == mesh;
            const bool layer =
                target.kind == TrackTarget::Kind::MaterialLayer && target.material.slot == slot;
            if (!(own || layer) || target.channel != Channel::Alpha) {
                continue;
            }
            std::cout << "    channel " << channel.id << (own ? " section" : " layer") << " sub "
                      << target.sub << " profile " << Profile(target.material.profile).name << "\n";
            for (const Clip& clip : before.clips) {
                for (const SubTrackContainer& container : clip.containers) {
                    const SubTrack* track = container.find(channel.id);
                    if (track == nullptr) {
                        continue;
                    }
                    std::cout << "      clip '" << clip.name << "' interp "
                              << static_cast<u32>(track->interp) << " keys " << track->times.size() << ":";
                    for (std::size_t k = 0; k < track->times.size() && k < 6; ++k) {
                        f32 value = 0.0f;
                        std::memcpy(&value,
                                    track->values.data() + k * (track->values.size() / track->times.size()),
                                    sizeof(f32));
                        std::cout << " " << track->times[k] << "=" << value;
                    }
                    std::cout << "\n";
                }
            }
        }
    }
    struct Step {
        const char* name;
        bool keys, materials, nodes, meshes;
    };
    for (const Step& step : {Step{"keys", true, false, false, false},
                             Step{"materials", false, true, false, false},
                             Step{"nodes", false, false, true, false},
                             Step{"meshes", false, false, false, true},
                             Step{"all", true, true, true, true}}) {
        OptimizeOptions options;
        options.reduceKeys = step.keys;
        options.mergeMaterials = step.materials;
        options.reduceNodes = step.nodes;
        options.mergeMeshes = step.meshes;
        Document after = before;
        const OptimizeReport report = OptimizeDocument(after, options);
        const Equivalence same = compare(before, after, report, 6);
        std::cout << "[probe] " << step.name << ": meshes " << before.models[0].meshes.size() << " -> "
                  << after.models[0].meshes.size() << ", nodes " << before.models[0].nodes.size()
                  << " -> " << after.models[0].nodes.size() << ", slots "
                  << before.models[0].materialSlots.size() << " -> "
                  << after.models[0].materialSlots.size() << ", textures " << before.textures.size()
                  << " -> " << after.textures.size() << "; misplaced " << same.misplaced
                  << ", materials changed " << same.materialChanged << " " << same.materialNote
                  << "\n";
        for (const Diagnostic& entry : report.diagnostics.all()) {
            std::cout << "    " << entry.message << "\n";
        }
    }
}

TEST_CASE("optimize sweep: every corpus mdx draws and animates as before",
          "[wem][optimize][corpus]") {
    const auto files = test::gather("WEM_MDX_CORPUS_DIR", ".mdx", {"MDL", "Wc3Mdx"});
    if (files.empty()) {
        SKIP("MDX corpus not found");
    }
    const std::size_t limit = test::sweepLimit(files.size(), 80);
    const MdxConverter converter;

    u32 swept = 0;
    u32 changed = 0;
    u32 meshesBefore = 0;
    u32 meshesAfter = 0;
    u32 nodesBefore = 0;
    u32 nodesAfter = 0;
    u32 vertices = 0;
    u32 brokenSources = 0;
    u32 promoted = 0;
    u32 actorModels = 0;
    u32 channelsDropped = 0;
    u32 orderShows = 0; // Meshes whose place among the draws can show.
    u32 fadingHd = 0;   // Of them, HD meshes played less than fully drawn.
    u64 keysBefore = 0;
    u64 keysAfter = 0;
    u32 failures = 0;
    std::vector<std::string> failing;
    for (std::size_t i = 0; i < limit; ++i) {
        if (test::isKnownBad(files[i])) {
            continue;
        }
        test::trace(files[i]);
        const auto bytes = test::readCorpusFile(files[i]);
        Result<Document> imported =
            converter.importFromBytes(std::span<const u8>(bytes.data(), bytes.size()));
        if (!imported.ok() || imported->models.empty()) {
            continue;
        }
        const Document& before = *imported;
        Document after = before;
        const OptimizeReport report = OptimizeDocument(after);
        ++swept;
        changed += report.changed() ? 1u : 0u;
        meshesBefore += static_cast<u32>(before.models[0].meshes.size());
        meshesAfter += static_cast<u32>(after.models[0].meshes.size());
        nodesBefore += before.models[0].nodes.size();
        nodesAfter += after.models[0].nodes.size();
        keysBefore += CountKeys(before, 0);
        keysAfter += CountKeys(after, 0);

        const std::string label = test::pathText(files[i].filename());
        const auto fail = [&](const std::string& why) {
            ++failures;
            if (failing.size() < 20) {
                failing.push_back(label + ": " + why);
            }
        };
        const Equivalence same = compare(before, after, report, 6);
        vertices += same.vertices;
        brokenSources += same.bothNonFinite != 0 ? 1u : 0u;
        if (same.misplaced != 0 || same.restMoved != 0 || same.countMismatch != 0 ||
            same.materialChanged != 0) {
            fail(std::to_string(same.misplaced) + " misplaced (worst " +
                 std::to_string(same.worst) + "), " + std::to_string(same.restMoved) +
                 " rest, " + std::to_string(same.countMismatch) + " counts, " +
                 std::to_string(same.materialChanged) + " materials" +
                 (same.materialNote.empty() ? "" : " (" + same.materialNote + ")"));
        }
        if (report.diagnostics.hasErrors()) {
            fail("the pass reported an error");
        }
        promoted += report.helpersPromoted;
        channelsDropped += report.channelsDropped;
        // A geoset the game sorts is left apart (§3.3), by two readings of the
        // same rule: the pass's, from the keys, and this one's, played.
        const std::vector<u8> fading = hdMeshesThatFade(before);
        for (u32 mesh = 0; mesh < before.models[0].meshes.size(); ++mesh) {
            const bool shows = !DrawsOrderFree(before, 0, mesh);
            orderShows += shows ? 1u : 0u;
            fadingHd += fading[mesh];
            if (fading[mesh] != 0 && !shows) {
                fail("mesh " + std::to_string(mesh) + " fades in an HD material and is called order-free");
            }
        }
        if (!report.models.empty()) {
            for (const std::vector<u32>& origins : report.models[0].meshOrigins) {
                for (const u32 origin : origins) {
                    if (origins.size() > 1 && origin < fading.size() && fading[origin] != 0) {
                        fail("mesh " + std::to_string(origin) + " fades in an HD material and was merged");
                    }
                }
            }
        }
        if (const u32 gained = verticesGainingNonBoneWeight(before, after, report); gained != 0) {
            fail(std::to_string(gained) + " vertices gained weight on a node that is not a bone");
        }
        // §4.4: an actor binds bones by name, so without its bone list no
        // node of the model may go, nor change parents.
        if (namesFaceFxActor(before.models[0])) {
            ++actorModels;
            if (namedTree(before.models[0]) != namedTree(after.models[0])) {
                fail("names a FaceFX actor and lost " +
                     std::to_string(before.models[0].nodes.size() - after.models[0].nodes.size()) +
                     " node(s)");
            }
        }
        // Only a document that validated before has to validate after.
        if (!Validate(before, ValidateLevel::Profile).hasErrors() &&
            Validate(after, ValidateLevel::Profile).hasErrors()) {
            fail("no longer validates: " +
                 Validate(after, ValidateLevel::Profile).formatHistogram());
        }
        for (const ProfileId profile : before.profiles) {
            if (converter.toMdx(before, profile, MdxFileVersion(profile)).ok() &&
                !converter.toMdx(after, profile, MdxFileVersion(profile)).ok()) {
                fail(std::string("toMdx fails at ") + Profile(profile).name);
            }
        }
    }

    std::cout << "[optimize sweep] " << swept << " file(s), " << changed << " changed; meshes "
              << meshesBefore << " -> " << meshesAfter << ", nodes " << nodesBefore << " -> "
              << nodesAfter << ", keys " << keysBefore << " -> " << keysAfter << "; " << vertices
              << " posed vertices compared; " << brokenSources
              << " source(s) pose to NaN, alike before and after; " << promoted
              << " helper(s) made bones; " << actorModels << " model(s) name a FaceFX actor; "
              << channelsDropped << " channel declaration(s) dropped; " << orderShows
              << " mesh(es) whose draw order can show, " << fadingHd << " of them HD and fading\n";
    for (const std::string& line : failing) {
        std::cout << "  " << line << "\n";
    }
    CHECK(swept > 0);
    CHECK(changed > 0);
    CHECK(failures == 0);
}
