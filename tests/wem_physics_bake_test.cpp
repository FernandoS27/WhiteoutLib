// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Physics baked into clips (EDIT_MODE_PHYSICS_BAKE_DESIGN.md), the library's
// half: G-SW, the switch rule row by row; G-PO, the clips a bake plays once;
// the lead-ins a bake runs; the clip's bake settings through a `.wem`.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/anim/key_reduce.h>
#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/anim/rests.h>
#include <whiteout/models/wem/anim/transition.h>
#include <whiteout/models/wem/nodes/remove.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/physics/bake.h>
#include <whiteout/models/wem/physics/crossing.h>
#include <whiteout/models/wem/physics/references.h>
#include <whiteout/models/wem/physics/switches.h>
#include <whiteout/models/wem/reflect_bytes.h>
#include <whiteout/models/wem/writer.h>

#include "whiteout/models/wem/chunk_tags.h"

using namespace whiteout;
using namespace whiteout::models::wem;
using Catch::Approx;

namespace {

Node Bone(const char* name, u32 parent) {
    Node node;
    node.name = name;
    node.kind = NodeKind::Bone;
    node.resetPayloadForKind();
    node.parent = parent;
    return node;
}

/// A chain of bones, a body on each, every body in one rig of @p start; the
/// bodies' motion by @p dynamic. Clips by name, each a second long.
struct Rig {
    Document document;
    std::vector<u32> bodies; ///< Body ids, one per bone.
    u32 rig = 0;

    Rig(RigStart start, std::vector<bool> dynamic, std::vector<std::string> clips) {
        document.profiles = {ProfileId::Wc3Reforged};
        document.defaultProfile = ProfileId::Wc3Reforged;
        Model& model = document.models.emplace_back();
        PhysicsRig made;
        made.id = model.physics.allocateId();
        made.start = start;
        rig = made.id;
        for (u32 b = 0; b < dynamic.size(); ++b) {
            model.nodes.add(Bone(("bone" + std::to_string(b)).c_str(), b == 0 ? kInvalidNode : b - 1));
            PhysicsBody body;
            body.id = model.physics.allocateId();
            body.node = b;
            body.motion = dynamic[b] ? BodyMotion::Dynamic : BodyMotion::Kinematic;
            model.physics.bodies.push_back(body);
            made.bodies.push_back(body.id);
            bodies.push_back(body.id);
        }
        model.physics.rigs.push_back(made);
        for (const std::string& name : clips) {
            Clip clip;
            clip.name = name;
            clip.model = 0;
            clip.duration = 1.0f;
            clip.looping = true;
            clip.containers.emplace_back();
            document.clips.push_back(clip);
        }
    }

    Model& model() {
        return document.models[0];
    }

    /// Keys @p record's @p channel in clip @p clip.
    void key(u32 clip, u32 record, Channel channel, std::vector<f32> times, std::vector<f32> values,
             Interpolation interp = Interpolation::Step) {
        SubTrack track;
        track.channel = PhysicsSwitchChannel(model(), record, channel);
        track.interp = interp;
        track.times = std::move(times);
        track.values.resize(values.size() * sizeof(f32));
        std::memcpy(track.values.data(), values.data(), track.values.size());
        document.clips[clip].containers.front().subTracks.push_back(std::move(track));
    }

    std::vector<BodySwitch> at(u32 clip, f32 seconds, SwitchScope scope = SwitchScope::Clip,
                               std::vector<BodySwitch> previous = {}) const {
        std::vector<BodySwitch> out;
        PhysicsSwitches(document, 0, clip, seconds, scope, previous, out);
        return out;
    }
};

std::vector<bool> Actives(const std::vector<BodySwitch>& switches) {
    std::vector<bool> out;
    for (const BodySwitch& s : switches) {
        out.push_back(s.active);
    }
    return out;
}

} // namespace

// ---- G-SW: the rule, row by row (§4.5) ---------------------------------------

TEST_CASE("G-SW an On death ragdoll drops in Death and nowhere else", "[physics][bake][switches]") {
    Rig rig(RigStart::OnDeath, {true, true}, {"Stand", "Death", "Death - 2", "Decay Flesh"});
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false});
    const std::vector<BodySwitch> death = rig.at(1, 0.0f);
    CHECK(Actives(death) == std::vector<bool>{true, true});
    CHECK(death[0].why == SwitchWhy::Death);
    CHECK(Actives(rig.at(2, 0.5f)) == std::vector<bool>{true, true});
    // Decay continues a death only once it is set up (§4.3).
    CHECK(Actives(rig.at(3, 0.5f)) == std::vector<bool>{false, false});
    // At rest nothing moves.
    CHECK(Actives(rig.at(kInvalidIndex, 0.0f)) == std::vector<bool>{false, false});
}

TEST_CASE("G-SW a clip's own Ragdoll now keys override the default", "[physics][bake][switches]") {
    Rig rig(RigStart::OnDeath, {true, true}, {"Death"});
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.3f}, {0.0f, 1.0f});
    CHECK(Actives(rig.at(0, 0.1f)) == std::vector<bool>{false, false});
    const std::vector<BodySwitch> after = rig.at(0, 0.5f);
    CHECK(Actives(after) == std::vector<bool>{true, true});
    CHECK(after[1].why == SwitchWhy::Dropped);
}

TEST_CASE("G-SW a body's own key decides even where its ragdoll does not run", "[physics][bake][switches]") {
    Rig rig(RigStart::OnDeath, {true, true}, {"Stand"});
    rig.key(0, rig.bodies[1], Channel::PhysicsDynamic, {0.0f, 0.5f}, {0.0f, 1.0f});
    CHECK(Actives(rig.at(0, 0.25f)) == std::vector<bool>{false, false});
    const std::vector<BodySwitch> later = rig.at(0, 0.75f);
    CHECK(Actives(later) == std::vector<bool>{false, true});
    CHECK(later[1].why == SwitchWhy::Keyed);
}

TEST_CASE("G-SW a switch holds its last key to a StarCraft II loop's end", "[physics][bake][switches]") {
    // StarCraft II wraps a looping track at its own last key; the rule reads
    // a clip's switches over the clip.
    Rig rig(RigStart::OnDeath, {true}, {"Stand"});
    rig.document.clips[0].readRule = ReadRule::Sc2;
    rig.key(0, rig.bodies[0], Channel::PhysicsDynamic, {0.0f, 0.5f}, {0.0f, 1.0f});
    CHECK(Actives(rig.at(0, 0.25f)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(0, 0.7f)) == std::vector<bool>{true});
    CHECK(Actives(rig.at(0, 1.0f)) == std::vector<bool>{true});
}

TEST_CASE("G-SW a partial ragdoll swings its tail and drops whole at its key", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {false, false, true, true}, {"Stand", "Death"});
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false, true, true});
    CHECK(Actives(rig.at(1, 0.1f)) == std::vector<bool>{false, false, true, true});
    rig.key(1, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.3f}, {0.0f, 1.0f});
    CHECK(Actives(rig.at(1, 0.1f)) == std::vector<bool>{false, false, true, true});
    const std::vector<BodySwitch> dropped = rig.at(1, 0.5f);
    CHECK(Actives(dropped) == std::vector<bool>{true, true, true, true});
    // The tail moves by its own State, the rest because the rig dropped.
    CHECK(dropped[0].why == SwitchWhy::Dropped);
    CHECK(dropped[3].why == SwitchWhy::Rest);
}

TEST_CASE("G-SW an exempt body ignores the drop and nothing else", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {false, true}, {"Death"});
    rig.model().physics.bodies[0].exemptFromRagdoll = true;
    rig.model().physics.bodies[1].exemptFromRagdoll = true;
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f}, {1.0f});
    const std::vector<BodySwitch> now = rig.at(0, 0.5f);
    CHECK(now[0].active == false);
    CHECK(now[0].why == SwitchWhy::Exempt);
    // Dynamic in a running ragdoll, it moves (the Test changed here, §8.6).
    CHECK(now[1].active == true);
}

TEST_CASE("G-SW Static never switches and Never rigs never run", "[physics][bake][switches]") {
    Rig rig(RigStart::Never, {true, true}, {"Death"});
    rig.model().physics.bodies[0].motion = BodyMotion::Static;
    rig.key(0, rig.bodies[1], Channel::PhysicsDynamic, {0.0f}, {1.0f});
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false});
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{false, false});
}

TEST_CASE("G-SW Inherit reads its ancestor in body order", "[physics][bake][switches]") {
    // Bone 1 under bone 0; the child's body listed first, so it reads its
    // parent's last step, and a child listed after reads this one's.
    Rig rig(RigStart::OnDeath, {true, true}, {"Death"});
    PhysicsSet& physics = rig.model().physics;
    std::swap(physics.bodies[0], physics.bodies[1]);
    physics.bodies[0].inheritDynamic = true;
    physics.bodies[0].motion = BodyMotion::Kinematic;
    const std::vector<BodySwitch> first = rig.at(0, 0.0f);
    // Dropped with the rig, the child is active either way.
    CHECK(first[0].active);
    // Out of the drop, it takes its ancestor's state.
    physics.bodies[0].exemptFromRagdoll = true;
    std::vector<BodySwitch> previous(2);
    CHECK(rig.at(0, 0.0f, SwitchScope::Clip, previous)[0].active == false);
    previous[1].active = true;
    const std::vector<BodySwitch> inherited = rig.at(0, 0.0f, SwitchScope::Clip, previous);
    CHECK(inherited[0].active);
    CHECK(inherited[0].why == SwitchWhy::Inherited);
    // Listed after its parent, it reads the parent's state this step.
    std::swap(physics.bodies[0], physics.bodies[1]);
    CHECK(rig.at(0, 0.0f)[1].active);
}

