// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file obj_converter.cpp
 * @brief `Document` ⇄ Wavefront OBJ + MTL (FBX_OBJ_DESIGN §§3–6).
 *
 * The basis is a signed permutation chosen by `AxisPreset` (an OBJ declares
 * none; Y-up facing +Z by default, glTF's), applied to positions and normals
 * here and nowhere else. UVs flip V: OBJ counts from the bottom of the image,
 * WEM — like glTF and every Blizzard format — from the top.
 */

#include "whiteout/models/wem/obj_converter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "whiteout/models/obj/parser.h"
#include "whiteout/models/obj/writer.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/ops.h"
#include "whiteout/models/wem/geometry/triangulation.h"
#include "whiteout/models/wem/materials/surface_flatten.h"
#include "whiteout/models/wem/skinning/deform.h"
#include "whiteout/textures/pbr_bake.h"

#include "export_sections.h"
#include "interchange_mesh.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kObjProfiles[] = {ProfileId::Generic};
constexpr f32 kPi = 3.14159265358979323846f;

bool IsAbsolutePath(const std::string& path) {
    return (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
           (path.size() > 1 && path[1] == ':');
}

// ============================================================================
// MTL -> WEM
// ============================================================================

class MtlImporter {
public:
    MtlImporter(Document& document, std::string directory)
        : document_(document), directory_(std::move(directory)) {
        if (!directory_.empty() && directory_.back() != '/' && directory_.back() != '\\') {
            directory_.push_back('/');
        }
    }

    Material import(const obj::Material* source, const std::string& name,
                    Diagnostics& diagnostics) {
        Material material;
        material.name = name;
        CommonMaterial& common = material.InitCommon();
        if (source == nullptr) {
            // A face with no `usemtl`, or a name the library lacks: the
            // format's default, a plain white surface.
            common.setKind(MaterialKind::LegacyDeferred);
            return material;
        }
        const obj::Material& mtl = *source;
        const f32 dissolve = mtl.dissolve.value_or(1.0f);
        if (dissolve < 1.0f) {
            common.blend = BlendMode::AlphaBlend;
        }
        if (mtl.dissolveMap.present()) {
            if (mtl.dissolveMap.file == mtl.diffuseMap.file) {
                // The colour image's own alpha cuts the surface — leaves,
                // hair cards. A cutout keeps the depth write a blend loses.
                common.blend = BlendMode::AlphaKey;
                common.alphaTestThreshold = 0.5f;
            } else {
                diagnostics.warn(DiagCode::LayerDropped,
                                 name + ": a dissolve image apart from the colour image has no "
                                        "WEM slot; dropped");
            }
        }
        if (mtl.illum.has_value() && *mtl.illum == 0) {
            common.flags |= MaterialFlags::Unlit;
        }

        const Vector3f kd = mtl.diffuse.value_or(Vector3f{1.0f, 1.0f, 1.0f});
        const TextureMap* normal = mtl.normalMap.present() ? &mtl.normalMap
                                   : mtl.bumpMap.present() ? &mtl.bumpMap
                                                           : nullptr;
        // `map_Bump` is a normal map in nearly every exporter alive; when a
        // `norm` sits beside it, it is the height it was meant to be. The host
        // demotes a "normal" whose pixels are a height map.
        const TextureMap* height =
            mtl.normalMap.present() && mtl.bumpMap.present() ? &mtl.bumpMap : nullptr;
        // A colour statement absent beside its map means the map alone; the
        // format's default would zero it.
        const Vector3f ke = mtl.emissive.value_or(mtl.emissiveMap.present()
                                                      ? Vector3f{1.0f, 1.0f, 1.0f}
                                                      : Vector3f{0.0f, 0.0f, 0.0f});

        if (mtl.isPbr()) {
            common.setKind(MaterialKind::PBRDeferred);
            PbrDeferredBody& body = *common.pbr();
            body.baseColorFactor = Vector4f{kd.x, kd.y, kd.z, dissolve};
            body.metallicFactor = mtl.metallic.value_or(mtl.metallicMap.present() ? 1.0f : 0.0f);
            body.roughnessFactor =
                mtl.roughness.value_or(mtl.roughnessMap.present()
                                           ? 1.0f
                                           : InterchangeRoughness(mtl.exponent.value_or(0.0f)));
            body.emissiveFactor = ke;
            set(body, PbrSlot::BaseColor, mtl.diffuseMap, ColorSpace::Srgb);
            set(body, PbrSlot::Normal, normal, ColorSpace::Linear);
            set(body, PbrSlot::Emissive, mtl.emissiveMap, ColorSpace::Srgb);
            set(body, PbrSlot::Metallic, mtl.metallicMap, ColorSpace::Linear);
            set(body, PbrSlot::Roughness, mtl.roughnessMap, ColorSpace::Linear);
            if (height != nullptr) {
                diagnostics.info(DiagCode::LayerDropped,
                                 name + ": a height map beside a normal map has no PBR slot");
            }
            return material;
        }

        common.setKind(MaterialKind::LegacyDeferred);
        LegacyDeferredBody& body = *common.legacy();
        body.diffuseFactor = Vector4f{kd.x, kd.y, kd.z, dissolve};
        const Vector3f ks = mtl.specular.value_or(mtl.specularMap.present()
                                                      ? Vector3f{1.0f, 1.0f, 1.0f}
                                                      : Vector3f{0.0f, 0.0f, 0.0f});
        // illum 1 is "colour on, ambient on": no highlight at all.
        const bool highlight = !(mtl.illum.has_value() && *mtl.illum == 1);
        body.specularFactor = highlight ? Vector4f{ks.x, ks.y, ks.z, 1.0f}
                                        : Vector4f{0.0f, 0.0f, 0.0f, 1.0f};
        body.specularExponent = mtl.exponent.value_or(0.0f);
        body.emissiveFactor = Vector4f{ke.x, ke.y, ke.z, 1.0f};
        set(body, LegacySlot::Diffuse, mtl.diffuseMap, ColorSpace::Srgb);
        set(body, LegacySlot::Normal, normal, ColorSpace::Linear);
        set(body, LegacySlot::Height, height, ColorSpace::Linear);
        if (highlight) {
            set(body, LegacySlot::Specular, mtl.specularMap, ColorSpace::Srgb);
            set(body, LegacySlot::Gloss, mtl.exponentMap, ColorSpace::Linear);
        }
        set(body, LegacySlot::Emissive, mtl.emissiveMap, ColorSpace::Srgb);
        return material;
    }

private:
    using TextureMap = obj::TextureMap;

    template <class Body, class Slot>
    void set(Body& body, Slot slot, const TextureMap& map, ColorSpace space) {
        set(body, slot, map.present() ? &map : nullptr, space);
    }

    template <class Body, class Slot>
    void set(Body& body, Slot slot, const TextureMap* map, ColorSpace space) {
        if (map != nullptr && map->present()) {
            body.set(slot, inputFor(*map, space));
        }
    }

    TextureInput inputFor(const TextureMap& map, ColorSpace space) {
        TextureInput input;
        input.texture = textureFor(map.file, space);
        input.colorSpace = space;
        if (map.clamp) {
            input.wrapU = WrapMode::Clamp;
            input.wrapV = WrapMode::Clamp;
        }
        if (map.offset.x != 0.0f || map.offset.y != 0.0f || map.scale.x != 1.0f ||
            map.scale.y != 1.0f) {
            // `uv' = uv * s + o` in OBJ's V-up space, restated for V-down:
            // v_w' = 1 - ((1 - v_w) * sv + ov) = v_w * sv + (1 - sv - ov).
            input.uvTransform.m[0][0] = map.scale.x;
            input.uvTransform.m[0][1] = 0.0f;
            input.uvTransform.m[0][2] = map.offset.x;
            input.uvTransform.m[1][0] = 0.0f;
            input.uvTransform.m[1][1] = map.scale.y;
            input.uvTransform.m[1][2] = 1.0f - map.scale.y - map.offset.y;
        }
        return input;
    }

    u32 textureFor(const std::string& file, ColorSpace space) {
        std::string path = file;
        if (!directory_.empty() && !IsAbsolutePath(path)) {
            path = directory_ + path;
        }
        for (std::size_t i = 0; i < document_.textures.size(); ++i) {
            if (document_.textures[i].path == path) {
                return static_cast<u32>(i);
            }
        }
        TextureRef ref;
        ref.key = TexturePath{path};
        ref.path = path;
        ref.declaredSpace = space;
        document_.textures.push_back(std::move(ref));
        return static_cast<u32>(document_.textures.size() - 1);
    }

    Document& document_;
    std::string directory_;
};

// ============================================================================
// FlatSurface -> MTL
// ============================================================================

class MtlExporter {
public:
    MtlExporter(const Document& document, InterchangeImageNames& names, bool pbr)
        : document_(document), names_(names), pbr_(pbr) {}

    obj::Material lower(const FlatSurface& surface, const std::string& name,
                        Diagnostics& diagnostics) {
        obj::Material out;
        out.name = name;
        const std::string& where = name;
        // Only a metal/rough surface speaks the PBR extension: a Phong source
        // writes its own statements, so each kind comes back as itself.
        const bool pbr = pbr_ && surface.source == MaterialKind::PBRDeferred;
        const Vector4f& base = surface.baseColorFactor;
        out.diffuse = Vector3f{base.x, base.y, base.z};
        switch (surface.blend) {
        case BlendMode::Opaque:
        case BlendMode::AlphaKey:
        case BlendMode::Transparent:
        case BlendMode::AlphaBlend:
            break;
        default:
            diagnostics.warn(DiagCode::LossyBlendMode,
                             where + ": " + std::string(ToString(surface.blend)) +
                                 " has no MTL equivalent; exported as plain transparency");
            break;
        }
        out.dissolve = base.w;

        if (surface.hasSpecular) {
            out.specular = surface.specularFactor;
            out.exponent = std::clamp(surface.specularExponent, 0.0f, 1000.0f);
        } else {
            // A metal's highlight takes its albedo; a dielectric's is the 4%
            // every surface reflects head-on.
            const f32 m = surface.metallicFactor;
            out.specular = Vector3f{0.04f + (base.x - 0.04f) * m, 0.04f + (base.y - 0.04f) * m,
                                    0.04f + (base.z - 0.04f) * m};
            out.exponent = std::clamp(textures::pbr::ExponentFromRoughness(surface.roughnessFactor),
                                      0.0f, 1000.0f);
        }
        out.illum = surface.unlit ? 0 : 2;
        if (pbr) {
            out.roughness = surface.roughnessFactor;
            out.metallic = surface.metallicFactor;
        }

        bool emissiveMap = false;
        bool normalMap = false;
        const SurfaceBinding* heightBinding = nullptr;
        for (const SurfaceBinding& binding : surface.bindings) {
            switch (binding.role) {
            case SurfaceRole::BaseColor:
                if (map(binding.input, ImageChannel::All, false, where, diagnostics,
                        out.diffuseMap) &&
                    (surface.blend == BlendMode::AlphaKey ||
                     surface.blend == BlendMode::Transparent)) {
                    out.dissolveMap = out.diffuseMap;
                }
                break;
            case SurfaceRole::Normal:
                if (map(binding.input, ImageChannel::All, true, where, diagnostics,
                        out.normalMap)) {
                    out.bumpMap = out.normalMap;
                    normalMap = true;
                }
                break;
            case SurfaceRole::Orm:
                if (pbr) {
                    map(binding.input, ImageChannel::Green, false, where, diagnostics,
                        out.roughnessMap);
                    map(binding.input, ImageChannel::Blue, false, where, diagnostics,
                        out.metallicMap);
                }
                diagnostics.info(DiagCode::LayerDropped,
                                 where + ": the ORM's occlusion has no MTL statement");
                break;
            case SurfaceRole::Occlusion:
                diagnostics.info(DiagCode::LayerDropped,
                                 where + ": the occlusion map has no MTL statement");
                break;
            case SurfaceRole::Emissive:
                emissiveMap =
                    map(binding.input, ImageChannel::All, false, where, diagnostics, out.emissiveMap);
                break;
            case SurfaceRole::Specular:
                map(binding.input, ImageChannel::All, false, where, diagnostics, out.specularMap);
                break;
            case SurfaceRole::Gloss:
                map(binding.input, ImageChannel::All, false, where, diagnostics, out.exponentMap);
                break;
            case SurfaceRole::Height:
                heightBinding = &binding;
                break;
            case SurfaceRole::Metallic:
                if (pbr) {
                    map(binding.input, ImageChannel::All, false, where, diagnostics,
                        out.metallicMap);
                }
                break;
            case SurfaceRole::Roughness:
                if (pbr) {
                    map(binding.input, ImageChannel::All, false, where, diagnostics,
                        out.roughnessMap);
                }
                break;
            case SurfaceRole::Dropped:
                diagnostics.warn(DiagCode::LayerDropped,
                                 where + ": " + binding.what + " does not cross to MTL");
                break;
            }
        }
        if (heightBinding != nullptr) {
            if (normalMap) {
                diagnostics.info(DiagCode::LayerDropped,
                                 where + ": a height map beside the normal map has no MTL "
                                         "statement of its own");
            } else {
                map(heightBinding->input, ImageChannel::All, false, where, diagnostics,
                    out.bumpMap);
            }
        }
        Vector3f emissive = surface.emissiveFactor;
        if (surface.emissiveIsGain() && !emissiveMap) {
            emissive = Vector3f{0.0f, 0.0f, 0.0f};
        }
        if (emissive.x != 0.0f || emissive.y != 0.0f || emissive.z != 0.0f || emissiveMap) {
            out.emissive = emissive;
        }
        return out;
    }

private:
    bool map(const TextureInput& input, ImageChannel channel, bool normal,
             const std::string& where, Diagnostics& diagnostics, obj::TextureMap& out) {
        if (!TextureExportable(document_, input)) {
            if (input.hasTexture()) {
                diagnostics.info(DiagCode::TextureUnresolved,
                                 where + ": a replaceable or generated-UV texture has no file to "
                                         "name");
            }
            return false;
        }
        if (input.uvSet != 0) {
            diagnostics.warn(DiagCode::UvSetLimit,
                             where + ": a texture on UV set " + std::to_string(input.uvSet) +
                                 " — OBJ has one; dropped");
            return false;
        }
        out.file = names_.nameFor(input.texture, channel, normal);
        out.clamp = input.wrapU == WrapMode::Clamp && input.wrapV == WrapMode::Clamp;
        if (!input.uvTransform.isIdentity()) {
            const auto& m = input.uvTransform.m;
            if (m[0][1] != 0.0f || m[1][0] != 0.0f) {
                diagnostics.warn(DiagCode::FeatureDropped,
                                 where + ": a rotated UV transform keeps only its offset and "
                                         "scale in MTL");
            }
            out.scale = Vector2f{m[0][0], m[1][1]};
            out.offset = Vector2f{m[0][2], 1.0f - m[1][1] - m[1][2]};
        }
        return true;
    }

    const Document& document_;
    InterchangeImageNames& names_;
    bool pbr_;
};

// ============================================================================
// Export
// ============================================================================

/// Exact-bit pools for `vt` / `vn`: corners that agree share an entry.
template <class T>
class Pool {
public:
    explicit Pool(std::vector<T>& values) : values_(values) {}

    u32 add(const T& value) {
        std::array<u8, sizeof(T)> bytes;
        std::memcpy(bytes.data(), &value, sizeof(T));
        std::string key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto found = index_.find(key);
        if (found != index_.end()) {
            return found->second;
        }
        values_.push_back(value);
        const u32 index = static_cast<u32>(values_.size() - 1);
        index_.emplace(std::move(key), index);
        return index;
    }

private:
    std::vector<T>& values_;
    std::unordered_map<std::string, u32> index_;
};

struct ExportState {
    ExportState(const Document& document_, ProfileId profile_, const ObjWriteOptions& options_,
                Diagnostics& diagnostics_, obj::Asset& asset_, obj::MaterialLibrary& library_,
                MtlExporter& materials_)
        : document(document_), profile(profile_), options(options_),
          axes(AxisBasis::FromPreset(options_.axes)), diagnostics(diagnostics_), asset(asset_),
          library(library_), materials(materials_) {}

    const Document& document;
    ProfileId profile;
    const ObjWriteOptions& options;
    AxisBasis axes;
    Diagnostics& diagnostics;
    obj::Asset& asset;
    obj::MaterialLibrary& library;
    MtlExporter& materials;
    std::vector<std::pair<const Material*, u32>> materialIndex;
    std::vector<std::string> objectNames;
    std::vector<bool> claimed;
    bool anyColor = false;
};

u32 MaterialFor(ExportState& state, const Material* material, const FlatSurface& surface,
                const std::string& slotName) {
    for (const auto& [source, index] : state.materialIndex) {
        if (source == material) {
            return index;
        }
    }
    std::string name = !material->name.empty() ? material->name : slotName;
    if (name.empty()) {
        name = "material";
    }
    for (char& c : name) {
        if (c == ' ' || c == '\t') {
            c = '_';
        }
    }
    std::string unique = name;
    for (u32 attempt = 2;; ++attempt) {
        bool taken = false;
        for (const obj::Material& existing : state.library.materials) {
            taken = taken || existing.name == unique;
        }
        if (!taken) {
            break;
        }
        unique = name + "_" + std::to_string(attempt);
    }
    state.library.materials.push_back(state.materials.lower(surface, unique, state.diagnostics));
    state.asset.materials.push_back(unique);
    const u32 index = static_cast<u32>(state.asset.materials.size() - 1);
    state.materialIndex.emplace_back(material, index);
    return index;
}

std::string UniqueObjectName(ExportState& state, std::string name) {
    for (char& c : name) {
        if (c == '\n' || c == '\r') {
            c = '_';
        }
    }
    std::string unique = name;
    for (u32 attempt = 2;; ++attempt) {
        if (std::find(state.objectNames.begin(), state.objectNames.end(), unique) ==
            state.objectNames.end()) {
            break;
        }
        unique = name + "_" + std::to_string(attempt);
    }
    state.objectNames.push_back(unique);
    return unique;
}

void ExportModel(ExportState& state, u32 modelIndex, const Matrix44f* placement, u32 depth) {
    const Document& document = state.document;
    const Model& model = document.models[modelIndex];
    const ProfileMaterialSet* set = model.setFor(state.profile);
    const u32 look = set != nullptr ? set->defaultLook : 0;

    // --- the slots' surfaces --------------------------------------------------
    std::vector<std::optional<FlatSurface>> surfaces(model.materialSlots.size());
    std::vector<u32> slotMaterial(model.materialSlots.size(), obj::kNone);
    std::vector<SlotDrawState> drawStates(model.materialSlots.size());
    for (u32 slot = 0; slot < model.materialSlots.size(); ++slot) {
        const Material* resolved = Resolve(model, slot, state.profile, look);
        if (resolved == nullptr) {
            continue;
        }
        surfaces[slot] = FlattenSurface(document, *resolved);
        drawStates[slot] = SlotDrawState{surfaces[slot]->invisible, surfaces[slot]->gameComposited};
    }
    const DefaultLookAlpha defaultLook(document, model, modelIndex, state.profile, look);
    const std::vector<Matrix44f> skin =
        placement == nullptr ? SkinningAt(document, modelIndex, state.options.pose, state.diagnostics)
                             : std::vector<Matrix44f>{};

    obj::Asset& asset = state.asset;
    Pool<Vector2f> uvPool(asset.uvs);
    Pool<Vector3f> normalPool(asset.normals);
    const bool flipWinding = state.axes.determinant() < 0.0f;

    for (u32 meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const Mesh& mesh = model.meshes[meshIndex];
        const PolygonWalk walk(mesh);
        if (!walk.ok() || walk.faceCount() == 0) {
            continue;
        }
        std::vector<Vector3f> positions =
            skin.empty() ? std::vector<Vector3f>(mesh.attributes
                                                     .get<Vector3f>(geom::names::kPosition,
                                                                    geom::Domain::Vertex)
                                                     .begin(),
                                                 mesh.attributes
                                                     .get<Vector3f>(geom::names::kPosition,
                                                                    geom::Domain::Vertex)
                                                     .end())
                         : skinning::DeformMesh(mesh, skin);
        std::vector<Vector3f> normals =
            skin.empty()
                ? std::vector<Vector3f>(
                      mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge)
                          .begin(),
                      mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge)
                          .end())
                : skinning::DeformNormals(mesh, skin);
        if (placement != nullptr) {
            for (Vector3f& p : positions) {
                p = PlacePoint(*placement, p);
            }
            for (Vector3f& n : normals) {
                n = PlaceDirection(*placement, n);
            }
        }
        const std::span<const Vector2f> uvs =
            mesh.attributes.get<Vector2f>(geom::names::uv(0), geom::Domain::Halfedge);
        const std::span<const std::array<u8, 4>> colors =
            state.options.vertexColors
                ? mesh.attributes.get<std::array<u8, 4>>(geom::names::color(0),
                                                         geom::Domain::Halfedge)
                : std::span<const std::array<u8, 4>>();
        const std::span<const u32> smoothing =
            mesh.attributes.get<u32>(geom::names::kSmoothGroup, geom::Domain::Face);
        const std::span<const u32> sectionOf = mesh.faceSections();

        // Vertices: every one, in WEM order; colours averaged over corners.
        const u32 base = static_cast<u32>(asset.positions.size());
        for (const Vector3f& p : positions) {
            asset.positions.push_back(state.axes.toFile(p));
        }
        if (!colors.empty()) {
            if (!state.anyColor) {
                asset.colors.assign(base, Vector3f{1.0f, 1.0f, 1.0f});
                state.anyColor = true;
            }
            std::vector<Vector4f> sum(positions.size(), Vector4f{0, 0, 0, 0});
            bool disagree = false;
            std::vector<std::array<u8, 4>> first(positions.size(), {0, 0, 0, 0});
            std::vector<u8> seen(positions.size(), 0);
            for (u32 f = 0; f < walk.faceCount(); ++f) {
                const std::span<const u32> hs = walk.halfedges(f);
                const std::span<const u32> vs = walk.vertices(f);
                for (u32 k = 0; k < hs.size(); ++k) {
                    if (hs[k] >= colors.size() || vs[k] >= sum.size()) {
                        continue;
                    }
                    const std::array<u8, 4>& c = colors[hs[k]];
                    if (seen[vs[k]] != 0 && c != first[vs[k]]) {
                        disagree = true;
                    }
                    if (seen[vs[k]] == 0) {
                        first[vs[k]] = c;
                        seen[vs[k]] = 1;
                    }
                    sum[vs[k]] = Vector4f{sum[vs[k]].x + c[0], sum[vs[k]].y + c[1],
                                          sum[vs[k]].z + c[2], sum[vs[k]].w + 1.0f};
                }
            }
            for (const Vector4f& s : sum) {
                const f32 n = s.w > 0.0f ? 1.0f / (255.0f * s.w) : 0.0f;
                asset.colors.push_back(s.w > 0.0f ? Vector3f{s.x * n, s.y * n, s.z * n}
                                                  : Vector3f{1.0f, 1.0f, 1.0f});
            }
            if (disagree) {
                state.diagnostics.info(DiagCode::VertexColorUnsupported,
                                       "mesh '" + mesh.name +
                                           "': OBJ colours are per vertex; corners that "
                                           "disagreed were averaged");
            }
        } else if (state.anyColor) {
            asset.colors.resize(asset.positions.size(), Vector3f{1.0f, 1.0f, 1.0f});
        }

        // One `o` per mesh, one `g` per section.
        const std::string object = UniqueObjectName(
            state, !mesh.name.empty() ? mesh.name : (model.name + "_" + std::to_string(meshIndex)));
        std::vector<u32> groupOfSection(mesh.sections.size() + 1, obj::kNone);
        const auto groupFor = [&](u32 section) {
            const u32 key = section < mesh.sections.size() ? section
                                                           : static_cast<u32>(mesh.sections.size());
            if (groupOfSection[key] == obj::kNone) {
                obj::Group group;
                group.object = object;
                group.group = section < mesh.sections.size() && !mesh.sections[section].name.empty()
                                  ? mesh.sections[section].name
                                  : object;
                for (char& c : group.group) {
                    if (c == '\n' || c == '\r') {
                        c = '_';
                    }
                }
                asset.groups.push_back(std::move(group));
                groupOfSection[key] = static_cast<u32>(asset.groups.size() - 1);
            }
            return groupOfSection[key];
        };

        // The triangles the model draws, when asked for them.
        std::vector<u32> triangles;
        std::vector<u32> triangleFace;
        if (state.options.triangulate) {
            geom::TriangulateMesh(mesh, triangles, &triangleFace);
        }
        std::vector<std::vector<u32>> trianglesOfFace;
        if (state.options.triangulate) {
            trianglesOfFace.resize(walk.faceCount());
            for (std::size_t t = 0; t < triangleFace.size(); ++t) {
                const u32 face = walk.faceOfSlot(triangleFace[t]);
                if (face != kInvalidIndex) {
                    trianglesOfFace[face].push_back(static_cast<u32>(t));
                }
            }
        }

        SectionSkipCounts skipped;
        std::vector<u32> cornerScratch;
        for (u32 f = 0; f < walk.faceCount(); ++f) {
            const u32 slot = walk.slot(f);
            const u32 section = slot < sectionOf.size() ? sectionOf[slot] : 0;
            const u32 materialSlot =
                section < mesh.sections.size() ? mesh.sections[section].materialSlot : kInvalidIndex;
            const bool bound = materialSlot < surfaces.size() && surfaces[materialSlot].has_value();
            const SectionSkip skip =
                SkipSection(mesh, meshIndex, section, materialSlot, state.profile,
                            bound ? &drawStates[materialSlot] : nullptr, &defaultLook);
            if (skip != SectionSkip::None) {
                skipped.count(skip);
                continue;
            }
            u32 material = obj::kNone;
            bool reverse = flipWinding;
            if (bound) {
                if (slotMaterial[materialSlot] == obj::kNone) {
                    slotMaterial[materialSlot] =
                        MaterialFor(state, Resolve(model, materialSlot, state.profile, look),
                                    *surfaces[materialSlot], model.materialSlots[materialSlot]);
                }
                material = slotMaterial[materialSlot];
                // OBJ has no culling statement; front-culled faces draw the
                // same with their winding reversed.
                reverse = reverse != (surfaces[materialSlot]->cull == CullMode::Front);
            }
            const std::span<const u32> hs = walk.halfedges(f);
            const std::span<const u32> vs = walk.vertices(f);

            const auto emit = [&](std::span<const u32> corners) {
                obj::Face face;
                face.firstCorner = static_cast<u32>(asset.corners.size());
                face.cornerCount = static_cast<u32>(corners.size());
                face.group = groupFor(section);
                face.material = material;
                face.smoothing = slot < smoothing.size() ? smoothing[slot] : 1u;
                for (u32 i = 0; i < corners.size(); ++i) {
                    const u32 k = corners[reverse ? corners.size() - 1 - i : i];
                    obj::Corner corner;
                    corner.position = base + vs[k];
                    if (hs[k] < uvs.size()) {
                        corner.uv = uvPool.add(Vector2f{uvs[hs[k]].x, 1.0f - uvs[hs[k]].y});
                    }
                    if (hs[k] < normals.size()) {
                        corner.normal = normalPool.add(state.axes.toFile(normals[hs[k]]));
                    }
                    asset.corners.push_back(corner);
                }
                asset.faces.push_back(face);
            };

            if (state.options.triangulate && hs.size() > 3) {
                for (const u32 t : trianglesOfFace[f]) {
                    cornerScratch.clear();
                    for (u32 c = 0; c < 3; ++c) {
                        const u32 k = walk.cornerOf(f, triangles[t * 3 + c]);
                        if (k != kInvalidIndex) {
                            cornerScratch.push_back(k);
                        }
                    }
                    if (cornerScratch.size() == 3) {
                        emit(cornerScratch);
                    }
                }
            } else {
                cornerScratch.resize(hs.size());
                for (u32 k = 0; k < hs.size(); ++k) {
                    cornerScratch[k] = k;
                }
                emit(cornerScratch);
            }
        }
        skipped.report(state.diagnostics, mesh.name, meshIndex, state.profile);
    }

    // Child models ride their attach points, baked where the attachment sits.
    if (!state.options.bakeChildModels || depth > 8) {
        return;
    }
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        const AttachmentPayload* attachment =
            std::get_if<AttachmentPayload>(&model.nodes.nodes[i].payload);
        if (attachment == nullptr || attachment->model == kInvalidIndex ||
            attachment->model >= document.models.size() || attachment->model == modelIndex) {
            continue;
        }
        Matrix44f frame = NodeFrame(document, modelIndex, i,
                                    placement == nullptr ? state.options.pose : std::nullopt);
        if (placement != nullptr) {
            frame = frame * *placement;
        }
        ExportModel(state, attachment->model, &frame, depth + 1);
    }
}

} // namespace

