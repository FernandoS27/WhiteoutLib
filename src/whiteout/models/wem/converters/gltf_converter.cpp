// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file gltf_converter.cpp
 * @brief `Document` ⇄ glTF 2.0 (GLTF_DESIGN §§2–4, §7).
 *
 * ### The basis change is a permutation, applied here and nowhere else
 *
 * WEM's canonical space is +X forward, +Y left, +Z up; glTF mandates +Y up,
 * +Z forward, +X left. Both are right-handed with matching left conventions, so
 * the change of basis is the pure cyclic permutation `gltf = (y, z, x)_blizzard`
 * — determinant +1, winding preserved, bit-exact. It is baked into positions,
 * normals, tangents, node TRS and matrices at this boundary, never expressed as
 * a root rotation for a DCC to trip over; neither `CoordSpace` enum grows a
 * value for it.
 *
 * Matrices additionally transpose: WEM's `ToMatrix` is row-vector (`p * M`,
 * translation in `data[3]`) and glTF is column-vector column-major, so
 * `M_gltf = C * Mᵀ * Cᵀ`. That transpose is the single easiest thing to get
 * wrong in the whole crossing (design R2); the P3 skin-identity gate is built
 * around it.
 *
 * ### Where the vertices live
 *
 * WEM stores mesh geometry in bind-pose model space, every profile, every rig
 * convention. glTF agrees for skinned meshes (skinned vertices ignore their
 * node's transform) and for a static mesh hung on an identity node — which is
 * exactly how both shapes export, so no vertex is ever re-posed here.
 */

#include "whiteout/models/wem/converters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "whiteout/models/gltf/parser.h"
#include "whiteout/models/gltf/writer.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"
#include "whiteout/models/wem/materials/gltf_core.h"

#include "gltf_anim.h"
#include "gltf_bin.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using gltf_detail::AddFloatAccessor;
using gltf_detail::AddView;
using gltf_detail::BinBuilder;
using gltf_detail::PermuteToGltf;

constexpr ProfileId kGltfProfiles[] = {ProfileId::Generic};

/// A WEM matrix (row-vector, `p * M`, translation in `data[3]`) as a glTF one
/// (column-vector column-major) in the permuted basis: the transpose and the
/// conjugation in one subscript step, `M_g[i][j] = M[p(j)][p(i)]` with
/// `p = {1, 2, 0, 3}`. The transpose half is the crossing's most dangerous
/// line (design R2); the skin-identity gate exists to catch it.
inline Matrix44f GltfMatrixFromWem(const Matrix44f& m) {
    constexpr int p[4] = {1, 2, 0, 3};
    Matrix44f out;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out.data[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                m.data[static_cast<std::size_t>(p[j])][static_cast<std::size_t>(p[i])];
        }
    }
    return out;
}

// ============================================================================
// Vertex-buffer extraction
// ============================================================================

/// The interleaved buffer's floats for one layout entry. Every request this
/// converter makes is `Float32`, so the read is a strided copy.
std::vector<f32> ExtractFloats(const utils::VertexBuffer& buffer,
                               const utils::VertexBuffer::Attribute& attribute) {
    const std::size_t count = buffer.vertexCount();
    const std::size_t components = attribute.component_count;
    std::vector<f32> out(count * components);
    for (std::size_t v = 0; v < count; ++v) {
        std::memcpy(out.data() + v * components,
                    buffer.data.data() + v * buffer.vertex_stride + attribute.offset,
                    components * sizeof(f32));
    }
    return out;
}

/// The spec requires unit normals and tangents, and the validator enforces it;
/// shipped content does not oblige (MDX carries all-zero normals on real
/// models). A vector already unit within 1e-6 of squared length passes through
/// bit-identically; a degenerate one becomes @p fallback; the rest normalize.
void SanitizeUnitVectors(std::vector<f32>& values, u32 components, const Vector3f& fallback) {
    if (components < 3) {
        return;
    }
    for (std::size_t i = 0; i + components <= values.size(); i += components) {
        const f32 lengthSq = values[i] * values[i] + values[i + 1] * values[i + 1] +
                             values[i + 2] * values[i + 2];
        if (lengthSq > 1.0f - 1e-6f && lengthSq < 1.0f + 1e-6f) {
            continue;
        }
        if (lengthSq < 1e-12f) {
            values[i] = fallback.x;
            values[i + 1] = fallback.y;
            values[i + 2] = fallback.z;
            continue;
        }
        const f32 inverse = 1.0f / std::sqrt(lengthSq);
        values[i] *= inverse;
        values[i + 1] *= inverse;
        values[i + 2] *= inverse;
    }
}

/// `gltf = (y, z, x)` on the first three of every @p components-wide element —
/// positions and normals whole, tangents leaving the w sign where it is.
void PermuteTriples(std::vector<f32>& values, u32 components) {
    if (components < 3) {
        return;
    }
    for (std::size_t i = 0; i + components <= values.size(); i += components) {
        const f32 x = values[i];
        const f32 y = values[i + 1];
        const f32 z = values[i + 2];
        values[i] = y;
        values[i + 1] = z;
        values[i + 2] = x;
    }
}

// ============================================================================
// Mesh export (GLTF_DESIGN §4)
// ============================================================================

/// One scalar sub-track's value at @p time — hold outside the key range, the
/// left key for a step, a lerp for everything smoother. A visibility gate does
/// not need the Hermite curve between an alpha of 0 and an alpha of 1.
f32 EvalScalarTrack(const SubTrack& track, f32 time) {
    if (track.times.empty()) {
        return 1.0f;
    }
    const u32 perKey = ValuesPerKey(track.interp);
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    if (time <= track.times.front()) {
        return values[0];
    }
    std::size_t k = 0;
    while (k + 1 < track.times.size() && track.times[k + 1] <= time) {
        ++k;
    }
    if (k + 1 >= track.times.size() || track.interp == Interpolation::Step) {
        return values[k * perKey];
    }
    const f32 span = track.times[k + 1] - track.times[k];
    const f32 u = span > 1e-9f ? (time - track.times[k]) / span : 0.0f;
    return values[k * perKey] * (1.0f - u) + values[(k + 1) * perKey] * u;
}