TEST_CASE("G-SW Inherit with no bodied ancestor stays with the animation", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {true}, {"Stand"});
    rig.model().physics.bodies[0].inheritDynamic = true;
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false});
}

TEST_CASE("G-SW a dropped body blends by its rig and a switched one by its own", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {false, true}, {"Death"});
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f}, {1.0f});
    rig.key(0, rig.rig, Channel::PhysicsBlend, {0.0f, 1.0f}, {1.0f, 0.0f}, Interpolation::Linear);
    rig.key(0, rig.bodies[1], Channel::PhysicsBlend, {0.0f}, {1.0f}, Interpolation::Linear);
    const std::vector<BodySwitch> now = rig.at(0, 0.5f);
    CHECK(now[0].blend == Approx(0.5f).margin(1e-3));
    CHECK(now[0].returning());
    CHECK(now[1].blend == Approx(1.0f));
    CHECK(now[1].dynamic());
}

TEST_CASE("G-SW a baked clip is its keys everywhere but Simulate", "[physics][bake][switches]") {
    Rig rig(RigStart::OnDeath, {true}, {"Death"});
    rig.document.clips[0].physics.emplace().baked.emplace();
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Simulate)) == std::vector<bool>{true});
}

TEST_CASE("G-SW the Test runs every rig but Never and the bodies no rig holds", "[physics][bake][switches]") {
    Rig rig(RigStart::OnDeath, {true, true}, {"Stand"});
    rig.model().physics.rigs.front().bodies.pop_back();
    // Outside the Test a loose body moves only where keyed.
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false});
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{true, true});
    SwitchReader clipReader(rig.document, 0, 0, SwitchScope::Clip);
    CHECK(clipReader.movable(0));
    CHECK_FALSE(clipReader.movable(1));
}

TEST_CASE("G-SW a drop carries into the clip that continues it", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {false, true}, {"Stand", "Death", "Decay Flesh", "Decay Bone"});
    rig.key(1, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.3f}, {0.0f, 1.0f});
    // Not set up, the Decays carry nothing, as today.
    CHECK(Actives(rig.at(2, 0.0f)) == std::vector<bool>{false, true});
    // Set up, Decay Bone comes from Decay Flesh, which comes from Death: the
    // corpse stays down, though Decay Flesh itself is not set up.
    rig.document.clips[3].physics.emplace();
    const std::vector<BodySwitch> bone = rig.at(3, 0.0f);
    CHECK(Actives(bone) == std::vector<bool>{true, true});
    CHECK(bone[0].why == SwitchWhy::Carried);
    // A key of 0 at 0 in the clip turns the drop off.
    rig.key(3, rig.rig, Channel::PhysicsRagdoll, {0.0f}, {0.0f});
    CHECK(Actives(rig.at(3, 0.5f)) == std::vector<bool>{false, true});
}

TEST_CASE("G-SW a corpse's clip keeps what its death let go of by a key", "[physics][bake][switches]") {
    // The second body is a prop no rig holds, which the Death lets go of.
    Rig rig(RigStart::OnDeath, {true, true}, {"Stand", "Death", "Dissipate", "Attack"});
    rig.model().physics.rigs.front().bodies.pop_back();
    rig.key(1, rig.bodies[1], Channel::PhysicsDynamic, {0.0f, 0.3f}, {0.0f, 1.0f});
    // Not set up, the Dissipate carries nothing.
    CHECK(Actives(rig.at(2, 0.5f)) == std::vector<bool>{false, false});
    rig.document.clips[2].physics.emplace();
    const std::vector<BodySwitch> corpse = rig.at(2, 0.0f);
    CHECK(Actives(corpse) == std::vector<bool>{true, true});
    CHECK(corpse[1].why == SwitchWhy::Carried);
    // A clip that is no corpse's carries the drop and not the prop.
    rig.document.clips[3].looping = false;
    rig.document.clips[3].physics.emplace().comesFrom = BakeFrom::Clip;
    rig.document.clips[3].physics->comesFromClip = "Death";
    CHECK(Actives(rig.at(3, 0.0f)) == std::vector<bool>{true, false});
    // Its own key decides where the clip has one.
    rig.key(2, rig.bodies[1], Channel::PhysicsDynamic, {0.0f}, {0.0f});
    CHECK(Actives(rig.at(2, 0.5f)) == std::vector<bool>{true, false});
}

TEST_CASE("G-SW the switches' rests are the rule's", "[physics][bake][switches]") {
    Rig always(RigStart::Always, {true, false}, {"Stand"});
    TrackTarget target;
    target.kind = TrackTarget::Kind::Physics;
    target.channel = Channel::PhysicsDynamic;
    target.sub = always.bodies[0];
    CHECK(PhysicsSwitchRest(always.model(), target, Game::Warcraft) == 1.0f);
    target.sub = always.bodies[1];
    CHECK(PhysicsSwitchRest(always.model(), target, Game::Warcraft) == 0.0f);
    target.channel = Channel::PhysicsBlend;
    CHECK(PhysicsSwitchRest(always.model(), target, Game::Warcraft) == 1.0f);
    target.channel = Channel::PhysicsRagdoll;
    target.sub = always.rig;
    CHECK(PhysicsSwitchRest(always.model(), target, Game::Warcraft) == 0.0f);
    // What the Animator fills an unkeyed switch with.
    Rig onDeath(RigStart::OnDeath, {true}, {"Death"});
    target.channel = Channel::PhysicsDynamic;
    target.sub = onDeath.bodies[0];
    const TrackRests rests = RestsOf(onDeath.document, 0, target, geom::AttrType::F32);
    f32 rest = -1.0f;
    std::memcpy(&rest, rests.unkeyed.data(), sizeof rest);
    CHECK(rest == 0.0f);
    target.sub = always.bodies[0];
    std::memcpy(&rest, RestsOf(always.document, 0, target, geom::AttrType::F32).unkeyed.data(), sizeof rest);
    CHECK(rest == 1.0f);
}

// ---- StarCraft II's own rule for a body no rig holds (SC2 physics plan U1) ----

namespace {

/// @p rig made a StarCraft II document with every body out of its rig, as an
/// `.m3` import has them: created kinematic, `simulates` its authored state.
void Shipped(Rig& rig) {
    rig.document.profiles = {ProfileId::Sc2};
    rig.document.defaultProfile = ProfileId::Sc2;
    rig.model().physics.rigs.clear();
    for (PhysicsBody& body : rig.model().physics.bodies) {
        body.motion = BodyMotion::Kinematic;
    }
}

} // namespace

TEST_CASE("G-SW a StarCraft II body no rig holds follows its own key", "[physics][bake][switches][sc2]") {
    Rig rig(RigStart::Always, {false}, {"Stand", "Death"});
    Shipped(rig);
    rig.model().physics.bodies[0].simulates = false;
    rig.key(1, rig.bodies[0], Channel::PhysicsDynamic, {0.0f, 0.3f}, {0.0f, 1.0f});
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(1, 0.1f)) == std::vector<bool>{false});
    const std::vector<BodySwitch> death = rig.at(1, 0.5f);
    CHECK(Actives(death) == std::vector<bool>{true});
    CHECK(death[0].why == SwitchWhy::Keyed);
    // The Test reads the same keys.
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(1, 0.5f, SwitchScope::Test)) == std::vector<bool>{true});
}

TEST_CASE("G-SW an unkeyed StarCraft II body that simulates is on everywhere", "[physics][bake][switches][sc2]") {
    Rig rig(RigStart::Always, {false}, {"Stand", "Death"});
    Shipped(rig);
    for (const SwitchScope scope : {SwitchScope::Clip, SwitchScope::Simulate, SwitchScope::Test}) {
        for (const u32 clip : {0u, 1u, kInvalidIndex}) {
            const std::vector<BodySwitch> now = rig.at(clip, 0.5f, scope);
            CHECK(Actives(now) == std::vector<bool>{true});
            CHECK(now[0].why == SwitchWhy::Rest);
        }
    }
    CHECK(SwitchReader(rig.document, 0, 0, SwitchScope::Clip).movable(0));
    TrackTarget target;
    target.kind = TrackTarget::Kind::Physics;
    target.channel = Channel::PhysicsDynamic;
    target.sub = rig.bodies[0];
    CHECK(PhysicsSwitchRest(rig.model(), target, Game::StarCraft) == 1.0f);
    // The editor's rule reads the same record off: kinematic, in no rig.
    CHECK(PhysicsSwitchRest(rig.model(), target, Game::Warcraft) == 0.0f);
}

TEST_CASE("G-SW a Static StarCraft II body is never on", "[physics][bake][switches][sc2]") {
    Rig rig(RigStart::Always, {false}, {"Stand"});
    Shipped(rig);
    rig.model().physics.bodies[0].motion = BodyMotion::Static;
    rig.key(0, rig.bodies[0], Channel::PhysicsDynamic, {0.0f}, {1.0f});
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false});
    CHECK(Actives(rig.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{false});
    CHECK_FALSE(SwitchReader(rig.document, 0, 0, SwitchScope::Clip).movable(0));
}

