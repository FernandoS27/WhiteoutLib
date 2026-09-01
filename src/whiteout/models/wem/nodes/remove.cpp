// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/nodes/remove.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// The nearest ancestor of @p node that is not itself being removed, or
/// `kInvalidNode` when the whole chain goes.
u32 survivingAncestor(const NodeTree& tree, u32 node, const std::vector<u8>& doomed) {
    u32 walk = node < tree.size() ? tree.nodes[node].parent : kInvalidNode;
    u32 guard = 0;
    while (walk != kInvalidNode && walk < tree.size()) {
        if (doomed[walk] == 0 && !tree.nodes[walk].removed) {
            return walk;
        }
        walk = tree.nodes[walk].parent;
        if (++guard > tree.size()) {
            break;
        }
    }
    return kInvalidNode;
}

/// Rewrites every influence naming a doomed bone to @p replacement, then merges
/// the duplicates a rewrite can create. Returns how many influences moved.
u32 reassignInfluences(geom::SkinBinding& skin, const std::vector<u8>& doomed,
                       const std::vector<u32>& replacement) {
    if (skin.empty()) {
        return 0;
    }
    u32 moved = 0;
    std::vector<u32> newOffsets;
    newOffsets.reserve(skin.offsets.size());
    std::vector<geom::Influence> rebuilt;
    rebuilt.reserve(skin.influences.size());
    std::vector<geom::Influence> merged;

    newOffsets.push_back(0);
    for (u32 v = 0; v + 1 < skin.offsets.size(); ++v) {
        merged.clear();
        for (u32 i = skin.offsets[v]; i < skin.offsets[v + 1]; ++i) {
            geom::Influence influence = skin.influences[i];
            if (influence.bone < doomed.size() && doomed[influence.bone] != 0) {
                influence.bone = replacement[influence.bone];
                ++moved;
                if (influence.bone == kInvalidNode) {
                    // The whole chain went; the influence has nowhere to land and
                    // dropping it is better than pointing it at node 0.
                    continue;
                }
            }
            bool combined = false;
            for (geom::Influence& existing : merged) {
                if (existing.bone == influence.bone) {
                    existing.weight += influence.weight;
                    combined = true;
                    break;
                }
            }
            if (!combined) {
                merged.push_back(influence);
            }
        }
        rebuilt.insert(rebuilt.end(), merged.begin(), merged.end());
        newOffsets.push_back(static_cast<u32>(rebuilt.size()));
    }

    skin.offsets = std::move(newOffsets);
    skin.influences = std::move(rebuilt);
    skin.sortByWeight();
    return moved;
}

std::string number(u64 value) {
    return std::to_string(value);
}

} // namespace

bool IsNodeRemoved(const NodeTree& tree, u32 node) {
    return node < tree.size() && tree.nodes[node].removed;
}

