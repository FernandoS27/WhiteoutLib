// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/meshes/remove.h>

#include <whiteout/models/wem/geometry/ops.h>
#include <whiteout/models/wem/nodes/emitters.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <variant>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

u32 remapped(std::span<const u32> meshRemap, u32 mesh) {
    return mesh < meshRemap.size() ? meshRemap[mesh] : kInvalidIndex;
}

/// `mdx::Bone::MULTIPLE_GEOSETS`: the file's "no geoset".
constexpr u32 kNoGeoset = 0xFFFFFFFFu;

/// The triangles the mesh draws, whatever its faces' corner counts.
u64 trianglesOf(const Mesh& mesh) {
    u64 triangles = 0;
    for (const u32 corners : mesh.faceSet().faceValence) {
        triangles += corners > 2 ? corners - 2 : 0;
    }
    return triangles;
}

} // namespace

void RemapMeshReferencers(Model& model, std::span<const u32> meshRemap, Diagnostics& out) {
    if (meshRemap.empty()) {
        return;
    }
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Section ||
            channel.target.mesh == kInvalidIndex) {
            continue;
        }
        const u32 to = remapped(meshRemap, channel.target.mesh);
        if (to == kInvalidIndex) {
            out.warn(DiagCode::AnimChannelInvalidated,
                     "channel " + number(channel.id) + " drove mesh " +
                         number(channel.target.mesh) + ", which is gone",
                     ElementRef(ElementKind::Channel, channel.id));
        }
        channel.target.mesh = to;
    }
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        Node& node = model.nodes.nodes[i];
        if (auto* bone = std::get_if<BonePayload>(&node.payload);
            bone != nullptr && bone->gateMesh != kInvalidIndex) {
            const u32 to = remapped(meshRemap, bone->gateMesh);
            if (to == kInvalidIndex) {
                out.warn(DiagCode::IndexOutOfRange,
                         "bone '" + node.name + "' was gated by mesh " + number(bone->gateMesh) +
                             ", which is gone; it has no gate now",
                         ElementRef(ElementKind::Node, i));
            }
            bone->gateMesh = to;
        }
        // Sections of `meshes[0]`: they mean something only while it stays first.
        if (auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            emitter != nullptr && !emitter->shapeSections.empty() &&
            remapped(meshRemap, 0) != 0) {
            out.warn(DiagCode::IndexOutOfRange,
                     "emitter '" + node.name +
                         "' emitted from sections of mesh 0, which is gone; its shape is cleared",
                     ElementRef(ElementKind::Node, i));
            emitter->shapeSections.clear();
        }
        // The raw `geosetId` an MDX bone keeps where its gate would not write it
        // back (`fromMdx`). Only an in-range value is a mesh index; anything
        // else is the file's own number and stays as it was.
        if (const NativeBag::Entry* raw = node.native.find("geosetId");
            raw != nullptr && raw->value >= 0 && static_cast<u64>(raw->value) < meshRemap.size()) {
            const u32 to = meshRemap[static_cast<std::size_t>(raw->value)];
            node.native.set("geosetId", to == kInvalidIndex ? static_cast<i64>(kNoGeoset)
                                                            : static_cast<i64>(to));
        }
    }
}

void EraseChannels(Document& document, u32 model, std::span<const u32> ids) {
    if (ids.empty() || model >= document.models.size()) {
        return;
    }
    const auto named = [ids](u32 id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (SubTrackContainer& container : clip.containers) {
            std::erase_if(container.subTracks,
                          [&](const SubTrack& track) { return named(track.channel); });
        }
    }
    std::erase_if(document.models[model].animChannels.channels,
                  [&](const AnimChannel& channel) { return named(channel.id); });
}

u32 RemoveMeshes(Document& document, u32 modelIndex, std::span<const u8> drop, Diagnostics& out) {
    if (modelIndex >= document.models.size()) {
        return 0;
    }
    Model& model = document.models[modelIndex];
    const u32 count = static_cast<u32>(model.meshes.size());
    const auto dropped = [&](u32 m) { return m < drop.size() && drop[m] != 0; };

    std::vector<u32> meshRemap(count, kInvalidIndex);
    u32 kept = 0;
    for (u32 m = 0; m < count; ++m) {
        if (!dropped(m)) {
            meshRemap[m] = kept++;
        }
    }
    if (kept == count) {
        return 0;
    }

    // A gone mesh's channels go with it, sub-tracks and all: invalidated, as
    // `RemapMeshReferencers` would leave them, they fail `Validate`.
    std::vector<u32> channels;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Section && dropped(channel.target.mesh)) {
            channels.push_back(channel.id);
        }
    }
    EraseChannels(document, modelIndex, channels);

    u32 next = 0;
    for (u32 m = 0; m < count; ++m) {
        if (meshRemap[m] != kInvalidIndex) {
            if (next != m) {
                model.meshes[next] = std::move(model.meshes[m]);
            }
            ++next;
        }
    }
    model.meshes.resize(kept);
    RemapMeshReferencers(model, meshRemap, out);
    return count - kept;
}