/// The alpha each section and material slot shows in the model's **default
/// look** — the first stand clip (else the first playable clip), sampled at
/// its start and midpoint.
///
/// MDX keys effect geosets invisible outside their own sequence — dissipate
/// orbs, hit flashes — through geoset and layer alpha, and core glTF cannot
/// animate either. A static export is the default look, so what that look
/// hides is skipped rather than shipped as permanently-visible white sheets
/// (GLTF_DESIGN §4; Blizzard's own WC3→SC2 converter makes the same call with
/// a 192/255 threshold).
class DefaultLookAlpha {
public:
    DefaultLookAlpha(const Document& document, const Model& model, u32 modelIndex,
                     ProfileId profile, u32 look) {
        const Clip* clip = nullptr;
        for (const Clip& candidate : document.clips) {
            if (candidate.model != modelIndex ||
                hasFlag(candidate.flags, ClipFlags::AutoPlay)) {
                continue;
            }
            bool stand = candidate.name.size() >= 5;
            for (std::size_t i = 0; stand && i < 5; ++i) {
                const char c = candidate.name[i];
                stand = (c | 0x20) == "stand"[i];
            }
            if (stand) {
                clip = &candidate;
                break;
            }
            if (clip == nullptr) {
                clip = &candidate;
            }
        }
        if (clip == nullptr) {
            return;
        }

        // Containers flatten by priority, the same rule the animation export
        // applies: the highest priority that keys a channel speaks for it.
        std::vector<u32> order(clip->containers.size());
        for (u32 i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            return clip->containers[a].priority > clip->containers[b].priority;
        });
        std::vector<u32> seen;
        for (const u32 containerIndex : order) {
            for (const SubTrack& track : clip->containers[containerIndex].subTracks) {
                if (std::find(seen.begin(), seen.end(), track.channel) != seen.end()) {
                    continue;
                }
                seen.push_back(track.channel);
                const AnimChannel* channel = model.animChannels.find(track.channel);
                if (channel == nullptr || channel->target.channel != Channel::Alpha ||
                    channel->valueType != geom::AttrType::F32 ||
                    !track.wellSized(channel->valueType) || track.times.empty()) {
                    continue;
                }
                // The larger of start and midpoint: a fade-in still counts as
                // shown, a sequence-scoped zero still counts as hidden.
                const f32 alpha = std::max(EvalScalarTrack(track, 0.0f),
                                           EvalScalarTrack(track, clip->duration * 0.5f));
                if (channel->target.kind == TrackTarget::Kind::Section) {
                    note(sections_, (static_cast<u64>(channel->target.mesh) << 32) |
                                        channel->target.sub,
                         alpha);
                } else if (channel->target.kind == TrackTarget::Kind::MaterialLayer &&
                           channel->target.material.profile == profile &&
                           channel->target.material.look == look) {
                    // Layers stack, so the slot shows if any layer does — max
                    // across the ordinals.
                    note(slots_, channel->target.material.slot, alpha, true);
                }
            }
        }
    }

    f32 sectionAlpha(u32 mesh, u32 section) const {
        return lookup(sections_, (static_cast<u64>(mesh) << 32) | section);
    }
    f32 slotAlpha(u32 slot) const {
        return lookup(slots_, slot);
    }

private:
    static void note(std::vector<std::pair<u64, f32>>& map, u64 key, f32 alpha,
                     bool takeMax = false) {
        for (auto& entry : map) {
            if (entry.first == key) {
                if (takeMax) {
                    entry.second = std::max(entry.second, alpha);
                }
                return;
            }
        }
        map.emplace_back(key, alpha);
    }
    static f32 lookup(const std::vector<std::pair<u64, f32>>& map, u64 key) {
        for (const auto& entry : map) {
            if (entry.first == key) {
                return entry.second;
            }
        }
        return 1.0f;
    }

    std::vector<std::pair<u64, f32>> sections_;
    std::vector<std::pair<u64, f32>> slots_;
};

struct MeshExportContext {
    gltf::Asset& asset;
    BinBuilder& bin;
    ProfileId profile;
    /// (materialSlot -> exported material), for the model being exported.
    const std::vector<GltfExportedMaterial>& slotMaterials;
    Diagnostics& diagnostics;
    /// How many joints the model's skin will have (= its node count) — the
    /// bound every JOINTS value must stay under.
    u32 jointCount = 0;
    /// Set by ExportMesh when the mesh carried skin data (a skin binding or a
    /// rigid section) — the holder node then references the model's skin.
    bool skinned = false;
    /// The default-look visibility gate, when the model has clips to ask.
    const DefaultLookAlpha* defaultLook = nullptr;
};

