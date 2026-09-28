// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/optimize.h>

#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/geometry/render_view.h>
#include <whiteout/models/wem/materials/ops.h>
#include <whiteout/models/wem/meshes/remove.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/reflect_bytes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

template <class T>
void appendRaw(std::vector<u8>& out, const T& value) {
    const auto* at = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), at, at + sizeof(T));
}

/// Length-prefixed, so two concatenations cannot run into each other.
void appendBytes(std::vector<u8>& out, const std::vector<u8>& bytes) {
    appendRaw(out, static_cast<u64>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Only an operation's errors are the pass's news: the warnings the removals
/// raise (an influence reassigned, a local recomposed) are what it asked for.
void keepErrors(Diagnostics& out, const Diagnostics& from) {
    for (const Diagnostic& entry : from.all()) {
        if (entry.severity == Severity::Error) {
            out.add(entry.severity, entry.code, entry.message, entry.where, entry.profile);
        }
    }
}

// ============================================================================
// "The same animation" (§3.2)
// ============================================================================

/// What @p channel plays, without saying whose it is: its declaration less the
/// owner, the track sets it belongs to, and in every clip of the model and
/// every container of it the sub-track or its absence.
std::vector<u8> ChannelSignature(const Document& document, u32 model, const AnimChannel& channel) {
    std::vector<u8> out;
    appendRaw(out, static_cast<u8>(channel.target.kind));
    appendRaw(out, static_cast<u32>(channel.target.material.profile));
    appendRaw(out, channel.target.material.look);
    appendRaw(out, channel.target.sub);
    appendRaw(out, static_cast<u8>(channel.target.channel));
    appendRaw(out, static_cast<u8>(channel.valueType));
    appendBytes(out, channel.initValue);
    const Model& owner = document.models[model];
    for (const TrackSet& set : owner.trackSets) {
        out.push_back(set.contains(channel.id) ? 1 : 0);
    }
    for (const Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (const SubTrackContainer& container : clip.containers) {
            const SubTrack* track = container.find(channel.id);
            out.push_back(track != nullptr ? 1 : 0);
            if (track != nullptr) {
                SubTrack anonymous = *track;
                anonymous.channel = 0;
                appendBytes(out, ReflectBytes(anonymous));
            }
        }
    }
    return out;
}

/// The channels @p owns picks, played as one: each one's signature, sorted, so
/// the order the importer declared them in does not matter.
template <class Owns>
std::vector<u8> AnimationSignature(const Document& document, u32 model, Owns&& owns) {
    std::vector<std::vector<u8>> each;
    for (const AnimChannel& channel : document.models[model].animChannels.channels) {
        if (owns(channel.target)) {
            each.push_back(ChannelSignature(document, model, channel));
        }
    }
    std::sort(each.begin(), each.end());
    std::vector<u8> out;
    for (const std::vector<u8>& one : each) {
        appendBytes(out, one);
    }
    return out;
}

template <class Owns>
std::vector<u32> ChannelIds(const Model& model, Owns&& owns) {
    std::vector<u32> ids;
    for (const AnimChannel& channel : model.animChannels.channels) {
        if (owns(channel.target)) {
            ids.push_back(channel.id);
        }
    }
    return ids;
}

/// `EraseChannels`, and the ids out of every track set too: a stale id is
/// ignored there, but a document the pass hands back should not carry one.
void DropChannels(Document& document, u32 model, const std::vector<u32>& ids) {
    if (ids.empty()) {
        return;
    }
    EraseChannels(document, model, ids);
    for (TrackSet& set : document.models[model].trackSets) {
        std::erase_if(set.channels, [&](u32 id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        });
    }
}

// ============================================================================
// Textures and material slots
// ============================================================================

/// Whether every material of the document carries an MDX block or none: the
/// one native kind whose index-addressed referencers §7.4 and §7.5 audited.
/// An `.m3` projection or composite names a material nothing here would move.
bool MaterialsAudited(const Document& document) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            for (const Material& material : set.materials) {
                if (material.nativeKind() != NativeKind::None &&
                    material.nativeKind() != NativeKind::Mdx) {
                    return false;
                }
            }
        }
    }
    return true;
}

void MergeDuplicateTextures(Document& document, OptimizeReport& report) {
    std::vector<std::vector<u8>> bytes;
    bytes.reserve(document.textures.size());
    for (const TextureRef& texture : document.textures) {
        bytes.push_back(ReflectBytes(texture));
    }
    // From the top, so a removal renumbers only what has been looked at.
    for (u32 t = static_cast<u32>(bytes.size()); t-- > 1;) {
        for (u32 keeper = 0; keeper < t; ++keeper) {
            if (bytes[keeper] != bytes[t]) {
                continue;
            }
            const RemovalResult removed = RemoveTexture(document, t, keeper);
            if (!removed.removed) {
                // A native block the texture table has no rows for: every
                // other texture would be refused the same way.
                return;
            }
            bytes.erase(bytes.begin() + t);
            ++report.texturesMerged;
            break;
        }
    }
}

void RemoveUnusedTextures(Document& document, OptimizeReport& report) {
    for (u32 t = static_cast<u32>(document.textures.size()); t-- > 0;) {
        if (CountTextureReferencers(document, t).total() != 0) {
            continue;
        }
        if (!RemoveTexture(document, t).removed) {
            return;
        }
        ++report.texturesRemoved;
    }
}