u32 DropLevelsOfDetail(Document& document, Diagnostics& out) {
    const auto isLevel = [](const Mesh& mesh) {
        return mesh.lodLevel != 0 && mesh.lodLevel != kAllLods;
    };
    u32 removed = 0;
    for (u32 m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        std::vector<u8> drop(model.meshes.size(), 0);
        std::array<u64, 4> triangles{};
        u32 levels = 0;
        for (std::size_t i = 0; i < model.meshes.size(); ++i) {
            const Mesh& mesh = model.meshes[i];
            if (mesh.lodLevel < triangles.size()) {
                triangles[mesh.lodLevel] += trianglesOf(mesh);
            }
            if (isLevel(mesh)) {
                drop[i] = 1;
                ++levels;
            }
        }
        if (levels == 0) {
            continue;
        }
        if (levels == model.meshes.size()) {
            // An empty model is a worse answer than a coarse one.
            out.info(DiagCode::LevelOfDetailDropped,
                     "model '" + model.name +
                         "' has no mesh at the base level of detail; its " + number(levels) +
                         " level mesh(es) were kept",
                     ElementRef(ElementKind::Document, m));
            continue;
        }

        // Recorded before anything goes, so turning generation on reproduces
        // the file's sizes (EDIT_MODE_MODELLING_DESIGN.md §8.2).
        model.lodExport.sourceHadLevels = true;
        for (u32 level = 1; level < triangles.size(); ++level) {
            if (triangles[0] != 0 && triangles[level] != 0) {
                model.lodExport.ratios[level - 1] =
                    static_cast<f32>(triangles[level]) / static_cast<f32>(triangles[0]);
            }
        }
        const u32 count = RemoveMeshes(document, m, drop, out);
        removed += count;
        out.info(DiagCode::LevelOfDetailDropped,
                 number(count) + " mesh(es) above the base level of detail were dropped from '" +
                     model.name + "'; the export generates a ladder when the model asks for one",
                 ElementRef(ElementKind::Document, m));
    }
    return removed;
}

void CheckMeshReferencers(const Model& model, Diagnostics& out) {
    const u32 meshCount = static_cast<u32>(model.meshes.size());
    const u32 firstSections =
        meshCount == 0 ? 0u : static_cast<u32>(model.meshes[0].sections.size());
    for (u32 i = 0; i < model.nodes.size(); ++i) {
        const Node& node = model.nodes.nodes[i];
        if (const auto* bone = std::get_if<BonePayload>(&node.payload);
            bone != nullptr && bone->gateMesh != kInvalidIndex && bone->gateMesh >= meshCount) {
            out.error(DiagCode::IndexOutOfRange,
                      "bone '" + node.name + "' is gated by mesh " + number(bone->gateMesh) +
                          " of " + number(meshCount),
                      ElementRef(ElementKind::Node, i));
        }
        if (const auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload)) {
            for (const u32 section : emitter->shapeSections) {
                if (section >= firstSections) {
                    out.warn(DiagCode::IndexOutOfRange,
                             "emitter '" + node.name + "' emits from section " + number(section) +
                                 " of mesh 0, which has " + number(firstSections),
                             ElementRef(ElementKind::Node, i));
                }
            }
        }
    }
}

