// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "m3_behaviors.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>

namespace whiteout {
namespace models {
namespace wem {
namespace m3_behaviors {

namespace {

std::string TrimNuls(std::string value) {
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

/// A set bag key: the `TRGD`s no turret is in, which no stage can hold. Text,
/// a line per group: its place among the groups, a tab, its name.
constexpr const char* kEmptyTurretGroups = "m3EmptyTurretGroups";

u32 NextId(const Model& model) {
    u32 next = 1;
    for (const PoseStage& stage : model.poseStages) {
        next = std::max(next, stage.id + 1);
    }
    return next;
}

} // namespace

void Import(const m3::Model& source, Model& model, Diagnostics& diagnostics) {
    const u32 boneCount = static_cast<u32>(std::min<std::size_t>(source.bones.size(), model.nodes.size()));
    u32 id = NextId(model);

    for (const m3::IKJoint& joint : source.ikJoints) {
        if (joint.boneIndex1 >= boneCount || joint.boneIndex2 >= boneCount) {
            diagnostics.warn(DiagCode::DanglingNodeReference,
                             "an IKJT names bone " + std::to_string(std::max(joint.boneIndex1, joint.boneIndex2)) +
                                 ", past the skeleton; left out");
            continue;
        }
        // The chain, root first: the end up its parents to the root. One that
        // does not reach it keeps its two ends, and `Validate` says so.
        std::vector<u32> chain;
        for (u32 n = joint.boneIndex2; n != kInvalidNode && n < model.nodes.size(); n = model.nodes.nodes[n].parent) {
            chain.push_back(n);
            if (n == joint.boneIndex1) {
                break;
            }
        }
        if (chain.empty() || chain.back() != joint.boneIndex1) {
            chain = {joint.boneIndex2, joint.boneIndex1};
        }
        std::reverse(chain.begin(), chain.end());

        PoseStage stage;
        stage.id = id++;
        stage.name = "Leg IK " + model.nodes.nodes[joint.boneIndex1].name;
        stage.kind = StageKind::Sc2LegIk;
        stage.driven = std::move(chain);
        Sc2LegIkParams params;
        params.probeUp = joint.raycastUp;
        params.probeDown = joint.raycastDown;
        params.maxSpeed = joint.maxSpeed;
        params.goalThreshold = joint.goalThreshold;
        params.dependents = joint.dependents;
        stage.legIk = std::move(params);
        model.poseStages.push_back(std::move(stage));
    }

    // Which group each turret is in, and where.
    struct Membership {
        std::string group;
        u32 order = 0xFFFFFFFFu;
        std::vector<u32> slots;
    };
    std::vector<Membership> memberOf(source.turretBehaviors.size());
    std::string empty;
    for (std::size_t g = 0; g < source.triggerData.size(); ++g) {
        const m3::TriggerData& group = source.triggerData[g];
        if (group.dataIndices.empty()) {
            empty += std::to_string(g) + "\t" + TrimNuls(group.name) + "\n";
        }
        for (std::size_t k = 0; k < group.dataIndices.size(); ++k) {
            const u32 turret = group.dataIndices[k];
            if (turret >= memberOf.size()) {
                continue;
            }
            // Its first group; two groups may share a name, so it is known by
            // its place.
            Membership& member = memberOf[turret];
            if (member.order == 0xFFFFFFFFu) {
                member = {TrimNuls(group.name), static_cast<u32>(g), {static_cast<u32>(k)}};
            } else if (member.order == g) {
                member.slots.push_back(static_cast<u32>(k));
            }
        }
    }
    for (std::size_t t = 0; t < source.turretBehaviors.size(); ++t) {
        const m3::TurretBehavior& turret = source.turretBehaviors[t];
        if (turret.boneIndex >= boneCount) {
            diagnostics.warn(DiagCode::DanglingNodeReference,
                             "a PATU names bone " + std::to_string(turret.boneIndex) + ", past the skeleton; left out");
            continue;
        }
        PoseStage stage;
        stage.id = id++;
        stage.name = "Turret " + model.nodes.nodes[turret.boneIndex].name;
        stage.kind = StageKind::Sc2Turret;
        stage.driven = {turret.boneIndex};
        Sc2TurretParams params;
        params.frame = turret.transform;
        params.rotationA = turret.unknown1;
        params.rotationB = turret.unknown2;
        params.mainTurret = turret.useAsMainTurret != 0;
        params.groupFlag = turret.turretGroupId;
        params.yawLimited = turret.yawLimited;
        params.yawMin = turret.yawMin;
        params.yawMax = turret.yawMax;
        params.yawWeight = turret.yawWeight;
        params.pitchLimited = turret.pitchLimited;
        params.pitchMin = turret.pitchMin;
        params.pitchMax = turret.pitchMax;
        params.pitchWeight = turret.pitchWeight;
        params.unknown3 = turret.unknown3;
        params.unknown4 = turret.unknown4;
        params.mainBoneOffset = turret.mainBoneOffset;
        params.group = memberOf[t].group;
        params.groupOrder = memberOf[t].order;
        params.groupSlots = memberOf[t].slots;
        params.version = turret.getVersion();
        stage.turret = std::move(params);
        model.poseStages.push_back(std::move(stage));
    }

    // On the material set beside the rest of the MODL the import keeps.
    if (!empty.empty() && !model.profileSets.empty()) {
        model.profileSets.front().native.setText(kEmptyTurretGroups, std::move(empty));
    }

    const std::size_t unread = source.ikTwoJoints.size() + source.ikCCD.size() + source.oneBoneSolvers.size();
    if (unread != 0) {
        diagnostics.warn(DiagCode::FeatureDropped,
                         std::to_string(unread) + " IK2J / IKCC / PAOB record(s), which no shipped model holds, "
                                                  "are not carried");
    }
}

void Export(const Model& model, const std::vector<u32>& boneOf, m3::Model& out, Diagnostics& diagnostics) {
    const auto bone = [&](u32 node) -> u32 { return node < boneOf.size() ? boneOf[node] : 0xFFFFu; };

    struct Placed {
        std::string name;
        u32 order;
        std::vector<std::pair<u32, u32>> members; ///< (slot, PATU index)
    };
    std::vector<Placed> groups;

    for (const PoseStage& stage : model.poseStages) {
        if (!stage.enabled || stage.driven.empty()) {
            continue;
        }
        if (stage.kind == StageKind::Sc2LegIk) {
            const u32 first = bone(stage.driven.front());
            const u32 last = bone(stage.driven.back());
            if (first == 0xFFFFu || last == 0xFFFFu) {
                diagnostics.warn(DiagCode::FeatureDropped, "leg IK '" + stage.name + "' drives a node with no bone");
                continue;
            }
            const Sc2LegIkParams params = stage.legIk.value_or(Sc2LegIkParams{});
            m3::IKJoint joint;
            joint.boneIndex1 = static_cast<u16>(first);
            joint.boneIndex2 = static_cast<u16>(last);
            joint.raycastUp = params.probeUp;
            joint.raycastDown = params.probeDown;
            joint.maxSpeed = params.maxSpeed;
            joint.goalThreshold = params.goalThreshold;
            joint.dependents = params.dependents;
            out.ikJoints.push_back(std::move(joint));
        } else if (stage.kind == StageKind::Sc2Turret) {
            const u32 driven = bone(stage.driven.front());
            if (driven == 0xFFFFu) {
                diagnostics.warn(DiagCode::FeatureDropped, "turret '" + stage.name + "' drives a node with no bone");
                continue;
            }
            const Sc2TurretParams params = stage.turret.value_or(Sc2TurretParams{});
            m3::TurretBehavior turret;
            if (params.version >= 0) {
                turret.setVersion(params.version);
            }
            turret.transform = params.frame;
            turret.unknown1 = params.rotationA;
            turret.unknown2 = params.rotationB;
            turret.boneIndex = static_cast<u16>(driven);
            turret.useAsMainTurret = params.mainTurret ? 1 : 0;
            turret.turretGroupId = params.groupFlag;
            turret.yawLimited = params.yawLimited;
            turret.yawMin = params.yawMin;
            turret.yawMax = params.yawMax;
            turret.yawWeight = params.yawWeight;
            turret.pitchLimited = params.pitchLimited;
            turret.pitchMin = params.pitchMin;
            turret.pitchMax = params.pitchMax;
            turret.pitchWeight = params.pitchWeight;
            turret.unknown3 = params.unknown3;
            turret.unknown4 = params.unknown4;
            turret.mainBoneOffset = params.mainBoneOffset;
            const u32 index = static_cast<u32>(out.turretBehaviors.size());
            out.turretBehaviors.push_back(std::move(turret));
            if (params.group.empty() && params.groupOrder == 0xFFFFFFFFu) {
                continue;
            }
            // The group the file held, or for a turret given one here, the
            // first of that name.
            auto at = std::find_if(groups.begin(), groups.end(), [&](const Placed& g) {
                return g.name == params.group && (params.groupOrder == 0xFFFFFFFFu || g.order == params.groupOrder);
            });
            if (at == groups.end()) {
                groups.push_back(Placed{params.group, params.groupOrder, {}});
                at = groups.end() - 1;
            }
            if (params.groupSlots.empty()) {
                at->members.emplace_back(0xFFFFFFFFu, index);
            }
            for (const u32 slot : params.groupSlots) {
                at->members.emplace_back(slot, index);
            }
        }
    }

    // A group the source held with no turret in it.
    std::string empty;
    for (const ProfileMaterialSet& set : model.profileSets) {
        if (empty.empty()) {
            empty = set.native.text(kEmptyTurretGroups);
        }
    }
    for (std::size_t at = 0; at < empty.size();) {
        const std::size_t tab = empty.find('\t', at);
        const std::size_t end = empty.find('\n', at);
        if (tab == std::string::npos || end == std::string::npos || tab > end) {
            break;
        }
        const std::string name = empty.substr(tab + 1, end - tab - 1);
        const u32 order = static_cast<u32>(std::strtoul(empty.substr(at, tab - at).c_str(), nullptr, 10));
        // A group given here under its name takes its place.
        const auto named = std::find_if(groups.begin(), groups.end(), [&](const Placed& g) {
            return g.name == name && g.order == 0xFFFFFFFFu;
        });
        if (named != groups.end()) {
            named->order = order;
        } else if (std::none_of(groups.begin(), groups.end(), [&](const Placed& g) { return g.order == order; })) {
            groups.push_back(Placed{name, order, {}});
        }
        at = end + 1;
    }

    // The groups in the order the source held them, a new one after; each
    // list in its own order.
    std::stable_sort(groups.begin(), groups.end(), [](const Placed& a, const Placed& b) { return a.order < b.order; });
    for (Placed& group : groups) {
        std::stable_sort(group.members.begin(), group.members.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        m3::TriggerData data;
        data.name = group.name;
        for (const auto& [slot, index] : group.members) {
            data.dataIndices.push_back(index);
        }
        out.triggerData.push_back(std::move(data));
    }
}

} // namespace m3_behaviors
} // namespace wem
} // namespace models
} // namespace whiteout