RemoveResult RemoveNode(NodeTree& tree, u32 node, RemovePolicy policy, SkinPolicy skinPolicy,
                        bool preserveWorld, NodeReferencers referencers) {
    RemoveResult result;
    if (node >= tree.size() || tree.nodes[node].removed) {
        result.diagnostics.error(DiagCode::DanglingNodeReference, "no such node: " + number(node),
                                 ElementRef(ElementKind::Node, node));
        return result;
    }

    // --- which nodes go ---------------------------------------------------------
    std::vector<u8> doomed(tree.size(), 0);
    if (policy == RemovePolicy::RemoveSubtree) {
        for (u32 member : tree.subtree(node)) {
            doomed[member] = 1;
            result.removedNodes.push_back(member);
        }
    } else {
        doomed[node] = 1;
        result.removedNodes.push_back(node);
    }
    std::sort(result.removedNodes.begin(), result.removedNodes.end());

    // Where a doomed bone's influences would go.
    std::vector<u32> replacement(tree.size(), kInvalidNode);
    for (u32 member : result.removedNodes) {
        replacement[member] = survivingAncestor(tree, member, doomed);
    }

    // --- the skin is the one hard case ------------------------------------------
    std::vector<u32> affectedMeshes;
    for (u32 m = 0; m < referencers.meshes.size(); ++m) {
        const Mesh& mesh = referencers.meshes[m];
        bool touched = false;
        for (const geom::Influence& influence : mesh.skin.influences) {
            if (influence.bone < doomed.size() && doomed[influence.bone] != 0) {
                touched = true;
                break;
            }
        }
        if (!touched) {
            for (const MeshSection& section : mesh.sections) {
                if (section.rigidNode.has_value() && *section.rigidNode < doomed.size() &&
                    doomed[*section.rigidNode] != 0) {
                    touched = true;
                    break;
                }
            }
        }
        if (touched) {
            affectedMeshes.push_back(m);
        }
    }

    if (!affectedMeshes.empty() && skinPolicy == SkinPolicy::Refuse) {
        for (u32 m : affectedMeshes) {
            result.diagnostics.error(DiagCode::NodeRemovalRefused,
                                     "mesh '" + referencers.meshes[m].name + "' binds to node " +
                                         number(node),
                                     ElementRef(ElementKind::Mesh, m));
        }
        return result;
    }

    for (u32 m : affectedMeshes) {
        Mesh& mesh = referencers.meshes[m];
        const u32 moved = reassignInfluences(mesh.skin, doomed, replacement);
        for (MeshSection& section : mesh.sections) {
            if (section.rigidNode.has_value() && *section.rigidNode < doomed.size() &&
                doomed[*section.rigidNode] != 0) {
                const u32 target = replacement[*section.rigidNode];
                if (target == kInvalidNode) {
                    section.rigidNode.reset();
                } else {
                    section.rigidNode = target;
                }
            }
        }
        // One diagnostic per mesh, not per influence: "412 influences moved on
        // this mesh" is the row a UI wants, and the count is in the message.
        result.diagnostics.warn(DiagCode::SkinInfluenceReassigned,
                                number(moved) + " influences reassigned on mesh '" + mesh.name +
                                    "'",
                                ElementRef(ElementKind::Mesh, m));
    }

    // --- reparent the survivors ---------------------------------------------------
    if (policy == RemovePolicy::ReparentChildren) {
        // Snapshot the children and their world binds before anything moves; a
        // recomposition against a half-updated tree would be against the wrong
        // parent.
        const NodeRange childRange = tree.children(node);
        const std::vector<u32> childList(childRange.begin(), childRange.end());
        std::vector<Transform> worldBefore;
        worldBefore.reserve(childList.size());
        for (u32 child : childList) {
            worldBefore.push_back(tree.worldBind(child));
        }

        const u32 newParent = tree.nodes[node].parent;
        for (u32 child : childList) {
            tree.nodes[child].parent = newParent;
        }
        tree.invalidateHierarchy();

        if (preserveWorld) {
            const Transform parentWorld =
                newParent == kInvalidNode ? Transform::identity() : tree.worldBind(newParent);
            const Transform parentInverse = Inverse(parentWorld);
            for (std::size_t i = 0; i < childList.size(); ++i) {
                Node& child = tree.nodes[childList[i]];
                child.local = Compose(parentInverse, worldBefore[i]);
                // Parent-relative pose values move with the local; model-space
                // ones are already right, which is one more reason D3's A/B poses
                // are stored as shipped.
                for (std::size_t p = 0; p < child.poses.size() && p < tree.poseSchema.size(); ++p) {
                    if (tree.poseSchema[p].space == PoseSpace::ParentRelative) {
                        child.poses[p] =
                            tree.poseSchema[p].inverse ? Inverse(child.local) : child.local;
                    }
                }
                result.diagnostics.info(DiagCode::BindPoseRecomposed,
                                        "local transform recomposed against the new parent",
                                        ElementRef(ElementKind::Node, childList[i]));
            }
        }
    }

    for (u32 member : result.removedNodes) {
        tree.nodes[member].removed = true;
    }
    tree.invalidateHierarchy();
    result.removed = true;
    return result;
}

// ============================================================================

