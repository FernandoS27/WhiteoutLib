// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/validate.h>

#include <string>

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/geometry/checks.h>
#include <whiteout/models/wem/materials/ops.h>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

// ---------------------------------------------------------------------------
// Structural
// ---------------------------------------------------------------------------

/// The mesh half, delegated to `checks.h` — the thing they check lives there.
/// Meshes are numbered across the whole document, since `ElementRef` addresses a
/// mesh and a document with several models is the rare case.
void checkMeshStructure(const Document& document, Diagnostics& out) {
    u32 meshIndex = 0;
    for (const Model& model : document.models) {
        for (const Mesh& mesh : model.meshes) {
            geom::CheckStructural(mesh, meshIndex++, out);
        }
    }
}

/// §6.3's last paragraph: a set for an undeclared profile is a structural error,
/// as is a `defaultProfile` outside the declared list.
void checkProfileDeclarations(const Document& document, Diagnostics& out) {
    if (!document.profiles.empty() && !document.carries(document.defaultProfile)) {
        out.error(DiagCode::ProfileNotCarried,
                  std::string("defaultProfile is ") + ToString(document.defaultProfile) +
                      ", which the document does not declare",
                  ElementRef(ElementKind::Document, 0), document.defaultProfile);
    }

    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const Model& model = document.models[m];
        ProfileMask seen = kNoProfiles;
        for (const ProfileMaterialSet& set : model.profileSets) {
            if (!document.carries(set.profile)) {
                out.error(DiagCode::ProfileNotCarried,
                          std::string("model carries a ") + ToString(set.profile) +
                              " material set the document does not declare",
                          ElementRef(ElementKind::Document, static_cast<u32>(m)), set.profile);
            }
            if (HasProfile(seen, set.profile)) {
                out.error(DiagCode::ProfileNotCarried,
                          std::string("a second ") + ToString(set.profile) + " material set",
                          ElementRef(ElementKind::Document, static_cast<u32>(m)), set.profile);
            }
            seen |= ProfileBit(set.profile);
        }
    }
}

/// The two parallel-array invariants: one binding per slot, one entry per look.
/// A `byLook` mismatch is a parse bug, not content variation (§8).
void checkBindingShape(const Document& document, Diagnostics& out) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            if (set.looks.empty()) {
                out.error(DiagCode::LookBindingMalformed,
                          "material set carries no looks; every set has at least one", ElementRef(),
                          set.profile);
            }
            if (set.slotBindings.size() != model.materialSlots.size()) {
                out.error(DiagCode::IndexOutOfRange,
                          "material set holds " + number(set.slotBindings.size()) +
                              " slot bindings for " + number(model.materialSlots.size()) + " slots",
                          ElementRef(), set.profile);
            }
            for (std::size_t slot = 0; slot < set.slotBindings.size(); ++slot) {
                const std::vector<u32>& byLook = set.slotBindings[slot].byLook;
                if (byLook.size() == set.looks.size()) {
                    continue;
                }
                out.error(DiagCode::LookBindingMalformed,
                          "binding holds " + number(byLook.size()) + " entries for " +
                              number(set.looks.size()) + " looks",
                          ElementRef(ElementKind::Slot, static_cast<u32>(slot)), set.profile);
            }
            // Only when there is a table to be out of range of: an empty one is
            // already reported above, and saying it twice buries the first.
            if (!set.looks.empty() && set.defaultLook >= set.looks.size()) {
                out.error(DiagCode::LookBindingMalformed,
                          "default look " + number(set.defaultLook) + " is past the " +
                              number(set.looks.size()) + " this set carries",
                          ElementRef(), set.profile);
            }
        }
    }
}

/// A model an attach point rides has to exist (§10.2).
///
/// The child model index lives on the Attachment node's own payload rather than
/// in a side table, which is what keeps `RemoveNode` from needing a referencer
/// row for it — but the index still points *out* of the node, so it is exactly
/// as checkable as any other cross-array reference.
void checkAttachments(const Document& document, Diagnostics& out) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const NodeTree& tree = document.models[m].nodes;
        for (std::size_t n = 0; n < tree.nodes.size(); ++n) {
            const auto* payload = std::get_if<AttachmentPayload>(&tree.nodes[n].payload);
            if (payload == nullptr || payload->model == kInvalidIndex) {
                continue;
            }
            if (payload->model >= document.models.size()) {
                out.error(DiagCode::IndexOutOfRange,
                          "attach point rides model " + number(payload->model) + " of " +
                              number(document.models.size()),
                          ElementRef(ElementKind::Node, static_cast<u32>(n)));
            } else if (payload->model == m) {
                out.error(DiagCode::IndexOutOfRange, "attach point rides its own model",
                          ElementRef(ElementKind::Node, static_cast<u32>(n)));
            }
        }
    }
}

