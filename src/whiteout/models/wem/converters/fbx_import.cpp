// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file fbx_import.cpp
 * @brief FBX → `Document` (FBX_OBJ_DESIGN §§3–8).
 *
 * Everything is evaluated in FBX's own column-vector double precision first —
 * the transform stack, the cluster binds, the curves — and crosses into WEM's
 * row-vector single-precision space at one point (`SpaceMap`), which applies
 * the file's axis declaration and units. Animation is resampled through the
 * whole stack and stored as each node's local transform against its parent's
 * world, which reproduces FBX's worlds exactly at every sample whatever the
 * inherit type, pivots or pre-rotations were.
 */

#include "whiteout/models/wem/fbx_converter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>

#include "whiteout/models/fbx/eval.h"
#include "whiteout/models/wem/anim/key_reduce.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/ops.h"
#include "whiteout/models/wem/materials/surface_flatten.h"

#include "interchange_mesh.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using fbx::Matrix4d;
using fbx::Vec3d;

constexpr ProfileId kFbxProfiles[] = {ProfileId::Generic};
constexpr f32 kPi = 3.14159265358979323846f;

// ============================================================================
// Space: the one crossing from FBX's space into WEM's
// ============================================================================

struct SpaceMap {
    AxisBasis basis;
    f64 unit = 1.0;

    Vector3f point(const Vec3d& p) const {
        return basis.fromFile(Vector3f{static_cast<f32>(p[0] * unit), static_cast<f32>(p[1] * unit),
                                       static_cast<f32>(p[2] * unit)});
    }
    Vector3f direction(const Vec3d& d) const {
        const f64 length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const f64 inv = length > 1e-30 ? 1.0 / length : 0.0;
        return basis.fromFile(Vector3f{static_cast<f32>(d[0] * inv), static_cast<f32>(d[1] * inv),
                                       static_cast<f32>(d[2] * inv)});
    }
    /// A column-vector FBX matrix as WEM's row-vector one, in WEM's space.
    Matrix44f matrix(const Matrix4d& m) const {
        Matrix44f row;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                row.data[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] =
                    static_cast<f32>(m.m[c][r]);
            }
        }
        for (std::size_t c = 0; c < 3; ++c) {
            row.data[3][c] = static_cast<f32>(m.m[c][3] * unit);
        }
        return basis.fromFile(row);
    }
};

/// Within single-precision noise of identity; @p reach is the length the
/// translation's noise scales with.
bool IsIdentity(const Matrix4d& m, f64 reach = 1.0) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const f64 tolerance = c == 3 && r < 3 ? 1e-5 * std::max(1.0, reach) : 1e-5;
            if (std::fabs(m.m[r][c] - (r == c ? 1.0 : 0.0)) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

/// The inverse transpose of @p m's linear part, for normals.
Matrix4d NormalMatrix(const Matrix4d& m) {
    Matrix4d linear = m;
    linear.m[0][3] = linear.m[1][3] = linear.m[2][3] = 0.0;
    const Matrix4d inv = linear.inverse();
    Matrix4d out;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r][c] = inv.m[c][r];
        }
    }
    return out;
}

Vec3d Vec(const f64* p) {
    return {p[0], p[1], p[2]};
}

std::vector<f64> F64sOf(const fbx::Node& node, const char* child) {
    const fbx::Node* found = node.child(child);
    return found != nullptr && !found->properties.empty() ? found->properties[0].toF64s()
                                                          : std::vector<f64>{};
}

std::vector<i32> I32sOf(const fbx::Node& node, const char* child) {
    const fbx::Node* found = node.child(child);
    return found != nullptr && !found->properties.empty() ? found->properties[0].toI32s()
                                                          : std::vector<i32>{};
}

std::string StringOf(const fbx::Node& node, const char* child) {
    const fbx::Node* found = node.child(child);
    return found != nullptr && !found->properties.empty()
               ? std::string(found->properties[0].toString())
               : std::string();
}

// ============================================================================
// Geometry layers
// ============================================================================

enum class Mapping : u8 { None, ByCorner, ByVertex, ByPolygon, ByEdge, AllSame };

struct Layer {
    Mapping mapping = Mapping::None;
    std::vector<f64> values;
    std::vector<i32> index;
    u32 components = 0;
    std::string name;

    bool present() const {
        return mapping != Mapping::None && !values.empty();
    }
    /// The value slot for one corner; nothing when the data runs short.
    std::optional<std::size_t> slot(std::size_t corner, std::size_t polygon, std::size_t vertex,
                                    std::size_t edge = 0) const {
        std::size_t element = 0;
        switch (mapping) {
        case Mapping::ByCorner:
            element = corner;
            break;
        case Mapping::ByVertex:
            element = vertex;
            break;
        case Mapping::ByPolygon:
            element = polygon;
            break;
        case Mapping::ByEdge:
            element = edge;
            break;
        case Mapping::AllSame:
            element = 0;
            break;
        case Mapping::None:
            return std::nullopt;
        }
        if (!index.empty()) {
            if (element >= index.size() || index[element] < 0) {
                return std::nullopt;
            }
            element = static_cast<std::size_t>(index[element]);
        }
        if ((element + 1) * components > values.size()) {
            return std::nullopt;
        }
        return element;
    }
    const f64* at(std::size_t element) const {
        return values.data() + element * components;
    }
};

Mapping MappingOf(std::string_view text) {
    if (text == "ByPolygonVertex") {
        return Mapping::ByCorner;
    }
    if (text == "ByVertice" || text == "ByVertex" || text == "ByControlPoint") {
        return Mapping::ByVertex;
    }
    if (text == "ByPolygon") {
        return Mapping::ByPolygon;
    }
    if (text == "ByEdge") {
        return Mapping::ByEdge;
    }
    if (text == "AllSame") {
        return Mapping::AllSame;
    }
    return Mapping::None;
}

Layer ReadLayer(const fbx::Node& element, const char* values, const char* index, u32 components) {
    Layer layer;
    layer.mapping = MappingOf(StringOf(element, "MappingInformationType"));
    layer.components = components;
    layer.values = F64sOf(element, values);
    const std::string reference = StringOf(element, "ReferenceInformationType");
    if (reference == "IndexToDirect" || reference == "Index") {
        layer.index = I32sOf(element, index);
    }
    layer.name = StringOf(element, "Name");
    return layer;
}