/// Exports one WEM mesh as one glTF mesh. Returns its index, or `gltf::kNone`
/// when nothing in it is drawn by the profile.
u32 ExportMesh(MeshExportContext& context, const Mesh& mesh, u32 meshOrdinal) {
    geom::RenderMeshDesc desc;
    desc.attributes.push_back({geom::names::kPosition, utils::AttributeClass::Position,
                               utils::AttributeEncoding::Float32, 0, 0});
    const bool hasNormal = mesh.attributes.has(geom::names::kNormal, geom::Domain::Halfedge);
    if (hasNormal) {
        desc.attributes.push_back({geom::names::kNormal, utils::AttributeClass::Normal,
                                   utils::AttributeEncoding::Float32, 0, 0});
    }
    const bool hasTangent = mesh.attributes.has(geom::names::kTangent, geom::Domain::Halfedge);
    if (hasTangent) {
        desc.attributes.push_back({geom::names::kTangent, utils::AttributeClass::Tangent,
                                   utils::AttributeEncoding::Float32, 0, 0});
    }
    std::vector<u32> uvSets;
    for (u32 uv = 0; uv < 8; ++uv) {
        if (mesh.attributes.has(geom::names::uv(uv), geom::Domain::Halfedge)) {
            desc.attributes.push_back({geom::names::uv(uv), utils::AttributeClass::UV,
                                       utils::AttributeEncoding::Float32, 0, 0});
            uvSets.push_back(uv);
        }
    }
    const bool hasColor = mesh.attributes.has(geom::names::color(0), geom::Domain::Halfedge);
    if (hasColor) {
        // A colour layer is RGBA bytes (`U8x4`); glTF's normalized
        // UNSIGNED_BYTE COLOR_0 holds exactly those bytes, so nothing is
        // rescaled on the way through.
        desc.attributes.push_back({geom::names::color(0), utils::AttributeClass::Color,
                                   utils::AttributeEncoding::UInt8, 4, 0});
    }
    desc.splitBySection = true;
    desc.triangulation = geom::TriangulationPolicy::FanFromFirstHalfedge;

    // The skin path covers rigid sections too: `rigidNode` means every vertex
    // binds there at weight 1, which the render view spells out (§5.6).
    bool hasSkinData = !mesh.skin.empty();
    for (const MeshSection& section : mesh.sections) {
        hasSkinData = hasSkinData || section.rigidNode.has_value();
    }
    desc.includeSkin = hasSkinData;
    desc.maxInfluences = 4;
    desc.blendIndexEncoding = utils::AttributeEncoding::UInt16;
    desc.blendWeightEncoding = utils::AttributeEncoding::Float32;

    geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
    context.diagnostics.append(render.diagnostics);
    if (render.vertexCount() == 0 || render.indices.empty()) {
        return gltf::kNone;
    }

    // --- attributes -> accessors, in the fixed semantic order ---------------
    gltf::Asset& asset = context.asset;
    std::vector<gltf::AttributeBinding> bindings;
    std::vector<u16> jointsRaw;
    std::vector<f32> weightsRaw;
    u32 uvSeen = 0;
    for (const utils::VertexBuffer::Attribute& attribute : render.vertices.layout) {
        const u32 components = static_cast<u32>(attribute.component_count);
        switch (attribute.attr_class) {
        case utils::AttributeClass::Position: {
            std::vector<f32> values = ExtractFloats(render.vertices, attribute);
            PermuteTriples(values, components);
            const u32 accessor = AddFloatAccessor(asset, context.bin, values, components,
                                                  gltf::AccessorType::Vec3, true);
            bindings.push_back({"POSITION", accessor});
            break;
        }
        case utils::AttributeClass::Normal: {
            std::vector<f32> values = ExtractFloats(render.vertices, attribute);
            PermuteTriples(values, components);
            SanitizeUnitVectors(values, components, Vector3f{0, 1, 0}); // glTF up.
            const u32 accessor = AddFloatAccessor(asset, context.bin, values, components,
                                                  gltf::AccessorType::Vec3, false);
            bindings.push_back({"NORMAL", accessor});
            break;
        }
        case utils::AttributeClass::Tangent: {
            std::vector<f32> values = ExtractFloats(render.vertices, attribute);
            PermuteTriples(values, components); // xyz permutes, the w sign stays put.
            SanitizeUnitVectors(values, components, Vector3f{1, 0, 0});
            const u32 accessor = AddFloatAccessor(asset, context.bin, values, components,
                                                  gltf::AccessorType::Vec4, false);
            bindings.push_back({"TANGENT", accessor});
            break;
        }
        case utils::AttributeClass::UV: {
            const std::vector<f32> values = ExtractFloats(render.vertices, attribute);
            const u32 wemSet = uvSeen < uvSets.size() ? uvSets[uvSeen] : uvSeen;
            ++uvSeen;
            const u32 accessor = AddFloatAccessor(asset, context.bin, values, components,
                                                  gltf::AccessorType::Vec2, false);
            // TEXCOORD index = the WEM uv-set index, so a material's `uvSet`
            // crosses without a per-mesh remap.
            bindings.push_back({"TEXCOORD_" + std::to_string(wemSet), accessor});
            break;
        }
        case utils::AttributeClass::Color: {
            // Raw bytes out of the interleaved buffer, re-declared normalized.
            const std::size_t count = render.vertices.vertexCount();
            std::vector<u8> raw(count * components);
            for (std::size_t v = 0; v < count; ++v) {
                std::memcpy(raw.data() + v * components,
                            render.vertices.data.data() + v * render.vertices.vertex_stride +
                                attribute.offset,
                            components);
            }
            const u32 view = AddView(asset, context.bin, raw.data(), raw.size(),
                                     gltf::BufferTarget::ArrayBuffer);
            gltf::Accessor accessor;
            accessor.bufferView = view;
            accessor.componentType = gltf::ComponentType::U8;
            accessor.normalized = true;
            accessor.count = static_cast<u32>(count);
            accessor.type =
                components == 3 ? gltf::AccessorType::Vec3 : gltf::AccessorType::Vec4;
            asset.accessors.push_back(std::move(accessor));
            bindings.push_back({"COLOR_0", static_cast<u32>(asset.accessors.size() - 1)});
            break;
        }
        case utils::AttributeClass::BlendIndices: {
            const std::size_t count = render.vertices.vertexCount();
            jointsRaw.resize(count * components);
            for (std::size_t v = 0; v < count; ++v) {
                std::memcpy(jointsRaw.data() + v * components,
                            render.vertices.data.data() + v * render.vertices.vertex_stride +
                                attribute.offset,
                            components * sizeof(u16));
            }
            break;
        }
        case utils::AttributeClass::BlendWeights: {
            weightsRaw = ExtractFloats(render.vertices, attribute);
            break;
        }
        default:
            break;
        }
    }

    if (!jointsRaw.empty() && weightsRaw.size() == jointsRaw.size()) {
        // glTF requires weights summing to 1 and WEM deliberately keeps what
        // the file shipped; the sum is restated here, on the staged copy of
        // the data and nowhere upstream. A vertex with no influence at all is
        // broken source data in a skinned mesh — it binds to joint 0 so the
        // file stays legal.
        u32 outOfRange = 0;
        for (std::size_t v = 0; v + 3 < weightsRaw.size(); v += 4) {
            // An influence naming a node the tree does not have cannot become
            // a joint; its weight moves to nothing and the count says so.
            for (int k = 0; k < 4; ++k) {
                const std::size_t slot = v + static_cast<std::size_t>(k);
                if (jointsRaw[slot] >= context.jointCount) {
                    jointsRaw[slot] = 0;
                    outOfRange += weightsRaw[slot] != 0.0f ? 1u : 0u;
                    weightsRaw[slot] = 0.0f;
                }
            }
            // Shipped content lists the same bone twice for one vertex (MDX
            // does it on real models); glTF forbids the repeat, so the pair
            // merges into one influence.
            for (int k1 = 0; k1 < 3; ++k1) {
                for (int k2 = k1 + 1; k2 < 4; ++k2) {
                    const std::size_t a = v + static_cast<std::size_t>(k1);
                    const std::size_t b = v + static_cast<std::size_t>(k2);
                    if (jointsRaw[a] == jointsRaw[b] && weightsRaw[b] != 0.0f &&
                        weightsRaw[a] != 0.0f) {
                        weightsRaw[a] += weightsRaw[b];
                        weightsRaw[b] = 0.0f;
                        jointsRaw[b] = 0;
                    }
                }
            }
            const f32 sum =
                weightsRaw[v] + weightsRaw[v + 1] + weightsRaw[v + 2] + weightsRaw[v + 3];
            if (sum > 1e-6f) {
                if (sum < 1.0f - 1e-6f || sum > 1.0f + 1e-6f) {
                    const f32 inverse = 1.0f / sum;
                    for (int k = 0; k < 4; ++k) {
                        weightsRaw[v + static_cast<std::size_t>(k)] *= inverse;
                    }
                }
            } else {
                weightsRaw[v] = 1.0f;
            }
            // A zero-weight slot's joint is noise; zero keeps validators and
            // renderers alike from reading meaning into it.
            for (int k = 0; k < 4; ++k) {
                if (weightsRaw[v + static_cast<std::size_t>(k)] == 0.0f) {
                    jointsRaw[v + static_cast<std::size_t>(k)] = 0;
                }
            }
        }
        if (outOfRange != 0) {
            context.diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "mesh '" + mesh.name + "': " + std::to_string(outOfRange) +
                                         " influence(s) name nodes past the tree; dropped",
                                     ElementRef(ElementKind::Mesh, meshOrdinal));
        }
        const u32 jointView = AddView(asset, context.bin, jointsRaw.data(),
                                      jointsRaw.size() * sizeof(u16),
                                      gltf::BufferTarget::ArrayBuffer);
        gltf::Accessor joints;
        joints.bufferView = jointView;
        joints.componentType = gltf::ComponentType::U16;
        joints.count = render.vertexCount();
        joints.type = gltf::AccessorType::Vec4;
        asset.accessors.push_back(std::move(joints));
        bindings.push_back({"JOINTS_0", static_cast<u32>(asset.accessors.size() - 1)});

        const u32 weightAccessor = AddFloatAccessor(asset, context.bin, weightsRaw, 4,
                                                    gltf::AccessorType::Vec4, false);
        bindings.push_back({"WEIGHTS_0", weightAccessor});
        context.skinned = true;
    }

    // --- indices -------------------------------------------------------------
    // A `CullMode::Front` material has no glTF spelling; its primitives draw
    // the same faces with the winding reversed instead (GLTF_DESIGN §5).
    std::vector<u32> indexData = std::move(render.indices);
    for (const geom::RenderRange& range : render.ranges) {
        if (range.materialSlot >= context.slotMaterials.size() ||
            !context.slotMaterials[range.materialSlot].reverseWinding) {
            continue;
        }
        for (u32 i = range.firstIndex; i + 2 < range.firstIndex + range.indexCount; i += 3) {
            std::swap(indexData[i + 1], indexData[i + 2]);
        }
    }
    const bool wide = render.vertexCount() > 0xFFFF;
    u32 indexView = 0;
    if (wide) {
        indexView = AddView(asset, context.bin, indexData.data(), indexData.size() * sizeof(u32),
                            gltf::BufferTarget::ElementArrayBuffer);
    } else {
        std::vector<u16> narrow(indexData.size());
        for (std::size_t i = 0; i < indexData.size(); ++i) {
            narrow[i] = static_cast<u16>(indexData[i]);
        }
        indexView = AddView(asset, context.bin, narrow.data(), narrow.size() * sizeof(u16),
                            gltf::BufferTarget::ElementArrayBuffer);
    }

    // --- primitives, one per drawn section -----------------------------------
    gltf::Mesh out;
    out.name = mesh.name.empty() ? ("mesh" + std::to_string(meshOrdinal)) : mesh.name;
    u32 undrawn = 0;
    u32 invisible = 0;
    u32 composited = 0;
    u32 restHidden = 0;
    for (const geom::RenderRange& range : render.ranges) {
        if (range.indexCount == 0) {
            continue;
        }
        if (range.section < mesh.sections.size() &&
            !HasProfile(mesh.sections[range.section].profiles, context.profile)) {
            ++undrawn;
            continue;
        }
        if (range.section < mesh.sections.size() &&
            hasFlag(mesh.sections[range.section].flags, SectionFlags::Hidden)) {
            // An M3 cloth simulation cage, an M2 disabled submesh — data the
            // renderer never draws either.
            ++undrawn;
            continue;
        }
        const GltfExportedMaterial* slotMaterial =
            range.materialSlot < context.slotMaterials.size()
                ? &context.slotMaterials[range.materialSlot]
                : nullptr;
        if (slotMaterial != nullptr && slotMaterial->invisible) {
            // The surface exists and does not draw (D3's alternate bodies);
            // exporting it would draw every alternate at once.
            ++invisible;
            continue;
        }
        if (slotMaterial != nullptr && slotMaterial->gameComposited) {
            // Team colour and team glow have no pixels to export — the game
            // composites them at run time; drawn without them this would be a
            // flat white sheet.
            ++composited;
            continue;
        }
        if (context.defaultLook != nullptr &&
            context.defaultLook->sectionAlpha(meshOrdinal, range.section) *
                    context.defaultLook->slotAlpha(range.materialSlot) <
                0.02f) {
            // Alpha-keyed to nothing in the default look — a dissipate orb, a
            // hit flash. glTF cannot animate it back on, so a static export
            // shows the look, not the effect stash.
            ++restHidden;
            continue;
        }
        gltf::Primitive primitive;
        primitive.attributes = bindings;
        gltf::Accessor indices;
        indices.bufferView = indexView;
        indices.byteOffset = range.firstIndex * (wide ? 4u : 2u);
        indices.componentType = wide ? gltf::ComponentType::U32 : gltf::ComponentType::U16;
        indices.count = range.indexCount;
        indices.type = gltf::AccessorType::Scalar;
        asset.accessors.push_back(std::move(indices));
        primitive.indices = static_cast<u32>(asset.accessors.size() - 1);
        if (slotMaterial != nullptr) {
            primitive.material = slotMaterial->material;
        }
        out.primitives.push_back(std::move(primitive));
    }
    if (invisible != 0) {
        context.diagnostics.info(
            DiagCode::SectionUndrawn,
            "mesh '" + out.name + "': " + std::to_string(invisible) +
                " section(s) bound to invisible materials, skipped",
            ElementRef(ElementKind::Mesh, meshOrdinal), context.profile);
    }
    if (composited != 0) {
        context.diagnostics.info(
            DiagCode::SectionUndrawn,
            "mesh '" + out.name + "': " + std::to_string(composited) +
                " section(s) coloured only by game-composited textures (team colour/glow), "
                "skipped",
            ElementRef(ElementKind::Mesh, meshOrdinal), context.profile);
    }
    if (restHidden != 0) {
        context.diagnostics.info(
            DiagCode::SectionUndrawn,
            "mesh '" + out.name + "': " + std::to_string(restHidden) +
                " section(s) alpha-keyed invisible in the default look, skipped",
            ElementRef(ElementKind::Mesh, meshOrdinal), context.profile);
    }
    if (undrawn != 0) {
        context.diagnostics.info(
            DiagCode::Unspecified,
            "mesh '" + out.name + "': " + std::to_string(undrawn) +
                " section(s) not drawn by profile " + ToString(context.profile) + ", skipped",
            ElementRef(ElementKind::Mesh, meshOrdinal), context.profile);
    }
    if (out.primitives.empty()) {
        return gltf::kNone;
    }
    asset.meshes.push_back(std::move(out));
    return static_cast<u32>(asset.meshes.size() - 1);
}