/// §10.8's own invariants: unique channel ids, sub-tracks that resolve, and a
/// key stream sized for the type its channel declares.
///
/// The size rule is the one worth having. `SubTrack::values` is a byte blob and
/// its element size comes from the *channel*, so a converter that keys three
/// floats into a channel declared `F32` produces a stream nothing can read and
/// nothing else would notice.
void checkAnimation(const Document& document, Diagnostics& out) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const AnimChannelTable& table = document.models[m].animChannels;
        for (std::size_t i = 0; i < table.channels.size(); ++i) {
            for (std::size_t j = i + 1; j < table.channels.size(); ++j) {
                if (table.channels[i].id != table.channels[j].id) {
                    continue;
                }
                out.error(DiagCode::MaterialBodyInvalid,
                          "two channels share id " + number(table.channels[j].id),
                          ElementRef(ElementKind::Channel, table.channels[j].id));
            }
        }
        for (const AnimChannel& channel : table.channels) {
            if (channel.target.kind == TrackTarget::Kind::Section) {
                const Model& model = document.models[m];
                const bool inRange =
                    channel.target.mesh < model.meshes.size() &&
                    channel.target.sub < model.meshes[channel.target.mesh].sections.size();
                if (!inRange) {
                    out.error(DiagCode::IndexOutOfRange,
                              "channel " + number(channel.id) + " drives section " +
                                  number(channel.target.sub) + " of mesh " +
                                  number(channel.target.mesh) + ", which the model does not have",
                              ElementRef(ElementKind::Channel, channel.id));
                }
            }
            if (channel.initValue.empty() || channel.hasInitValue()) {
                continue;
            }
            out.error(DiagCode::AttributeCountMismatch,
                      "channel " + number(channel.id) + " holds a " +
                          number(channel.initValue.size()) + "-byte rest value for a " +
                          ToString(channel.valueType) + " channel",
                      ElementRef(ElementKind::Channel, channel.id));
        }
    }

    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        const ElementRef where(ElementKind::Clip, static_cast<u32>(c));

        if (clip.model >= document.models.size()) {
            out.error(DiagCode::ClipTargetMissing,
                      "clip '" + clip.name + "' drives model " + number(clip.model) + " of " +
                          number(document.models.size()),
                      where);
            continue;
        }
        if (clip.containers.empty()) {
            out.warn(DiagCode::ClipTargetMissing, "clip '" + clip.name + "' holds no containers",
                     where);
        }

        const AnimChannelTable& table = document.models[clip.model].animChannels;
        for (std::size_t k = 0; k < clip.containers.size(); ++k) {
            for (const SubTrack& track : clip.containers[k].subTracks) {
                const AnimChannel* channel = table.find(track.channel);
                if (channel == nullptr) {
                    out.error(
                        DiagCode::AnimChannelInvalidated,
                        "a sub-track joins on channel " + number(track.channel) + ", which model " +
                            number(clip.model) + " does not declare",
                        ElementRef(ElementKind::Track, static_cast<u32>(c), static_cast<u32>(k)));
                    continue;
                }
                if (track.wellSized(channel->valueType)) {
                    continue;
                }
                out.error(DiagCode::AttributeCountMismatch,
                          "a sub-track holds " + number(track.values.size()) + " bytes for " +
                              number(track.keyCount()) + " " + ToString(track.interp) +
                              " keys of " + ToString(channel->valueType),
                          ElementRef(ElementKind::Track, static_cast<u32>(c), static_cast<u32>(k)));
            }
        }
    }

    for (std::size_t a = 0; a < document.animSets.size(); ++a) {
        const AnimSet& set = document.animSets[a];
        const ElementRef where(ElementKind::Clip, static_cast<u32>(a));
        if (set.baseAnimSet != kInvalidIndex && set.baseAnimSet >= document.animSets.size()) {
            out.error(DiagCode::ClipTargetMissing,
                      "anim set '" + set.name + "' falls back to set " + number(set.baseAnimSet) +
                          " of " + number(document.animSets.size()),
                      where);
        }
        for (const AnimTag& tag : set.byTag) {
            if (tag.clip < document.clips.size()) {
                continue;
            }
            out.error(DiagCode::ClipTargetMissing,
                      "anim set '" + set.name + "' maps tag " + number(tag.tagId) + " to clip " +
                          number(tag.clip) + " of " + number(document.clips.size()),
                      where);
        }
    }

    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const u32 set = document.models[m].animSet;
        if (set == kInvalidIndex || set < document.animSets.size()) {
            continue;
        }
        out.error(DiagCode::ClipTargetMissing,
                  "model plays anim set " + number(set) + " of " + number(document.animSets.size()),
                  ElementRef(ElementKind::Document, static_cast<u32>(m)));
    }
}