TEST_CASE("G-SW a StarCraft II Inherit body follows its ancestor, or stays", "[physics][bake][switches][sc2]") {
    Rig rig(RigStart::Always, {false, false}, {"Stand"});
    Shipped(rig);
    PhysicsSet& physics = rig.model().physics;
    physics.bodies[1].inheritDynamic = true;
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{true, true});
    CHECK(rig.at(0, 0.5f)[1].why == SwitchWhy::Inherited);
    physics.bodies[0].simulates = false;
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false});
    // With no bodied ancestor it stays with its animation.
    physics.bodies[0].inheritDynamic = true;
    physics.bodies[0].simulates = true;
    CHECK(Actives(rig.at(0, 0.5f)) == std::vector<bool>{false, false});
}

TEST_CASE("G-SW a StarCraft II rig member and a Warcraft III body read as before", "[physics][bake][switches][sc2]") {
    // A rig member keeps the editor's rule: an On death ragdoll is still in Stand.
    Rig member(RigStart::OnDeath, {true, false}, {"Stand"});
    member.document.profiles = {ProfileId::Sc2};
    member.document.defaultProfile = ProfileId::Sc2;
    CHECK(Actives(member.at(0, 0.5f)) == std::vector<bool>{false, false});
    CHECK(Actives(member.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{true, false});
    // The same loose bodies under Warcraft III: still outside the Test, and
    // the Test runs only the Dynamic one.
    Rig loose(RigStart::Always, {true, false}, {"Stand"});
    loose.model().physics.rigs.clear();
    CHECK(Actives(loose.at(0, 0.5f)) == std::vector<bool>{false, false});
    CHECK(Actives(loose.at(0, 0.5f, SwitchScope::Test)) == std::vector<bool>{true, false});
    CHECK_FALSE(SwitchReader(loose.document, 0, 0, SwitchScope::Clip).movable(0));
}

TEST_CASE("G-SW a switch on the wrong kind of record is invalid", "[physics][bake][switches]") {
    Rig rig(RigStart::Always, {true}, {"Stand"});
    rig.key(0, rig.bodies[0], Channel::PhysicsRagdoll, {0.0f}, {1.0f});
    Diagnostics report;
    CheckPhysics(rig.model(), report);
    CHECK(report.countOf(DiagCode::PhysicsReferenceInvalid) == 1u);
    Diagnostics clean;
    Rig good(RigStart::Always, {true}, {"Stand"});
    good.key(0, good.rig, Channel::PhysicsRagdoll, {0.0f}, {1.0f});
    good.key(0, good.rig, Channel::PhysicsBlend, {0.0f}, {1.0f});
    good.key(0, good.bodies[0], Channel::PhysicsBlend, {0.0f}, {1.0f});
    CheckPhysics(good.model(), clean);
    CHECK(clean.countOf(DiagCode::PhysicsReferenceInvalid) == 0u);
}

// ---- G-PO: what plays once (§7.1) --------------------------------------------

TEST_CASE("G-PO the base name drops the comment and the variant", "[physics][bake][names]") {
    CHECK(BaseSequenceName("Stand - 2") == "Stand");
    CHECK(BaseSequenceName("Stand 2") == "Stand");
    CHECK(BaseSequenceName("Attack Slam - 3") == "Attack Slam");
    CHECK(BaseSequenceName("  Walk  ") == "Walk");
    CHECK(BaseSequenceName("Stand Victory") == "Stand Victory");
}

TEST_CASE("G-PO the games' played-once names", "[physics][bake][names]") {
    for (const char* once : {"Attack", "Attack 2", "Attack - 3", "Attack Slam", "AttackUnarmed", "Spell",
                             "Spell Slam", "Spell Throw", "SpellCastDirected", "Death", "Death - 2", "Decay Flesh",
                             "Decay Bone", "Birth", "Dissipate", "Morph", "Morph Alternate", "Stand Hit"}) {
        INFO(once);
        CHECK(PlaysOnce(once));
    }
    for (const char* loops : {"Stand", "Stand 2", "Stand Ready", "Stand Victory", "Stand Channel", "Stand Work",
                              "Spell Channel", "Walk", "Walk Fast", "Portrait", "Portrait Talk", "Sleep",
                              "Cinematic"}) {
        INFO(loops);
        CHECK_FALSE(PlaysOnce(loops));
    }
}

TEST_CASE("G-PO a bake loops by the flag or the name or as set", "[physics][bake][names]") {
    Clip clip;
    clip.name = "Attack";
    clip.looping = true;
    CHECK_FALSE(BakeLoops(clip));
    clip.name = "Walk";
    CHECK(BakeLoops(clip));
    clip.looping = false;
    CHECK_FALSE(BakeLoops(clip));
    clip.physics.emplace().plays = BakePlays::Loops;
    CHECK(BakeLoops(clip));
    clip.name = "Walk";
    clip.looping = true;
    clip.physics->plays = BakePlays::Once;
    CHECK_FALSE(BakeLoops(clip));
}

TEST_CASE("G-PO a Diablo III clip plays once by any of its words", "[physics][bake][names]") {
    Clip clip;
    clip.looping = true;
    clip.native.set("animSnoId", i64{1});
    for (const char* once : {"Zombie_Female_attack_01", "OmniNPC_Male_2HS_getHit_01", "Zombie_Female_death_explode",
                             "OmniNPC_Male_BOW_cast_AOE_01", "Templar_HTH_Run_Outro_01", "Zombie_Female_spawn"}) {
        INFO(once);
        clip.name = once;
        CHECK_FALSE(BakeLoops(clip));
    }
    for (const char* loops : {"Zombie_Female_walk", "OmniNPC_Male_BOW_idle_01", "OmniNPC_Male_on_edge_falling_loop"}) {
        INFO(loops);
        clip.name = loops;
        CHECK(BakeLoops(clip));
    }
    // Elsewhere a name's first words say it.
    Clip warcraft;
    warcraft.looping = true;
    warcraft.name = "Stand Hit Attack";
    CHECK_FALSE(BakeLoops(warcraft));
    warcraft.name = "Walk Attack";
    CHECK(BakeLoops(warcraft));
}

// ---- The lead-ins (§7.2, §7.4) -------------------------------------------------

TEST_CASE("a clip comes from Stand and a corpse from its death and a loop from itself", "[physics][bake][chain]") {
    Rig rig(RigStart::OnDeath, {true},
            {"Stand Ready", "Stand - 1", "Attack", "Death", "Decay Flesh", "Decay Bone", "Walk", "Decay", "Dissipate"});
    std::vector<Clip>& clips = rig.document.clips;
    clips[2].looping = clips[3].looping = clips[4].looping = clips[5].looping = clips[7].looping = false;
    clips[8].looping = false;
    CHECK(BakeComesFrom(rig.document, 6).kind == BakeFrom::Itself);
    const BakeLeadIn attack = BakeComesFrom(rig.document, 2);
    CHECK(attack.kind == BakeFrom::Clip);
    CHECK(attack.clip == 1u); // the first looping Stand, not Stand Ready
    CHECK(BakeComesFrom(rig.document, 3).clip == 1u);
    CHECK(BakeComesFrom(rig.document, 4).clip == 3u);
    CHECK(BakeComesFrom(rig.document, 5).clip == 4u);
    CHECK(BakeComesFrom(rig.document, 7).clip == 3u);
    // A hero's corpse dissipates where its death left it.
    CHECK(BakeComesFrom(rig.document, 8).clip == 3u);
    CHECK(BakeChain(rig.document, 8) == std::vector<u32>{1, 3});
    CHECK(BakeChain(rig.document, 5) == std::vector<u32>{1, 3, 4});
    CHECK(BakeGoesTo(rig.document, 2) == 1u);
    CHECK(BakeGoesTo(rig.document, 3) == kInvalidIndex);
    CHECK(BakeGoesTo(rig.document, 6) == kInvalidIndex);
    // By name, and a first frame held when the model has no Stand.
    clips[2].physics.emplace().comesFrom = BakeFrom::Clip;
    clips[2].physics->comesFromClip = "Walk";
    CHECK(BakeComesFrom(rig.document, 2).clip == 6u);
    clips[1].name = "Idle";
    CHECK(BakeComesFrom(rig.document, 3).kind == BakeFrom::FirstFrame);
}

TEST_CASE("a lead-in chain that comes back on itself stops", "[physics][bake][chain]") {
    Rig rig(RigStart::OnDeath, {true}, {"Attack", "Spell"});
    std::vector<Clip>& clips = rig.document.clips;
    clips[0].physics.emplace().comesFrom = BakeFrom::Clip;
    clips[0].physics->comesFromClip = "Spell";
    clips[1].physics.emplace().comesFrom = BakeFrom::Clip;
    clips[1].physics->comesFromClip = "Attack";
    bool cycle = false;
    CHECK(BakeChain(rig.document, 0, &cycle) == std::vector<u32>{1});
    CHECK(cycle);
}

// ---- The settings through a `.wem` (§9.2) --------------------------------------

TEST_CASE("a clip's bake settings and record survive a .wem", "[physics][bake][io]") {
    Rig rig(RigStart::Always, {true}, {"Walk"});
    ClipPhysics& physics = rig.document.clips[0].physics.emplace();
    physics.plays = BakePlays::Loops;
    physics.comesFrom = BakeFrom::Clip;
    physics.comesFromClip = "Stand";
    physics.preheat = 5;
    physics.match = LoopMatch::Offset;
    physics.where = MatchWhere::Both;
    physics.ease = MatchEase::Custom;
    physics.curve = {{0.0f, 0.0f}, {0.4f, 0.7f}, {1.0f, 1.0f}};
    physics.tolerance = 0.0002f;
    physics.stepRate = 240;
    physics.floor = BakeFloor::FollowsRoot;
    physics.travel = true;
    BakeWorldForce blast;
    blast.kind = BakeWorldForce::Kind::Blast;
    blast.start = 0.3f;
    blast.centreNode = 0;
    blast.channels = 1u << 17;
    physics.world = {BakeWorldForce{}, blast};
    PhysicsBake& baked = physics.baked.emplace();
    baked.source.resize(1);
    SubTrack replaced;
    replaced.channel = 7;
    replaced.times = {0.0f, 1.0f};
    replaced.values.resize(2 * sizeof(f32));
    baked.source[0].subTracks.push_back(replaced);
    baked.appended = {9, 10};
    baked.inputs = 0x1234567890ull;
    baked.settling = {18.4f, 3.1f};
    baked.stretchChannels = {7};
    baked.stretchCounts = {1};
    baked.stretchTimes = {0.25f, 0.3f, 0.9f};
    baked.stretchOutside = {0x9876543210ull};
    baked.stretchInside = 0x1122334455ull;
    const std::vector<u8> bytes = Writer().write(rig.document);
    const std::optional<Document> read = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    REQUIRE(read->clips.size() == 1u);
    REQUIRE(read->clips[0].physics.has_value());
    CHECK(ReflectBytes(*read->clips[0].physics) == ReflectBytes(physics));
    // A clip with none reads back with none.
    rig.document.clips[0].physics.reset();
    const std::vector<u8> plain = Writer().write(rig.document);
    CHECK_FALSE(Parser().parse(std::span<const u8>(plain.data(), plain.size()))->clips[0].physics.has_value());
}

TEST_CASE("a clip written before v5 reads with no bake settings", "[physics][bake][io]") {
    Rig rig(RigStart::Always, {true}, {"Walk"});
    rig.document.clips[0].readRule = ReadRule::Sc2;
    std::vector<u8> bytes = Writer().write(rig.document);
    // The v4 file an older writer made: the clip's last byte, its empty bake
    // optional, cut, the fill slid in behind it, and the chunk stamped v4.
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    bool cut = false;
    for (u32 i = 0; i < header.indexCount; ++i) {
        IndexEntry entry{};
        const std::size_t at = header.indexOffset + i * sizeof(IndexEntry);
        std::memcpy(&entry, bytes.data() + at, sizeof(entry));
        if (entry.tag != ChunkTagTraits<Clip>::value) {
            continue;
        }
        REQUIRE(entry.count == 1u);
        u32 end = header.indexOffset > entry.offset ? header.indexOffset : static_cast<u32>(bytes.size());
        for (u32 j = 0; j < header.indexCount; ++j) {
            IndexEntry other{};
            std::memcpy(&other, bytes.data() + header.indexOffset + j * sizeof(IndexEntry), sizeof(other));
            if (other.offset > entry.offset && other.offset < end) {
                end = other.offset;
            }
        }
        u32 last = end;
        while (last > entry.offset && bytes[last - 1] == 0xAA) {
            --last;
        }
        REQUIRE(last > entry.offset);
        REQUIRE(bytes[last - 1] == 0u);
        std::memmove(bytes.data() + last - 1, bytes.data() + last, end - last);
        bytes[end - 1] = 0xAA;
        entry.version = 4;
        std::memcpy(bytes.data() + at, &entry, sizeof(entry));
        cut = true;
    }
    REQUIRE(cut);
    const std::optional<Document> read = Parser().parse(std::span<const u8>(bytes.data(), bytes.size()));
    REQUIRE(read.has_value());
    REQUIRE(read->clips.size() == 1u);
    CHECK(read->clips[0].name == "Walk");
    CHECK(read->clips[0].readRule == ReadRule::Sc2);
    CHECK_FALSE(read->clips[0].physics.has_value());
}

TEST_CASE("a blast's node follows a node removal", "[physics][bake][io]") {
    Rig rig(RigStart::Always, {true, true}, {"Death"});
    BakeWorldForce blast;
    blast.kind = BakeWorldForce::Kind::Blast;
    blast.centreNode = 1;
    rig.document.clips[0].physics.emplace().world = {blast, blast};
    rig.document.clips[0].physics->world[1].centreNode = 0;
    std::vector<u32> remap = {kInvalidNode, 0};
    Diagnostics report;
    NodeReferencers referencers;
    referencers.clips = rig.document.clips;
    RemapNodeReferencers(rig.model().nodes, remap, referencers, report);
    CHECK(rig.document.clips[0].physics->world[0].centreNode == 0u);
    CHECK(rig.document.clips[0].physics->world[1].centreNode == kInvalidNode);
}

// ---- The reducer keeps what it is told to (§8.3) --------------------------------

TEST_CASE("a reduction keeps the keys it is told to keep", "[physics][bake][reduce]") {
    Rig rig(RigStart::Always, {true}, {"Walk"});
    Model& model = rig.model();
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = 0;
    channel.target.channel = Channel::Translation;
    channel.valueType = geom::AttrType::F32x3;
    model.animChannels.add(channel);
    // A straight line with a key in its middle that nothing needs.
    SubTrack line;
    line.channel = channel.id;
    line.interp = Interpolation::Linear;
    line.times = {0.0f, 0.5f, 1.0f};
    for (const f32 x : {0.0f, 5.0f, 10.0f}) {
        const Vector3f v{x, 0, 0};
        const u8* at = reinterpret_cast<const u8*>(&v);
        line.values.insert(line.values.end(), at, at + sizeof v);
    }
    rig.document.clips[0].containers.front().subTracks.push_back(line);
    const auto keysAfter = [&](bool keep) {
        Document copy = rig.document;
        KeyReduceOptions options;
        options.tolerance = 0.01f;
        options.threads = 1;
        if (keep) {
            options.kept.push_back(KeptKeys{0, channel.id, {0.5f}});
        }
        ReduceKeys(copy, 0, options);
        return copy.clips[0].containers.front().subTracks.front().times.size();
    };
    CHECK(keysAfter(false) == 2u);
    CHECK(keysAfter(true) == 3u);
}

// ---- The games' clip switches (§7.2) ---------------------------------------------

TEST_CASE("each game switches clips its own way", "[physics][bake][transition]") {
    CHECK(ClipTransitionOf(Game::Warcraft).curve == ClipTransition::Curve::Snapshot);
    CHECK(ClipTransitionOf(Game::Wow).curve == ClipTransition::Curve::Smoothstep);
    CHECK(ClipTransitionOf(Game::StarCraft).seconds == Approx(0.15f));
    const ClipTransition smooth{0.2f, ClipTransition::Curve::Smoothstep};
    CHECK(TransitionWeight(smooth, 0.1f) == Approx(0.5f));
    CHECK(TransitionWeight(smooth, 0.05f) == Approx(0.15625f));
    CHECK(TransitionWeight(ClipTransition{0.2f, ClipTransition::Curve::Linear}, 0.05f) == Approx(0.25f));
    CHECK(TransitionWeight(ClipTransition{0.0f, ClipTransition::Curve::Linear}, 0.0f) == 1.0f);
}

// ---- The run and the bake, on a host of the test's own (§6-§8) ----------------

namespace {

/// A host whose every simulated body swings its node about Z as a damped
/// pendulum from where it was let go: deterministic, and engine-free.
struct Swing {
    std::map<u32, std::pair<f32, f32>> state; ///< By body index: angle, speed.

    BakeHooks Hooks() {
        BakeHooks hooks;
        hooks.reset = [this](const Document&, u32, std::span<const u8>) { state.clear(); };
        hooks.step = [this](const Document& document, u32 model, const BakeStep& step, Pose& pose) {
            const PhysicsSet& physics = document.models[model].physics;
            for (u32 b = 0; b < physics.bodies.size() && b < step.switches.size(); ++b) {
                const u32 node = physics.bodies[b].node;
                if (!step.switches[b].active) {
                    state.erase(b);
                    continue;
                }
                auto [it, fresh] = state.try_emplace(b, 0.0f, 2.0f);
                auto& [angle, speed] = it->second;
                speed += (-30.0f * std::sin(angle) - 0.8f * speed) * step.dt;
                angle += speed * step.dt;
                const f32 blend = step.switches[b].blend;
                // About two axes, so turns taken on the left and on the right differ.
                const Quaternion swung = Quaternion::from_axis_angle(Vector3f{0, 0, 1}, angle) *
                                         Quaternion::from_axis_angle(Vector3f{1, 0, 0}, 0.5f * angle);
                pose.local[node].rotation = Quaternion::slerp(pose.local[node].rotation, swung, blend).normalized();
            }
            Animator(document, model).compose(pose);
        };
        return hooks;
    }
};

/// A root turning on Linear keys and a tail on it: the tail Dynamic in an
/// *Always* rig, the root Follows.
Rig Tailed(std::vector<std::string> clips, Interpolation rootInterp = Interpolation::Slerp) {
    Rig rig(RigStart::Always, {false, true}, std::move(clips));
    Model& model = rig.model();
    AnimChannel channel;
    channel.id = model.animChannels.nextFreeId();
    channel.target.kind = TrackTarget::Kind::Node;
    channel.target.node = 0;
    channel.target.channel = Channel::Rotation;
    channel.valueType = geom::AttrType::Quat;
    model.animChannels.add(channel);
    AnimChannel tail = channel;
    tail.id = model.animChannels.nextFreeId();
    tail.target.node = 1;
    model.animChannels.add(tail);
    for (Clip& clip : rig.document.clips) {
        SubTrack track;
        track.channel = channel.id;
        track.interp = rootInterp;
        track.times = {0.0f, 0.5f, 1.0f};
        for (const f32 a : {0.0f, 0.6f, 0.0f}) {
            const Quaternion q = Quaternion::from_axis_angle(Vector3f{1, 0, 0}, a);
            const u8* at = reinterpret_cast<const u8*>(&q);
            track.values.insert(track.values.end(), at, at + sizeof q);
        }
        clip.containers.front().subTracks.push_back(track);
        // The tail's own keys, which a bake splices around.
        SubTrack own;
        own.channel = tail.id;
        own.interp = Interpolation::Slerp;
        own.times = {0.0f, 0.25f, 0.75f, 1.0f};
        for (const f32 a : {0.0f, 0.2f, -0.2f, 0.0f}) {
            const Quaternion q = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, a);
            const u8* at = reinterpret_cast<const u8*>(&q);
            own.values.insert(own.values.end(), at, at + sizeof q);
        }
        clip.containers.front().subTracks.push_back(own);
    }
    return rig;
}

const SubTrack* TrackOf(const Clip& clip, const Model& model, u32 node, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node && entry.target.channel == channel) {
            return FindSubTrack(clip, entry.id);
        }
    }
    return nullptr;
}