bool TargetsSlot(const TrackTarget& target, u32 slot) {
    return IsMaterialTarget(target.kind) && target.material.slot == slot;
}

/// What slot @p slot draws: per set, per look, the bound material's bytes;
/// and the material channels on it.
std::vector<u8> SlotSignature(const Document& document, u32 model, u32 slot) {
    const Model& owner = document.models[model];
    std::vector<u8> out;
    for (const ProfileMaterialSet& set : owner.profileSets) {
        appendRaw(out, static_cast<u32>(set.profile));
        if (slot >= set.slotBindings.size()) {
            out.push_back(0);
            continue;
        }
        out.push_back(1);
        const SlotBinding& binding = set.slotBindings[slot];
        appendRaw(out, static_cast<u64>(binding.byLook.size()));
        for (const u32 material : binding.byLook) {
            const bool bound = material < set.materials.size();
            out.push_back(bound ? 1 : 0);
            if (!bound) {
                continue;
            }
            // `fromMdx` names a material for its slot and keeps the HD shader
            // in the block; only a material written from its common layer
            // writes its name, as that shader.
            Material drawn = set.materials[material];
            if (drawn.NativeIsAuthoritative()) {
                drawn.name.clear();
            }
            appendBytes(out, ReflectBytes(drawn));
        }
    }
    appendBytes(out, AnimationSignature(document, model, [slot](const TrackTarget& target) {
                    return TargetsSlot(target, slot);
                }));
    return out;
}

bool SlotUsed(const Model& model, u32 slot) {
    for (const Mesh& mesh : model.meshes) {
        for (const MeshSection& section : mesh.sections) {
            if (section.materialSlot == slot) {
                return true;
            }
        }
    }
    bool linked = false;
    for (const Node& node : model.nodes.nodes) {
        ForEachMaterialLink(node.payload, [&](const u32& link) { linked = linked || link == slot; });
    }
    return linked;
}

void MergeSlots(Document& document, u32 model, OptimizeReport& report) {
    Model& owner = document.models[model];
    std::vector<std::vector<u8>> signatures;
    for (u32 s = 0; s < owner.materialSlots.size(); ++s) {
        signatures.push_back(SlotSignature(document, model, s));
    }
    for (u32 slot = static_cast<u32>(signatures.size()); slot-- > 1;) {
        for (u32 keeper = 0; keeper < slot; ++keeper) {
            if (signatures[keeper] != signatures[slot] || !SlotCanReplace(owner, slot, keeper)) {
                continue;
            }
            // The keeper's twins, so they go rather than be invalidated.
            DropChannels(document, model, ChannelIds(owner, [slot](const TrackTarget& target) {
                             return TargetsSlot(target, slot);
                         }));
            const RemovalResult removed = RemoveSlot(owner, slot, keeper);
            keepErrors(report.diagnostics, removed.diagnostics);
            if (removed.removed) {
                signatures.erase(signatures.begin() + slot);
                ++report.slotsMerged;
            }
            break;
        }
    }
    for (u32 slot = static_cast<u32>(owner.materialSlots.size()); slot-- > 0;) {
        if (SlotUsed(owner, slot)) {
            continue;
        }
        DropChannels(document, model, ChannelIds(owner, [slot](const TrackTarget& target) {
                         return TargetsSlot(target, slot);
                     }));
        const RemovalResult removed = RemoveSlot(owner, slot);
        keepErrors(report.diagnostics, removed.diagnostics);
        if (removed.removed) {
            ++report.slotsRemoved;
        }
    }
}

// ============================================================================
// Nodes (§4)
// ============================================================================

bool Near(f32 a, f32 b, f32 tolerance = 1e-5f) {
    return std::fabs(a - b) <= tolerance;
}

bool IsIdentity(const Transform& transform) {
    const Vector3f& t = transform.translation;
    const Quaternion& q = transform.rotation;
    const Vector3f& s = transform.scale;
    const f32 w = std::fabs(q.w);
    return Near(t.x, 0) && Near(t.y, 0) && Near(t.z, 0) && Near(q.x, 0) && Near(q.y, 0) &&
           Near(q.z, 0) && Near(w, 1) && Near(s.x, 1) && Near(s.y, 1) && Near(s.z, 1);
}

bool SameMatrix(const Matrix44f& a, const Matrix44f& b) {
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            const f32 scale = r == 3 ? 1.0f + std::fabs(a.data[r][c]) : 1.0f;
            if (!Near(a.data[r][c], b.data[r][c], 1e-4f * scale)) {
                return false;
            }
        }
    }
    return true;
}