/// §7.2's body invariants: at most one entry per deferred slot, `Orm` exclusive
/// with the unpacked three, at most one feature of a kind per layer.
void checkMaterialBodies(const Document& document, Diagnostics& out) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            for (std::size_t m = 0; m < set.materials.size(); ++m) {
                const CommonMaterial& common = set.materials[m].Common();
                const ElementRef where(ElementKind::Material, static_cast<u32>(m));

                if (const LegacyDeferredBody* legacy = common.legacy()) {
                    bool seen[static_cast<std::size_t>(LegacySlot::Count)] = {};
                    for (const auto& entry : legacy->slots) {
                        const std::size_t slot = static_cast<std::size_t>(entry.first);
                        if (slot >= static_cast<std::size_t>(LegacySlot::Count) || seen[slot]) {
                            out.error(DiagCode::MaterialBodyInvalid,
                                      std::string("legacy slot '") + ToString(entry.first) +
                                          "' appears twice",
                                      where, set.profile);
                            continue;
                        }
                        seen[slot] = true;
                    }
                }

                if (const PbrDeferredBody* pbr = common.pbr()) {
                    bool seen[static_cast<std::size_t>(PbrSlot::Count)] = {};
                    for (const auto& entry : pbr->slots) {
                        const std::size_t slot = static_cast<std::size_t>(entry.first);
                        if (slot >= static_cast<std::size_t>(PbrSlot::Count) || seen[slot]) {
                            out.error(DiagCode::MaterialBodyInvalid,
                                      std::string("pbr slot '") + ToString(entry.first) +
                                          "' appears twice",
                                      where, set.profile);
                            continue;
                        }
                        seen[slot] = true;
                    }
                    // Packed and unpacked are alternatives, not composable: a body
                    // carrying both says two different things about roughness.
                    const bool unpacked = seen[static_cast<std::size_t>(PbrSlot::Metallic)] ||
                                          seen[static_cast<std::size_t>(PbrSlot::Roughness)] ||
                                          seen[static_cast<std::size_t>(PbrSlot::AmbientOcclusion)];
                    if (seen[static_cast<std::size_t>(PbrSlot::Orm)] && unpacked) {
                        out.error(DiagCode::MaterialBodyInvalid,
                                  "pbr body carries Orm and the unpacked metallic/roughness/ao",
                                  where, set.profile);
                    }
                }

                for (std::size_t i = 0; i < common.features.size(); ++i) {
                    for (std::size_t j = i + 1; j < common.features.size(); ++j) {
                        const MaterialFeature& a = common.features[i];
                        const MaterialFeature& b = common.features[j];
                        const ElementRef feature(ElementKind::Feature, static_cast<u32>(m), b.id);

                        // Ids are the join key animation sub-tracks use (§10.8.1),
                        // so a repeat silently retargets one of them.
                        if (a.id == b.id) {
                            out.error(DiagCode::MaterialBodyInvalid,
                                      "two features share id " + number(b.id), feature,
                                      set.profile);
                        }
                        if (a.kind() == b.kind() && a.layer == b.layer) {
                            out.error(DiagCode::MaterialBodyInvalid,
                                      std::string("two ") + ToString(a.kind()) +
                                          " features on the same layer",
                                      feature, set.profile);
                        }
                    }
                }
            }
        }
    }
}