Quaternion RotationAt(const Document& document, u32 clip, u32 node, f32 seconds) {
    Mix mix;
    mix.plays.push_back(Play{clip, seconds, 1.0f, false});
    Pose pose;
    Animator(document, 0).sample(mix, pose, true);
    return pose.local[node].rotation;
}

f32 Degrees(const Quaternion& a, const Quaternion& b) {
    return 2.0f * std::acos(std::min(std::abs(a.normalized().dot(b.normalized())), 1.0f)) * 57.2957795f;
}

} // namespace

TEST_CASE("G-B1 a run is the same run twice", "[physics][bake][run]") {
    Rig rig = Tailed({"Walk"});
    rig.document.clips[0].physics.emplace().preheat = 3;
    Swing a, b;
    const BakeRun first = SimulateClip(rig.document, 0, a.Hooks());
    const BakeRun second = SimulateClip(rig.document, 0, b.Hooks());
    REQUIRE(first.complete);
    CHECK(first.locals.size() == second.locals.size());
    CHECK(std::memcmp(first.locals.data(), second.locals.data(), first.locals.size() * sizeof(Transform)) == 0);
    CHECK(first.inputs == second.inputs);
    // Three loops ahead of the clip, its frame 0 at step 3 × its steps.
    CHECK(first.steps == 60u);
    CHECK(first.first == 180u);
    CHECK(first.times.front() == Approx(-3.0f));
    CHECK(first.nodes == std::vector<u32>{0, 1});
    CHECK(first.settling.size() == 3u);
}

