// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/game_minimum.h>

#include <whiteout/models/wem/anim/track_read.h>

#include <span>
#include <string>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// The root bone a model with none is given, named as both games' rigs name theirs.
constexpr const char* kRootBoneName = "Bone_Root";

/// Whether @p node is one a file of @p profile writes.
bool Written(const NodeTree& tree, u32 node, ProfileId profile) {
    return node < tree.size() && !tree.nodes[node].removed && HasProfile(tree.nodes[node].profiles, profile);
}

/// Whether every section of @p mesh binds its vertices through `rigidNode`,
/// which a converter reads in place of the skin array.
bool AllRigid(const Mesh& mesh, const NodeTree& tree, ProfileId profile) {
    if (mesh.sections.empty()) {
        return false;
    }
    for (const MeshSection& section : mesh.sections) {
        if (!section.rigidNode || !Written(tree, *section.rigidNode, profile)) {
            return false;
        }
    }
    return true;
}

/// The first node @p influences bind that the file writes: the heaviest, as a
/// vertex's influences are sorted.
u32 FirstWritten(std::span<const geom::Influence> influences, const NodeTree& tree, ProfileId profile) {
    for (const geom::Influence& influence : influences) {
        if (influence.weight > 0.0f && Written(tree, influence.bone, profile)) {
            return influence.bone;
        }
    }
    return kInvalidNode;
}

/// @p mesh's vertices that bind no node the file writes, ascending.
std::vector<u32> Unbound(const Mesh& mesh, const NodeTree& tree, ProfileId profile) {
    std::vector<u32> out;
    if (AllRigid(mesh, tree, profile)) {
        return out;
    }
    const u32 count = mesh.vertexCount();
    for (u32 v = 0; v < count; ++v) {
        if (v >= mesh.skin.vertexCount() || FirstWritten(mesh.skin.forVertex(v), tree, profile) == kInvalidNode) {
            out.push_back(v);
        }
    }
    return out;
}

bool HasClip(const Document& document, u32 model) {
    for (const Clip& clip : document.clips) {
        if (clip.model == model && !IsGlobalLoop(clip)) {
            return true;
        }
    }
    return false;
}

std::string UniqueName(const NodeTree& tree, const std::string& stem) {
    const auto taken = [&tree](const std::string& name) {
        for (const Node& node : tree.nodes) {
            if (node.name == name) {
                return true;
            }
        }
        return false;
    };
    std::string name = stem;
    for (u32 n = 2; taken(name); ++n) {
        name = stem + "_" + std::to_string(n);
    }
    return name;
}

} // namespace

bool NeedsGameMinimum(const Document& document, ProfileId profile, u32 model) {
    if (model >= document.models.size()) {
        return false;
    }
    if (!HasClip(document, model)) {
        return true;
    }
    const Model& target = document.models[model];
    for (const Mesh& mesh : target.meshes) {
        if (!Unbound(mesh, target.nodes, profile).empty()) {
            return true;
        }
    }
    return false;
}

GameMinimumReport AddGameMinimum(Document& document, ProfileId profile, Diagnostics& out, u32 model) {
    GameMinimumReport report;
    if (model >= document.models.size()) {
        return report;
    }
    Model& target = document.models[model];
    NodeTree& tree = target.nodes;

    u32 firstBone = kInvalidNode;
    for (u32 n = 0; n < tree.size() && firstBone == kInvalidNode; ++n) {
        if (tree.nodes[n].kind == NodeKind::Bone && Written(tree, n, profile)) {
            firstBone = n;
        }
    }
    u32 meshesBound = 0;
    for (Mesh& mesh : target.meshes) {
        const std::vector<u32> unbound = Unbound(mesh, tree, profile);
        if (unbound.empty()) {
            continue;
        }
        u32 bone = kInvalidNode;
        for (u32 v = 0; v < mesh.skin.vertexCount() && bone == kInvalidNode; ++v) {
            bone = FirstWritten(mesh.skin.forVertex(v), tree, profile);
        }
        if (bone == kInvalidNode) {
            if (firstBone == kInvalidNode) {
                Node root;
                root.kind = NodeKind::Bone;
                root.name = UniqueName(tree, kRootBoneName);
                root.resetPayloadForKind();
                firstBone = tree.add(std::move(root));
                // Its rows of every pose the tree carries, the identity at the
                // origin, which is where the root stands.
                tree.conformPoses();
                report.boneAdded = firstBone;
            }
            bone = firstBone;
        }
        // Rebuilt in vertex order rather than spliced a vertex at a time: an
        // imported static mesh is every vertex at once.
        const geom::Influence whole{bone, 1.0f};
        geom::SkinBinding rebuilt;
        std::size_t next = 0;
        for (u32 v = 0; v < mesh.vertexCount(); ++v) {
            if (next < unbound.size() && unbound[next] == v) {
                rebuilt.appendVertex(std::span<const geom::Influence>(&whole, 1));
                ++next;
            } else {
                rebuilt.appendVertex(mesh.skin.forVertex(v));
            }
        }
        mesh.skin = std::move(rebuilt);
        report.verticesBound += static_cast<u32>(unbound.size());
        ++meshesBound;
    }
    if (report.boneAdded != kInvalidNode) {
        out.info(DiagCode::Unspecified,
                 "root bone '" + tree.nodes[report.boneAdded].name +
                     "' added: the model had no bone to bind its meshes to",
                 ElementRef(ElementKind::Node, report.boneAdded), profile);
    }
    if (report.verticesBound != 0) {
        out.info(DiagCode::Unspecified,
                 std::to_string(report.verticesBound) + " vertices of " + std::to_string(meshesBound) +
                     " mesh(es) bound to no bone; each is bound whole to one",
                 ElementRef(ElementKind::Document, 0), profile);
    }

    if (!HasClip(document, model)) {
        Clip clip;
        clip.name = kGameMinimumClip;
        clip.model = model;
        clip.duration = kGameMinimumClipSeconds;
        clip.looping = true;
        clip.readRule = RuleOf(GameOf(profile));
        clip.containers.emplace_back();
        document.clips.push_back(std::move(clip));
        report.clipAdded = true;
        out.info(DiagCode::Unspecified,
                 std::string("'") + kGameMinimumClip + "' added: the model had no animation, and the game plays one at rest",
                 ElementRef(ElementKind::Clip, static_cast<u32>(document.clips.size() - 1)), profile);
    }
    return report;
}

} // namespace wem
} // namespace models
} // namespace whiteout