/// Whether @p channel's rest value is the rest @p node's rig already stands
/// at: the identity on a pivot rig, the node's `local` on an explicit one.
bool RestsAtRest(const AnimChannel& channel, const Node& node, RigConvention rig) {
    if (!channel.hasInitValue()) {
        return true;
    }
    const std::size_t floats = channel.initValue.size() / sizeof(f32);
    f32 v[4] = {0, 0, 0, 0};
    std::memcpy(v, channel.initValue.data(), std::min<std::size_t>(floats, 4) * sizeof(f32));
    const bool pivot = rig == RigConvention::PivotRelative;
    const Transform rest = pivot ? Transform::identity() : node.local;
    switch (channel.target.channel) {
    case Channel::Translation:
        return floats == 3 && Near(v[0], rest.translation.x) && Near(v[1], rest.translation.y) &&
               Near(v[2], rest.translation.z);
    case Channel::Rotation: {
        if (floats != 4) {
            return false;
        }
        const Quaternion& q = rest.rotation;
        const f32 sign = v[0] * q.x + v[1] * q.y + v[2] * q.z + v[3] * q.w < 0 ? -1.0f : 1.0f;
        return Near(sign * v[0], q.x) && Near(sign * v[1], q.y) && Near(sign * v[2], q.z) &&
               Near(sign * v[3], q.w);
    }
    case Channel::Scale:
        if (floats == 1) {
            return Near(v[0], rest.scale.x) && Near(v[0], rest.scale.y) && Near(v[0], rest.scale.z);
        }
        return floats == 3 && Near(v[0], rest.scale.x) && Near(v[1], rest.scale.y) &&
               Near(v[2], rest.scale.z);
    case Channel::Visibility:
        if (channel.valueType == geom::AttrType::Bool) {
            return channel.initValue[0] != 0;
        }
        return floats == 1 && v[0] > 0.5f;
    default:
        // A property a bone or helper has no rest for: say it moves.
        return false;
    }
}

bool HasInheritBits(const Node& node) {
    return hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
           hasFlag(node.flags, NodeFlags::DontInheritRotation) ||
           hasFlag(node.flags, NodeFlags::DontInheritScale) ||
           hasFlag(node.flags, NodeFlags::ModelSpace);
}

u32 GateOf(const Node& node) {
    const auto* bone = std::get_if<BonePayload>(&node.payload);
    return bone != nullptr ? bone->gateMesh : kInvalidIndex;
}

/// What pass 4 needs to know about one model before anything moves.
struct NodeFacts {
    std::vector<u8> keyed;     ///< A channel keys it, or rests it elsewhere.
    std::vector<u8> named;     ///< Something outside the tree holds its index.
    std::vector<u8> engine;    ///< The game resolves it by name or id (§4.3).
    std::vector<std::vector<u32>> channels; ///< Its node channels' ids.
};

NodeFacts FactsOf(const Document& document, u32 model) {
    const Model& owner = document.models[model];
    const NodeTree& tree = owner.nodes;
    const u32 count = tree.size();
    NodeFacts facts;
    facts.keyed.assign(count, 0);
    facts.named.assign(count, 0);
    facts.engine.assign(count, 0);
    facts.channels.assign(count, {});
    const auto name = [&](u32 node) {
        if (node < count) {
            facts.named[node] = 1;
        }
    };

    std::set<u32> keyedChannels;
    for (const Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (const ClipEvent& event : clip.events) {
            name(event.node);
        }
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                if (track.keyCount() != 0) {
                    keyedChannels.insert(track.channel);
                }
            }
        }
    }
    for (const AnimChannel& channel : owner.animChannels.channels) {
        if (channel.target.kind != TrackTarget::Kind::Node || channel.target.node >= count) {
            continue;
        }
        const u32 node = channel.target.node;
        facts.channels[node].push_back(channel.id);
        if (keyedChannels.count(channel.id) != 0 ||
            !RestsAtRest(channel, tree.nodes[node], tree.rig)) {
            facts.keyed[node] = 1;
        }
    }

    for (const PoseStage& stage : owner.poseStages) {
        for (const u32 node : stage.driven) {
            name(node);
        }
        for (const u32 node : stage.targets) {
            name(node);
        }
        for (const StageSource& source : stage.sources) {
            name(source.node);
        }
        name(stage.upNode);
    }
    for (const Node& node : tree.nodes) {
        ForEachNodeLink(node, [&](const u32& link, EmitterLink) { name(link); });
    }
    // A body or a cloth collider rides its node: reducing it away would take
    // the physics along.
    for (const PhysicsBody& body : owner.physics.bodies) {
        name(body.node);
    }
    for (const ClothCollider& collider : owner.physics.colliders) {
        name(collider.node);
    }
    for (const Mesh& mesh : owner.meshes) {
        for (const MeshSection& section : mesh.sections) {
            const i64 gate = section.native.value(kSectionVisibilityNode, -1);
            if (gate >= 0 && gate != kSectionAlwaysDrawn) {
                name(static_cast<u32>(gate));
            }
        }
    }
    for (u32 n = 0; n < count; ++n) {
        const Node& node = tree.nodes[n];
        facts.engine[n] =
            IsWarcraftEngineBoneName(node.name) || node.native.value("keyBoneId", -1) >= 0 ? 1 : 0;
    }
    return facts;
}

bool HoldsWeight(const Model& model, u32 node) {
    for (const Mesh& mesh : model.meshes) {
        for (const geom::Influence& influence : mesh.skin.influences) {
            if (influence.bone == node) {
                return true;
            }
        }
        for (const MeshSection& section : mesh.sections) {
            if (section.rigidNode == node) {
                return true;
            }
        }
    }
    return false;
}