// ============================================================================
// ObjConverter
// ============================================================================

std::string ObjConverter::formatId() const {
    return "obj";
}

std::string ObjConverter::formatName() const {
    return "Wavefront OBJ";
}

std::span<const ProfileId> ObjConverter::profiles() const {
    return kObjProfiles;
}

bool ObjConverter::supportsImport() const {
    return true;
}

bool ObjConverter::supportsExport() const {
    return true;
}

u32 ObjConverter::defaultExportVersion() const {
    return 0;
}

Result<Document> ObjConverter::importFromBytes(std::span<const u8> data) const {
    const obj::ParseOutcome parsed = obj::Parser::FromText(
        std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    if (!parsed.ok()) {
        Result<Document> result;
        result.diagnostics.error(DiagCode::UnsupportedVersion, parsed.error);
        return result;
    }
    Result<Document> result = fromObj(*parsed.asset, nullptr);
    for (const std::string& warning : parsed.warnings) {
        result.diagnostics.warn(DiagCode::Unspecified, warning);
    }
    return result;
}

Result<std::vector<u8>> ObjConverter::exportToBytes(const Document& document, ProfileId profile,
                                                    u32 /*version*/) const {
    Result<ObjExport> converted = toObj(document, profile);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    const std::string text = obj::Writer::ToText(converted->asset);
    result.value = std::vector<u8>(text.begin(), text.end());
    return result;
}

Result<Document> ObjConverter::fromObj(const obj::Asset& source,
                                       const obj::MaterialLibrary* materials,
                                       const ObjReadOptions& options) const {
    Result<Document> result;
    Diagnostics& diagnostics = result.diagnostics;
    const AxisBasis axes = AxisBasis::FromPreset(options.axes);

    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;

    Model model;
    model.name = "obj";
    model.nodes.rig = RigConvention::ExplicitBind;
    ProfileMaterialSet set;
    set.profile = ProfileId::Generic;
    set.looks = LookTable::Single();

    // --- materials: one slot per `usemtl` name, the default one on demand ----
    MtlImporter importer(document, options.textureDirectory);
    std::vector<u32> slotOf(source.materials.size() + 1, kInvalidIndex);
    const auto slotFor = [&](u32 material) {
        const std::size_t key = material < source.materials.size() ? material
                                                                   : source.materials.size();
        if (slotOf[key] == kInvalidIndex) {
            const std::string name =
                material < source.materials.size() ? source.materials[material] : "default";
            const obj::Material* mtl = materials != nullptr && material < source.materials.size()
                                           ? materials->find(name)
                                           : nullptr;
            if (mtl == nullptr && materials != nullptr && material < source.materials.size()) {
                diagnostics.warn(DiagCode::SlotNotBound,
                                 "material '" + name + "' is not in the material library; "
                                                       "imported plain white");
            }
            set.materials.push_back(importer.import(mtl, name, diagnostics));
            slotOf[key] = model.addSlot(name);
        }
        return slotOf[key];
    };

    // --- meshes: one per `o` (or per `g` in a file with no `o`) --------------
    bool anyObject = false;
    for (const obj::Group& group : source.groups) {
        anyObject = anyObject || !group.object.empty();
    }
    std::vector<std::string> meshNames;
    std::unordered_map<std::string, u32> meshOf;
    std::vector<std::vector<u32>> facesOf;
    for (u32 f = 0; f < source.faces.size(); ++f) {
        const obj::Face& face = source.faces[f];
        const std::string name =
            face.group < source.groups.size()
                ? (anyObject ? source.groups[face.group].object : source.groups[face.group].group)
                : std::string();
        auto found = meshOf.find(name);
        if (found == meshOf.end()) {
            found = meshOf.emplace(name, static_cast<u32>(meshNames.size())).first;
            meshNames.push_back(name);
            facesOf.emplace_back();
        }
        facesOf[found->second].push_back(f);
    }

    const bool colored = source.colors.size() == source.positions.size() && !source.colors.empty();
    std::vector<u32> vertexOf(source.positions.size(), kInvalidIndex);
    std::vector<u32> stamp(source.positions.size(), kInvalidIndex);
    std::vector<geom::VertexId> corners;
    u32 nonFinite = 0;
    for (u32 m = 0; m < meshNames.size(); ++m) {
        geom::MeshBuilder builder;
        bool hasUv = false;
        bool hasNormal = false;
        for (const u32 f : facesOf[m]) {
            const obj::Face& face = source.faces[f];
            for (u32 k = 0; k < face.cornerCount; ++k) {
                const obj::Corner& corner = source.corners[face.firstCorner + k];
                hasUv = hasUv || corner.uv != obj::kNone;
                hasNormal = hasNormal || corner.normal != obj::kNone;
            }
        }
        std::vector<u32> sectionOfSlot;
        for (const u32 f : facesOf[m]) {
            const obj::Face& face = source.faces[f];
            const u32 slot = slotFor(face.material);
            if (slot >= sectionOfSlot.size()) {
                sectionOfSlot.resize(slot + 1, kInvalidIndex);
            }
            if (sectionOfSlot[slot] == kInvalidIndex) {
                MeshSection section;
                section.name = model.materialSlots[slot];
                section.materialSlot = slot;
                sectionOfSlot[slot] = builder.addSection(std::move(section));
            }
            corners.clear();
            for (u32 k = 0; k < face.cornerCount; ++k) {
                const u32 p = source.corners[face.firstCorner + k].position;
                if (stamp[p] != m) {
                    stamp[p] = m;
                    Vector3f position = source.positions[p];
                    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                        !std::isfinite(position.z)) {
                        position = Vector3f{0.0f, 0.0f, 0.0f};
                        ++nonFinite;
                    }
                    vertexOf[p] = builder.addVertex(axes.fromFile(position)).value();
                }
                corners.push_back(geom::VertexId(vertexOf[p]));
            }
            const geom::FaceId id = builder.addFace(corners, sectionOfSlot[slot]);
            for (u32 k = 0; k < face.cornerCount; ++k) {
                const obj::Corner& corner = source.corners[face.firstCorner + k];
                if (hasUv && corner.uv != obj::kNone) {
                    const Vector2f& uv = source.uvs[corner.uv];
                    builder.setCornerAttr(id, k, geom::names::uv(0), Vector2f{uv.x, 1.0f - uv.y});
                }
                if (hasNormal && corner.normal != obj::kNone) {
                    builder.setCornerAttr(id, k, geom::names::kNormal,
                                          axes.fromFile(source.normals[corner.normal]));
                }
                if (colored) {
                    const Vector3f& c = source.colors[corner.position];
                    const auto encode = [](f32 value) {
                        const f32 clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
                        return static_cast<u8>(clamped * 255.0f + 0.5f);
                    };
                    builder.setCornerAttr(id, k, geom::names::color(0),
                                          std::array<u8, 4>{encode(c.x), encode(c.y), encode(c.z),
                                                            u8(255)});
                }
            }
        }
        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        if (outcome.mesh.faceCount() == 0) {
            continue;
        }
        Mesh& mesh = outcome.mesh;
        if (!hasNormal) {
            if (source.smoothingStated) {
                // `s N` groups smooth within themselves; `s off` faces are
                // flat, each a group of its own.
                const std::vector<u32> survivors =
                    SurvivingInputFaces(static_cast<u32>(facesOf[m].size()), mesh.repairLog);
                u32 nextLone = 0;
                for (const u32 f : facesOf[m]) {
                    nextLone = std::max(nextLone, source.faces[f].smoothing + 1);
                }
                std::span<u32> groups = mesh.attributes.getOrCreate<u32>(
                    geom::names::kSmoothGroup, geom::Domain::Face, geom::AttrType::U32);
                for (std::size_t i = 0; i < survivors.size() && i < groups.size(); ++i) {
                    const u32 smoothing = source.faces[facesOf[m][survivors[i]]].smoothing;
                    groups[i] = smoothing != 0 ? smoothing : nextLone++;
                }
                geom::RecomputeNormals(mesh, kPi);
            } else {
                geom::RecomputeNormals(mesh);
            }
        }
        mesh.name = !meshNames[m].empty() ? meshNames[m] : ("mesh" + std::to_string(m));
        mesh.recomputeBounds();
        model.meshes.push_back(std::move(mesh));
    }
    if (nonFinite != 0) {
        diagnostics.warn(DiagCode::Unspecified,
                         std::to_string(nonFinite) +
                             " vertex position(s) were not finite numbers; placed at the origin");
    }
    if (source.skippedElements != 0) {
        diagnostics.info(DiagCode::FeatureDropped,
                         std::to_string(source.skippedElements) +
                             " line, point or free-form element(s) skipped; a mesh holds faces");
    }

    set.resizeBindings(model.materialSlots.size());
    for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
        set.slotBindings[slot].byLook[0] = static_cast<u32>(slot);
    }
    model.profileSets.push_back(std::move(set));

    ResetExtent(model.bounds);
    for (const Mesh& mesh : model.meshes) {
        GrowExtent(model.bounds, mesh.bounds.minimum);
        GrowExtent(model.bounds, mesh.bounds.maximum);
    }
    if (model.meshes.empty()) {
        model.bounds = Extent{};
    } else {
        FinishExtent(model.bounds);
    }
    document.bounds = model.bounds;
    document.models.push_back(std::move(model));
    result.value = std::move(document);
    return result;
}

Result<ObjExport> ObjConverter::toObj(const Document& document, ProfileId profile,
                                      const ObjWriteOptions& options) const {
    Result<ObjExport> result;
    // Any carried profile, as glTF: OBJ serves `Generic` only on the way in.
    if (!document.carries(profile)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 "the document does not carry profile " +
                                     std::string(ToString(profile)),
                                 {}, profile);
        return result;
    }
    ObjExport out;
    if (!options.materialLibrary.empty()) {
        out.asset.materialLibraries.push_back(options.materialLibrary);
    }
    InterchangeImageNames names(document);
    MtlExporter materials(document, names, options.pbrExtension);
    ExportState state(document, profile, options, result.diagnostics, out.asset, out.materials,
                      materials);
    state.claimed = ClaimedChildModels(document);

    for (u32 m = 0; m < document.models.size(); ++m) {
        if (options.bakeChildModels && state.claimed[m]) {
            continue; // Exported where its attachment places it.
        }
        ExportModel(state, m, nullptr, 0);
    }
    if (state.anyColor) {
        out.asset.colors.resize(out.asset.positions.size(), Vector3f{1.0f, 1.0f, 1.0f});
    }
    out.images = names.take();
    result.value = std::move(out);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