/// The geometry's layer elements of one kind, in their layer-index order.
std::vector<const fbx::Node*> Elements(const fbx::Node& geometry, std::string_view kind) {
    std::vector<std::pair<i64, const fbx::Node*>> found;
    for (const fbx::Node& child : geometry.children) {
        if (child.name == kind) {
            found.emplace_back(child.properties.empty() ? 0 : child.properties[0].toI64(), &child);
        }
    }
    std::stable_sort(found.begin(), found.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const fbx::Node*> out;
    for (const auto& entry : found) {
        out.push_back(entry.second);
    }
    return out;
}

// ============================================================================
// The importer
// ============================================================================

struct ModelInfo {
    const fbx::Object* object = nullptr;
    i64 parent = 0; ///< The FBX parent model's id; 0 for the scene root.
    fbx::TransformStack stack;
    Matrix4d restWorld;
    u32 wem = kInvalidNode;
    bool synthetic = false;
};

class Importer {
public:
    Importer(const fbx::Scene& scene, const FbxReadOptions& options, Diagnostics& diagnostics)
        : scene_(scene), options_(options), diagnostics_(diagnostics) {}

    FbxImport run() {
        setUpSpace();
        document_.declare(ProfileId::Generic);
        document_.defaultProfile = ProfileId::Generic;
        document_.unitScale = 1.0f;
        model_.name = "fbx";
        model_.nodes.rig = RigConvention::ExplicitBind;
        set_.profile = ProfileId::Generic;
        set_.looks = LookTable::Single();

        collectModels();
        buildNodes();
        importMeshes();
        importAnimation();

        set_.resizeBindings(model_.materialSlots.size());
        for (std::size_t slot = 0; slot < model_.materialSlots.size(); ++slot) {
            set_.slotBindings[slot].byLook[0] = static_cast<u32>(slot);
        }
        model_.profileSets.push_back(std::move(set_));
        ResetExtent(model_.bounds);
        for (const Mesh& mesh : model_.meshes) {
            GrowExtent(model_.bounds, mesh.bounds.minimum);
            GrowExtent(model_.bounds, mesh.bounds.maximum);
        }
        if (model_.meshes.empty()) {
            model_.bounds = Extent{};
        } else {
            FinishExtent(model_.bounds);
        }
        document_.bounds = model_.bounds;
        document_.models.push_back(std::move(model_));
        if (!document_.clips.empty()) {
            ReduceKeysExactly(document_, 0);
        }
        FbxImport out;
        out.document = std::move(document_);
        out.media = std::move(media_);
        out.sources = std::move(sources_);
        out.unitScaleFactor = scene_.globals().unitScaleFactor;
        return out;
    }

private:
    // ---- space ---------------------------------------------------------------

    void setUpSpace() {
        const fbx::GlobalSettings& g = scene_.globals();
        if (options_.axes.has_value()) {
            space_.basis = AxisBasis::FromPreset(*options_.axes);
        } else if (const std::optional<AxisBasis> declared = AxisBasis::FromDeclaration(
                       static_cast<u32>(g.upAxis), g.upAxisSign, static_cast<u32>(g.frontAxis),
                       g.frontAxisSign, static_cast<u32>(g.coordAxis), g.coordAxisSign)) {
            space_.basis = *declared;
        } else {
            diagnostics_.warn(DiagCode::Unspecified,
                              "the file's axis declaration names one axis twice; read as Y up");
            space_.basis = AxisBasis::FromPreset(AxisPreset::YUp);
        }
        if (space_.basis.determinant() < 0.0f) {
            diagnostics_.warn(DiagCode::Unspecified,
                              "the file declares a left-handed space; it is mirrored into WEM's "
                              "and its faces turned to keep their fronts");
        }
        space_.unit = options_.normaliseUnits && g.unitScaleFactor > 0.0 ? g.unitScaleFactor : 1.0;
    }

    // ---- nodes -----------------------------------------------------------------

    void collectModels() {
        for (const fbx::Object& object : scene_.objects()) {
            if (object.kind != "Model") {
                continue;
            }
            ModelInfo info;
            info.object = &object;
            for (const fbx::Object* parent : scene_.parents(object.id, "Model")) {
                info.parent = parent->id;
                break;
            }
            info.stack = fbx::TransformStack::Of(object.properties);
            info.synthetic = object.properties.integer("wem:synthetic", 0) != 0;
            modelIndex_.emplace(object.id, models_.size());
            models_.push_back(info);
        }
        // Parents first: depth-first from the roots, children in file order.
        std::vector<std::vector<std::size_t>> children(models_.size());
        std::vector<std::size_t> roots;
        for (std::size_t i = 0; i < models_.size(); ++i) {
            const auto parent = modelIndex_.find(models_[i].parent);
            if (models_[i].parent != 0 && parent != modelIndex_.end() && parent->second != i) {
                children[parent->second].push_back(i);
            } else {
                models_[i].parent = 0;
                roots.push_back(i);
            }
        }
        std::vector<u8> seen(models_.size(), 0);
        std::vector<std::size_t> stack(roots.rbegin(), roots.rend());
        while (!stack.empty()) {
            const std::size_t i = stack.back();
            stack.pop_back();
            if (seen[i] != 0) {
                continue;
            }
            seen[i] = 1;
            order_.push_back(i);
            for (auto it = children[i].rbegin(); it != children[i].rend(); ++it) {
                stack.push_back(*it);
            }
        }
        for (std::size_t i = 0; i < models_.size(); ++i) {
            if (seen[i] == 0) {
                models_[i].parent = 0;
                order_.push_back(i);
                diagnostics_.warn(DiagCode::DanglingNodeReference,
                                  "model '" + models_[i].object->name +
                                      "' sits in a parent cycle; imported as a root");
            }
        }
        for (const std::size_t i : order_) {
            ModelInfo& info = models_[i];
            const Matrix4d local = info.stack.local();
            if (info.parent == 0) {
                info.restWorld = local;
            } else {
                const ModelInfo& parent = models_[modelIndex_.at(info.parent)];
                info.restWorld = fbx::ComposeWorld(parent.restWorld, parent.stack, info.stack, local);
            }
        }
    }

    /// The WEM node a model's children hang from: a synthetic model's own
    /// parent's, all the way up.
    u32 wemParentOf(const ModelInfo& info) const {
        i64 parent = info.parent;
        while (parent != 0) {
            const ModelInfo& p = models_[modelIndex_.at(parent)];
            if (!p.synthetic) {
                return p.wem;
            }
            parent = p.parent;
        }
        return kInvalidNode;
    }

    Matrix4d parentWorldOf(const ModelInfo& info, const std::vector<Matrix4d>& worlds) const {
        i64 parent = info.parent;
        while (parent != 0) {
            const std::size_t index = modelIndex_.at(parent);
            if (!models_[index].synthetic) {
                return worlds[index];
            }
            parent = models_[index].parent;
        }
        return Matrix4d{};
    }

    void buildNodes() {
        // Which models a skin cluster links: those, and their ancestors, are bones.
        std::vector<u8> linked(models_.size(), 0);
        for (const fbx::Object& object : scene_.objects()) {
            if (object.kind != "Deformer" || object.subclass != "Cluster") {
                continue;
            }
            for (const fbx::Object* bone : scene_.children(object.id, "Model")) {
                auto found = modelIndex_.find(bone->id);
                while (found != modelIndex_.end()) {
                    linked[found->second] = 1;
                    const i64 parent = models_[found->second].parent;
                    found = parent != 0 ? modelIndex_.find(parent) : modelIndex_.end();
                }
            }
        }
        std::vector<Matrix4d> restWorlds(models_.size());
        for (std::size_t i = 0; i < models_.size(); ++i) {
            restWorlds[i] = models_[i].restWorld;
        }
        u32 sheared = 0;
        for (const std::size_t i : order_) {
            ModelInfo& info = models_[i];
            if (info.synthetic) {
                continue;
            }
            Node node;
            node.name = info.object->name;
            node.parent = wemParentOf(info);
            const std::string& sub = info.object->subclass;
            if (sub == "LimbNode" || sub == "Root" || linked[i] != 0) {
                node.kind = NodeKind::Bone;
            } else if (sub == "Camera") {
                node.kind = NodeKind::Camera;
            } else if (sub == "Light") {
                node.kind = NodeKind::Light;
            }
            // Only a kind whose meaning is the placement comes back: an emitter,
            // light or shape restored without the payload FBX never carried
            // would draw a default one.
            const std::string_view marked = info.object->properties.string("wem:kind");
            if (!marked.empty() && node.kind == NodeKind::Helper) {
                const std::optional<NodeKind> kind = NodeKindFromName(marked);
                if (kind == NodeKind::Attachment || kind == NodeKind::Event) {
                    node.kind = *kind;
                }
            }
            node.resetPayloadForKind();
            readAttribute(info, node);

            const Matrix44f world = space_.matrix(restWorlds[i]);
            const Matrix44f parentWorld = space_.matrix(parentWorldOf(info, restWorlds));
            const Matrix44f local = world * Matrix44f::inverse(parentWorld);
            node.local = FromMatrix(local);
            sheared += Residual(ToMatrix(node.local), local) > 1e-3f ? 1u : 0u;
            info.wem = model_.nodes.add(std::move(node));
        }
        if (sheared != 0) {
            diagnostics_.warn(DiagCode::BoneShearProjected,
                              std::to_string(sheared) +
                                  " node(s) carry shear no TRS holds; their rest is the nearest");
        }
    }

    static std::optional<NodeKind> NodeKindFromName(std::string_view name) {
        for (u32 k = 0; k < static_cast<u32>(NodeKind::Count); ++k) {
            if (name == ToString(static_cast<NodeKind>(k))) {
                return static_cast<NodeKind>(k);
            }
        }
        return std::nullopt;
    }

    static f32 Residual(const Matrix44f& a, const Matrix44f& b) {
        f32 worst = 0.0f;
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                worst = std::max(worst, std::fabs(a.data[r][c] - b.data[r][c]));
            }
        }
        return worst;
    }

    void readAttribute(const ModelInfo& info, Node& node) const {
        for (const fbx::Object* attribute : scene_.children(info.object->id, "NodeAttribute")) {
            const fbx::PropertyTable& p = attribute->properties;
            if (node.kind == NodeKind::Camera) {
                CameraPayload camera;
                camera.fov = static_cast<f32>(p.number("FieldOfView", 40.0) * kPi / 180.0);
                camera.nearClip = static_cast<f32>(p.number("NearPlane", 1.0) * space_.unit);
                camera.farClip = static_cast<f32>(p.number("FarPlane", 10000.0) * space_.unit);
                node.payload = camera;
            } else if (node.kind == NodeKind::Light) {
                LightPayload light;
                const i64 type = p.integer("LightType", 0);
                light.kind = type == 1 ? LightKind::Directional
                             : type == 2 ? LightKind::Spot
                                         : LightKind::Omni;
                const Vec3d color = p.vec3("Color", {1, 1, 1});
                light.color = Vector3f{static_cast<f32>(color[0]), static_cast<f32>(color[1]),
                                       static_cast<f32>(color[2])};
                light.intensity = static_cast<f32>(p.number("Intensity", 100.0) / 100.0);
                light.attenuationStart = static_cast<f32>(p.number("DecayStart", 0.0) * space_.unit);
                light.attenuationEnd =
                    static_cast<f32>(p.number("FarAttenuationEnd", 0.0) * space_.unit);
                light.hotSpot = static_cast<f32>(p.number("InnerAngle", 0.0) * kPi / 180.0);
                light.falloff = static_cast<f32>(p.number("OuterAngle", 45.0) * kPi / 180.0);
                node.payload = light;
            }
            break;
        }
    }

    // ---- materials ---------------------------------------------------------------

    u32 textureFor(const fbx::Object& texture, ColorSpace space) {
        const auto cached = textureOf_.find(texture.id);
        if (cached != textureOf_.end()) {
            return cached->second;
        }
        const auto slashed = [](std::string text) {
            std::replace(text.begin(), text.end(), '\\', '/');
            return text;
        };
        FbxTextureSource source{slashed(StringOf(*texture.node, "RelativeFilename")),
                                slashed(StringOf(*texture.node, "FileName"))};
        std::string path = !source.relative.empty() ? source.relative : source.absolute;
        if (path.empty()) {
            path = texture.name;
        }
        sources_.push_back(std::move(source));
        TextureRef ref;
        ref.key = TexturePath{path};
        ref.path = path;
        ref.declaredSpace = space;
        document_.textures.push_back(std::move(ref));
        const u32 index = static_cast<u32>(document_.textures.size() - 1);
        textureOf_.emplace(texture.id, index);
        for (const fbx::Object* video : scene_.children(texture.id, "Video")) {
            const fbx::Node* content = video->node->child("Content");
            if (content == nullptr || content->properties.empty()) {
                continue;
            }
            // Binary carries the bytes raw; ASCII as base64 strings.
            std::string bytes;
            for (const fbx::Property& part : content->properties) {
                if (part.type == fbx::PropertyType::Raw) {
                    bytes += part.text;
                } else if (part.type == fbx::PropertyType::String) {
                    bytes += DecodeBase64(part.text);
                }
            }
            if (!bytes.empty()) {
                media_.push_back(FbxMedia{index, std::move(bytes)});
            }
            break;
        }
        return index;
    }

    static std::string DecodeBase64(std::string_view text) {
        std::string out;
        u32 buffer = 0;
        int bits = 0;
        for (const char c : text) {
            int value = -1;
            if (c >= 'A' && c <= 'Z') {
                value = c - 'A';
            } else if (c >= 'a' && c <= 'z') {
                value = c - 'a' + 26;
            } else if (c >= '0' && c <= '9') {
                value = c - '0' + 52;
            } else if (c == '+') {
                value = 62;
            } else if (c == '/') {
                value = 63;
            }
            if (value < 0) {
                continue;
            }
            buffer = (buffer << 6) | static_cast<u32>(value);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return out;
    }

    TextureInput inputFor(const fbx::Object& texture, ColorSpace space,
                          const std::vector<std::string>& uvSets) {
        TextureInput input;
        input.texture = textureFor(texture, space);
        input.colorSpace = space;
        const fbx::PropertyTable& p = texture.properties;
        input.wrapU = p.integer("WrapModeU", 0) == 1 ? WrapMode::Clamp : WrapMode::Repeat;
        input.wrapV = p.integer("WrapModeV", 0) == 1 ? WrapMode::Clamp : WrapMode::Repeat;
        const std::string_view set = p.string("UVSet");
        for (u32 i = 0; i < uvSets.size(); ++i) {
            if (!set.empty() && uvSets[i] == set) {
                input.uvSet = i;
            }
        }
        const Vec3d t = p.vec3("Translation");
        const Vec3d s = p.vec3("Scaling", {1, 1, 1});
        if (t[0] != 0.0 || t[1] != 0.0 || s[0] != 1.0 || s[1] != 1.0) {
            // `uv' = uv * s + t` in V-up space, restated for WEM's V-down.
            input.uvTransform.m[0][0] = static_cast<f32>(s[0]);
            input.uvTransform.m[0][2] = static_cast<f32>(t[0]);
            input.uvTransform.m[1][1] = static_cast<f32>(s[1]);
            input.uvTransform.m[1][2] = static_cast<f32>(1.0 - s[1] - t[1]);
        }
        return input;
    }

    /// The texture a material property is connected to: a layered texture
    /// keeps its first layer.
    const fbx::Object* sourceTexture(const fbx::Object& texture, const std::string& material) {
        if (texture.kind != "LayeredTexture") {
            return &texture;
        }
        const std::vector<const fbx::Object*> layers = scene_.children(texture.id, "Texture");
        if (layers.empty()) {
            return nullptr;
        }
        if (layers.size() > 1) {
            diagnostics_.warn(DiagCode::LayerDropped, material + ": a layered texture keeps its first layer");
        }
        return layers.front();
    }

    /// 3ds Max's PBR materials, as Max 2027 writes them: the Physical
    /// material's `3dsMax|Parameters|` compound (`ORIGINAL_MTL` PHYSICAL_MTL)
    /// and the glTF material's `3dsMax|main|`. Both carry a Phong restatement
    /// in the standard properties, which is skipped. Null when neither.
    std::optional<u32> pbrSlotFor(const fbx::Object& material, const std::vector<std::string>& uvSets) {
        const fbx::PropertyTable& p = material.properties;
        const bool physical = p.string("3dsMax|ORIGINAL_MTL") == "PHYSICAL_MTL" ||
                              p.has("3dsMax|Parameters|base_color");
        const bool gltf = !physical && p.has("3dsMax|main|baseColor");
        if (!physical && !gltf) {
            return std::nullopt;
        }
        const std::string prefix = physical ? "3dsMax|Parameters|" : "3dsMax|main|";
        const auto at = [&](const char* name) { return prefix + name; };
        Material out;
        out.name = material.name;
        CommonMaterial& common = out.InitCommon();
        common.setKind(MaterialKind::PBRDeferred);
        PbrDeferredBody& body = *common.pbr();
        const auto f32of = [](f64 v) { return static_cast<f32>(v); };
        bool glossiness = false;
        if (physical) {
            const Vec3d base = p.vec3(at("base_color"), {0.5, 0.5, 0.5});
            const f64 weight = p.number(at("base_weight"), 1.0);
            const f64 opacity = std::clamp(1.0 - p.number(at("transparency"), 0.0), 0.0, 1.0);
            body.baseColorFactor = Vector4f{f32of(base[0] * weight), f32of(base[1] * weight),
                                            f32of(base[2] * weight), f32of(opacity)};
            glossiness = p.integer(at("roughness_inv"), 0) != 0;
            const f64 roughness = p.number(at("roughness"), 0.0);
            body.roughnessFactor = f32of(glossiness ? 1.0 - roughness : roughness);
            body.metallicFactor = f32of(p.number(at("metalness"), 0.0));
            const Vec3d emit = p.vec3(at("emit_color"));
            const f64 emission = p.number(at("emission"), 0.0);
            body.emissiveFactor =
                Vector3f{f32of(emit[0] * emission), f32of(emit[1] * emission), f32of(emit[2] * emission)};
            if (opacity < 0.999) {
                common.blend = BlendMode::AlphaBlend;
            }
        } else {
            const Vec3d base = p.vec3(at("baseColor"), {1, 1, 1});
            body.baseColorFactor = Vector4f{f32of(base[0]), f32of(base[1]), f32of(base[2]), 1.0f};
            body.roughnessFactor = f32of(p.number(at("roughness"), 1.0));
            body.metallicFactor = f32of(p.number(at("metalness"), 1.0));
            const Vec3d emit = p.vec3(at("emissionColor"));
            body.emissiveFactor = Vector3f{f32of(emit[0]), f32of(emit[1]), f32of(emit[2])};
            // Max's own glTF exporter writes 2 as MASK and 3 as BLEND.
            const i64 alphaMode = p.integer(at("alphaMode"), 1);
            if (alphaMode == 2) {
                common.blend = BlendMode::AlphaKey;
                common.alphaTestThreshold = f32of(p.number(at("alphaCutoff"), 0.5));
            } else if (alphaMode == 3) {
                common.blend = BlendMode::AlphaBlend;
            }
            if (p.integer(at("DoubleSided"), 0) != 0) {
                common.cull = CullMode::None;
            }
            if (p.integer("3dsMax|extension|unlit", 0) != 0) {
                common.flags |= MaterialFlags::Unlit;
            }
        }

        struct Map {
            const char* name;
            PbrSlot slot;
            ColorSpace space;
        };
        static constexpr Map kPhysical[] = {
            {"base_color_map", PbrSlot::BaseColor, ColorSpace::Srgb},
            {"roughness_map", PbrSlot::Roughness, ColorSpace::Linear},
            {"metalness_map", PbrSlot::Metallic, ColorSpace::Linear},
            {"bump_map", PbrSlot::Normal, ColorSpace::Linear},
            {"emit_color_map", PbrSlot::Emissive, ColorSpace::Srgb},
        };
        static constexpr Map kGltf[] = {
            {"baseColorMap", PbrSlot::BaseColor, ColorSpace::Srgb},
            {"roughnessMap", PbrSlot::Roughness, ColorSpace::Linear},
            {"metalnessMap", PbrSlot::Metallic, ColorSpace::Linear},
            {"normalMap", PbrSlot::Normal, ColorSpace::Linear},
            {"ambientOcclusionMap", PbrSlot::AmbientOcclusion, ColorSpace::Linear},
            {"emissionMap", PbrSlot::Emissive, ColorSpace::Srgb},
        };
        const std::span<const Map> maps = physical ? std::span<const Map>(kPhysical) : std::span<const Map>(kGltf);
        const fbx::Object* cutout = nullptr;
        for (const auto& [texture, property] : scene_.propertyInputs(material.id)) {
            if ((texture->kind != "Texture" && texture->kind != "LayeredTexture") ||
                !property.starts_with(prefix)) {
                continue;
            }
            const std::string_view name = property.substr(prefix.size());
            // The Normal Bump's second connection; the first is `bump_map`.
            if (name == "normalCamera") {
                continue;
            }
            const fbx::Object* source = sourceTexture(*texture, material.name);
            if (source == nullptr) {
                continue;
            }
            if (name == "cutout_map" || name == "AlphaMap") {
                cutout = source;
                continue;
            }
            const auto found = std::find_if(maps.begin(), maps.end(),
                                            [&](const Map& map) { return name == map.name; });
            if (found == maps.end()) {
                diagnostics_.info(DiagCode::LayerDropped, material.name + ": texture on '" +
                                                              std::string(name) + "' has no WEM slot");
                continue;
            }
            if (found->slot == PbrSlot::Roughness && glossiness) {
                diagnostics_.warn(DiagCode::LayerDropped,
                                  material.name + ": a glossiness map (roughness_inv) has no WEM slot");
                continue;
            }
            body.set(found->slot, inputFor(*source, found->space, uvSets));
            if (found->slot == PbrSlot::Emissive && body.emissiveFactor == Vector3f{0, 0, 0}) {
                body.emissiveFactor = Vector3f{1, 1, 1};
            }
        }
        if (cutout != nullptr) {
            // The colour image's alpha is the cutout when the two name one file.
            const TextureInput* base = body.find(PbrSlot::BaseColor);
            if (base != nullptr && base->texture == textureFor(*cutout, ColorSpace::Linear)) {
                common.blend = common.blend == BlendMode::AlphaBlend ? common.blend : BlendMode::AlphaKey;
                if (common.alphaTestThreshold <= 0.0f) {
                    common.alphaTestThreshold = 0.5f;
                }
            } else {
                diagnostics_.warn(DiagCode::LayerDropped, material.name + ": a cutout image apart from "
                                                                          "the colour image has no WEM slot");
            }
        }
        set_.materials.push_back(std::move(out));
        const u32 slot = model_.addSlot(material.name);
        slotOf_.emplace(material.id, slot);
        return slot;
    }

    u32 slotFor(const fbx::Object& material, const std::vector<std::string>& uvSets) {
        const auto cached = slotOf_.find(material.id);
        if (cached != slotOf_.end()) {
            return cached->second;
        }
        if (const std::optional<u32> pbr = pbrSlotFor(material, uvSets)) {
            return *pbr;
        }
        Material out;
        out.name = material.name;
        CommonMaterial& common = out.InitCommon();
        common.setKind(MaterialKind::LegacyDeferred);
        LegacyDeferredBody& body = *common.legacy();
        const fbx::PropertyTable& p = material.properties;
        const Vec3d diffuse = p.vec3("DiffuseColor", {0.8, 0.8, 0.8});
        const f64 diffuseFactor = p.number("DiffuseFactor", 1.0);
        f64 opacity = 1.0;
        if (p.has("Opacity")) {
            opacity = p.number("Opacity", 1.0);
        } else if (p.has("TransparencyFactor")) {
            const Vec3d tc = p.vec3("TransparentColor", {1, 1, 1});
            opacity = 1.0 - p.number("TransparencyFactor", 0.0) * (tc[0] + tc[1] + tc[2]) / 3.0;
        }
        body.diffuseFactor = Vector4f{static_cast<f32>(diffuse[0] * diffuseFactor),
                                      static_cast<f32>(diffuse[1] * diffuseFactor),
                                      static_cast<f32>(diffuse[2] * diffuseFactor),
                                      static_cast<f32>(std::clamp(opacity, 0.0, 1.0))};
        if (material.subclass == "phong" || p.has("SpecularColor")) {
            const Vec3d specular = p.vec3("SpecularColor", {0.2, 0.2, 0.2});
            const f64 factor = p.number("SpecularFactor", 1.0);
            body.specularFactor = Vector4f{static_cast<f32>(specular[0] * factor),
                                           static_cast<f32>(specular[1] * factor),
                                           static_cast<f32>(specular[2] * factor), 1.0f};
            body.specularExponent =
                static_cast<f32>(p.number("ShininessExponent", p.number("Shininess", 20.0)));
        }
        const Vec3d emissive = p.vec3("EmissiveColor");
        const f64 emissiveFactor = p.number("EmissiveFactor", 1.0);
        body.emissiveFactor = Vector4f{static_cast<f32>(emissive[0] * emissiveFactor),
                                       static_cast<f32>(emissive[1] * emissiveFactor),
                                       static_cast<f32>(emissive[2] * emissiveFactor), 1.0f};
        if (opacity < 0.999) {
            common.blend = BlendMode::AlphaBlend;
        }

        bool sawNormal = false;
        for (const auto& [texture, property] : scene_.propertyInputs(material.id)) {
            if (texture->kind != "Texture" && texture->kind != "LayeredTexture") {
                continue;
            }
            const fbx::Object* source = sourceTexture(*texture, material.name);
            if (source == nullptr) {
                continue;
            }
            if (property == "DiffuseColor") {
                body.set(LegacySlot::Diffuse, inputFor(*source, ColorSpace::Srgb, uvSets));
            } else if (property == "NormalMap" || (property == "Bump" && !sawNormal)) {
                // 3ds Max writes a Normal Bump map to `Bump`; the host demotes a
                // height map by its pixels.
                body.set(LegacySlot::Normal, inputFor(*source, ColorSpace::Linear, uvSets));
                sawNormal = true;
            } else if (property == "SpecularColor" || property == "SpecularFactor") {
                body.set(LegacySlot::Specular, inputFor(*source, ColorSpace::Srgb, uvSets));
            } else if (property == "ShininessExponent" || property == "Shininess") {
                body.set(LegacySlot::Gloss, inputFor(*source, ColorSpace::Linear, uvSets));
            } else if (property == "EmissiveColor" || property == "EmissiveFactor") {
                body.set(LegacySlot::Emissive, inputFor(*source, ColorSpace::Srgb, uvSets));
                if (!p.has("EmissiveColor")) {
                    body.emissiveFactor = Vector4f{1, 1, 1, 1};
                }
            } else if (property == "TransparentColor" || property == "TransparencyFactor") {
                // The colour image's alpha is the cutout when the two name one file.
                const TextureInput* diffuseInput = body.find(LegacySlot::Diffuse);
                const u32 alphaTexture = textureFor(*source, ColorSpace::Linear);
                if (diffuseInput != nullptr && diffuseInput->texture == alphaTexture) {
                    common.blend = BlendMode::AlphaKey;
                    common.alphaTestThreshold = 0.5f;
                } else {
                    diagnostics_.warn(DiagCode::LayerDropped,
                                      material.name + ": a transparency image apart from the "
                                                      "colour image has no WEM slot");
                }
            } else {
                diagnostics_.info(DiagCode::LayerDropped,
                                  material.name + ": texture on '" + std::string(property) +
                                      "' has no WEM slot");
            }
        }
        set_.materials.push_back(std::move(out));
        const u32 slot = model_.addSlot(material.name);
        slotOf_.emplace(material.id, slot);
        return slot;
    }

    u32 defaultSlot() {
        if (defaultSlot_ == kInvalidIndex) {
            Material out;
            out.name = "default";
            out.InitCommon().setKind(MaterialKind::LegacyDeferred);
            set_.materials.push_back(std::move(out));
            defaultSlot_ = model_.addSlot("default");
        }
        return defaultSlot_;
    }

    // ---- meshes -------------------------------------------------------------------

    void importMeshes() {
        std::unordered_map<i64, u32> geometryUses;
        for (const std::size_t i : order_) {
            const ModelInfo& info = models_[i];
            for (const fbx::Object* geometry : scene_.children(info.object->id, "Geometry")) {
                if (geometry->subclass != "Mesh") {
                    if (geometry->subclass == "Shape") {
                        continue; // a blend shape target, handled as a drop below
                    }
                    diagnostics_.info(DiagCode::FeatureDropped,
                                      "geometry '" + geometry->name + "' is a " +
                                          geometry->subclass + ", not a polygon mesh; skipped");
                    continue;
                }
                if (geometryUses[geometry->id]++ != 0) {
                    diagnostics_.info(DiagCode::Unspecified,
                                      "geometry '" + geometry->name +
                                          "' is instanced under several models; duplicated");
                }
                importMesh(info, *geometry);
            }
        }
        u32 shapes = 0;
        for (const fbx::Object& object : scene_.objects()) {
            shapes += object.kind == "Deformer" && object.subclass == "BlendShape" ? 1u : 0u;
        }
        if (shapes != 0) {
            diagnostics_.warn(DiagCode::FeatureDropped,
                              std::to_string(shapes) +
                                  " blend shape deformer(s) dropped: WEM has no morph targets");
        }
    }

    void importMesh(const ModelInfo& info, const fbx::Object& geometry) {
        const fbx::Node& node = *geometry.node;
        const std::vector<f64> controlPoints = F64sOf(node, "Vertices");
        const std::vector<i32> polygonIndex = I32sOf(node, "PolygonVertexIndex");
        const std::size_t pointCount = controlPoints.size() / 3;
        if (pointCount == 0 || polygonIndex.empty()) {
            return;
        }

        // Layers.
        Layer normals;
        if (const std::vector<const fbx::Node*> e = Elements(node, "LayerElementNormal"); !e.empty()) {
            normals = ReadLayer(*e.front(), "Normals", "NormalsIndex", 3);
        }
        Layer tangents;
        Layer binormals;
        if (const auto e = Elements(node, "LayerElementTangent"); !e.empty()) {
            tangents = ReadLayer(*e.front(), "Tangents", "TangentsIndex", 3);
        }
        if (const auto e = Elements(node, "LayerElementBinormal"); !e.empty()) {
            binormals = ReadLayer(*e.front(), "Binormals", "BinormalsIndex", 3);
        }
        std::vector<Layer> uvs;
        std::vector<std::string> uvNames;
        for (const fbx::Node* element : Elements(node, "LayerElementUV")) {
            if (uvs.size() == 8) {
                diagnostics_.warn(DiagCode::UvSetLimit,
                                  "geometry '" + geometry.name + "': UV sets past eight dropped");
                break;
            }
            uvs.push_back(ReadLayer(*element, "UV", "UVIndex", 2));
            uvNames.push_back(uvs.back().name);
        }
        Layer colors;
        if (const auto e = Elements(node, "LayerElementColor"); !e.empty()) {
            colors = ReadLayer(*e.front(), "Colors", "ColorIndex", 4);
        }
        Layer materials;
        if (const auto e = Elements(node, "LayerElementMaterial"); !e.empty()) {
            materials.mapping = MappingOf(StringOf(*e.front(), "MappingInformationType"));
            materials.components = 1;
            const std::vector<i32> raw = I32sOf(*e.front(), "Materials");
            materials.values.assign(raw.begin(), raw.end());
        }
        Layer smoothing;
        if (const auto e = Elements(node, "LayerElementSmoothing"); !e.empty()) {
            smoothing.mapping = MappingOf(StringOf(*e.front(), "MappingInformationType"));
            smoothing.components = 1;
            const std::vector<i32> raw = I32sOf(*e.front(), "Smoothing");
            smoothing.values.assign(raw.begin(), raw.end());
        }

        // Where the geometry sits at bind: a skin's clusters say, as
        // TransformLink · Transform (the stored `Transform` is relative to the
        // link); otherwise its model's rest world. The geometric offset is the
        // geometry's own.
        const fbx::Object* skin = nullptr;
        for (const fbx::Object* deformer : scene_.children(geometry.id, "Deformer")) {
            if (deformer->subclass == "Skin") {
                skin = deformer;
                break;
            }
        }
        std::vector<const fbx::Object*> clusters;
        if (skin != nullptr) {
            for (const fbx::Object* cluster : scene_.children(skin->id, "Deformer")) {
                if (cluster->subclass == "Cluster") {
                    clusters.push_back(cluster);
                }
            }
        }
        Matrix4d place = info.restWorld;
        f64 reach = 1.0;
        if (!clusters.empty()) {
            const std::vector<f64> transform = F64sOf(*clusters.front()->node, "Transform");
            const std::vector<f64> link = F64sOf(*clusters.front()->node, "TransformLink");
            if (transform.size() == 16 && link.size() == 16) {
                place = Matrix4d::FromStored(link) * Matrix4d::FromStored(transform);
                reach = std::max({std::fabs(link[12]), std::fabs(link[13]), std::fabs(link[14])});
            }
        }
        place = place * info.stack.geometric();
        // An identity placement is skipped, not multiplied: `0 * -0 + 0` is
        // +0, and the basis would then hand a signed zero back flipped. A
        // link times its stored inverse is identity only to float noise.
        const bool placed = !IsIdentity(place, reach);
        const Matrix4d normalPlace = NormalMatrix(place);
        const bool flip = space_.basis.determinant() < 0.0f;

        // Materials of this model, in connection order.
        const std::vector<const fbx::Object*> modelMaterials =
            scene_.children(info.object->id, "Material");

        geom::MeshBuilder builder;
        std::vector<u32> vertexOf(pointCount, kInvalidIndex);
        std::vector<geom::VertexId> corners;
        std::unordered_map<u32, u32> sectionOfSlot;
        std::vector<u32> polygonStart;   // global corner index each input polygon starts at
        std::vector<u32> polygonCorners;
        std::vector<u32> polygonOrdinal; // its FBX polygon index, which ByPolygon layers read
        u32 nonFinite = 0;
        u32 badIndices = 0;

        std::size_t corner = 0;
        std::size_t polygon = 0;
        while (corner < polygonIndex.size()) {
            const std::size_t first = corner;
            std::size_t last = corner;
            while (last < polygonIndex.size() && polygonIndex[last] >= 0) {
                ++last;
            }
            if (last >= polygonIndex.size()) {
                ++badIndices; // the final polygon was never closed
                break;
            }
            const std::size_t count = last - first + 1;
            corner = last + 1;
            const std::size_t thisPolygon = polygon++;
            if (count < 3) {
                continue;
            }
            corners.clear();
            bool bad = false;
            for (std::size_t k = 0; k < count; ++k) {
                i32 p = polygonIndex[first + k];
                if (p < 0) {
                    p = ~p;
                }
                if (static_cast<std::size_t>(p) >= pointCount) {
                    bad = true;
                    break;
                }
                if (vertexOf[static_cast<std::size_t>(p)] == kInvalidIndex) {
                    const f64* raw = controlPoints.data() + static_cast<std::size_t>(p) * 3;
                    Vec3d position = placed ? place.point(Vec(raw)) : Vec(raw);
                    if (!std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                        !std::isfinite(position[2])) {
                        position = {0, 0, 0};
                        ++nonFinite;
                    }
                    vertexOf[static_cast<std::size_t>(p)] = builder.addVertex(space_.point(position)).value();
                }
                corners.push_back(geom::VertexId(vertexOf[static_cast<std::size_t>(p)]));
            }
            if (bad) {
                ++badIndices;
                continue;
            }
            // The section: this polygon's material.
            u32 slot = kInvalidIndex;
            if (materials.mapping != Mapping::None && !materials.values.empty()) {
                const std::size_t m = materials.mapping == Mapping::AllSame ? 0 : thisPolygon;
                if (m < materials.values.size()) {
                    const i32 local = static_cast<i32>(materials.values[m]);
                    if (local >= 0 && static_cast<std::size_t>(local) < modelMaterials.size()) {
                        slot = slotFor(*modelMaterials[static_cast<std::size_t>(local)], uvNames);
                    }
                }
            } else if (!modelMaterials.empty()) {
                slot = slotFor(*modelMaterials.front(), uvNames);
            }
            if (slot == kInvalidIndex) {
                slot = defaultSlot();
            }
            auto section = sectionOfSlot.find(slot);
            if (section == sectionOfSlot.end()) {
                MeshSection s;
                s.name = model_.materialSlots[slot];
                s.materialSlot = slot;
                if (clusters.empty()) {
                    s.rigidNode = info.synthetic ? std::optional<u32>() : std::optional<u32>(info.wem);
                }
                section = sectionOfSlot.emplace(slot, builder.addSection(std::move(s))).first;
            }
            if (flip) {
                std::reverse(corners.begin(), corners.end());
            }
            const geom::FaceId face = builder.addFace(corners, section->second);
            polygonStart.push_back(static_cast<u32>(first));
            polygonCorners.push_back(static_cast<u32>(count));
            polygonOrdinal.push_back(static_cast<u32>(thisPolygon));
            for (std::size_t k = 0; k < count; ++k) {
                // A mirror reversed the loop; corner k of the face is source k'.
                const std::size_t source = flip ? count - 1 - k : k;
                const std::size_t c = first + source;
                i32 p = polygonIndex[c];
                if (p < 0) {
                    p = ~p;
                }
                const u32 at = static_cast<u32>(k);
                if (normals.present()) {
                    if (auto s = normals.slot(c, thisPolygon, static_cast<std::size_t>(p))) {
                        const Vec3d n = Vec(normals.at(*s));
                        builder.setCornerAttr(face, at, geom::names::kNormal,
                                              space_.direction(placed ? normalPlace.direction(n) : n));
                    }
                }
                if (tangents.present()) {
                    if (auto s = tangents.slot(c, thisPolygon, static_cast<std::size_t>(p))) {
                        const Vec3d t = place.direction(Vec(tangents.at(*s)));
                        f32 handedness = 1.0f;
                        if (normals.present() && binormals.present()) {
                            const auto ns = normals.slot(c, thisPolygon, static_cast<std::size_t>(p));
                            const auto bs = binormals.slot(c, thisPolygon, static_cast<std::size_t>(p));
                            if (ns && bs) {
                                const f64* n = normals.at(*ns);
                                const f64* b = binormals.at(*bs);
                                const f64* tt = tangents.at(*s);
                                const Vec3d cross{n[1] * tt[2] - n[2] * tt[1], n[2] * tt[0] - n[0] * tt[2],
                                                  n[0] * tt[1] - n[1] * tt[0]};
                                handedness = cross[0] * b[0] + cross[1] * b[1] + cross[2] * b[2] < 0.0
                                                 ? -1.0f
                                                 : 1.0f;
                            }
                        }
                        const Vector3f axis = space_.direction(t);
                        builder.setCornerAttr(face, at, geom::names::kTangent,
                                              Vector4f{axis.x, axis.y, axis.z,
                                                       flip ? -handedness : handedness});
                    }
                }
                for (u32 set = 0; set < uvs.size(); ++set) {
                    if (auto s = uvs[set].slot(c, thisPolygon, static_cast<std::size_t>(p))) {
                        const f64* uv = uvs[set].at(*s);
                        builder.setCornerAttr(face, at, geom::names::uv(set),
                                              Vector2f{static_cast<f32>(uv[0]),
                                                       static_cast<f32>(1.0 - uv[1])});
                    }
                }
                if (colors.present()) {
                    if (auto s = colors.slot(c, thisPolygon, static_cast<std::size_t>(p))) {
                        const f64* rgba = colors.at(*s);
                        const auto encode = [](f64 value) {
                            const f64 clamped = value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
                            return static_cast<u8>(clamped * 255.0 + 0.5);
                        };
                        builder.setCornerAttr(face, at, geom::names::color(0),
                                              std::array<u8, 4>{encode(rgba[0]), encode(rgba[1]),
                                                                encode(rgba[2]), encode(rgba[3])});
                    }
                }
            }
        }

        // The skin: each cluster's control points onto its bone.
        u32 unlinked = 0;
        for (const fbx::Object* cluster : clusters) {
            const fbx::Object* bone = nullptr;
            for (const fbx::Object* model : scene_.children(cluster->id, "Model")) {
                bone = model;
                break;
            }
            const auto boneInfo = bone != nullptr ? modelIndex_.find(bone->id) : modelIndex_.end();
            if (boneInfo == modelIndex_.end() || models_[boneInfo->second].wem == kInvalidNode) {
                ++unlinked;
                continue;
            }
            const u32 wemBone = models_[boneInfo->second].wem;
            const std::vector<i32> indexes = I32sOf(*cluster->node, "Indexes");
            const std::vector<f64> weights = F64sOf(*cluster->node, "Weights");
            for (std::size_t i = 0; i < indexes.size() && i < weights.size(); ++i) {
                if (indexes[i] < 0 || static_cast<std::size_t>(indexes[i]) >= pointCount) {
                    continue;
                }
                const u32 vertex = vertexOf[static_cast<std::size_t>(indexes[i])];
                if (vertex != kInvalidIndex && weights[i] > 0.0) {
                    builder.addInfluence(geom::VertexId(vertex), wemBone, static_cast<f32>(weights[i]));
                }
            }
            noteBind(wemBone, *cluster);
        }
        if (unlinked != 0) {
            diagnostics_.warn(DiagCode::SkinBindingMalformed,
                              "geometry '" + geometry.name + "': " + std::to_string(unlinked) +
                                  " cluster(s) link no imported model; their weights are dropped");
        }

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        if (outcome.mesh.faceCount() == 0) {
            return;
        }
        Mesh& mesh = outcome.mesh;
        if (!normals.present()) {
            restateSmoothing(mesh, smoothing, node, polygonStart, polygonCorners, polygonOrdinal);
        }
        mesh.name = !info.object->name.empty() ? info.object->name : geometry.name;
        mesh.recomputeBounds();
        model_.meshes.push_back(std::move(mesh));
        if (nonFinite != 0 || badIndices != 0) {
            diagnostics_.warn(DiagCode::IndexOutOfRange,
                              "geometry '" + geometry.name + "': " + std::to_string(badIndices) +
                                  " malformed polygon(s) dropped, " + std::to_string(nonFinite) +
                                  " non-finite position(s) placed at the origin");
        }
    }

    /// Normals for a mesh that stated none, from its smoothing: a 3ds Max
    /// bitmask per polygon (an edge is hard where two faces share no group),
    /// or a hard/soft flag per edge.
    void restateSmoothing(Mesh& mesh, const Layer& smoothing, const fbx::Node& node,
                          const std::vector<u32>& polygonStart,
                          const std::vector<u32>& polygonCorners,
                          const std::vector<u32>& polygonOrdinal) {
        if (!smoothing.present()) {
            geom::RecomputeNormals(mesh);
            return;
        }
        const std::vector<u32> survivors =
            SurvivingInputFaces(static_cast<u32>(polygonStart.size()), mesh.repairLog);
        const PolygonWalk walk(mesh);
        if (!walk.ok()) {
            geom::RecomputeNormals(mesh);
            return;
        }
        std::span<u8> sharp =
            mesh.attributes.getOrCreate<u8>(geom::names::kSharp, geom::Domain::Edge, geom::AttrType::Bool);
        if (smoothing.mapping == Mapping::ByPolygon) {
            // The polygon index the layer reads counts every FBX polygon, kept or not.
            std::vector<u32> groupOfFace(walk.faceCount(), 0);
            for (u32 f = 0; f < walk.faceCount() && f < survivors.size(); ++f) {
                const u32 fbxPolygon = polygonOrdinal[survivors[f]];
                groupOfFace[f] = fbxPolygon < smoothing.values.size()
                                     ? static_cast<u32>(static_cast<i64>(smoothing.values[fbxPolygon]))
                                     : 0u;
            }
            const geom::Topology& topology = mesh.topology();
            for (u32 e = 0; e < topology.edgeCount() && e < sharp.size(); ++e) {
                const geom::HalfedgeId h0 = geom::Topology::halfedge(geom::EdgeId(e), 0);
                const geom::HalfedgeId h1 = geom::Topology::halfedge(geom::EdgeId(e), 1);
                if (topology.isBoundary(h0) || topology.isBoundary(h1)) {
                    continue;
                }
                const u32 fa = walk.faceOfSlot(topology.face(h0).value());
                const u32 fb = walk.faceOfSlot(topology.face(h1).value());
                if (fa == kInvalidIndex || fb == kInvalidIndex) {
                    continue;
                }
                sharp[e] = (groupOfFace[fa] & groupOfFace[fb]) == 0 ? 1 : 0;
            }
        } else if (smoothing.mapping == Mapping::ByEdge) {
            // `Edges` lists, per FBX edge, the polygon corner it starts at.
            const std::vector<i32> edges = I32sOf(node, "Edges");
            std::vector<u32> faceOfInput(polygonStart.size(), kInvalidIndex);
            for (u32 f = 0; f < survivors.size(); ++f) {
                faceOfInput[survivors[f]] = f;
            }
            for (std::size_t e = 0; e < edges.size() && e < smoothing.values.size(); ++e) {
                const u32 cornerIndex = static_cast<u32>(edges[e]);
                const auto at = std::upper_bound(polygonStart.begin(), polygonStart.end(), cornerIndex);
                if (at == polygonStart.begin()) {
                    continue;
                }
                const std::size_t input = static_cast<std::size_t>(at - polygonStart.begin()) - 1;
                const u32 k = cornerIndex - polygonStart[input];
                if (k >= polygonCorners[input] || faceOfInput[input] == kInvalidIndex) {
                    continue;
                }
                const std::span<const u32> hs = walk.halfedges(faceOfInput[input]);
                if (k < hs.size()) {
                    const u32 edge = hs[k] >> 1;
                    if (edge < sharp.size()) {
                        sharp[edge] = smoothing.values[e] == 0.0 ? 1 : 0;
                    }
                }
            }
        }
        geom::RecomputeNormals(mesh, kPi);
    }

    void noteBind(u32 bone, const fbx::Object& cluster) {
        if (bindSet_.size() < model_.nodes.size()) {
            bindSet_.resize(model_.nodes.size(), 0);
        }
        if (bindSet_[bone] != 0) {
            return; // the first cluster to bind a bone speaks for it
        }
        const std::vector<f64> link = F64sOf(*cluster.node, "TransformLink");
        if (link.size() != 16) {
            return;
        }
        ensureBindSchema();
        Node& node = model_.nodes.nodes[bone];
        const Matrix44f inverseBind = Matrix44f::inverse(space_.matrix(Matrix4d::FromStored(link)));
        if (node.poseMatrices.size() < node.poses.size()) {
            node.poseMatrices.resize(node.poses.size(), Matrix44f::identity());
        }
        node.poseMatrices[0] = inverseBind;
        node.poses[0] = FromMatrix(inverseBind);
        bindSet_[bone] = 1;
    }

    void ensureBindSchema() {
        if (!model_.nodes.poseSchema.empty()) {
            return;
        }
        PoseSchema bind;
        bind.name = "bind";
        bind.space = PoseSpace::Model;
        bind.inverse = true;
        bind.storage = PoseStorage::Matrix;
        model_.nodes.poseSchema.push_back(bind);
        model_.nodes.authoritativePose = 0;
        model_.nodes.conformPoses();
        // Every node's bind starts at its rest; clusters overwrite the bones.
        for (u32 i = 0; i < model_.nodes.size(); ++i) {
            Node& node = model_.nodes.nodes[i];
            const Matrix44f inverseRest = Matrix44f::inverse(ToMatrix(model_.nodes.worldBind(i)));
            node.poseMatrices.assign(node.poses.size(), inverseRest);
            node.poses[0] = FromMatrix(inverseRest);
        }
    }

    // ---- animation ----------------------------------------------------------------

    struct Driven {
        std::array<std::optional<fbx::Curve>, 3> translation;
        std::array<std::optional<fbx::Curve>, 3> rotation;
        std::array<std::optional<fbx::Curve>, 3> scaling;
        std::optional<fbx::Curve> visibility;
        bool any = false;
    };

    static void ReadChannels(const fbx::Scene& scene, const fbx::Object& curveNode,
                             std::array<std::optional<fbx::Curve>, 3>& out) {
        for (const auto& [curve, property] : scene.propertyInputs(curveNode.id)) {
            if (curve->kind != "AnimationCurve") {
                continue;
            }
            const int axis = property == "d|X" ? 0 : property == "d|Y" ? 1 : property == "d|Z" ? 2 : -1;
            if (axis >= 0) {
                out[static_cast<std::size_t>(axis)] = fbx::Curve::Of(*curve);
            }
        }
    }

    void importAnimation() {
        const f64 rate = options_.sampleRate > 0.0f ? options_.sampleRate
                                                    : scene_.globals().frameRate();
        for (const fbx::Object& stack : scene_.objects()) {
            if (stack.kind != "AnimationStack") {
                continue;
            }
            const std::vector<const fbx::Object*> layers = scene_.children(stack.id, "AnimationLayer");
            if (layers.empty()) {
                continue;
            }
            u32 layersWithCurves = 0;
            for (const fbx::Object* layer : layers) {
                layersWithCurves += scene_.children(layer->id, "AnimationCurveNode").empty() ? 0u : 1u;
            }
            if (layersWithCurves > 1) {
                diagnostics_.warn(DiagCode::AnimLayersFlattened,
                                  "stack '" + stack.name +
                                      "': only its base layer is read; the others are dropped");
            }
            std::vector<Driven> driven(models_.size());
            std::vector<i64> keyTimes;
            for (const fbx::Object* curveNode : scene_.children(layers.front()->id, "AnimationCurveNode")) {
                for (const auto& [target, property] : scene_.propertyParents(curveNode->id)) {
                    const auto found = modelIndex_.find(target->id);
                    if (target->kind != "Model" || found == modelIndex_.end()) {
                        continue;
                    }
                    Driven& d = driven[found->second];
                    if (property == "Lcl Translation") {
                        ReadChannels(scene_, *curveNode, d.translation);
                    } else if (property == "Lcl Rotation") {
                        ReadChannels(scene_, *curveNode, d.rotation);
                    } else if (property == "Lcl Scaling") {
                        ReadChannels(scene_, *curveNode, d.scaling);
                    } else if (property == "Visibility") {
                        std::array<std::optional<fbx::Curve>, 3> channels;
                        for (const auto& [curve, channel] : scene_.propertyInputs(curveNode->id)) {
                            if (curve->kind == "AnimationCurve") {
                                channels[0] = fbx::Curve::Of(*curve);
                                break;
                            }
                        }
                        d.visibility = channels[0];
                    } else {
                        continue;
                    }
                    d.any = true;
                    for (const auto* channels : {&d.translation, &d.rotation, &d.scaling}) {
                        for (const auto& curve : *channels) {
                            if (curve) {
                                keyTimes.insert(keyTimes.end(), curve->times.begin(), curve->times.end());
                            }
                        }
                    }
                }
            }
            if (std::none_of(driven.begin(), driven.end(), [](const Driven& d) { return d.any; })) {
                continue;
            }
            // The window: the stack's own, else what its keys cover.
            i64 start = stack.properties.integer("LocalStart", 0);
            i64 stop = stack.properties.integer("LocalStop", 0);
            if (stop <= start) {
                start = stack.properties.integer("ReferenceStart", 0);
                stop = stack.properties.integer("ReferenceStop", 0);
            }
            if (stop <= start && !keyTimes.empty()) {
                start = *std::min_element(keyTimes.begin(), keyTimes.end());
                stop = *std::max_element(keyTimes.begin(), keyTimes.end());
            }
            if (stop <= start) {
                continue;
            }
            std::vector<i64> times;
            const i64 step = std::max<i64>(1, static_cast<i64>(std::llround(
                                                  static_cast<f64>(fbx::kTicksPerSecond) / rate)));
            for (i64 t = start; t < stop; t += step) {
                times.push_back(t);
            }
            times.push_back(stop);
            for (const i64 t : keyTimes) {
                if (t > start && t < stop) {
                    times.push_back(t);
                }
            }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());
            sampleClip(stack, driven, times, start, stop);
        }
        // The take the file was saved on comes first: it is what a DCC plays on open.
        if (const fbx::Node* takes = scene_.file().find("Takes"); takes != nullptr) {
            const std::string current = StringOf(*takes, "Current");
            const auto found = std::find_if(document_.clips.begin(), document_.clips.end(),
                                            [&](const Clip& clip) { return clip.name == current; });
            if (!current.empty() && found != document_.clips.end()) {
                std::rotate(document_.clips.begin(), found, found + 1);
            }
        }
    }

    void sampleClip(const fbx::Object& stack, const std::vector<Driven>& driven,
                    const std::vector<i64>& times, i64 start, i64 stop) {
        Clip clip;
        clip.name = stack.name;
        clip.model = 0;
        clip.looping = true;
        clip.duration = static_cast<f32>(static_cast<f64>(stop - start) / fbx::kTicksPerSecond);

        struct Tracks {
            std::vector<f32> t;
            std::vector<f32> translation;
            std::vector<f32> rotation;
            std::vector<f32> scale;
            std::vector<f32> visibility;
            Quaternion last{0, 0, 0, 1};
            bool started = false;
        };
        std::vector<Tracks> tracks(models_.size());
        std::vector<Matrix4d> worlds(models_.size());
        std::vector<fbx::TransformStack> stacks(models_.size());
        for (const i64 time : times) {
            const f32 seconds = static_cast<f32>(static_cast<f64>(time - start) / fbx::kTicksPerSecond);
            for (const std::size_t i : order_) {
                const ModelInfo& info = models_[i];
                fbx::TransformStack s = info.stack;
                const Driven& d = driven[i];
                for (std::size_t a = 0; a < 3; ++a) {
                    if (d.translation[a]) {
                        s.translation[a] = d.translation[a]->evaluate(time);
                    }
                    if (d.rotation[a]) {
                        s.rotation[a] = d.rotation[a]->evaluate(time);
                    }
                    if (d.scaling[a]) {
                        s.scaling[a] = d.scaling[a]->evaluate(time);
                    }
                }
                const Matrix4d local = s.local();
                if (info.parent == 0) {
                    worlds[i] = local;
                } else {
                    const std::size_t p = modelIndex_.at(info.parent);
                    worlds[i] = fbx::ComposeWorld(worlds[p], stacks[p], s, local);
                }
                stacks[i] = s;
            }
            for (const std::size_t i : order_) {
                const ModelInfo& info = models_[i];
                const Driven& d = driven[i];
                if (!d.any || info.synthetic || info.wem == kInvalidNode) {
                    continue;
                }
                const Matrix44f world = space_.matrix(worlds[i]);
                const Matrix44f parentWorld = space_.matrix(parentWorldOf(info, worlds));
                const Transform local = FromMatrix(world * Matrix44f::inverse(parentWorld));
                Tracks& t = tracks[i];
                Quaternion q = local.rotation;
                if (t.started && q.x * t.last.x + q.y * t.last.y + q.z * t.last.z + q.w * t.last.w < 0.0f) {
                    q = Quaternion{-q.x, -q.y, -q.z, -q.w};
                }
                t.last = q;
                t.started = true;
                t.t.push_back(seconds);
                t.translation.insert(t.translation.end(),
                                     {local.translation.x, local.translation.y, local.translation.z});
                t.rotation.insert(t.rotation.end(), {q.x, q.y, q.z, q.w});
                t.scale.insert(t.scale.end(), {local.scale.x, local.scale.y, local.scale.z});
                if (d.visibility) {
                    t.visibility.push_back(d.visibility->evaluate(time) > 0.5 ? 1.0f : 0.0f);
                }
            }
        }

        SubTrackContainer container;
        for (const std::size_t i : order_) {
            Tracks& t = tracks[i];
            if (t.t.empty()) {
                continue;
            }
            const u32 node = models_[i].wem;
            const auto add = [&](Channel channel, geom::AttrType type, Interpolation interp,
                                 const std::vector<f32>& values) {
                SubTrack track;
                track.channel = channelFor(node, channel, type);
                track.interp = interp;
                track.times = t.t;
                track.values.resize(values.size() * sizeof(f32));
                std::memcpy(track.values.data(), values.data(), track.values.size());
                container.subTracks.push_back(std::move(track));
            };
            add(Channel::Translation, geom::AttrType::F32x3, Interpolation::Linear, t.translation);
            add(Channel::Rotation, geom::AttrType::Quat, Interpolation::Slerp, t.rotation);
            add(Channel::Scale, geom::AttrType::F32x3, Interpolation::Linear, t.scale);
            if (!t.visibility.empty()) {
                add(Channel::Visibility, geom::AttrType::F32, Interpolation::Step, t.visibility);
            }
        }
        clip.containers.push_back(std::move(container));
        document_.clips.push_back(std::move(clip));
    }

    u32 channelFor(u32 node, Channel property, geom::AttrType valueType) {
        for (const AnimChannel& channel : model_.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == node &&
                channel.target.channel == property) {
                return channel.id;
            }
        }
        AnimChannel channel;
        channel.id = model_.animChannels.nextFreeId();
        channel.target.kind = TrackTarget::Kind::Node;
        channel.target.node = node;
        channel.target.channel = property;
        channel.valueType = valueType;
        return model_.animChannels.add(channel);
    }

    const fbx::Scene& scene_;
    const FbxReadOptions& options_;
    Diagnostics& diagnostics_;
    SpaceMap space_;
    Document document_;
    Model model_;
    ProfileMaterialSet set_;
    std::vector<ModelInfo> models_;
    std::unordered_map<i64, std::size_t> modelIndex_;
    std::vector<std::size_t> order_;
    std::unordered_map<i64, u32> textureOf_;
    std::unordered_map<i64, u32> slotOf_;
    u32 defaultSlot_ = kInvalidIndex;
    std::vector<u8> bindSet_;
    std::vector<FbxMedia> media_;
    std::vector<FbxTextureSource> sources_;
};

} // namespace