std::vector<u32> LiveChildren(const NodeTree& tree, u32 node) {
    std::vector<u32> out;
    for (const u32 child : tree.children(node)) {
        if (!tree.nodes[child].removed) {
            out.push_back(child);
        }
    }
    return out;
}

/// The nearest bone above @p node that is still in the tree, or `kInvalidNode`.
u32 BoneAbove(const NodeTree& tree, u32 node) {
    u32 walk = tree.nodes[node].parent;
    for (u32 guard = 0; walk < tree.size() && guard <= tree.size(); ++guard) {
        const Node& at = tree.nodes[walk];
        if (!at.removed && at.kind == NodeKind::Bone) {
            return walk;
        }
        walk = at.parent;
    }
    return kInvalidNode;
}

bool Uniform(std::span<const geom::Influence> influences) {
    for (const geom::Influence& influence : influences) {
        if (!Near(influence.weight, influences[0].weight, 1e-4f)) {
            return false;
        }
    }
    return true;
}

/// A classic matrix group averages its bones, so a vertex the file wrote as one
/// must still be one once @p node's weight has moved to @p parent (§4.1).
bool KeepsClassicGroups(const Model& model, u32 node, u32 parent) {
    std::vector<geom::Influence> after;
    for (const Mesh& mesh : model.meshes) {
        for (u32 v = 0; v < mesh.skin.vertexCount(); ++v) {
            const std::span<const geom::Influence> before = mesh.skin.forVertex(v);
            const bool touched = std::any_of(before.begin(), before.end(),
                                             [&](const geom::Influence& i) { return i.bone == node; });
            if (!touched || !Uniform(before)) {
                continue;
            }
            after.clear();
            for (geom::Influence influence : before) {
                if (influence.bone == node) {
                    influence.bone = parent;
                }
                auto same = std::find_if(after.begin(), after.end(), [&](const geom::Influence& i) {
                    return i.bone == influence.bone;
                });
                if (same != after.end()) {
                    same->weight += influence.weight;
                } else {
                    after.push_back(influence);
                }
            }
            if (!Uniform(after)) {
                return false;
            }
        }
    }
    return true;
}

/// Whether @p gate names a mesh the game places its gated bones as a drop
/// shadow for.
bool ShadowGate(const Model& model, u32 gate) {
    if (gate >= model.meshes.size()) {
        return false;
    }
    const std::vector<MeshSection>& sections = model.meshes[gate].sections;
    return std::any_of(sections.begin(), sections.end(), [](const MeshSection& section) {
        return hasFlag(section.flags, SectionFlags::ProjectedShadow);
    });
}

/// @p helper as a bone gated on @p gate. An `.mdx` node states its kind in its
/// flags word as well as by its chunk, and the writer copies the word.
void PromoteToBone(Node& helper, u32 gate) {
    helper.kind = NodeKind::Bone;
    helper.resetPayloadForKind();
    std::get<BonePayload>(helper.payload).gateMesh = gate;
    if (const NativeBag::Entry* bits = helper.native.find("mdxFlagBits")) {
        helper.native.set("mdxFlagBits",
                          bits->value | static_cast<i64>(mdx::Node::NodeFlag::Bone));
    }
}

bool ReduciblePayload(const Node& node) {
    return !node.removed && (node.kind == NodeKind::Bone || node.kind == NodeKind::Helper);
}

