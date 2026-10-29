// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/meshes/remove.h>

#include <whiteout/models/wem/geometry/kernel.h>
#include <whiteout/models/wem/geometry/merge.h>
#include <whiteout/models/wem/nodes/emitters.h>
#include <whiteout/models/wem/physics/references.h>

#include <algorithm>
#include <optional>
#include <string>

#include "remove_common.h"

namespace whiteout {
namespace models {
namespace wem {

using meshdetail::number;

std::optional<MergedMesh> MergedMeshOf(const Model& model, std::span<const u32> meshes, u32 keep,
                                       Diagnostics& out) {
    const auto refuse = [&out](const std::string& why) -> std::optional<MergedMesh> {
        out.error(DiagCode::OperationUnsupported, "merge refused: " + why);
        return std::nullopt;
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
        // A Drip's strands are a mesh its effect builds again and takes away.
        for (const SoftBody& body : model.physics.soft) {
            if (body.drips == m) {
                return refuse("mesh " + number(m) + " is the strands of soft body '" + body.name + "'");
            }
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
    return MergedMesh{std::move(merged), sectionRemap};
}

MeshMergeResult MergeMeshesInto(Model& model, std::span<const u32> meshes, u32 keep) {
    MeshMergeResult result;
    std::optional<MergedMesh> built = MergedMeshOf(model, meshes, keep, result.diagnostics);
    if (!built) {
        return result;
    }
    return MergeMeshesInto(model, meshes, keep, std::move(*built));
}

MeshMergeResult MergeMeshesInto(Model& model, std::span<const u32> meshes, u32 keep,
                                MergedMesh built) {
    MeshMergeResult result;
    std::vector<u32> picked(meshes.begin(), meshes.end());
    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    const std::vector<u32>& sectionRemap = built.sectionRemap;

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
    // Where each input's sections start among the merged mesh's, in merge order.
    std::vector<u32> sectionBase(count, kInvalidIndex);
    {
        u32 base = 0;
        for (const u32 m : picked) {
            if (m == keep) {
                sectionBase[m] = base;
                base += static_cast<u32>(model.meshes[m].sections.size());
            }
        }
        for (const u32 m : picked) {
            if (m != keep) {
                sectionBase[m] = base;
                base += static_cast<u32>(model.meshes[m].sections.size());
            }
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
        if (auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload)) {
            std::vector<u32> sections;
            for (u32 entry : emitter->shapeSections) {
                const u32 mesh = Sc2ShapeSectionMesh(entry);
                if (mesh < count && sectionBase[mesh] != kInvalidIndex) {
                    const u32 at = sectionBase[mesh] + Sc2ShapeSectionIndex(entry);
                    entry = Sc2ShapeSection(keep, at < sectionRemap.size() && sectionRemap[at] != geom::kInvalidId
                                                      ? sectionRemap[at]
                                                      : 0u);
                }
                if (std::find(sections.begin(), sections.end(), entry) == sections.end()) {
                    sections.push_back(entry);
                }
            }
            emitter->shapeSections = std::move(sections);
        }
    }

    // A cloth's cage and bindings are sections of their own, and every section
    // was just folded into the first: with two or more, none is anything a
    // cloth can name, and the cloth goes.
    const std::vector<u32> alone =
        sectionRemap.size() > 1 ? std::vector<u32>(sectionRemap.size(), kInvalidIndex) : sectionRemap;
    const std::vector<u32> clothsGone = RemapPhysicsSections(model.physics, keep, alone);
    if (!clothsGone.empty()) {
        result.diagnostics.warn(DiagCode::ClothTopologyInvalid,
                                number(static_cast<u32>(clothsGone.size())) +
                                    " cloths had their cage merged into another section",
                                ElementRef(ElementKind::Mesh, keep));
        InvalidatePhysicsChannels(model.animChannels, clothsGone, result.diagnostics);
        DetachPoseStages(model.poseStages, clothsGone);
    }

    // A soft body's points went into the kept mesh with their layers, the
    // faces of each absorbed mesh after those before it: it follows them.
    {
        std::vector<u8> merged(count, 0);
        std::vector<u32> faceBase(count, 0);
        u32 faces = static_cast<u32>(model.meshes[keep].faceCount());
        merged[keep] = 1;
        for (const u32 m : picked) {
            if (m != keep) {
                merged[m] = 1;
                faceBase[m] = faces;
                faces += static_cast<u32>(model.meshes[m].faceCount());
            }
        }
        if (const u32 letGo = SoftFollowMergedMeshes(model.physics, merged, keep, faceBase); letGo != 0) {
            result.diagnostics.warn(DiagCode::PhysicsReferenceInvalid,
                                    number(letGo) + " parts no longer ride a soft body rigidly: their mesh "
                                                    "was merged with another",
                                    ElementRef(ElementKind::Mesh, keep));
        }
    }

    model.meshes[keep] = std::move(built.mesh);
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