// ============================================================================
// Node export (GLTF_DESIGN §7)
// ============================================================================

/// Appends one identity root for @p model and every node of its tree under it.
/// Returns the base index of the *tree* nodes (the root sits at `base - 1`).
///
/// The root exists for glTF's sake: a WEM tree is routinely a forest, and skin
/// joints without a common root are a validator error. It also gives a child
/// model a single node an attach point can claim.
u32 ExportNodes(gltf::Asset& asset, const Model& model, Diagnostics& diagnostics, u32 modelIndex) {
    const NodeTree& tree = model.nodes;
    {
        gltf::Node root;
        root.name = model.name.empty() ? ("model" + std::to_string(modelIndex)) : model.name;
        asset.nodes.push_back(std::move(root));
    }
    const u32 rootIndex = static_cast<u32>(asset.nodes.size() - 1);
    const u32 base = static_cast<u32>(asset.nodes.size());
    u32 recomposed = 0;
    for (u32 i = 0; i < tree.size(); ++i) {
        const Node& node = tree.nodes[i];
        gltf::Node out;
        out.name = node.name;

        Transform local = node.local;
        const bool flagged = hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
                             hasFlag(node.flags, NodeFlags::DontInheritRotation) ||
                             hasFlag(node.flags, NodeFlags::DontInheritScale) ||
                             hasFlag(node.flags, NodeFlags::ModelSpace);
        if (flagged && node.parent != kInvalidNode) {
            // glTF nodes always inherit. Recompose this node's local so the
            // plain chain reproduces its bind world; the flag's animated
            // meaning is P4's declared loss.
            local = Compose(Inverse(tree.worldBind(node.parent)), tree.worldBind(i));
            ++recomposed;
        }
        out.translation = PermuteToGltf(local.translation);
        out.rotation = PermuteToGltf(local.rotation);
        out.scale = Vector3f{local.scale.y, local.scale.z, local.scale.x};
        asset.nodes.push_back(std::move(out));
    }
    for (u32 i = 0; i < tree.size(); ++i) {
        const u32 parent = tree.nodes[i].parent;
        if (parent != kInvalidNode && parent < tree.size()) {
            asset.nodes[base + parent].children.push_back(base + i);
        } else {
            asset.nodes[rootIndex].children.push_back(base + i);
        }
    }
    if (recomposed != 0) {
        diagnostics.info(DiagCode::Unspecified,
                         "model '" + model.name + "': " + std::to_string(recomposed) +
                             " node(s) with inherit/model-space flags recomposed to plain "
                             "parenting; the flags' animated meaning does not cross",
                         ElementRef(ElementKind::Document, modelIndex));
    }
    return base;
}

} // namespace

// ============================================================================
// GltfConverter
// ============================================================================

std::string GltfConverter::formatId() const {
    return "gltf";
}

std::string GltfConverter::formatName() const {
    return "glTF 2.0";
}

std::span<const ProfileId> GltfConverter::profiles() const {
    return kGltfProfiles;
}

bool GltfConverter::supportsImport() const {
    return true;
}

bool GltfConverter::supportsExport() const {
    return true;
}

u32 GltfConverter::defaultExportVersion() const {
    return 2;
}

Result<Document> GltfConverter::importFromBytes(std::span<const u8> data) const {
    gltf::ParseOutcome parsed = gltf::Parser::FromBytes(data);
    if (!parsed.ok()) {
        Result<Document> result;
        result.diagnostics.error(DiagCode::UnsupportedVersion, parsed.error);
        return result;
    }
    Result<Document> result = fromGltf(*parsed.asset);
    for (const std::string& warning : parsed.warnings) {
        result.diagnostics.warn(DiagCode::Unspecified, warning);
    }
    return result;
}

Result<std::vector<u8>> GltfConverter::exportToBytes(const Document& document, ProfileId profile,
                                                     u32 /*version*/) const {
    Result<gltf::Asset> converted = toGltf(document, profile);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    result.value = gltf::Writer::ToGlb(*converted);
    return result;
}