TEST_CASE("a once-played clip runs after what it comes from", "[physics][bake][run]") {
    Rig rig = Tailed({"Stand", "Attack", "Spell"});
    rig.document.clips[1].looping = false;
    rig.document.clips[2].looping = false;
    rig.document.clips[1].physics.emplace().preheat = 2;
    Swing swing;
    const BakeRun attack = SimulateClip(rig.document, 1, swing.Hooks());
    REQUIRE(attack.complete);
    REQUIRE(attack.pieces.size() == 2u);
    CHECK(attack.pieces[0].clip == 0u);
    CHECK(attack.first == 120u);
    // With its first frame held instead: a second a preheat loop.
    rig.document.clips[1].physics->comesFrom = BakeFrom::FirstFrame;
    const BakeRun held = SimulateClip(rig.document, 1, swing.Hooks());
    CHECK(held.pieces.front().held);
    CHECK(held.first == 120u);
}

TEST_CASE("a corpse's clip starts where its death's own bake ended", "[physics][bake][run]") {
    // A Death that steps at another length than the Dissipate continuing it,
    // and preheats another number of times.
    Rig rig = Tailed({"Stand", "Death", "Dissipate"});
    std::vector<Clip>& clips = rig.document.clips;
    clips[1].looping = clips[2].looping = false;
    clips[1].duration = 0.99f;
    clips[1].physics.emplace().preheat = 3;
    clips[2].physics.emplace().preheat = 1;
    Swing swing;
    const BakeRun death = SimulateClip(rig.document, 1, swing.Hooks());
    const BakeRun corpse = SimulateClip(rig.document, 2, swing.Hooks());
    REQUIRE(death.complete);
    REQUIRE(corpse.complete);
    // Stand as the Death preheats it, the Death at its own step, then the clip.
    REQUIRE(corpse.pieces.size() == 3u);
    CHECK(corpse.first == death.first + death.steps);
    CHECK(corpse.times.front() == Approx(death.times.front() - 1.0f).margin(0.02));
    const std::size_t shared = static_cast<std::size_t>(corpse.first) * corpse.nodes.size();
    REQUIRE(death.locals.size() >= shared);
    CHECK(std::memcmp(death.locals.data(), corpse.locals.data(), shared * sizeof(Transform)) == 0);

    // Handed back at the switch, the tail leaves from where the simulation
    // left it, not from where the Death keys it, and is on its keys once the
    // switch is over.
    rig.key(2, rig.bodies[1], Channel::PhysicsDynamic, {0.0f}, {0.0f});
    const BakeRun handed = SimulateClip(rig.document, 2, swing.Hooks());
    REQUIRE(handed.complete);
    const Quaternion left = handed.localsAt(handed.first - 1)[1].rotation;
    REQUIRE(Degrees(left, RotationAt(rig.document, 1, 1, clips[1].duration)) > 0.5f);
    CHECK(Degrees(handed.localsAt(handed.first)[1].rotation, left) < 0.01f);
    const u32 over = handed.first + 12;
    CHECK(Degrees(handed.localsAt(over)[1].rotation, RotationAt(rig.document, 2, 1, handed.localTimes[over])) < 0.01f);
}

TEST_CASE("G-B2 the bake writes only where the body simulated", "[physics][bake][splice]") {
    // A Death whose ragdoll drops at 0.3 s: its root is keyed dynamic there.
    Rig rig = Tailed({"Death"});
    rig.model().physics.bodies[1].motion = BodyMotion::Kinematic;
    rig.document.clips[0].looping = false;
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.3f}, {0.0f, 1.0f});
    rig.document.clips[0].physics.emplace().tolerance = 0.0f;
    Swing swing;
    const BakeRun run = SimulateClip(rig.document, 0, swing.Hooks());
    REQUIRE(run.complete);
    const Document before = rig.document;
    const BakeReport report = BakeClip(rig.document, run);
    REQUIRE(report.ok);
    CHECK(report.nodes == 2u);
    const Clip& baked = rig.document.clips[0];
    REQUIRE(baked.physics->baked.has_value());
    // Every source key before the drop's edge is the source's, bit for bit.
    const SubTrack* source = TrackOf(before.clips[0], before.models[0], 1, Channel::Rotation);
    const SubTrack* now = TrackOf(baked, rig.model(), 1, Channel::Rotation);
    REQUIRE(source != nullptr);
    REQUIRE(now != nullptr);
    for (std::size_t k = 0; k < source->times.size() && source->times[k] <= 0.25f; ++k) {
        REQUIRE(k < now->times.size());
        CHECK(now->times[k] == source->times[k]);
        CHECK(std::memcmp(now->values.data() + k * sizeof(Quaternion), source->values.data() + k * sizeof(Quaternion),
                          sizeof(Quaternion)) == 0);
    }
    // Up to the drop the clip plays as it did.
    for (f32 t = 0.0f; t < 0.28f; t += 0.01f) {
        CHECK(Degrees(RotationAt(rig.document, 0, 1, t), RotationAt(before, 0, 1, t)) < 1e-3f);
    }
    // After it, the swing the run recorded.
    const Transform* late = run.localsAt(run.stepAt(0.8f));
    const auto nodeAt = std::find(run.nodes.begin(), run.nodes.end(), 1u) - run.nodes.begin();
    CHECK(Degrees(RotationAt(rig.document, 0, 1, 0.8f), late[nodeAt].rotation) < 0.05f);
    // Unbaked, the clip is its source again, to the byte.
    UnbakeClip(rig.document, 0);
    CHECK(ReflectBytes(rig.document.clips[0].containers) == ReflectBytes(before.clips[0].containers));
    CHECK(ReflectBytes(rig.model().animChannels) == ReflectBytes(before.models[0].animChannels));
    CHECK_FALSE(rig.document.clips[0].physics->baked.has_value());
}