/// §6.5's third site: a native block whose kind disagrees with its set's profile.
void checkNativeKinds(const Document& document, Diagnostics& out) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            if (static_cast<u32>(set.profile) >= static_cast<u32>(ProfileId::Count)) {
                continue;
            }
            const NativeKind allowed = Profile(set.profile).nativeMaterialKind;
            for (std::size_t m = 0; m < set.materials.size(); ++m) {
                const NativeKind actual = set.materials[m].nativeKind();
                if (actual == NativeKind::None || actual == allowed) {
                    continue;
                }
                out.error(DiagCode::NativeKindProfileMismatch,
                          std::string("a ") + ToString(actual) + " native block in a " +
                              ToString(set.profile) + " set, which takes " + ToString(allowed),
                          ElementRef(ElementKind::Material, static_cast<u32>(m)), set.profile);
            }
        }
    }
}

/// The §10.6 node row animation adds: a channel's target and an event's node.
///
/// Profile-level rather than structural because it is the same class of check as
/// the coverage rule — a cross-array join whose failure means the document is
/// wrong about itself, not that it cannot be read.
void checkAnimReferencers(const Document& document, Diagnostics& out) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        const Model& model = document.models[m];
        const u32 nodeCount = model.nodes.size();

        for (const AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind != TrackTarget::Kind::Node) {
                continue;
            }
            if (channel.target.node >= nodeCount) {
                out.error(DiagCode::DanglingNodeReference,
                          "channel " + number(channel.id) + " drives node " +
                              number(channel.target.node) + " of " + number(nodeCount),
                          ElementRef(ElementKind::Channel, channel.id));
            } else if (model.nodes.nodes[channel.target.node].removed) {
                out.warn(DiagCode::AnimChannelInvalidated,
                         "channel " + number(channel.id) + " drives a removed node",
                         ElementRef(ElementKind::Channel, channel.id));
            }
        }
    }

    for (std::size_t c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model >= document.models.size()) {
            continue; // Reported by the structural rule.
        }
        const NodeTree& tree = document.models[clip.model].nodes;
        for (const ClipEvent& event : clip.events) {
            if (event.node == kInvalidNode) {
                continue;
            }
            // Only that the node exists: the *kind* is not an invariant. MDX and
            // `.m2` key events at a dedicated `Event` node, but `.m3` keys them at
            // a bone and D3 at an attachment, so requiring one kind would flag
            // half of shipped content for having the shape its format has.
            if (event.node >= tree.size()) {
                out.error(DiagCode::DanglingNodeReference,
                          "event '" + event.name + "' fires at node " + number(event.node) +
                              " of " + number(tree.size()),
                          ElementRef(ElementKind::Clip, static_cast<u32>(c)));
            }
        }
    }
}

/// The §7.5 referencer table, per model.
void checkMaterialReferencers(const Document& document, Diagnostics& out) {
    for (std::size_t m = 0; m < document.models.size(); ++m) {
        CheckMaterialReferencers(document.models[m], static_cast<u32>(m), out);
    }
}

// ---------------------------------------------------------------------------
// Manifold
// ---------------------------------------------------------------------------

void checkMeshManifold(const Document& document, Diagnostics& out) {
    u32 meshIndex = 0;
    for (const Model& model : document.models) {
        for (const Mesh& mesh : model.meshes) {
            geom::CheckManifold(mesh, meshIndex++, out);
        }
    }
}

// ---------------------------------------------------------------------------
// Profile
// ---------------------------------------------------------------------------