/// Pass 4 on one model. Returns old -> new, empty when nothing went.
std::vector<u32> ReduceNodes(Document& document, u32 model, OptimizeReport& report) {
    Model& owner = document.models[model];
    NodeTree& tree = owner.nodes;
    if (tree.empty()) {
        return {};
    }
    const NodeFacts facts = FactsOf(document, model);
    const bool classic = document.carries(ProfileId::Wc3Classic);
    const bool pivot = tree.rig == RigConvention::PivotRelative;

    NodeReferencers referencers;
    referencers.meshes = std::span<Mesh>(owner.meshes);
    referencers.channels = &owner.animChannels;
    referencers.stages = &owner.poseStages;
    referencers.physics = &owner.physics;

    // Parents first, as the roots' subtrees give them: an `.mdx` import is in
    // object-id order, which is not.
    std::vector<u32> order;
    order.reserve(tree.size());
    for (const u32 root : tree.roots()) {
        for (const u32 node : tree.subtree(root)) {
            order.push_back(node);
        }
    }
    const auto untouchable = [&](u32 n) {
        return !ReduciblePayload(tree.nodes[n]) || facts.named[n] != 0 || facts.engine[n] != 0;
    };

    // A model with geometry keeps a bone: an `.mdx` with none is a shape no
    // shipped model has, whatever its vertices bind.
    u32 removed = 0;
    u32 bones = 0;
    for (const Node& node : tree.nodes) {
        bones += node.kind == NodeKind::Bone ? 1u : 0u;
    }
    const auto lastBone = [&](u32 n) {
        return tree.nodes[n].kind == NodeKind::Bone && bones <= 1 && !owner.meshes.empty();
    };
    const auto remove = [&](u32 n, SkinPolicy skin) {
        const bool bone = tree.nodes[n].kind == NodeKind::Bone;
        const RemoveResult result =
            RemoveNode(tree, n, RemovePolicy::ReparentChildren, skin, true, referencers);
        keepErrors(report.diagnostics, result.diagnostics);
        if (result.removed) {
            ++removed;
            bones -= bone ? 1u : 0u;
        }
    };

    // §4.2 dead leaves, leaves first so a chain of them collapses.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const u32 n = *it;
        if (untouchable(n) || lastBone(n) || !LiveChildren(tree, n).empty() ||
            HoldsWeight(owner, n)) {
            continue;
        }
        remove(n, SkinPolicy::Refuse);
    }

    // §4.1 pass-throughs, parents first.
    for (const u32 n : order) {
        const Node& node = tree.nodes[n];
        if (untouchable(n) || facts.keyed[n] != 0 || node.flags != NodeFlags::None) {
            continue;
        }
        const u32 parent = node.parent;
        const bool weighted = HoldsWeight(owner, n);
        if (parent == kInvalidNode && weighted) {
            continue;
        }
        const std::vector<u32> children = LiveChildren(tree, n);
        if (std::any_of(children.begin(), children.end(),
                        [&](u32 c) { return HasInheritBits(tree.nodes[c]); })) {
            continue;
        }
        const u32 gate = GateOf(node);
        if (ShadowGate(owner, gate)) {
            continue;
        }
        if (node.kind == NodeKind::Bone && !children.empty()) {
            const u32 above = BoneAbove(tree, n);
            const u32 inherited = above == kInvalidNode ? kInvalidIndex : GateOf(tree.nodes[above]);
            if (gate != inherited) {
                continue;
            }
        }
        if (!pivot) {
            if (!children.empty() && !IsIdentity(node.local)) {
                continue;
            }
            if (weighted &&
                !SameMatrix(tree.inverseBindMatrix(n) * ToMatrix(node.local),
                            tree.inverseBindMatrix(parent))) {
                continue;
            }
        }
        if (classic && weighted && !KeepsClassicGroups(owner, n, parent)) {
            continue;
        }
        if (lastBone(n)) {
            continue;
        }
        // Weights land on the parent, so a helper parent becomes a bone. It
        // is then the nearest bone of everything under it, so it takes the
        // gate they followed; a drop-shadow gate would also place it, so that
        // one keeps the bone instead.
        if (weighted && tree.nodes[parent].kind == NodeKind::Helper) {
            const u32 above = BoneAbove(tree, parent);
            const u32 inherited = above == kInvalidNode ? kInvalidIndex : GateOf(tree.nodes[above]);
            if (ShadowGate(owner, inherited)) {
                continue;
            }
            PromoteToBone(tree.nodes[parent], inherited);
            ++bones;
            ++report.helpersPromoted;
        }
        remove(n, SkinPolicy::ReassignToParent);
    }
    if (removed == 0) {
        return {};
    }

    // A gone node's channels go with it rather than be left invalidated.
    std::vector<u32> channels;
    for (u32 n = 0; n < tree.size(); ++n) {
        if (tree.nodes[n].removed) {
            channels.insert(channels.end(), facts.channels[n].begin(), facts.channels[n].end());
        }
    }
    DropChannels(document, model, channels);

    Diagnostics compacted;
    NodeRemaps remaps = CompactNodes(tree, referencers, compacted);
    keepErrors(report.diagnostics, compacted);
    // `CompactNodes` takes a clip span, and this model's clips are not one.
    for (Clip& clip : document.clips) {
        if (clip.model != model) {
            continue;
        }
        for (ClipEvent& event : clip.events) {
            if (event.node < remaps.nodes.size()) {
                event.node = remaps.nodes[event.node];
            }
        }
    }
    report.nodesRemoved += removed;
    return std::move(remaps.nodes);
}

// ============================================================================
// Meshes (§3)
// ============================================================================

/// The layers a merge carries by value: every vertex and corner layer but the
/// authoring ones, which no exporter writes.
std::vector<u8> LayoutSignature(const Mesh& mesh) {
    std::vector<std::string> layers;
    for (const geom::AttrLayer& layer : mesh.attributes.layers()) {
        if (layer.domain != geom::Domain::Vertex && layer.domain != geom::Domain::Halfedge) {
            continue;
        }
        if (layer.name.rfind(geom::names::kSelectionPrefix, 0) == 0 ||
            layer.name == geom::names::kMergeGroup || layer.name == geom::names::kSkinLocked ||
            layer.name == geom::names::kModelled) {
            continue;
        }
        layers.push_back(layer.name + '\0' + static_cast<char>(layer.domain) +
                         static_cast<char>(layer.type));
    }
    std::sort(layers.begin(), layers.end());
    std::vector<u8> out;
    for (const std::string& layer : layers) {
        appendBytes(out, std::vector<u8>(layer.begin(), layer.end()));
    }
    return out;
}

/// The profiles a section is drawn in, of the ones the document declares.
std::vector<ProfileId> DrawnIn(const Document& document, const MeshSection& section) {
    std::vector<ProfileId> out;
    for (const ProfileId profile : document.profiles) {
        if (HasProfile(section.profiles, profile)) {
            out.push_back(profile);
        }
    }
    return out;
}