TEST_CASE("G-B2 a Hermite source keeps its curve outside the span", "[physics][bake][splice]") {
    Rig rig = Tailed({"Death"});
    rig.model().physics.bodies[1].motion = BodyMotion::Kinematic;
    rig.document.clips[0].looping = false;
    // The root's translation, a Hermite curve, and the root dropped at 0.5 s.
    Model& model = rig.model();
    AnimChannel move;
    move.id = model.animChannels.nextFreeId();
    move.target.kind = TrackTarget::Kind::Node;
    move.target.node = 1;
    move.target.channel = Channel::Translation;
    move.valueType = geom::AttrType::F32x3;
    model.animChannels.add(move);
    SubTrack curve;
    curve.channel = move.id;
    curve.interp = Interpolation::Hermite;
    curve.times = {0.0f, 0.4f, 1.0f};
    const Vector3f values[] = {{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {2, 1, 0}, {1, 0, 0}, {0, 3, 0}, {0, 0, 1}, {2, 0, 0}, {0, 0, 0}};
    for (const Vector3f& v : values) {
        const u8* at = reinterpret_cast<const u8*>(&v);
        curve.values.insert(curve.values.end(), at, at + sizeof v);
    }
    rig.document.clips[0].containers.front().subTracks.push_back(curve);
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.5f}, {0.0f, 1.0f});
    rig.document.clips[0].physics.emplace().tolerance = 0.0f;
    // The swing moves the node: here, its translation too.
    Swing swing;
    BakeHooks hooks = swing.Hooks();
    const auto turn = hooks.step;
    hooks.step = [&](const Document& document, u32 m, const BakeStep& step, Pose& pose) {
        turn(document, m, step, pose);
        if (step.switches[1].active) {
            pose.local[1].translation = pose.local[1].translation + Vector3f{0, 0, -0.5f};
            Animator(document, m).compose(pose);
        }
    };
    const Document before = rig.document;
    const BakeRun run = SimulateClip(rig.document, 0, hooks);
    REQUIRE(BakeClip(rig.document, run).ok);
    const auto translationAt = [](const Document& document, f32 t) {
        Mix mix;
        mix.plays.push_back(Play{0, t, 1.0f, false});
        Pose pose;
        Animator(document, 0).sample(mix, pose, true);
        return pose.local[1].translation;
    };
    for (f32 t = 0.0f; t < 0.48f; t += 0.005f) {
        CHECK((translationAt(rig.document, t) - translationAt(before, t)).length() < 1e-5f);
    }
    CHECK((translationAt(rig.document, 0.8f) - translationAt(before, 0.8f)).length() > 0.4f);
}

TEST_CASE("G-B3 a loop's seam meets in place each way of matching it", "[physics][bake][seam]") {
    for (const MatchWhere where : {MatchWhere::End, MatchWhere::Start, MatchWhere::Both}) {
        for (const LoopMatch match : {LoopMatch::Crossfade, LoopMatch::Offset}) {
            Rig rig = Tailed({"Stand"});
            ClipPhysics& settings = rig.document.clips[0].physics.emplace();
            settings.where = where;
            settings.match = match;
            settings.tolerance = 0.0f;
            settings.preheat = 2;
            Swing swing;
            const BakeRun run = SimulateClip(rig.document, 0, swing.Hooks());
            const BakeReport report = BakeClip(rig.document, run);
            REQUIRE(report.ok);
            INFO("where " << static_cast<int>(where) << " match " << static_cast<int>(match));
            CHECK(report.seamBefore > 1.0f);
            CHECK(report.seamAfter < 1e-3f);
            if (match != LoopMatch::Crossfade) {
                continue;
            }
            // And in speed: the step over the seam is no bigger than a step
            // inside the clip.
            const f32 dt = run.dt;
            f32 interior = 0.0f;
            for (f32 t = dt; t + dt <= 1.0f; t += dt) {
                interior = std::max(interior, Degrees(RotationAt(rig.document, 0, 1, t), RotationAt(rig.document, 0, 1, t + dt)));
            }
            const f32 across = Degrees(RotationAt(rig.document, 0, 1, 1.0f - dt), RotationAt(rig.document, 0, 1, dt));
            CHECK(across <= 2.0f * interior * 1.5f);
        }
    }
}

TEST_CASE("G-B4 preheat settles the loop", "[physics][bake][preheat]") {
    Rig rig = Tailed({"Stand"});
    ClipPhysics& settings = rig.document.clips[0].physics.emplace();
    settings.preheat = 5;
    settings.match = LoopMatch::Off;
    Swing swing;
    const BakeRun run = SimulateClip(rig.document, 0, swing.Hooks());
    REQUIRE(run.settling.size() == 5u);
    CHECK(run.settling.back() < run.settling.front());
    settings.preheat = 0;
    const BakeRun cold = SimulateClip(rig.document, 0, swing.Hooks());
    Document a = rig.document, b = rig.document;
    a.clips[0].physics->preheat = 5;
    const BakeReport warmed = BakeClip(a, run);
    const BakeReport raw = BakeClip(b, cold);
    CHECK(warmed.seamBefore < raw.seamBefore);
}

TEST_CASE("a bake is redone the same and knows when it is out of date", "[physics][bake][record]") {
    Rig rig = Tailed({"Stand"});
    rig.document.clips[0].physics.emplace();
    Swing swing;
    const BakeRun run = SimulateClip(rig.document, 0, swing.Hooks());
    REQUIRE(BakeClip(rig.document, run).ok);
    const std::vector<u8> once = ReflectBytes(rig.document.clips[0]);
    // Rebaking from the same run puts back the source first.
    REQUIRE(BakeClip(rig.document, run).ok);
    CHECK(ReflectBytes(rig.document.clips[0]) == once);
    // The baked keys are not an input: a run of the baked clip is the same run.
    CHECK(BakeInputs(rig.document, 0) == run.inputs);
    CHECK(SimulateClip(rig.document, 0, swing.Hooks()).locals.size() == run.locals.size());
    rig.model().physics.bodies[1].linearDamping = 0.5f;
    CHECK(BakeInputs(rig.document, 0) != run.inputs);
}

TEST_CASE("a bake reduces only its own keys and keeps the source's", "[physics][bake][reduce]") {
    Rig rig = Tailed({"Death"});
    rig.model().physics.bodies[1].motion = BodyMotion::Kinematic;
    rig.document.clips[0].looping = false;
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.6f}, {0.0f, 1.0f});
    rig.document.clips[0].physics.emplace().tolerance = 0.001f;
    Swing swing;
    const BakeRun run = SimulateClip(rig.document, 0, swing.Hooks());
    const Document before = rig.document;
    const BakeReport reduced = BakeClip(rig.document, run);
    REQUIRE(reduced.ok);
    Document everyStep = before;
    everyStep.clips[0].physics->tolerance = 0.0f;
    const BakeReport full = BakeClip(everyStep, run);
    CHECK(reduced.keys < full.keys);
    // The tail's source keys before the span survive the reduction.
    const SubTrack* now = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    REQUIRE(now != nullptr);
    CHECK(std::count(now->times.begin(), now->times.end(), 0.25f) == 1);
    CHECK(std::count(now->times.begin(), now->times.end(), 0.0f) == 1);
}

namespace {

/// @p rig's node channel for @p node's rotation.
u32 RotationChannel(Rig& rig, u32 node) {
    for (const AnimChannel& channel : rig.model().animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == node &&
            channel.target.channel == Channel::Rotation) {
            return channel.id;
        }
    }
    return kInvalidIndex;
}

/// @p clip's track of @p channel, every key set to @p radians about @p axis.
void Hold(Clip& clip, u32 channel, Vector3f axis, f32 radians) {
    for (SubTrack& track : clip.containers.front().subTracks) {
        if (track.channel != channel) {
            continue;
        }
        const Quaternion q = Quaternion::from_axis_angle(axis, radians);
        for (std::size_t k = 0; k < track.times.size(); ++k) {
            std::memcpy(track.values.data() + k * sizeof q, &q, sizeof q);
        }
    }
}

} // namespace