/// §6.3's coverage rule. A hole is an error, not a fallback — falling back
/// silently is how a character ships with one untextured shoulder.
void checkCoverage(const Document& document, Diagnostics& out) {
    for (std::size_t modelIndex = 0; modelIndex < document.models.size(); ++modelIndex) {
        const Model& model = document.models[modelIndex];

        std::vector<bool> slotUsed(model.materialSlots.size(), false);

        for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const Mesh& mesh = model.meshes[meshIndex];
            for (std::size_t s = 0; s < mesh.sections.size(); ++s) {
                const MeshSection& section = mesh.sections[s];
                const ElementRef where(ElementKind::Section, static_cast<u32>(s),
                                       static_cast<u32>(meshIndex));

                if (section.profiles == kNoProfiles) {
                    out.warn(DiagCode::SectionUndrawn,
                             "section '" + section.name + "' is drawn by no profile", where);
                    continue;
                }
                if (section.materialSlot < slotUsed.size()) {
                    slotUsed[section.materialSlot] = true;
                }

                for (u32 p = 0; p < static_cast<u32>(ProfileId::Count); ++p) {
                    const ProfileId profile = static_cast<ProfileId>(p);
                    if (!HasProfile(section.profiles, profile) || !document.carries(profile)) {
                        continue;
                    }
                    const ProfileMaterialSet* set = model.setFor(profile);
                    if (set == nullptr) {
                        out.error(DiagCode::ProfileCoverageIncomplete,
                                  std::string("section '") + section.name + "' draws in " +
                                      ToString(profile) + ", which has no material set",
                                  where, profile);
                        continue;
                    }
                    if (section.materialSlot >= set->slotBindings.size()) {
                        continue; // Already reported by the shape and referencer rules.
                    }
                    const SlotBinding& binding = set->slotBindings[section.materialSlot];
                    for (u32 look = 0; look < binding.byLook.size(); ++look) {
                        if (binding.bound(look)) {
                            continue;
                        }
                        out.error(
                            DiagCode::SlotNotBound,
                            std::string("slot '") + model.materialSlots[section.materialSlot] +
                                "' look " + number(look) + " is unbound in " + ToString(profile),
                            ElementRef(ElementKind::Slot, section.materialSlot, look), profile);
                    }
                }
            }
        }

        for (std::size_t slot = 0; slot < slotUsed.size(); ++slot) {
            if (slotUsed[slot]) {
                continue;
            }
            out.info(DiagCode::MaterialSlotUnused,
                     "slot '" + model.materialSlots[slot] + "' is bound by no section",
                     ElementRef(ElementKind::Slot, static_cast<u32>(slot)));
        }
    }
}

/// The material half of §5.7's third group: what a profile's exporter accepts.
void checkMaterialLimits(const Document& document, Diagnostics& out) {
    for (const Model& model : document.models) {
        for (const ProfileMaterialSet& set : model.profileSets) {
            if (static_cast<u32>(set.profile) >= static_cast<u32>(ProfileId::Count)) {
                continue;
            }
            const ProfileDesc& desc = Profile(set.profile);
            for (std::size_t m = 0; m < set.materials.size(); ++m) {
                const CommonMaterial& common = set.materials[m].Common();
                const ElementRef where(ElementKind::Material, static_cast<u32>(m));

                if (!HasMaterialKind(desc.commonKinds, common.kind())) {
                    out.error(DiagCode::UnsupportedMaterialKind,
                              std::string(ToString(set.profile)) + " does not accept a " +
                                  ToString(common.kind()) + " material",
                              where, set.profile);
                }
                if (!desc.acceptsBlendMode(common.blend)) {
                    out.warn(DiagCode::LossyBlendMode,
                             std::string(ToString(set.profile)) + " cannot write blend mode '" +
                                 ToString(common.blend) + "'",
                             where, set.profile);
                }
            }
        }
    }
}