/// No material it can draw with reads the scene (§3.3).
bool OrderFree(const Document& document, const Model& model, const MeshSection& section) {
    for (const ProfileId profile : DrawnIn(document, section)) {
        const ProfileMaterialSet* set = model.setFor(profile);
        const u32 looks = set != nullptr ? static_cast<u32>(set->looks.size()) : 0u;
        for (u32 look = 0; look < std::max(looks, 1u); ++look) {
            const Material* material = Resolve(model, section.materialSlot, profile, look);
            if (material == nullptr) {
                continue;
            }
            const BlendMode blend = material->Common().blend;
            if (blend != BlendMode::Opaque && blend != BlendMode::AlphaKey &&
                blend != BlendMode::Transparent) {
                return false;
            }
        }
    }
    return true;
}

std::vector<u8> MergeKey(const Document& document, u32 model, u32 mesh) {
    const Mesh& source = document.models[model].meshes[mesh];
    const MeshSection& section = source.sections[0];
    std::vector<u8> out;
    appendRaw(out, source.lodLevel);
    appendRaw(out, section.materialSlot);
    appendRaw(out, section.profiles);
    out.push_back(section.rigidNode.has_value() ? 1 : 0);
    appendRaw(out, section.rigidNode.value_or(0));
    appendRaw(out, section.selectionGroup);
    appendRaw(out, static_cast<u32>(section.flags));
    appendBytes(out, ReflectBytes(section.native));
    out.push_back(source.skin.empty() ? 1 : 0);
    appendBytes(out, LayoutSignature(source));
    appendBytes(out, AnimationSignature(document, model, [mesh](const TrackTarget& target) {
                    return target.kind == TrackTarget::Kind::Section && target.mesh == mesh;
                }));
    return out;
}

std::set<u32> BonesOf(const Mesh& mesh) {
    std::set<u32> bones;
    for (const geom::Influence& influence : mesh.skin.influences) {
        if (influence.weight > 0.0f) {
            bones.insert(influence.bone);
        }
    }
    if (mesh.sections[0].rigidNode) {
        bones.insert(*mesh.sections[0].rigidNode);
    }
    return bones;
}

/// The limits of every profile @p section is drawn in (§3.4), for the greedy
/// batching; a Warcraft III batch is asked of the converter after.
struct MergeLimits {
    u32 vertices = ~0u;
    u32 bones = ~0u;
};

MergeLimits LimitsOf(const Document& document, const MeshSection& section) {
    MergeLimits limits;
    for (const ProfileId profile : DrawnIn(document, section)) {
        const ProfileDesc& desc = Profile(profile);
        if (desc.indexWidth == IndexWidth::U16) {
            limits.vertices = std::min(limits.vertices, 0xFFFFu);
        }
        if (desc.maxBonesPerPalette != 0) {
            limits.bones = std::min(limits.bones, desc.maxBonesPerPalette);
        }
        if (GameOf(profile) == Game::Warcraft) {
            limits.bones = std::min(limits.bones, 256u);
        }
    }
    return limits;
}

/// What `toMdx` would say of @p merged, for each Warcraft III profile it is
/// drawn in: a vertex count or a palette it could not write.
bool WarcraftWrites(const Document& document, u32 model, const Mesh& merged) {
    const MdxConverter converter;
    for (const ProfileId profile : DrawnIn(document, merged.sections[0])) {
        if (GameOf(profile) != Game::Warcraft) {
            continue;
        }
        const Diagnostics said =
            converter.checkGeoset(document, model, merged, profile, MdxFileVersion(profile));
        if (said.countOf(DiagCode::IndexWidthExceeded) != 0 ||
            said.countOf(DiagCode::BonePaletteLimit) != 0) {
            return false;
        }
    }
    return true;
}

/// Each mesh's faces by their corner positions, bit for bit and in no order,
/// built when first asked for.
///
/// Opaque geosets draw in mesh order, and a merge moves an absorbed mesh up to
/// the keeper's place: ahead of every mesh between them. Under the depth test
/// that changes nothing, except where two faces lie exactly on each other -- an
/// overlay made by duplicating faces -- and the one drawn last wins.
class FaceKeys {
public:
    explicit FaceKeys(const Model& model) : model_(model), keys_(model.meshes.size()) {}