NodeRemaps CompactNodes(NodeTree& tree, NodeReferencers referencers, Diagnostics& out) {
    NodeRemaps remaps;
    remaps.nodes.assign(tree.size(), kInvalidNode);
    for (u32 i = 0; i < tree.size(); ++i) {
        if (!tree.nodes[i].removed) {
            remaps.nodes[i] = remaps.newCount++;
        }
    }
    if (remaps.newCount == tree.size()) {
        return remaps; // Nothing to do.
    }

    std::vector<Node> survivors;
    survivors.reserve(remaps.newCount);
    for (u32 i = 0; i < tree.size(); ++i) {
        if (remaps.nodes[i] == kInvalidNode) {
            continue;
        }
        Node moved = std::move(tree.nodes[i]);
        // Walk up to the first surviving ancestor. `RemoveNode` already
        // reparented or removed the descendants, so this is a safety net, not the
        // normal path.
        u32 parent = moved.parent;
        u32 guard = 0;
        while (parent != kInvalidNode && parent < remaps.nodes.size() &&
               remaps.nodes[parent] == kInvalidNode) {
            parent = tree.nodes[parent].parent;
            if (++guard > tree.size()) {
                parent = kInvalidNode;
                break;
            }
        }
        moved.parent = parent == kInvalidNode ? kInvalidNode : remaps.nodes[parent];
        survivors.push_back(std::move(moved));
    }
    tree.nodes = std::move(survivors);
    tree.invalidateHierarchy();

    // --- the §10.6 referencer table ------------------------------------------------
    //
    // One block per row. A later phase adds a block, not a mechanism.

    for (u32 m = 0; m < referencers.meshes.size(); ++m) {
        Mesh& mesh = referencers.meshes[m];

        // SkinBinding::Influence::bone
        u32 dangling = 0;
        for (geom::Influence& influence : mesh.skin.influences) {
            if (influence.bone >= remaps.nodes.size()) {
                ++dangling;
                continue;
            }
            const u32 fresh = remaps.nodes[influence.bone];
            if (fresh == kInvalidNode) {
                ++dangling;
            } else {
                influence.bone = fresh;
            }
        }
        if (dangling != 0) {
            out.error(DiagCode::DanglingNodeReference,
                      number(dangling) + " influences on mesh '" + mesh.name +
                          "' name a node that no longer exists",
                      ElementRef(ElementKind::Mesh, m));
        }

        // MeshSection::rigidNode
        for (u32 s = 0; s < mesh.sections.size(); ++s) {
            MeshSection& section = mesh.sections[s];
            if (!section.rigidNode.has_value()) {
                continue;
            }
            const u32 old = *section.rigidNode;
            const u32 fresh = old < remaps.nodes.size() ? remaps.nodes[old] : kInvalidNode;
            if (fresh == kInvalidNode) {
                out.error(DiagCode::DanglingNodeReference,
                          "rigidNode names a node that no longer exists",
                          ElementRef(ElementKind::Section, s));
                section.rigidNode.reset();
            } else {
                section.rigidNode = fresh;
            }
        }
    }

    // AnimChannel::target.node
    if (referencers.channels != nullptr) {
        u32 dangling = 0;
        for (AnimChannel& channel : referencers.channels->channels) {
            if (channel.target.kind != TrackTarget::Kind::Node ||
                channel.target.node == kInvalidNode) {
                continue;
            }
            const u32 old = channel.target.node;
            const u32 fresh = old < remaps.nodes.size() ? remaps.nodes[old] : kInvalidNode;
            channel.target.node = fresh;
            if (fresh == kInvalidNode) {
                ++dangling;
            }
        }
        if (dangling != 0) {
            out.warn(DiagCode::AnimChannelInvalidated,
                     number(dangling) + " channels named a node that no longer exists",
                     ElementRef());
        }
    }

    // ClipEvent::node
    for (u32 c = 0; c < referencers.clips.size(); ++c) {
        Clip& clip = referencers.clips[c];
        u32 dangling = 0;
        for (ClipEvent& event : clip.events) {
            if (event.node == kInvalidNode) {
                continue;
            }
            const u32 fresh =
                event.node < remaps.nodes.size() ? remaps.nodes[event.node] : kInvalidNode;
            event.node = fresh;
            if (fresh == kInvalidNode) {
                ++dangling;
            }
        }
        if (dangling != 0) {
            out.error(DiagCode::DanglingNodeReference,
                      number(dangling) + " events in clip '" + clip.name +
                          "' fire at a node that no longer exists",
                      ElementRef(ElementKind::Clip, c));
        }
    }

    return remaps;
}

void CheckNodeReferencers(const NodeTree& tree, std::span<const Mesh> meshes, Diagnostics& out,
                          const AnimChannelTable* channels, std::span<const Clip> clips) {
    const u32 count = tree.size();
    for (u32 m = 0; m < meshes.size(); ++m) {
        const Mesh& mesh = meshes[m];

        u32 outOfRange = 0;
        u32 notABone = 0;
        for (const geom::Influence& influence : mesh.skin.influences) {
            if (influence.bone >= count) {
                ++outOfRange;
            } else if (tree.nodes[influence.bone].kind != NodeKind::Bone) {
                // §5.6: influences must land on Bone nodes.
                ++notABone;
            }
        }
        if (outOfRange != 0) {
            out.error(DiagCode::DanglingNodeReference,
                      number(outOfRange) + " influences name a node outside the tree",
                      ElementRef(ElementKind::Mesh, m));
        }
        if (notABone != 0) {
            out.error(DiagCode::DanglingNodeReference,
                      number(notABone) + " influences name a node that is not a bone",
                      ElementRef(ElementKind::Mesh, m));
        }

        for (u32 s = 0; s < mesh.sections.size(); ++s) {
            const MeshSection& section = mesh.sections[s];
            if (section.rigidNode.has_value() && *section.rigidNode >= count) {
                out.error(DiagCode::DanglingNodeReference,
                          "rigidNode names a node outside the tree",
                          ElementRef(ElementKind::Section, s));
            }
        }
    }

    if (channels != nullptr) {
        for (const AnimChannel& channel : channels->channels) {
            if (channel.target.kind != TrackTarget::Kind::Node) {
                continue;
            }
            if (channel.target.node >= count) {
                out.error(DiagCode::DanglingNodeReference,
                          "channel " + number(channel.id) + " names a node outside the tree",
                          ElementRef(ElementKind::Channel, channel.id));
            }
        }
    }

    for (u32 c = 0; c < clips.size(); ++c) {
        for (const ClipEvent& event : clips[c].events) {
            if (event.node != kInvalidNode && event.node >= count) {
                out.error(DiagCode::DanglingNodeReference,
                          "event '" + event.name + "' fires at a node outside the tree",
                          ElementRef(ElementKind::Clip, c));
            }
        }
    }
}

} // namespace wem
} // namespace models
} // namespace whiteout