TEST_CASE("a bake keeps its channel ids through a Rebake and the last Unbake drops them", "[physics][bake][record]") {
    // The tail has no channel: Stand's bake declares one and Walk's keys it too.
    Rig rig = Tailed({"Stand", "Walk", "Idle"});
    const u32 tail = RotationChannel(rig, 1);
    std::erase_if(rig.model().animChannels.channels, [&](const AnimChannel& channel) { return channel.id == tail; });
    for (Clip& clip : rig.document.clips) {
        std::erase_if(clip.containers.front().subTracks, [&](const SubTrack& track) { return track.channel == tail; });
        clip.physics.emplace().tolerance = 0.0f;
    }
    // Idle holds the tail still, so its bake keys nothing.
    rig.key(2, rig.bodies[1], Channel::PhysicsDynamic, {0.0f}, {0.0f});
    Swing swing;
    const auto bake = [&](u32 clip) { REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, clip, swing.Hooks())).ok); };
    const auto upToDate = [](const Document& document, u32 clip) {
        return document.clips[clip].physics->baked->inputs == BakeInputs(document, clip);
    };
    bake(0);
    // A channel declared since, as a K elsewhere declares one.
    const u32 later = PhysicsSwitchChannel(rig.model(), rig.bodies[0], Channel::PhysicsDynamic);
    // A Rebake keys the ids it had: the table only grows, as a journal of
    // appends needs.
    const std::vector<u8> table = ReflectBytes(rig.model().animChannels);
    bake(0);
    CHECK(ReflectBytes(rig.model().animChannels) == table);
    bake(1);
    CHECK(ReflectBytes(rig.model().animChannels) == table);
    bake(2);
    CHECK(rig.document.clips[2].physics->baked->channels.empty());
    CHECK(upToDate(rig.document, 0));
    CHECK(upToDate(rig.document, 1));
    CHECK(upToDate(rig.document, 2));
    // Unbakes that keep their channels leave the other bakes up to date, a
    // channel that nothing keys any more included.
    Document kept = rig.document;
    UnbakeClip(kept, 0, false);
    CHECK(upToDate(kept, 1));
    UnbakeClip(kept, 1, false);
    CHECK(upToDate(kept, 2));
    // The last Unbake takes the tail's channel with it.
    UnbakeClip(rig.document, 0);
    CHECK(RotationChannel(rig, 1) != kInvalidIndex);
    UnbakeClip(rig.document, 1);
    CHECK(RotationChannel(rig, 1) == kInvalidIndex);
    CHECK(rig.model().animChannels.find(later) != nullptr);
}

TEST_CASE("a Rebake of what a clip goes to makes it out of date", "[physics][bake][record]") {
    Rig rig = Tailed({"Stand", "Attack"});
    rig.document.clips[1].looping = false;
    for (Clip& clip : rig.document.clips) {
        clip.physics.emplace();
    }
    Swing swing;
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 1, swing.Hooks())).ok);
    const u64 attack = rig.document.clips[1].physics->baked->inputs;
    CHECK(BakeInputs(rig.document, 1) == attack);
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    CHECK(BakeInputs(rig.document, 1) != attack);
}

namespace {

/// @p clip's track of @p channel, to key by hand as Animations does.
SubTrack* Keys(Clip& clip, u32 channel) {
    for (SubTrack& track : clip.containers.front().subTracks) {
        if (track.channel == channel) {
            return &track;
        }
    }
    return nullptr;
}

/// A Death whose ragdoll drops at 0.3 s, baked with a key every step. Of the
/// tail's own keys, those at 0 and 0.25 s stand before the drop, and those at
/// 0.75 and 1 s are under what the bake keyed.
Rig BakedDeath(Swing& swing) {
    Rig rig = Tailed({"Death"});
    rig.model().physics.bodies[1].motion = BodyMotion::Kinematic;
    rig.document.clips[0].looping = false;
    rig.key(0, rig.rig, Channel::PhysicsRagdoll, {0.0f, 0.3f}, {0.0f, 1.0f});
    rig.document.clips[0].physics.emplace().tolerance = 0.0f;
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    return rig;
}

/// @p record as a build before the stretches wrote it.
void ForgetStretches(PhysicsBake& record) {
    record.stretchChannels.clear();
    record.stretchCounts.clear();
    record.stretchTimes.clear();
    record.stretchOutside.clear();
    record.stretchInside = 0;
}

} // namespace

TEST_CASE("a key set beside a bake's keys stays through a Rebake and an Unbake", "[physics][bake][record]") {
    Swing swing;
    Rig rig = BakedDeath(swing);
    const u32 tail = RotationChannel(rig, 1);
    Document source = rig.document;
    UnbakeClip(source, 0);
    CHECK(BakeInDate(rig.document, 0));
    CHECK_FALSE(BakedKeysEdited(rig.document, 0));
    // In Animations, on the baked clip: the tail's first key turned.
    const Quaternion turned = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.5f);
    SubTrack* keys = Keys(rig.document.clips[0], tail);
    REQUIRE(keys != nullptr);
    REQUIRE(keys->times.front() == 0.0f);
    std::memcpy(keys->values.data(), &turned, sizeof turned);
    // The clip's own key and none of the bake's: nothing baked was edited,
    // and the bake no longer holds what the clip plays into it.
    CHECK_FALSE(BakedKeysEdited(rig.document, 0));
    CHECK_FALSE(BakeInDate(rig.document, 0));
    // Baked again it is as it was set, and the key after it the source's.
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    CHECK(BakeInDate(rig.document, 0));
    const SubTrack* now = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    REQUIRE(now != nullptr);
    CHECK(std::memcmp(now->values.data(), &turned, sizeof turned) == 0);
    CHECK(Degrees(RotationAt(rig.document, 0, 1, 0.25f), RotationAt(source, 0, 1, 0.25f)) < 1e-3f);
    // Unbaked: the source with that key, every other as it had it.
    UnbakeClip(rig.document, 0);
    const SubTrack* back = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    const SubTrack* was = TrackOf(source.clips[0], source.models[0], 1, Channel::Rotation);
    REQUIRE(back != nullptr);
    REQUIRE(was != nullptr);
    REQUIRE(back->times == was->times);
    CHECK(std::memcmp(back->values.data(), &turned, sizeof turned) == 0);
    CHECK(std::memcmp(back->values.data() + sizeof turned, was->values.data() + sizeof turned,
                      was->values.size() - sizeof turned) == 0);
    // The root's keys, which no one touched, to the byte.
    const SubTrack* root = TrackOf(rig.document.clips[0], rig.model(), 0, Channel::Rotation);
    const SubTrack* rootWas = TrackOf(source.clips[0], source.models[0], 0, Channel::Rotation);
    REQUIRE(root != nullptr);
    REQUIRE(rootWas != nullptr);
    CHECK(ReflectBytes(*root) == ReflectBytes(*rootWas));
}

TEST_CASE("a key changed where the animation leaves for the simulation holds to it", "[physics][bake][record]") {
    Swing swing;
    Rig rig = BakedDeath(swing);
    const u32 tail = RotationChannel(rig, 1);
    // The tail's last key before the drop, at 0.25 s, turned by hand. Under
    // the bake's keys the source goes on to its key at 0.75 s, which the
    // baked clip does not show.
    const Quaternion turned = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.5f);
    SubTrack* keys = Keys(rig.document.clips[0], tail);
    REQUIRE(keys != nullptr);
    REQUIRE(keys->times.size() > 2);
    REQUIRE(keys->times[1] == 0.25f);
    std::memcpy(keys->values.data() + sizeof turned, &turned, sizeof turned);
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    for (f32 t = 0.25f; t < 0.28f; t += 0.005f) {
        CHECK(Degrees(RotationAt(rig.document, 0, 1, t), turned) < 0.05f);
    }
    // The source holds it to the drop, and has what it had from there on.
    UnbakeClip(rig.document, 0);
    const SubTrack* back = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    REQUIRE(back != nullptr);
    REQUIRE(back->times.size() == 5u);
    CHECK(back->times[2] == Approx(0.3f));
    CHECK(std::memcmp(back->values.data() + 2 * sizeof turned, &turned, sizeof turned) == 0);
    CHECK(back->times[3] == 0.75f);
    // An earlier key changed leaves the way into the simulation as it was.
    Rig other = BakedDeath(swing);
    SubTrack* first = Keys(other.document.clips[0], RotationChannel(other, 1));
    REQUIRE(first != nullptr);
    std::memcpy(first->values.data(), &turned, sizeof turned);
    UnbakeClip(other.document, 0);
    CHECK(TrackOf(other.document.clips[0], other.model(), 1, Channel::Rotation)->times.size() == 4u);
}

TEST_CASE("a bake's keys taken away by hand leave nothing to put back under them", "[physics][bake][record]") {
    Swing swing;
    Rig rig = BakedDeath(swing);
    // Every key of the tail erased, and one set at the start.
    const Quaternion turned = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.5f);
    SubTrack* keys = Keys(rig.document.clips[0], RotationChannel(rig, 1));
    REQUIRE(keys != nullptr);
    keys->times = {0.0f};
    keys->values.resize(sizeof turned);
    std::memcpy(keys->values.data(), &turned, sizeof turned);
    UnbakeClip(rig.document, 0);
    const SubTrack* back = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    REQUIRE(back != nullptr);
    REQUIRE(back->times == std::vector<f32>{0.0f});
    CHECK(std::memcmp(back->values.data(), &turned, sizeof turned) == 0);
    // Only what the bake keyed erased, the keys before the drop as they were: the same.
    Rig other = BakedDeath(swing);
    SubTrack* rest = Keys(other.document.clips[0], RotationChannel(other, 1));
    REQUIRE(rest != nullptr);
    REQUIRE(rest->times.size() > 2);
    REQUIRE(rest->times[1] == 0.25f);
    rest->times.resize(2);
    rest->values.resize(2 * sizeof turned);
    UnbakeClip(other.document, 0);
    const SubTrack* kept = TrackOf(other.document.clips[0], other.model(), 1, Channel::Rotation);
    REQUIRE(kept != nullptr);
    CHECK(kept->times == std::vector<f32>{0.0f, 0.25f});
}