    /// Whether moving mesh @p absorbed up to @p batch's keeper would take it
    /// past a mesh that has one of its faces. The batch's own members move
    /// with it, in their order.
    bool overlaysBetween(const std::vector<u32>& batch, u32 absorbed) {
        for (u32 between = batch[0] + 1; between < absorbed; ++between) {
            if (std::find(batch.begin(), batch.end(), between) != batch.end()) {
                continue;
            }
            const std::set<Key>& theirs = of(between);
            if (theirs.empty()) {
                continue;
            }
            for (const Key& key : of(absorbed)) {
                if (theirs.count(key) != 0) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    using Key = std::vector<u32>;

    const std::set<Key>& of(u32 mesh) {
        if (!keys_[mesh]) {
            std::set<Key>& out = keys_[mesh].emplace();
            const Mesh& source = model_.meshes[mesh];
            const geom::FaceSet& set = source.faceSet();
            const auto positions =
                source.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            std::size_t corner = 0;
            for (const u32 valence : set.faceValence) {
                std::vector<std::array<u32, 3>> corners;
                for (u32 i = 0; i < valence; ++i, ++corner) {
                    const u32 vertex = set.cornerVertex[corner];
                    const Vector3f p = vertex < positions.size() ? positions[vertex] : Vector3f{};
                    corners.push_back({std::bit_cast<u32>(p.x), std::bit_cast<u32>(p.y),
                                       std::bit_cast<u32>(p.z)});
                }
                std::sort(corners.begin(), corners.end());
                Key key;
                for (const auto& c : corners) {
                    key.insert(key.end(), c.begin(), c.end());
                }
                out.insert(std::move(key));
            }
        }
        return *keys_[mesh];
    }

    const Model& model_;
    std::vector<std::optional<std::set<Key>>> keys_;
};

void RemapBatch(std::vector<u32>& batch, std::span<const u32> meshRemap) {
    for (u32& mesh : batch) {
        mesh = mesh < meshRemap.size() ? meshRemap[mesh] : kInvalidIndex;
    }
}

/// Applies a mesh renumbering to the origins list.
void RemapOrigins(std::vector<std::vector<u32>>& origins, std::span<const u32> meshRemap) {
    std::vector<std::vector<u32>> next;
    for (u32 m = 0; m < origins.size() && m < meshRemap.size(); ++m) {
        if (meshRemap[m] != kInvalidIndex) {
            if (next.size() <= meshRemap[m]) {
                next.resize(meshRemap[m] + 1);
            }
            next[meshRemap[m]] = std::move(origins[m]);
        }
    }
    origins = std::move(next);
}

void RemoveEmptyMeshes(Document& document, u32 model, std::vector<std::vector<u32>>& origins,
                       OptimizeReport& report) {
    Model& owner = document.models[model];
    std::vector<u8> drop(owner.meshes.size(), 0);
    bool any = false;
    for (u32 m = 0; m < owner.meshes.size(); ++m) {
        drop[m] = owner.meshes[m].faceCount() == 0 ? 1 : 0;
    }
    // A geoset nothing draws still gates the bones linked to it.
    for (const Node& node : owner.nodes.nodes) {
        if (GateOf(node) < drop.size()) {
            drop[GateOf(node)] = 0;
        }
        if (const auto* emitter = std::get_if<Sc2ParticleEmitterPayload>(&node.payload);
            emitter != nullptr && !emitter->shapeSections.empty() && !drop.empty()) {
            drop[0] = 0;
        }
    }
    for (const u8 d : drop) {
        any = any || d != 0;
    }
    if (!any) {
        return;
    }
    std::vector<u32> meshRemap(owner.meshes.size(), kInvalidIndex);
    for (u32 m = 0, next = 0; m < owner.meshes.size(); ++m) {
        if (drop[m] == 0) {
            meshRemap[m] = next++;
        }
    }
    Diagnostics removed;
    report.meshesRemoved += RemoveMeshes(document, model, drop, removed);
    keepErrors(report.diagnostics, removed);
    RemapOrigins(origins, meshRemap);
}

void MergeMeshes(Document& document, u32 model, const OptimizeOptions& options,
                 std::vector<std::vector<u32>>& origins, OptimizeReport& report) {
    Model& owner = document.models[model];

    // Each mergeable mesh by its key, in mesh order.
    std::map<std::vector<u8>, std::vector<u32>> groups;
    for (u32 m = 0; m < owner.meshes.size(); ++m) {
        const Mesh& mesh = owner.meshes[m];
        if (mesh.sections.size() != 1 || mesh.faceCount() == 0) {
            continue;
        }
        if (!options.mergeBlended && !OrderFree(document, owner, mesh.sections[0])) {
            continue;
        }
        groups[MergeKey(document, model, m)].push_back(m);
    }

    // First-fit batches: the first mesh left keeps, and every later one joins
    // it that the limits and the draw order allow; the rest wait for the next
    // round. Counted by a standard render view.
    FaceKeys faces(owner);
    std::vector<std::vector<u32>> batches;
    for (const auto& [key, members] : groups) {
        const MergeLimits limits = LimitsOf(document, owner.meshes[members[0]].sections[0]);
        std::map<u32, u64> counts;
        for (const u32 m : members) {
            counts[m] = geom::BuildRenderMesh(owner.meshes[m], geom::RenderMeshDesc::Standard())
                            .vertexCount();
        }
        std::vector<u32> pending = members;
        while (pending.size() > 1) {
            std::vector<u32> batch{pending[0]};
            u64 vertices = counts[pending[0]];
            std::set<u32> bones = BonesOf(owner.meshes[pending[0]]);
            std::vector<u32> waiting;
            for (std::size_t i = 1; i < pending.size(); ++i) {
                const u32 m = pending[i];
                std::set<u32> joined = bones;
                const std::set<u32> own = BonesOf(owner.meshes[m]);
                joined.insert(own.begin(), own.end());
                if (vertices + counts[m] > limits.vertices || joined.size() > limits.bones ||
                    faces.overlaysBetween(batch, m)) {
                    waiting.push_back(m);
                    continue;
                }
                batch.push_back(m);
                vertices += counts[m];
                bones = std::move(joined);
            }
            if (batch.size() > 1) {
                batches.push_back(std::move(batch));
            }
            pending = std::move(waiting);
        }
    }

    for (std::size_t b = 0; b < batches.size(); ++b) {
        std::vector<u32> batch = batches[b];
        // Shrunk from the end until the converter would write it whole.
        std::optional<MergedMesh> built;
        while (batch.size() > 1) {
            Diagnostics refused;
            built = MergedMeshOf(owner, batch, batch[0], refused);
            if (built && WarcraftWrites(document, model, built->mesh)) {
                break;
            }
            built.reset();
            batch.pop_back();
        }
        if (!built) {
            continue;
        }
        // The absorbed meshes' geoset animation is the keeper's twin.
        std::vector<u32> absorbedChannels;
        for (std::size_t i = 1; i < batch.size(); ++i) {
            const u32 mesh = batch[i];
            const std::vector<u32> ids = ChannelIds(owner, [mesh](const TrackTarget& target) {
                return target.kind == TrackTarget::Kind::Section && target.mesh == mesh;
            });
            absorbedChannels.insert(absorbedChannels.end(), ids.begin(), ids.end());
        }
        DropChannels(document, model, absorbedChannels);

        const u32 keep = batch[0];
        std::vector<u32> joined = origins[keep];
        std::vector<u32> absorbed(batch.begin() + 1, batch.end());
        std::sort(absorbed.begin(), absorbed.end());
        for (const u32 mesh : absorbed) {
            joined.insert(joined.end(), origins[mesh].begin(), origins[mesh].end());
        }
        const MeshMergeResult merged = MergeMeshesInto(owner, batch, keep, std::move(*built));
        keepErrors(report.diagnostics, merged.diagnostics);
        if (!merged.ok) {
            continue;
        }
        origins[keep] = std::move(joined);
        RemapOrigins(origins, merged.meshRemap);
        for (std::size_t later = b + 1; later < batches.size(); ++later) {
            RemapBatch(batches[later], merged.meshRemap);
        }
        report.meshesMerged += static_cast<u32>(batch.size() - 1);
    }
}

bool ReducesNodes(const Document& document, const OptimizeOptions& options) {
    return options.reduceNodes &&
           (options.reduceNodesOfEveryGame || GameOf(document.defaultProfile) == Game::Warcraft);
}

} // namespace

bool IsWarcraftEngineBoneName(const std::string& name) {
    // `LookupBoneId`'s tokenizer separators and tokens, matched without case
    // as `TknzFindTokenId` does.
    static constexpr const char* kTokens[] = {"bone_head", "bone_chest", "bone_foot", "bone_hand",
                                              "bone_turret"};
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find_first_of(" \t\r\n\",;", start);
        if (end == std::string::npos) {
            end = name.size();
        }
        if (end > start) {
            std::string token = name.substr(start, end - start);
            std::transform(token.begin(), token.end(), token.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char* known : kTokens) {
                if (token == known) {
                    return true;
                }
            }
        }
        start = end + 1;
    }
    return false;
}

OptimizeReport OptimizeDocument(Document& document, const OptimizeOptions& options) {
    OptimizeReport report;
    report.models.resize(document.models.size());

    // Keys first: a channel left playing its rest makes a node inert and two
    // geosets or materials alike (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md §3).
    for (u32 m = 0; options.reduceKeys && m < document.models.size(); ++m) {
        const ExactKeyReport keys = ReduceKeysExactly(document, m);
        report.keysRemoved += keys.keysRemoved;
        report.subTracksDropped += keys.subTracksDropped;
        report.tracksCollapsed += keys.tracksCollapsed;
    }

    if (options.mergeMaterials && MaterialsAudited(document)) {
        // Textures first, so two materials that named twin textures compare
        // equal; the unused ones last, since a slot that went can leave one.
        MergeDuplicateTextures(document, report);
        for (u32 m = 0; m < document.models.size(); ++m) {
            MergeSlots(document, m, report);
        }
        RemoveUnusedTextures(document, report);
    }

    for (u32 m = 0; m < document.models.size(); ++m) {
        ModelOptimizeReport& model = report.models[m];
        if (ReducesNodes(document, options)) {
            model.nodeRemap = ReduceNodes(document, m, report);
        }
        if (!options.mergeMeshes) {
            continue;
        }
        std::vector<std::vector<u32>> origins(document.models[m].meshes.size());
        for (u32 i = 0; i < origins.size(); ++i) {
            origins[i] = {i};
        }
        const u32 before = report.meshesMerged + report.meshesRemoved;
        RemoveEmptyMeshes(document, m, origins, report);
        MergeMeshes(document, m, options, origins, report);
        if (report.meshesMerged + report.meshesRemoved != before) {
            model.meshOrigins = std::move(origins);
        }
    }

    if (report.changed()) {
        report.diagnostics.info(DiagCode::Unspecified,
                                "optimized: " + number(report.meshesMerged) + " mesh(es) merged, " +
                                    number(report.meshesRemoved) + " empty, " +
                                    number(report.nodesRemoved) + " node(s) removed, " +
                                    number(report.slotsMerged + report.slotsRemoved) +
                                    " material slot(s) and " +
                                    number(report.texturesMerged + report.texturesRemoved) +
                                    " texture(s) merged or dropped, " +
                                    number(report.keysRemoved) + " key(s) removed");
    }
    return report;
}

} // namespace wem
} // namespace models
} // namespace whiteout
