// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// Shared fixtures for the P3b material tests. One valid document, built the way
/// a converter would build it, so each test can break exactly one thing and see
/// exactly one diagnostic.

#include <string>
#include <vector>

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/geometry/builder.h>
#include <whiteout/models/wem/model.h>

namespace wemfix {

using namespace whiteout;
using namespace whiteout::models::wem;

/// One triangle per section, each with its own three vertices, section *i*
/// bound to material slot *i*.
inline Mesh makeMesh(const std::vector<std::string>& sectionNames) {
    geom::MeshBuilder builder;
    for (std::size_t i = 0; i < sectionNames.size(); ++i) {
        MeshSection section;
        section.name = sectionNames[i];
        section.materialSlot = static_cast<u32>(i);
        builder.addSection(section);
    }
    for (std::size_t i = 0; i < sectionNames.size(); ++i) {
        const f32 offset = static_cast<f32>(i) * 4.0f;
        const geom::VertexId a = builder.addVertex(Vector3f{offset, 0, 0});
        const geom::VertexId b = builder.addVertex(Vector3f{offset + 1, 0, 0});
        const geom::VertexId c = builder.addVertex(Vector3f{offset, 1, 0});
        builder.addTriangle(a, b, c, static_cast<u32>(i));
    }
    auto outcome = builder.build();
    outcome.mesh.name = "mesh";
    return std::move(outcome.mesh);
}

/// A texture input naming @p texture, with a distinguishable constant so a
/// carried-across input can be told from a default-constructed one.
inline TextureInput makeInput(u32 texture, f32 tint = 0.5f) {
    TextureInput input;
    input.texture = texture;
    input.constant = Vector4f{tint, tint, tint, 1.0f};
    return input;
}

/// A composite material: layer 0 sets Color, then @p extraColorLayers modulate
/// onto it, then one Normal layer.
inline Material makeComposite(const std::string& name, u32 extraColorLayers = 0) {
    Material material;
    material.name = name;
    CompositeBody body;
    CompositeLayer base;
    base.input = makeInput(0);
    base.target = SurfaceChannel::Color;
    base.op = CompositeOp::Set;
    body.layers.push_back(base);
    for (u32 i = 0; i < extraColorLayers; ++i) {
        CompositeLayer extra;
        extra.input = makeInput(1 + i);
        extra.target = SurfaceChannel::Color;
        extra.op = CompositeOp::Modulate;
        body.layers.push_back(extra);
    }
    CompositeLayer normal;
    normal.input = makeInput(9);
    normal.target = SurfaceChannel::Normal;
    normal.op = CompositeOp::Set;
    body.layers.push_back(normal);
    material.InitCommon().body = std::move(body);
    return material;
}

/// The set a converter produces: one look, one material per slot, in order.
inline ProfileMaterialSet makeSet(ProfileId profile, std::vector<Material> materials) {
    ProfileMaterialSet set;
    set.profile = profile;
    set.looks = LookTable::Single();
    set.materials = std::move(materials);
    set.slotBindings.resize(set.materials.size());
    for (std::size_t i = 0; i < set.slotBindings.size(); ++i) {
        set.slotBindings[i].byLook.assign(1, static_cast<u32>(i));
    }
    return set;
}

/// Two slots, one mesh with two sections, one Sc2 material set covering both.
/// Structurally and profile-valid as it stands.
inline Model makeModel(ProfileId profile = ProfileId::Sc2) {
    Model model;
    model.name = "model";
    model.materialSlots = {"body", "trim"};
    model.meshes.push_back(makeMesh({"body", "trim"}));
    for (MeshSection& section : model.meshes[0].sections) {
        section.profiles = ProfileBit(profile);
    }
    std::vector<Material> materials;
    materials.push_back(makeComposite("bodyMat"));
    materials.push_back(makeComposite("trimMat"));
    model.profileSets.push_back(makeSet(profile, std::move(materials)));
    return model;
}

inline Document makeDocument(ProfileId profile = ProfileId::Sc2) {
    Document document;
    document.name = "doc";
    document.declare(profile);
    document.defaultProfile = profile;
    document.models.push_back(makeModel(profile));
    for (u32 i = 0; i < 10; ++i) {
        TextureRef texture;
        texture.key = TexturePath{"tex" + std::to_string(i) + ".dds"};
        texture.path = "tex" + std::to_string(i) + ".dds";
        document.textures.push_back(std::move(texture));
    }
    return document;
}

/// The diagnostics histogram as one line, so a failure names what broke.
inline std::string codes(const Diagnostics& diagnostics) {
    std::string out;
    for (const Diagnostics::CodeCount& row : diagnostics.histogram()) {
        if (!out.empty()) {
            out += " ";
        }
        out += ToString(row.code);
        out += "x";
        out += std::to_string(row.count);
    }
    return out;
}

/// Errors only — the half that means "invalid", as opposed to "lossy".
inline std::string errorCodes(const Diagnostics& diagnostics) {
    Diagnostics errors;
    for (const Diagnostic& entry : diagnostics.bySeverity(Severity::Error)) {
        errors.add(entry.severity, entry.code, entry.message, entry.where, entry.profile);
    }
    return codes(errors);
}

} // namespace wemfix