// ============================================================================
// FbxConverter
// ============================================================================

std::string FbxConverter::formatId() const {
    return "fbx";
}

std::string FbxConverter::formatName() const {
    return "Autodesk FBX";
}

std::span<const ProfileId> FbxConverter::profiles() const {
    return kFbxProfiles;
}

bool FbxConverter::supportsImport() const {
    return true;
}

bool FbxConverter::supportsExport() const {
    return true;
}

u32 FbxConverter::defaultExportVersion() const {
    return 7400;
}

Result<Document> FbxConverter::importFromBytes(std::span<const u8> data) const {
    Result<Document> result;
    fbx::ReadOutcome read = fbx::Read(data);
    if (!read.ok()) {
        result.diagnostics.error(DiagCode::UnsupportedVersion, read.error);
        return result;
    }
    fbx::SceneOutcome scene = fbx::Scene::Build(std::move(*read.file));
    if (!scene.ok()) {
        result.diagnostics.error(DiagCode::UnsupportedVersion, scene.error);
        return result;
    }
    Result<FbxImport> imported = fromFbx(*scene.scene);
    result.diagnostics = std::move(imported.diagnostics);
    for (const std::string& warning : read.warnings) {
        result.diagnostics.warn(DiagCode::Unspecified, warning);
    }
    for (const std::string& warning : scene.warnings) {
        result.diagnostics.warn(DiagCode::Unspecified, warning);
    }
    if (imported.ok()) {
        result.value = std::move(imported->document);
    }
    return result;
}

Result<FbxImport> FbxConverter::fromFbx(const fbx::Scene& scene,
                                        const FbxReadOptions& options) const {
    Result<FbxImport> result;
    Importer importer(scene, options, result.diagnostics);
    result.value = importer.run();
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
