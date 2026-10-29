// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/meshes/remove.h>

#include <whiteout/models/wem/geometry/kernel.h>
#include <whiteout/models/wem/nodes/emitters.h>
#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <variant>

#include "remove_common.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using meshdetail::number;

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
    // A cloth whose cage mesh is gone goes, and a binding on a gone mesh.
    const std::vector<u32> clothsGone = RemapPhysicsMeshes(model.physics, meshRemap);
    if (!clothsGone.empty()) {
        out.warn(DiagCode::ClothTopologyInvalid,
                 number(static_cast<u32>(clothsGone.size())) + " cloths lost their cage mesh",
                 ElementRef());
        InvalidatePhysicsChannels(model.animChannels, clothsGone, out);
        DetachPoseStages(model.poseStages, clothsGone);
    }
    // A soft body left with no mesh goes, and its channels with it.
    const std::vector<u32> softGone = RemapSoftMeshes(model.physics, meshRemap);
    if (!softGone.empty()) {
        out.warn(DiagCode::PhysicsReferenceInvalid,
                 number(static_cast<u32>(softGone.size())) + " soft bodies lost their meshes",
                 ElementRef());
        InvalidatePhysicsChannels(model.animChannels, softGone, out);
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
        // Each section follows its mesh, and one of a mesh that is gone goes.
        if (auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            emitter != nullptr && !emitter->shapeSections.empty()) {
            const std::size_t before = emitter->shapeSections.size();
            std::erase_if(emitter->shapeSections, [&](u32 entry) {
                return remapped(meshRemap, Sc2ShapeSectionMesh(entry)) == kInvalidIndex;
            });
            for (u32& entry : emitter->shapeSections) {
                entry = Sc2ShapeSection(remapped(meshRemap, Sc2ShapeSectionMesh(entry)), Sc2ShapeSectionIndex(entry));
            }
            if (emitter->shapeSections.size() != before) {
                out.warn(DiagCode::IndexOutOfRange,
                         "emitter '" + node.name + "' emitted from " +
                             number(before - emitter->shapeSections.size()) +
                             " section(s) of a mesh that is gone; they are dropped",
                         ElementRef(ElementKind::Node, i));
            }
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
            for (const u32 entry : emitter->shapeSections) {
                const u32 mesh = Sc2ShapeSectionMesh(entry);
                const u32 sections = mesh < meshCount ? static_cast<u32>(model.meshes[mesh].sections.size()) : 0u;
                if (Sc2ShapeSectionIndex(entry) >= sections) {
                    out.warn(DiagCode::IndexOutOfRange,
                             "emitter '" + node.name + "' emits from section " +
                                 number(Sc2ShapeSectionIndex(entry)) + " of mesh " + number(mesh) +
                                 ", which has " + number(sections),
                             ElementRef(ElementKind::Node, i));
                }
            }
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