MeshMergeResult MergeMeshesInto(Model& model, std::span<const u32> meshes, u32 keep) {
    MeshMergeResult result;
    const auto refuse = [&result](const std::string& why) {
        result.diagnostics.error(DiagCode::OperationUnsupported, "merge refused: " + why);
        return std::move(result);
    };

    std::vector<u32> picked(meshes.begin(), meshes.end());
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    if (picked.size() < 2) {
        return refuse("fewer than two meshes");
    }
    for (const u32 m : picked) {
        if (m >= model.meshes.size()) {
            return refuse("mesh " + number(m) + " does not exist");
        }
    }
    if (!std::binary_search(picked.begin(), picked.end(), keep)) {
        return refuse("the mesh kept is not among the meshes merged");
    }
    const Mesh& primary = model.meshes[keep];
    for (const u32 m : picked) {
        const Mesh& mesh = model.meshes[m];
        // A merged geoset has one `lod`: LOD 0 merged into LOD 1 would draw part
        // of the model at the wrong distance.
        if (mesh.lodLevel != primary.lodLevel) {
            return refuse("mesh " + number(m) + " is another level of detail");
        }
        if (mesh.sections.size() > 1) {
            return refuse("mesh " + number(m) + " has " + number(mesh.sections.size()) +
                          " sections");
        }
    }

    // Copies, primary first and the rest in index order: that is the order the
    // merged faces draw in.
    std::vector<u32> order{keep};
    for (const u32 m : picked) {
        if (m != keep) {
            order.push_back(m);
        }
    }
    const bool primaryTangents =
        primary.attributes.has(geom::names::kTangent, geom::Domain::Halfedge);
    std::vector<Mesh> inputs;
    inputs.reserve(order.size());
    for (const u32 m : order) {
        Mesh copy = model.meshes[m];
        // `MergeMeshes` copies the corners only of an input with connectivity,
        // and a mesh read from `.wem` has none: without this its normals and UVs
        // would come out zero, and say nothing.
        if (!copy.ensureConnectivity().ok()) {
            return refuse("mesh " + number(m) + "'s connectivity does not build");
        }
        // A missing UV set is zero and harmless; a missing tangent shades an
        // HD normal map flat. The primary's own are never re-derived.
        if (primaryTangents && !copy.attributes.has(geom::names::kTangent, geom::Domain::Halfedge)) {
            geom::RecomputeTangents(copy, 0);
        }
        if (copy.sections.empty()) {
            copy.sections.emplace_back();
        }
        inputs.push_back(std::move(copy));
    }

    // One geoset binds its vertices one way: where the inputs disagree on a
    // rigid node, every rigid section is baked into the skin first.
    bool rigidAgree = true;
    for (const Mesh& input : inputs) {
        rigidAgree = rigidAgree && input.sections[0].rigidNode == inputs[0].sections[0].rigidNode;
    }
    if (!rigidAgree) {
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].sections[0].rigidNode && !geom::BakeRigidNode(inputs[i], 0)) {
                return refuse("mesh " + number(order[i]) +
                              "'s rigid section shares vertices with another section");
            }
        }
    }

    Mesh merged = geom::MergeMeshes(inputs);
    std::vector<u32> all(merged.sections.size());
    for (u32 s = 0; s < all.size(); ++s) {
        all[s] = s;
    }
    const std::vector<u32> sectionRemap = geom::MergeSections(merged, all, 0);
    if (sectionRemap.empty()) {
        return refuse("the meshes' sections are gated on different nodes");
    }
    // Everything but the faces is the primary's. Its repair log still holds:
    // its vertices and faces come first, in their own order.
    merged.name = primary.name;
    merged.lodLevel = primary.lodLevel;
    merged.repairLog = primary.repairLog;
    for (geom::FaceRecord& record : merged.repairLog.droppedFaces) {
        record.section = 0;
    }
    merged.recomputeBounds();

    // The referencers, while the old numbering still holds.
    const u32 count = static_cast<u32>(model.meshes.size());
    const auto absorbed = [&](u32 m) {
        return m != keep && std::binary_search(picked.begin(), picked.end(), m);
    };
    std::vector<u32> meshRemap(count, kInvalidIndex);
    for (u32 m = 0, next = 0; m < count; ++m) {
        if (!absorbed(m)) {
            meshRemap[m] = next++;
        }
    }
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Section) {
            continue;
        }
        if (absorbed(channel.target.mesh)) {
            // Never dropped: a channel id is never reused, and its sub-tracks
            // stay, joining on nothing — as a removed node's do.
            result.diagnostics.warn(DiagCode::AnimChannelInvalidated,
                                    "channel " + number(channel.id) + " drove mesh " +
                                        number(channel.target.mesh) + ", merged into mesh " +
                                        number(keep),
                                    ElementRef(ElementKind::Channel, channel.id));
            channel.target.mesh = kInvalidIndex;
            ++result.channelsInvalidated;
        } else if (channel.target.mesh == keep) {
            const u32 sub = channel.target.sub;
            channel.target.sub = sub < sectionRemap.size() && sectionRemap[sub] != geom::kInvalidId
                                     ? sectionRemap[sub]
                                     : 0u;
        }
    }
    for (Node& node : model.nodes.nodes) {
        if (auto* bone = std::get_if<BonePayload>(&node.payload);
            bone != nullptr && absorbed(bone->gateMesh)) {
            bone->gateMesh = keep;
            ++result.linksMoved;
        }
        if (auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            emitter != nullptr && keep == 0) {
            for (u32& section : emitter->shapeSections) {
                if (section < sectionRemap.size() && sectionRemap[section] != geom::kInvalidId) {
                    section = sectionRemap[section];
                }
            }
        }
    }

    model.meshes[keep] = std::move(merged);
    for (u32 m = count; m-- > 0;) {
        if (absorbed(m)) {
            model.meshes.erase(model.meshes.begin() + m);
        }
    }
    RemapMeshReferencers(model, meshRemap, result.diagnostics);

    result.ok = true;
    result.merged = meshRemap[keep];
    result.meshRemap = std::move(meshRemap);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