/// The geometry half. A mesh is shared, so it must satisfy every profile that
/// draws any of its sections.
///
/// `maxBonesPerPalette` is deliberately absent: a palette is built by the
/// exporter (§5.6 — WEM never holds one), so the check belongs where the
/// partitioning happens rather than here, where it would have to invent one.
void checkGeometryLimits(const Document& document, Diagnostics& out) {
    u32 meshIndex = 0;
    for (const Model& model : document.models) {
        for (const Mesh& mesh : model.meshes) {
            const u32 index = meshIndex++;
            const ElementRef where(ElementKind::Mesh, index);

            ProfileMask drawn = kNoProfiles;
            for (const MeshSection& section : mesh.sections) {
                drawn |= section.profiles;
            }

            u32 uvSets = 0;
            while (uvSets < 8 &&
                   mesh.attributes.has(geom::names::uv(uvSets), geom::Domain::Halfedge)) {
                ++uvSets;
            }
            const bool hasVertexColor =
                mesh.attributes.has(geom::names::color(0), geom::Domain::Halfedge);

            u32 maxValence = 3;
            for (u32 valence : mesh.faceSet().faceValence) {
                maxValence = valence > maxValence ? valence : maxValence;
            }
            const u32 influences = mesh.skin.maxInfluences();

            for (u32 p = 0; p < static_cast<u32>(ProfileId::Count); ++p) {
                const ProfileId profile = static_cast<ProfileId>(p);
                if (!HasProfile(drawn, profile) || !document.carries(profile)) {
                    continue;
                }
                const ProfileDesc& desc = Profile(profile);

                if (desc.indexWidth == IndexWidth::U16 && mesh.vertexCount() > 0xFFFFu) {
                    out.error(DiagCode::IndexWidthExceeded,
                              number(mesh.vertexCount()) + " vertices exceeds " +
                                  ToString(profile) + "'s 16-bit indices",
                              where, profile);
                }
                if (!desc.allowsNgons && maxValence > 3) {
                    out.error(DiagCode::NgonUnsupported,
                              "a face has " + number(maxValence) + " corners and " +
                                  ToString(profile) + " takes triangles",
                              where, profile);
                }
                if (uvSets > desc.maxUvSets) {
                    out.warn(DiagCode::UvSetLimit,
                             number(uvSets) + " uv sets exceeds " + ToString(profile) + "'s " +
                                 number(desc.maxUvSets),
                             where, profile);
                }
                if (hasVertexColor && !desc.allowsVertexColor) {
                    out.warn(DiagCode::VertexColorUnsupported,
                             std::string(ToString(profile)) + " has no vertex colour", where,
                             profile);
                }
                if (influences > desc.maxBoneInfluences) {
                    out.warn(DiagCode::BoneInfluenceLimit,
                             number(influences) + " influences exceeds " + ToString(profile) +
                                 "'s " + number(desc.maxBoneInfluences),
                             where, profile);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Rule tables — one per level, in the order they run.
//
// Each phase appends here and nowhere else.
//
//   P1  structural: connectivity relations, index ranges, attribute counts,
//                   skin offsets. manifold: the C1-C9 contract.
//   P3  structural: profile declarations, binding shape, body invariants,
//                   native-kind agreement (§6.5), the §7.5 referencer table.
//       profile:    coverage (§6.3), material and geometry limits.
//   P6  structural: the child model an attach point rides, and the default look.
//   P7  structural: the channel table's ids and the sub-track key sizing.
//       profile:    the animation referencer rows of §10.6 and §7.5.
// ---------------------------------------------------------------------------

constexpr ValidationRule kStructuralRules[] = {
    checkMeshStructure,  checkProfileDeclarations, checkBindingShape,
    checkMaterialBodies, checkNativeKinds,         checkMaterialReferencers,
    checkAttachments,    checkAnimation,           nullptr,
};
constexpr ValidationRule kManifoldRules[] = {checkMeshManifold, nullptr};
constexpr ValidationRule kProfileRules[] = {
    checkCoverage, checkMaterialLimits, checkGeometryLimits, checkAnimReferencers, nullptr,
};

/// The tables above carry a `nullptr` terminator because a zero-sized array is
/// ill-formed; `rulesOf` hands back the populated prefix.
std::span<const ValidationRule> rulesOf(const ValidationRule* table, std::size_t capacity) {
    std::size_t count = 0;
    while (count < capacity && table[count] != nullptr) {
        ++count;
    }
    return std::span<const ValidationRule>(table, count);
}

} // namespace

const char* ToString(ValidateLevel level) {
    switch (level) {
    case ValidateLevel::Structural:
        return "structural";
    case ValidateLevel::Manifold:
        return "manifold";
    case ValidateLevel::Profile:
        return "profile";
    }
    return "invalid";
}

std::span<const ValidationRule> ValidationRulesFor(ValidateLevel level) {
    switch (level) {
    case ValidateLevel::Structural:
        return rulesOf(kStructuralRules, sizeof(kStructuralRules) / sizeof(kStructuralRules[0]));
    case ValidateLevel::Manifold:
        return rulesOf(kManifoldRules, sizeof(kManifoldRules) / sizeof(kManifoldRules[0]));
    case ValidateLevel::Profile:
        return rulesOf(kProfileRules, sizeof(kProfileRules) / sizeof(kProfileRules[0]));
    }
    return std::span<const ValidationRule>();
}

Diagnostics Validate(const Document& document, ValidateLevel level) {
    Diagnostics out;
    const auto run = [&](ValidateLevel which) {
        for (ValidationRule rule : ValidationRulesFor(which)) {
            rule(document, out);
        }
    };

    // Cumulative: a profile check on a structurally broken document reports the
    // structural fault first, which is the one worth fixing.
    run(ValidateLevel::Structural);
    if (level == ValidateLevel::Structural) {
        return out;
    }
    run(ValidateLevel::Manifold);
    if (level == ValidateLevel::Manifold) {
        return out;
    }
    run(ValidateLevel::Profile);
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