TEST_CASE("an older record's bone with its keys taken away is keyed from nothing", "[physics][bake][record][io]") {
    Swing swing;
    Rig rig = BakedDeath(swing);
    const u32 tail = RotationChannel(rig, 1);
    Document source = rig.document;
    UnbakeClip(source, 0);
    // As a build before the stretches left it: baked, then every key of the
    // tail erased on the baked clip, and read.
    ForgetStretches(*rig.document.clips[0].physics->baked);
    std::erase_if(rig.document.clips[0].containers.front().subTracks,
                  [&](const SubTrack& track) { return track.channel == tail; });
    MarkBakeStretches(rig.document);
    CHECK(BakedKeysEdited(rig.document, 0));
    // One key set at the start, as Animations sets one on a bone with none.
    const Quaternion turned = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.5f);
    SubTrack key;
    key.channel = tail;
    key.interp = Interpolation::Slerp;
    key.times = {0.0f};
    key.values.resize(sizeof turned);
    std::memcpy(key.values.data(), &turned, sizeof turned);
    rig.document.clips[0].containers.front().subTracks.push_back(key);
    // Baked, it plays that key up to the drop, and none of what was erased.
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    for (const f32 t : {0.0f, 0.125f, 0.25f}) {
        CHECK(Degrees(RotationAt(rig.document, 0, 1, t), turned) < 0.05f);
    }
    CHECK_FALSE(BakedKeysEdited(rig.document, 0));
    UnbakeClip(rig.document, 0);
    const SubTrack* back = TrackOf(rig.document.clips[0], rig.model(), 1, Channel::Rotation);
    REQUIRE(back != nullptr);
    REQUIRE(back->times == std::vector<f32>{0.0f});
    CHECK(std::memcmp(back->values.data(), &turned, sizeof turned) == 0);
    // The root, which no one touched, is its source's to the byte.
    const SubTrack* root = TrackOf(rig.document.clips[0], rig.model(), 0, Channel::Rotation);
    const SubTrack* rootWas = TrackOf(source.clips[0], source.models[0], 0, Channel::Rotation);
    REQUIRE(root != nullptr);
    REQUIRE(rootWas != nullptr);
    CHECK(ReflectBytes(*root) == ReflectBytes(*rootWas));
}

TEST_CASE("a record made before the stretches were kept gets them when it is read", "[physics][bake][record][io]") {
    Swing swing;
    Rig rig = BakedDeath(swing);
    Document source = rig.document;
    UnbakeClip(source, 0);
    ForgetStretches(*rig.document.clips[0].physics->baked);
    const std::vector<u8> bytes = Writer().write(rig.document);
    const auto read = [&] { return Parser().parse(std::span<const u8>(bytes.data(), bytes.size())); };
    std::optional<Document> document = read();
    REQUIRE(document.has_value());
    const PhysicsBake& record = *document->clips[0].physics->baked;
    CHECK(record.stretchInside != 0);
    CHECK(record.stretchChannels.size() == 2u);
    CHECK_FALSE(BakedKeysEdited(*document, 0));
    // Untouched, it unbakes to the byte.
    Document plain = *document;
    UnbakeClip(plain, 0);
    CHECK(ReflectBytes(plain.clips[0].containers) == ReflectBytes(source.clips[0].containers));
    // A key set beside the bake's keys is the clip's own, as in a record made now.
    const Quaternion turned = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.5f);
    u32 tail = kInvalidIndex;
    for (const AnimChannel& channel : document->models[0].animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node && channel.target.node == 1 &&
            channel.target.channel == Channel::Rotation) {
            tail = channel.id;
        }
    }
    SubTrack* keys = Keys(document->clips[0], tail);
    REQUIRE(keys != nullptr);
    std::memcpy(keys->values.data(), &turned, sizeof turned);
    CHECK_FALSE(BakedKeysEdited(*document, 0));
    UnbakeClip(*document, 0);
    const SubTrack* back = TrackOf(document->clips[0], document->models[0], 1, Channel::Rotation);
    const SubTrack* was = TrackOf(source.clips[0], source.models[0], 1, Channel::Rotation);
    REQUIRE(back != nullptr);
    REQUIRE(back->times == was->times);
    CHECK(std::memcmp(back->values.data(), &turned, sizeof turned) == 0);
    CHECK(std::memcmp(back->values.data() + sizeof turned, was->values.data() + sizeof turned,
                      was->values.size() - sizeof turned) == 0);
    // One whose baked keys were edited before the read: they read as the
    // bake's, the record as edited, and it unbakes to its source.
    Document edited = rig.document;
    SubTrack* late = Keys(edited.clips[0], RotationChannel(rig, 1));
    REQUIRE(late != nullptr);
    std::memcpy(late->values.data() + (late->times.size() - 1) * sizeof turned, &turned, sizeof turned);
    const std::vector<u8> editedBytes = Writer().write(edited);
    std::optional<Document> again = Parser().parse(std::span<const u8>(editedBytes.data(), editedBytes.size()));
    REQUIRE(again.has_value());
    CHECK(again->clips[0].physics->baked->stretchInside != 0);
    CHECK(BakedKeysEdited(*again, 0));
    UnbakeClip(*again, 0);
    CHECK(ReflectBytes(again->clips[0].containers) == ReflectBytes(source.clips[0].containers));
}

TEST_CASE("where a bake keyed is in no other clip's key", "[physics][bake][record]") {
    Rig rig = Tailed({"Stand", "Attack"});
    rig.document.clips[1].looping = false;
    for (Clip& clip : rig.document.clips) {
        clip.physics.emplace();
    }
    Swing swing;
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 1, swing.Hooks())).ok);
    REQUIRE(BakeInDate(rig.document, 1));
    // Attack is matched into Stand's record as an older build wrote it, too.
    ForgetStretches(*rig.document.clips[0].physics->baked);
    CHECK(BakeInDate(rig.document, 1));
    MarkBakeStretches(rig.document);
    CHECK(rig.document.clips[0].physics->baked->stretchInside != 0);
    CHECK(BakeInDate(rig.document, 1));
    CHECK(BakeInDate(rig.document, 0));
}

TEST_CASE("a clip that goes to one played once meets its frame 0", "[physics][bake][seam]") {
    Rig rig = Tailed({"Attack", "Spell"});
    for (Clip& clip : rig.document.clips) {
        clip.looping = false;
    }
    ClipPhysics& settings = rig.document.clips[0].physics.emplace();
    settings.goesTo = BakeTo::Clip;
    settings.goesToClip = "Spell";
    settings.tolerance = 0.0f;
    // Spell's tail ends turned from where it starts.
    const u32 tail = RotationChannel(rig, 1);
    for (SubTrack& track : rig.document.clips[1].containers.front().subTracks) {
        if (track.channel == tail) {
            const Quaternion q = Quaternion::from_axis_angle(Vector3f{0, 1, 0}, 0.6f);
            std::memcpy(track.values.data() + (track.times.size() - 1) * sizeof q, &q, sizeof q);
        }
    }
    REQUIRE(Degrees(RotationAt(rig.document, 1, 1, 0.0f), RotationAt(rig.document, 1, 1, 1.0f)) > 10.0f);
    Swing swing;
    REQUIRE(BakeClip(rig.document, SimulateClip(rig.document, 0, swing.Hooks())).ok);
    CHECK(Degrees(RotationAt(rig.document, 0, 1, 1.0f), RotationAt(rig.document, 1, 1, 0.0f)) < 0.05f);
}

TEST_CASE("a loop that comes from another clip is offset rather than cross-faded", "[physics][bake][seam]") {
    Rig rig = Tailed({"Stand", "Walk"});
    ClipPhysics& settings = rig.document.clips[1].physics.emplace();
    settings.comesFrom = BakeFrom::Clip;
    settings.comesFromClip = "Stand";
    settings.tolerance = 0.0f;
    Swing swing;
    const BakeReport report = BakeClip(rig.document, SimulateClip(rig.document, 1, swing.Hooks()));
    REQUIRE(report.ok);
    CHECK(report.offsetInstead);
    CHECK(report.seamAfter < 1e-3f);
}

TEST_CASE("the switch into a clip is never in its keys", "[physics][bake][splice]") {
    // The root swings in the middle of Stand, which Attack comes from, and
    // Stand holds it turned where Attack does not: the game's switch blends
    // it back over Attack's start, which is the game's to do.
    Rig rig = Tailed({"Stand", "Attack"});
    rig.document.clips[1].looping = false;
    rig.key(0, rig.bodies[0], Channel::PhysicsDynamic, {0.0f, 0.2f, 0.4f}, {0.0f, 1.0f, 0.0f});
    Hold(rig.document.clips[0], RotationChannel(rig, 0), Vector3f{1, 0, 0}, 0.6f);
    ClipPhysics& settings = rig.document.clips[1].physics.emplace();
    settings.tolerance = 0.0f;
    settings.goesTo = BakeTo::Nothing; // its end is the match's, not the switch's
    const Document before = rig.document;
    Swing swing;
    const BakeRun run = SimulateClip(rig.document, 1, swing.Hooks());
    REQUIRE(std::find(run.nodes.begin(), run.nodes.end(), 0u) != run.nodes.end());
    REQUIRE(BakeClip(rig.document, run).ok);
    for (f32 t = 0.0f; t <= 1.0f; t += 0.01f) {
        CHECK(Degrees(RotationAt(rig.document, 1, 0, t), RotationAt(before, 1, 0, t)) < 0.05f);
    }
}