// ============================================================================
// Import (GLTF_DESIGN §4 reverse, §5 reverse, §7)
// ============================================================================

namespace {

/// The inverse permutation: `blizzard = (z, x, y)_gltf`.
inline Vector3f PermuteFromGltf(const Vector3f& v) {
    return {v.z, v.x, v.y};
}

inline Quaternion PermuteFromGltf(const Quaternion& q) {
    return {q.z, q.x, q.y, q.w};
}

/// Inverse of `GltfMatrixFromWem`: column-vector glTF matrix to WEM's
/// row-vector convention in the canonical basis, `M_w[i][j] = M_g[q(j)][q(i)]`
/// with `q = {2, 0, 1, 3}`.
inline Matrix44f WemMatrixFromGltf(const Matrix44f& m) {
    constexpr int q[4] = {2, 0, 1, 3};
    Matrix44f out;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            out.data[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                m.data[static_cast<std::size_t>(q[j])][static_cast<std::size_t>(q[i])];
        }
    }
    return out;
}

/// Each glTF node's global transform as a column-vector matrix, composed the
/// way a glTF consumer composes it (T · R · S down the tree).
std::vector<Matrix44f> GltfWorldColumns(const gltf::Asset& source,
                                        std::span<const u32> parentOf,
                                        std::span<const u32> order) {
    std::vector<Matrix44f> world(source.nodes.size(), Matrix44f::identity());
    for (const u32 index : order) {
        const gltf::Node& node = source.nodes[index];
        Matrix44f local;
        if (node.hasMatrix) {
            local = node.matrix;
        } else {
            Matrix44f rotation = Matrix44f::rotation(node.rotation).transpose();
            Matrix44f scale = Matrix44f::identity();
            scale.data[0][0] = node.scale.x;
            scale.data[1][1] = node.scale.y;
            scale.data[2][2] = node.scale.z;
            local = rotation * scale;
            local.data[0][3] = node.translation.x;
            local.data[1][3] = node.translation.y;
            local.data[2][3] = node.translation.z;
        }
        world[index] =
            parentOf[index] == kInvalidNode ? local : world[parentOf[index]] * local;
    }
    return world;
}

/// Applies a column matrix to a position / direction (w = 1 / 0).
inline Vector3f ApplyColumn(const Matrix44f& m, const Vector3f& v, f32 w) {
    return {m.data[0][0] * v.x + m.data[0][1] * v.y + m.data[0][2] * v.z + m.data[0][3] * w,
            m.data[1][0] * v.x + m.data[1][1] * v.y + m.data[1][2] * v.z + m.data[1][3] * w,
            m.data[2][0] * v.x + m.data[2][1] * v.y + m.data[2][2] * v.z + m.data[2][3] * w};
}

/// Joints/weights of one primitive, every set concatenated per vertex —
/// variable width is native to the CSR binding.
struct ImportedSkin {
    std::vector<u32> joints;  ///< setCount * 4 per vertex.
    std::vector<f32> weights;
    u32 lanes = 0;
};

bool readPrimitiveSkin(const gltf::Asset& source, const gltf::Primitive& primitive,
                       ImportedSkin& out) {
    out.lanes = 0;
    for (u32 set = 0;; ++set) {
        const u32 joints = primitive.attribute("JOINTS_" + std::to_string(set));
        const u32 weights = primitive.attribute("WEIGHTS_" + std::to_string(set));
        if (joints == gltf::kNone || weights == gltf::kNone) {
            break;
        }
        std::vector<u32> jointValues;
        std::vector<f32> weightValues;
        if (!gltf::ReadAccessorU32(source, joints, jointValues) ||
            !gltf::ReadAccessorF32(source, weights, weightValues) ||
            jointValues.size() != weightValues.size()) {
            return false;
        }
        if (set == 0) {
            out.joints = std::move(jointValues);
            out.weights = std::move(weightValues);
        } else {
            // Interleave per vertex: vertex v's lanes grow by 4.
            const std::size_t vertices = jointValues.size() / 4;
            std::vector<u32> mergedJoints((out.lanes + 4) * vertices);
            std::vector<f32> mergedWeights((out.lanes + 4) * vertices);
            for (std::size_t v = 0; v < vertices; ++v) {
                for (u32 lane = 0; lane < out.lanes; ++lane) {
                    mergedJoints[v * (out.lanes + 4) + lane] = out.joints[v * out.lanes + lane];
                    mergedWeights[v * (out.lanes + 4) + lane] =
                        out.weights[v * out.lanes + lane];
                }
                for (u32 lane = 0; lane < 4; ++lane) {
                    mergedJoints[v * (out.lanes + 4) + out.lanes + lane] =
                        jointValues[v * 4 + lane];
                    mergedWeights[v * (out.lanes + 4) + out.lanes + lane] =
                        weightValues[v * 4 + lane];
                }
            }
            out.joints = std::move(mergedJoints);
            out.weights = std::move(mergedWeights);
        }
        out.lanes += 4;
    }
    return true;
}

} // namespace

Result<Document> GltfConverter::fromGltf(const gltf::Asset& source) const {
    Result<Document> result;
    Diagnostics& diagnostics = result.diagnostics;

    Document document;
    document.declare(ProfileId::Generic);
    document.defaultProfile = ProfileId::Generic;
    if (source.scene != gltf::kNone && source.scene < source.scenes.size()) {
        document.name = source.scenes[source.scene].name;
    }
    if (source.scenes.size() > 1) {
        diagnostics.info(DiagCode::FeatureDropped,
                         std::to_string(source.scenes.size() - 1) +
                             " extra scene(s) ignored; the default scene is the import");
    }

    Model model;
    model.name = !document.name.empty() ? document.name : "gltf";

    // --- nodes, parents-first ------------------------------------------------
    const std::size_t nodeCount = source.nodes.size();
    std::vector<u32> parentOf(nodeCount, kInvalidNode);
    for (std::size_t i = 0; i < nodeCount; ++i) {
        for (const u32 child : source.nodes[i].children) {
            if (child >= nodeCount) {
                continue;
            }
            if (parentOf[child] != kInvalidNode) {
                diagnostics.warn(DiagCode::DanglingNodeReference,
                                 "node " + std::to_string(child) +
                                     " has two parents; the first one keeps it");
                continue;
            }
            parentOf[child] = static_cast<u32>(i);
        }
    }
    // Depth-first from the roots; a node a cycle strands imports as a root.
    std::vector<u32> order;
    order.reserve(nodeCount);
    {
        std::vector<u8> visited(nodeCount, 0);
        std::vector<u32> stack;
        for (std::size_t i = 0; i < nodeCount; ++i) {
            if (parentOf[i] == kInvalidNode) {
                stack.push_back(static_cast<u32>(i));
            }
        }
        // Reverse so lower indices pop first — deterministic order.
        std::reverse(stack.begin(), stack.end());
        while (!stack.empty()) {
            const u32 index = stack.back();
            stack.pop_back();
            if (visited[index] != 0) {
                continue;
            }
            visited[index] = 1;
            order.push_back(index);
            const std::vector<u32>& children = source.nodes[index].children;
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (*it < nodeCount) {
                    stack.push_back(*it);
                }
            }
        }
        for (std::size_t i = 0; i < nodeCount; ++i) {
            if (visited[i] == 0) {
                parentOf[i] = kInvalidNode;
                order.push_back(static_cast<u32>(i));
                diagnostics.warn(DiagCode::DanglingNodeReference,
                                 "node " + std::to_string(i) +
                                     " sits in a parent cycle; imported as a root");
            }
        }
    }

    std::vector<u32> wemIndex(nodeCount, kInvalidNode);
    for (const u32 index : order) {
        const gltf::Node& sourceNode = source.nodes[index];
        Node node;
        node.name = !sourceNode.name.empty() ? sourceNode.name
                                             : ("node" + std::to_string(index));
        node.parent = parentOf[index] != kInvalidNode ? wemIndex[parentOf[index]] : kInvalidNode;
        if (sourceNode.hasMatrix) {
            const Matrix44f wem = WemMatrixFromGltf(sourceNode.matrix);
            node.local = FromMatrix(wem);
            // Did the TRS reproduce it? A residual is shear no TRS can hold.
            const Matrix44f recomposed = ToMatrix(node.local);
            f32 residual = 0;
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) {
                    residual = std::max(
                        residual,
                        std::fabs(recomposed.data[static_cast<std::size_t>(r)]
                                                 [static_cast<std::size_t>(c)] -
                                  wem.data[static_cast<std::size_t>(r)]
                                          [static_cast<std::size_t>(c)]));
                }
            }
            if (residual > 1e-3f) {
                diagnostics.warn(DiagCode::BoneShearProjected,
                                 "node '" + node.name +
                                     "' matrix does not decompose to TRS; residual " +
                                     std::to_string(residual));
            }
        } else {
            node.local.translation = PermuteFromGltf(sourceNode.translation);
            node.local.rotation = PermuteFromGltf(sourceNode.rotation);
            node.local.scale =
                Vector3f{sourceNode.scale.z, sourceNode.scale.x, sourceNode.scale.y};
        }
        if (sourceNode.camera != gltf::kNone && sourceNode.camera < source.cameras.size()) {
            const gltf::Camera& camera = source.cameras[sourceNode.camera];
            node.kind = NodeKind::Camera;
            node.payload = CameraPayload{camera.yfov, camera.znear, camera.zfar};
        } else if (sourceNode.light != gltf::kNone && sourceNode.light < source.lights.size()) {
            const gltf::Light& light = source.lights[sourceNode.light];
            node.kind = NodeKind::Light;
            LightPayload payload;
            payload.kind = light.kind == gltf::LightKind::Directional ? LightKind::Directional
                           : light.kind == gltf::LightKind::Spot      ? LightKind::Spot
                                                                      : LightKind::Omni;
            payload.color = light.color;
            payload.intensity = light.intensity;
            payload.attenuationEnd = light.range; // Start is glTF's declared loss.
            payload.hotSpot = light.innerConeAngle;
            payload.falloff = light.outerConeAngle;
            node.payload = payload;
        }
        wemIndex[index] = model.nodes.add(std::move(node));
    }
    model.nodes.rig = RigConvention::ExplicitBind;

    // --- skins: joints become bones, binds arrive as shipped matrices --------
    if (!source.skins.empty()) {
        PoseSchema bind;
        bind.name = "bind";
        bind.space = PoseSpace::Model;
        bind.inverse = true;
        bind.storage = PoseStorage::Matrix;
        model.nodes.poseSchema.push_back(bind);
        model.nodes.authoritativePose = 0;

        for (const gltf::Skin& skin : source.skins) {
            for (const u32 joint : skin.joints) {
                if (joint >= nodeCount) {
                    continue;
                }
                // The joint and every ancestor: a bone chain broken by a
                // helper in the middle would tear the skin apart.
                u32 walk = joint;
                while (walk != kInvalidNode) {
                    Node& node = model.nodes.nodes[wemIndex[walk]];
                    if (node.kind == NodeKind::Helper) {
                        node.kind = NodeKind::Bone;
                        node.resetPayloadForKind();
                    }
                    walk = parentOf[walk];
                }
            }
        }
        model.nodes.conformPoses();

        std::vector<f32> inverseBinds;
        std::vector<u8> bindSet(model.nodes.size(), 0);
        for (const gltf::Skin& skin : source.skins) {
            const bool haveMatrices =
                skin.inverseBindMatrices != gltf::kNone &&
                gltf::ReadAccessorF32(source, skin.inverseBindMatrices, inverseBinds) &&
                inverseBinds.size() == skin.joints.size() * 16;
            for (std::size_t j = 0; j < skin.joints.size(); ++j) {
                if (skin.joints[j] >= nodeCount) {
                    continue;
                }
                Node& node = model.nodes.nodes[wemIndex[skin.joints[j]]];
                Matrix44f ibm = Matrix44f::identity(); // The spec's default.
                if (haveMatrices) {
                    for (int col = 0; col < 4; ++col) {
                        for (int row = 0; row < 4; ++row) {
                            ibm.data[static_cast<std::size_t>(row)]
                                    [static_cast<std::size_t>(col)] =
                                inverseBinds[j * 16 + static_cast<std::size_t>(col * 4 + row)];
                        }
                    }
                    ibm = WemMatrixFromGltf(ibm);
                }
                if (bindSet[wemIndex[skin.joints[j]]] != 0) {
                    continue; // Two skins, one joint: the first bind stands.
                }
                bindSet[wemIndex[skin.joints[j]]] = 1;
                if (node.poseMatrices.size() < node.poses.size()) {
                    node.poseMatrices.resize(node.poses.size(), Matrix44f::identity());
                }
                node.poseMatrices[0] = ibm;
                node.poses[0] = FromMatrix(ibm);
            }
        }
    }

    // --- materials -----------------------------------------------------------
    GltfMaterialImporter materials(source, document);
    ProfileMaterialSet set;
    set.profile = ProfileId::Generic;
    set.looks = LookTable::Single();
    // One slot per glTF material, plus one for the default material when a
    // primitive names none.
    std::vector<u32> slotForMaterial(source.materials.size() + 1, kInvalidIndex);
    const auto slotFor = [&](u32 material) {
        const std::size_t key =
            material == gltf::kNone ? source.materials.size() : material;
        if (slotForMaterial[key] != kInvalidIndex) {
            return slotForMaterial[key];
        }
        Material imported = materials.importMaterial(
            material == gltf::kNone ? gltf::kNone : material, diagnostics);
        const u32 slot = model.addSlot(imported.name);
        set.materials.push_back(std::move(imported));
        slotForMaterial[key] = slot;
        return slot;
    };

    // --- meshes, one per referencing node ------------------------------------
    const std::vector<Matrix44f> worlds = GltfWorldColumns(source, parentOf, order);
    std::vector<u32> meshSeen(source.meshes.size(), 0);
    u32 skippedPrimitives = 0;
    for (const u32 nodeIndex : order) {
        const gltf::Node& sourceNode = source.nodes[nodeIndex];
        if (sourceNode.mesh == gltf::kNone || sourceNode.mesh >= source.meshes.size()) {
            continue;
        }
        const gltf::Mesh& sourceMesh = source.meshes[sourceNode.mesh];
        if (meshSeen[sourceNode.mesh]++ != 0) {
            diagnostics.info(DiagCode::Unspecified,
                             "mesh '" + sourceMesh.name +
                                 "' is instanced under several nodes; duplicated");
        }
        const bool skinned =
            sourceNode.skin != gltf::kNone && sourceNode.skin < source.skins.size();
        const gltf::Skin* skin = skinned ? &source.skins[sourceNode.skin] : nullptr;
        // A static mesh's vertices live in node space; WEM stores bind-pose
        // model space, so they bake through the node's world and the section
        // binds rigid to it — the round trip un-bakes via the inverse bind.
        const Matrix44f& world = worlds[nodeIndex];

        geom::MeshBuilder builder;
        // Weld position-identical vertices back into one — the corner
        // attributes keep the seam values, which is what halfedge-domain
        // attributes are for (§4). Skin data joins the key: two vertices that
        // deform differently are different vertices, wherever they sit.
        std::vector<u64> weldKeys;
        std::vector<geom::VertexId> weldVertices;
        const auto weld = [&](const Vector3f& position, std::span<const u8> skinBytes,
                              std::span<const geom::Influence> influences) {
            u64 hash = 1469598103934665603ull;
            const auto mix = [&hash](const void* data, std::size_t size) {
                const u8* bytes = static_cast<const u8*>(data);
                for (std::size_t i = 0; i < size; ++i) {
                    hash = (hash ^ bytes[i]) * 1099511628211ull;
                }
            };
            mix(&position, sizeof(position));
            mix(skinBytes.data(), skinBytes.size());
            for (std::size_t i = 0; i < weldKeys.size(); ++i) {
                if (weldKeys[i] == hash) {
                    return weldVertices[i];
                }
            }
            const geom::VertexId vertex = builder.addVertex(position);
            for (const geom::Influence& influence : influences) {
                builder.addInfluence(vertex, influence.bone, influence.weight);
            }
            weldKeys.push_back(hash);
            weldVertices.push_back(vertex);
            return vertex;
        };

        for (std::size_t p = 0; p < sourceMesh.primitives.size(); ++p) {
            const gltf::Primitive& primitive = sourceMesh.primitives[p];
            if (primitive.mode != gltf::PrimitiveMode::Triangles) {
                ++skippedPrimitives;
                continue;
            }
            std::vector<f32> positions;
            if (!gltf::ReadAccessorF32(source, primitive.attribute("POSITION"), positions) ||
                positions.empty()) {
                ++skippedPrimitives;
                continue;
            }
            const std::size_t vertexCount = positions.size() / 3;
            std::vector<f32> normals;
            gltf::ReadAccessorF32(source, primitive.attribute("NORMAL"), normals);
            std::vector<f32> tangents;
            gltf::ReadAccessorF32(source, primitive.attribute("TANGENT"), tangents);
            std::vector<f32> colors;
            gltf::ReadAccessorF32(source, primitive.attribute("COLOR_0"), colors);
            const u32 colorComponents =
                colors.empty() ? 0 : static_cast<u32>(colors.size() / vertexCount);
            std::vector<std::pair<u32, std::vector<f32>>> uvSets;
            for (u32 uv = 0; uv < 8; ++uv) {
                std::vector<f32> values;
                if (gltf::ReadAccessorF32(source,
                                          primitive.attribute("TEXCOORD_" + std::to_string(uv)),
                                          values) &&
                    !values.empty()) {
                    uvSets.emplace_back(uv, std::move(values));
                }
            }
            ImportedSkin primitiveSkin;
            if (skinned && !readPrimitiveSkin(source, primitive, primitiveSkin)) {
                diagnostics.warn(DiagCode::SkinBindingMalformed,
                                 "mesh '" + sourceMesh.name + "' primitive " +
                                     std::to_string(p) + ": unreadable joints/weights");
                primitiveSkin = ImportedSkin{};
            }

            std::vector<u32> indices;
            if (primitive.indices != gltf::kNone) {
                if (!gltf::ReadAccessorU32(source, primitive.indices, indices)) {
                    ++skippedPrimitives;
                    continue;
                }
            } else {
                indices.resize(vertexCount);
                for (u32 i = 0; i < vertexCount; ++i) {
                    indices[i] = i;
                }
            }

            MeshSection section;
            section.name = sourceMesh.name.empty()
                               ? ("primitive" + std::to_string(p))
                               : (sourceMesh.name + "_" + std::to_string(p));
            section.materialSlot = slotFor(primitive.material);
            if (!skinned) {
                section.rigidNode = wemIndex[nodeIndex];
            }
            const u32 sectionIndex = builder.addSection(std::move(section));

            // Vertices, welded; a rigid mesh bakes through its node's world.
            std::vector<geom::VertexId> vertexOf(vertexCount);
            std::vector<geom::Influence> influences;
            for (std::size_t v = 0; v < vertexCount; ++v) {
                Vector3f position{positions[v * 3], positions[v * 3 + 1],
                                  positions[v * 3 + 2]};
                if (!skinned) {
                    position = ApplyColumn(world, position, 1.0f);
                }
                position = PermuteFromGltf(position);
                influences.clear();
                std::span<const u8> skinBytes;
                if (primitiveSkin.lanes != 0) {
                    const std::size_t base = v * primitiveSkin.lanes;
                    skinBytes = std::span<const u8>(
                        reinterpret_cast<const u8*>(primitiveSkin.joints.data() + base),
                        primitiveSkin.lanes * sizeof(u32));
                    for (u32 lane = 0; lane < primitiveSkin.lanes; ++lane) {
                        const f32 weight = primitiveSkin.weights[base + lane];
                        const u32 joint = primitiveSkin.joints[base + lane];
                        if (weight > 0.0f && skin != nullptr &&
                            joint < skin->joints.size() &&
                            skin->joints[joint] < nodeCount) {
                            influences.push_back(
                                geom::Influence{wemIndex[skin->joints[joint]], weight});
                        }
                    }
                }
                vertexOf[v] = weld(position, skinBytes, influences);
            }

            for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
                if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount ||
                    indices[i + 2] >= vertexCount) {
                    continue;
                }
                const geom::FaceId face =
                    builder.addTriangle(vertexOf[indices[i]], vertexOf[indices[i + 1]],
                                        vertexOf[indices[i + 2]], sectionIndex);
                for (u32 corner = 0; corner < 3; ++corner) {
                    const u32 vertex = indices[i + corner];
                    if (vertex * 3 + 2 < normals.size()) {
                        Vector3f normal{normals[vertex * 3], normals[vertex * 3 + 1],
                                        normals[vertex * 3 + 2]};
                        if (!skinned) {
                            normal = ApplyColumn(world, normal, 0.0f);
                            const f32 length =
                                std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                          normal.z * normal.z);
                            if (length > 1e-6f) {
                                normal = normal * (1.0f / length);
                            }
                        }
                        builder.setCornerAttr(face, corner, geom::names::kNormal,
                                              PermuteFromGltf(normal));
                    }
                    if (vertex * 4 + 3 < tangents.size()) {
                        Vector3f axis{tangents[vertex * 4], tangents[vertex * 4 + 1],
                                      tangents[vertex * 4 + 2]};
                        if (!skinned) {
                            axis = ApplyColumn(world, axis, 0.0f);
                            const f32 length = std::sqrt(axis.x * axis.x + axis.y * axis.y +
                                                         axis.z * axis.z);
                            if (length > 1e-6f) {
                                axis = axis * (1.0f / length);
                            }
                        }
                        const Vector3f permuted = PermuteFromGltf(axis);
                        builder.setCornerAttr(
                            face, corner, geom::names::kTangent,
                            Vector4f{permuted.x, permuted.y, permuted.z,
                                     tangents[vertex * 4 + 3]});
                    }
                    for (const auto& [uvIndex, values] : uvSets) {
                        if (vertex * 2 + 1 < values.size()) {
                            builder.setCornerAttr(
                                face, corner, geom::names::uv(uvIndex),
                                Vector2f{values[vertex * 2], values[vertex * 2 + 1]});
                        }
                    }
                    if (colorComponents != 0 &&
                        (vertex + 1) * colorComponents <= colors.size()) {
                        const f32* rgba = colors.data() + vertex * colorComponents;
                        const auto encode = [](f32 value) {
                            const f32 clamped = value < 0.0f ? 0.0f
                                                : value > 1.0f ? 1.0f
                                                               : value;
                            return static_cast<u8>(clamped * 255.0f + 0.5f);
                        };
                        builder.setCornerAttr(
                            face, corner, geom::names::color(0),
                            std::array<u8, 4>{encode(rgba[0]), encode(rgba[1]),
                                              encode(rgba[2]),
                                              colorComponents == 4 ? encode(rgba[3])
                                                                   : u8(255)});
                    }
                }
            }
        }

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        if (outcome.mesh.faceCount() == 0) {
            continue;
        }
        outcome.mesh.name = !sourceMesh.name.empty()
                                ? sourceMesh.name
                                : ("mesh" + std::to_string(sourceNode.mesh));
        outcome.mesh.recomputeBounds();
        model.meshes.push_back(std::move(outcome.mesh));
    }
    if (skippedPrimitives != 0) {
        diagnostics.warn(DiagCode::FeatureDropped,
                         std::to_string(skippedPrimitives) +
                             " primitive(s) skipped (non-triangle mode or unreadable data)");
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

    if (!source.animations.empty()) {
        gltf_anim::Import(source, document, wemIndex, diagnostics);
    }

    result.value = std::move(document);
    return result;
}

Result<gltf::Asset> GltfConverter::toGltf(const Document& document, ProfileId profile,
                                          const GltfWriteOptions& options) const {
    Result<gltf::Asset> result;
    // Not `checkExportProfile`: that also requires @p profile to be one this
    // converter *serves*, and glTF serves only `Generic` on the import side.
    // Export deliberately takes any carried profile — that is what makes glTF
    // export work for every format that can export WEM (GLTF_DESIGN §2).
    if (!document.carries(profile)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 "the document does not carry profile " +
                                     std::string(ToString(profile)),
                                 {}, profile);
        return result;
    }

    gltf::Asset asset;
    asset.asset.generator = "WhiteoutLib GltfConverter";
    BinBuilder bin;
    GltfMaterialExporter materials(document, asset);

    gltf::Scene scene;
    scene.name = document.name;
    std::vector<u32> modelNodeBase(document.models.size(), 0);

    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const Model& model = document.models[m];
        const u32 modelIndex = static_cast<u32>(m);

        // The §5 crossing, per slot at the set's default look. An unbound slot
        // exports no material — glTF's default is the honest stand-in, and
        // `Validate` already reports the coverage hole.
        const ProfileMaterialSet* set = model.setFor(profile);
        const u32 look = set != nullptr ? set->defaultLook : 0;
        std::vector<GltfExportedMaterial> slotMaterials(model.materialSlots.size());
        for (std::size_t slot = 0; slot < model.materialSlots.size(); ++slot) {
            const Material* resolved = Resolve(model, static_cast<u32>(slot), profile, look);
            if (resolved != nullptr) {
                slotMaterials[slot] = materials.exportMaterial(
                    *resolved, model.materialSlots[slot], result.diagnostics);
            }
        }

        const u32 nodeBase = ExportNodes(asset, model, result.diagnostics, modelIndex);
        modelNodeBase[m] = nodeBase;

        // One skin per model, created when a mesh first needs it. Every node
        // is a joint — joint index == model node index — so influence values
        // cross unremapped; the inverse binds come from `inverseBindMatrix`
        // (which answers correctly under either rig convention), transposed
        // and conjugated per §3.
        u32 modelSkin = gltf::kNone;
        const auto ensureSkin = [&]() {
            if (modelSkin != gltf::kNone || model.nodes.empty()) {
                return modelSkin;
            }
            std::vector<f32> inverseBinds;
            inverseBinds.reserve(static_cast<std::size_t>(model.nodes.size()) * 16);
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                const Matrix44f matrix = GltfMatrixFromWem(model.nodes.inverseBindMatrix(i));
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        inverseBinds.push_back(matrix.data[static_cast<std::size_t>(row)]
                                                          [static_cast<std::size_t>(col)]);
                    }
                }
            }
            gltf::Skin skin;
            skin.name = model.name;
            skin.inverseBindMatrices =
                AddFloatAccessor(asset, bin, inverseBinds, 16, gltf::AccessorType::Mat4, false,
                                 gltf::BufferTarget::None);
            skin.skeleton = nodeBase - 1; // The model's synthetic root.
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                skin.joints.push_back(nodeBase + i);
            }
            asset.skins.push_back(std::move(skin));
            modelSkin = static_cast<u32>(asset.skins.size() - 1);
            return modelSkin;
        };

        const DefaultLookAlpha defaultLook(document, model, modelIndex, profile, look);
        MeshExportContext context{asset, bin, profile, slotMaterials, result.diagnostics,
                                  model.nodes.size()};
        context.defaultLook = &defaultLook;
        for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const Mesh& mesh = model.meshes[meshIndex];
            if (options.baseLodOnly && mesh.lodLevel != 0) {
                result.diagnostics.info(
                    DiagCode::LevelOfDetailDropped,
                    "mesh '" + mesh.name + "' is LOD " + std::to_string(mesh.lodLevel) +
                        "; base only",
                    ElementRef(ElementKind::Mesh, static_cast<u32>(meshIndex)));
                continue;
            }
            context.skinned = false;
            const u32 exported = ExportMesh(context, mesh, static_cast<u32>(meshIndex));
            if (exported == gltf::kNone) {
                continue;
            }
            // Vertices are bind-pose model space, so the mesh hangs on an
            // identity node (see the file comment); a skinned mesh's node
            // transform is ignored anyway, per spec.
            gltf::Node holder;
            holder.name = asset.meshes[exported].name;
            holder.mesh = exported;
            if (context.skinned && !model.nodes.empty()) {
                holder.skin = ensureSkin();
            }
            asset.nodes.push_back(std::move(holder));
        }
    }

    // Child models ride their attach points (§7): an `AttachmentPayload` that
    // resolved to a model in this document parents that model's roots under
    // the attachment node. A model claimed twice keeps its second reference at
    // the scene root — glTF nodes have one parent, and duplicating a subtree
    // is a cost nobody asked for.
    if (options.bakeChildModels) {
        std::vector<bool> claimed(document.models.size(), false);
        for (std::size_t m = 0; m < document.models.size(); ++m) {
            const Model& model = document.models[m];
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                const Node& node = model.nodes.nodes[i];
                const AttachmentPayload* attachment =
                    std::get_if<AttachmentPayload>(&node.payload);
                if (attachment == nullptr || attachment->model == kInvalidIndex ||
                    attachment->model >= document.models.size() || attachment->model == m) {
                    continue;
                }
                if (claimed[attachment->model]) {
                    result.diagnostics.info(
                        DiagCode::Unspecified,
                        "model '" + document.models[attachment->model].name +
                            "' is attached more than once; the extra reference stays at "
                            "the scene root");
                    continue;
                }
                claimed[attachment->model] = true;
                // The child model's synthetic root is the one node to claim.
                asset.nodes[modelNodeBase[m] + i].children.push_back(
                    modelNodeBase[attachment->model] - 1);
            }
        }
    }

    gltf_anim::Export(document, asset, bin, modelNodeBase, result.diagnostics);

    // Scene roots are whatever nothing claimed as a child — model roots the
    // attachment pass left alone, and every mesh holder.
    {
        std::vector<bool> isChild(asset.nodes.size(), false);
        for (const gltf::Node& node : asset.nodes) {
            for (const u32 child : node.children) {
                isChild[child] = true;
            }
        }
        for (u32 i = 0; i < asset.nodes.size(); ++i) {
            if (!isChild[i]) {
                scene.nodes.push_back(i);
            }
        }
    }

    asset.scenes.push_back(std::move(scene));
    asset.scene = 0;

    if (!bin.bytes.empty()) {
        gltf::Buffer buffer;
        buffer.byteLength = static_cast<u32>(bin.bytes.size());
        buffer.data = std::move(bin.bytes);
        asset.buffers.push_back(std::move(buffer));
    }

    result.value = std::move(asset);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
