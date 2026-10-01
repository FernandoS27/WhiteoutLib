// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/switches.h>

#include <whiteout/models/wem/anim/track_read.h>
#include <whiteout/models/wem/physics/crossing.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// A clip chain deeper than this is a cycle, whatever it names.
constexpr u32 kMaxChain = 16;

bool Holds(const PhysicsRig& rig, u32 body) {
    return std::find(rig.bodies.begin(), rig.bodies.end(), body) != rig.bodies.end();
}

/// @p clip's keyed track of @p channel, or null.
const SubTrack* Keyed(const Clip* clip, u32 channel) {
    if (clip == nullptr || channel == kInvalidIndex) {
        return nullptr;
    }
    const SubTrack* track = FindSubTrack(*clip, channel);
    return track != nullptr && !track->times.empty() ? track : nullptr;
}

f32 Sample(const Clip& clip, const SubTrack& track, f32 seconds, f32 fallback, bool held) {
    // Read over the clip, never wrapped: a StarCraft II loop wraps each track
    // at its own last key, which would turn a switch left on back off.
    SampleWindow window = ClipWindow(clip, Milliseconds(seconds), -1);
    window.loop = false;
    const std::vector<u8> value =
        SampleSubTrack(clip, track, geom::AttrType::F32, window,
                       std::span<const u8>(reinterpret_cast<const u8*>(&fallback), sizeof fallback), held);
    f32 out = fallback;
    if (value.size() == sizeof(f32)) {
        std::memcpy(&out, value.data(), sizeof(f32));
    }
    return out;
}

bool NamedWord(std::string_view name, std::string_view word) {
    if (name.size() < word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < word.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != word[i]) {
            return false;
        }
    }
    return name.size() == word.size() || name[word.size()] == ' ';
}

/// @p name's words, lower case: split at spaces and where a lower-case letter
/// meets an upper-case one.
std::vector<std::string> Words(std::string_view name) {
    std::vector<std::string> out;
    std::string word;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        const bool split = std::isspace(c) != 0 || c == '_';
        const bool upper = i > 0 && std::isupper(c) != 0 && std::islower(static_cast<unsigned char>(name[i - 1])) != 0;
        if ((split || upper) && !word.empty()) {
            out.push_back(word);
            word.clear();
        }
        if (!split) {
            word.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    if (!word.empty()) {
        out.push_back(word);
    }
    return out;
}

/// Whether @p rig is dropped in @p clip where the clip does not key it:
/// carried from what it comes from, or an *On death* default. A clip carries
/// only when it is set up or runs in a set-up clip's lead-in (@p chained), so
/// a document with no bake settings carries nothing.
bool DefaultDropped(const Document& document, u32 clip, const PhysicsRig& rig, u32 depth, bool chained,
                    SwitchWhy* why);

/// Whether @p rig is dropped at the end of @p clip, a lead-in.
bool EndsDropped(const Document& document, u32 clip, const PhysicsRig& rig, u32 depth) {
    const Clip& owner = document.clips[clip];
    const Model& model = document.models[owner.model];
    if (const SubTrack* track = Keyed(&owner, FindPhysicsChannel(model, rig.id, Channel::PhysicsRagdoll))) {
        return Sample(owner, *track, owner.duration, 0.0f, true) >= 0.5f;
    }
    return DefaultDropped(document, clip, rig, depth, true, nullptr);
}

bool DefaultDropped(const Document& document, u32 clip, const PhysicsRig& rig, u32 depth, bool chained,
                    SwitchWhy* why) {
    if (rig.start == RigStart::Never || clip >= document.clips.size() || depth > kMaxChain) {
        return false;
    }
    const Clip& owner = document.clips[clip];
    if (chained || owner.physics.has_value()) {
        const BakeLeadIn from = BakeComesFrom(document, clip);
        if (from.kind == BakeFrom::Clip && from.clip != clip && EndsDropped(document, from.clip, rig, depth + 1)) {
            if (why != nullptr) {
                *why = SwitchWhy::Carried;
            }
            return true;
        }
    }
    if (rig.start == RigStart::OnDeath && IsDeathClipName(owner.name)) {
        if (why != nullptr) {
            *why = SwitchWhy::Death;
        }
        return true;
    }
    return false;
}

/// The model's first looping clip whose base name is @p base.
u32 FirstLoopNamed(const Document& document, u32 model, std::string_view base) {
    for (u32 c = 0; c < document.clips.size(); ++c) {
        const Clip& clip = document.clips[c];
        if (clip.model == model && BakeLoops(clip) && BaseSequenceName(clip.name) == base) {
            return c;
        }
    }
    return kInvalidIndex;
}

} // namespace

f32 PhysicsSwitchRest(const Model& model, const TrackTarget& target) {
    const PhysicsSet& physics = model.physics;
    switch (target.channel) {
    case Channel::PhysicsDynamic: {
        const PhysicsBody* body = physics.body(target.sub);
        if (body == nullptr || body->motion != BodyMotion::Dynamic || !body->simulates) {
            return 0.0f;
        }
        const bool runs = std::any_of(physics.rigs.begin(), physics.rigs.end(), [&](const PhysicsRig& rig) {
            return RigRuns(rig.start) && Holds(rig, body->id);
        });
        return runs ? 1.0f : 0.0f;
    }
    case Channel::ClothActive: {
        const Cloth* cloth = physics.cloth(target.sub);
        return cloth != nullptr && cloth->active ? 1.0f : 0.0f;
    }
    case Channel::PhysicsBlend:
        return 1.0f;
    default:
        return 0.0f;
    }
}

u32 FindPhysicsChannel(const Model& model, u32 record, Channel channel) {
    for (const AnimChannel& entry : model.animChannels.channels) {
        if (entry.target.kind == TrackTarget::Kind::Physics && entry.target.sub == record &&
            entry.target.channel == channel) {
            return entry.id;
        }
    }
    return kInvalidIndex;
}

SwitchReader::SwitchReader(const Document& document, u32 model, u32 clip, SwitchScope scope, bool chained) {
    if (model >= document.models.size()) {
        return;
    }
    document_ = &document;
    clip_ = clip < document.clips.size() && document.clips[clip].model == model ? &document.clips[clip] : nullptr;
    baked_ = clip_ != nullptr && scope != SwitchScope::Simulate && clip_->physics.has_value() &&
             clip_->physics->baked.has_value();
    const Model& owner = document.models[model];
    const PhysicsSet& physics = owner.physics;
    const NodeTree& tree = owner.nodes;

    rigs_.resize(physics.rigs.size());
    for (u32 r = 0; r < physics.rigs.size(); ++r) {
        const PhysicsRig& rig = physics.rigs[r];
        Rig& out = rigs_[r];
        out.never = rig.start == RigStart::Never;
        if (out.never) {
            continue;
        }
        out.ragdoll = Keyed(clip_, FindPhysicsChannel(owner, rig.id, Channel::PhysicsRagdoll));
        out.blend = Keyed(clip_, FindPhysicsChannel(owner, rig.id, Channel::PhysicsBlend));
        if (clip_ != nullptr) {
            out.fallback = DefaultDropped(document, clip, rig, 0, chained, &out.fallbackWhy);
        }
    }

    // Which bodies some clip keys: a loose one moves only there.
    const auto keyedSomewhere = [&](u32 channel) {
        return channel != kInvalidIndex && std::any_of(document.clips.begin(), document.clips.end(), [&](const Clip& c) {
                   return c.model == model && Keyed(&c, channel) != nullptr;
               });
    };
    bodies_.resize(physics.bodies.size());
    for (u32 b = 0; b < physics.bodies.size(); ++b) {
        const PhysicsBody& body = physics.bodies[b];
        Body& out = bodies_[b];
        bool held = false, heldRunning = false;
        for (u32 r = 0; r < physics.rigs.size(); ++r) {
            if (!Holds(physics.rigs[r], body.id)) {
                continue;
            }
            held = true;
            out.rigs.push_back(r);
            heldRunning = heldRunning || (scope == SwitchScope::Test ? !rigs_[r].never : RigRuns(physics.rigs[r].start));
        }
        const bool onlyNever = held && std::all_of(out.rigs.begin(), out.rigs.end(), [&](u32 r) { return rigs_[r].never; });
        out.still = body.motion == BodyMotion::Static || onlyNever;
        // The Test runs a body no rig holds, as it always has.
        const bool runs = heldRunning || (!held && scope == SwitchScope::Test);
        out.rest = runs && body.motion == BodyMotion::Dynamic && body.simulates;
        out.exempt = body.exemptFromRagdoll;
        out.inherit = body.inheritDynamic;
        out.dynamic = Keyed(clip_, FindPhysicsChannel(owner, body.id, Channel::PhysicsDynamic));
        out.blend = Keyed(clip_, FindPhysicsChannel(owner, body.id, Channel::PhysicsBlend));
        for (u32 at = body.node < tree.size() ? tree.nodes[body.node].parent : kInvalidNode;
             at < tree.size() && out.ancestor == kInvalidIndex; at = tree.nodes[at].parent) {
            for (u32 k = 0; k < physics.bodies.size(); ++k) {
                if (physics.bodies[k].node == at) {
                    out.ancestor = k;
                    break;
                }
            }
            if (at == tree.nodes[at].parent) {
                break;
            }
        }
        out.movable = !out.still && (held || scope == SwitchScope::Test ||
                                     keyedSomewhere(FindPhysicsChannel(owner, body.id, Channel::PhysicsDynamic)));
    }
}

f32 SwitchReader::sample(const SubTrack* track, f32 seconds, f32 fallback, bool held) const {
    return track != nullptr && clip_ != nullptr ? Sample(*clip_, *track, seconds, fallback, held) : fallback;
}

bool SwitchReader::dropped(u32 rig, f32 seconds, SwitchWhy* why) const {
    if (rig >= rigs_.size() || rigs_[rig].never) {
        return false;
    }
    const Rig& r = rigs_[rig];
    if (r.ragdoll != nullptr) {
        if (why != nullptr) {
            *why = SwitchWhy::Dropped;
        }
        return sample(r.ragdoll, seconds, 0.0f, true) >= 0.5f;
    }
    if (why != nullptr) {
        *why = r.fallbackWhy;
    }
    return r.fallback;
}

f32 SwitchReader::rigBlend(u32 rig, f32 seconds) const {
    return rig < rigs_.size() ? std::clamp(sample(rigs_[rig].blend, seconds, 1.0f, false), 0.0f, 1.0f) : 1.0f;
}

bool SwitchReader::keysBody(u32 body) const {
    return body < bodies_.size() && bodies_[body].dynamic != nullptr;
}

bool SwitchReader::keysRig(u32 rig) const {
    return rig < rigs_.size() && rigs_[rig].ragdoll != nullptr;
}

bool SwitchReader::movable(u32 body) const {
    return body < bodies_.size() && bodies_[body].movable;
}

void SwitchReader::read(f32 seconds, std::span<const BodySwitch> previous, std::vector<BodySwitch>& out) const {
    out.assign(bodies_.size(), BodySwitch{});
    if (baked_) {
        return;
    }
    for (u32 i = 0; i < bodies_.size(); ++i) {
        const Body& body = bodies_[i];
        BodySwitch& now = out[i];
        if (body.still) {
            continue;
        }
        // Dropped by a rig holding it, unless it ignores the drop.
        u32 dropper = kInvalidIndex;
        SwitchWhy dropWhy = SwitchWhy::Still;
        bool ignored = false;
        for (const u32 r : body.rigs) {
            SwitchWhy why = SwitchWhy::Still;
            if (dropped(r, seconds, &why)) {
                if (body.exempt) {
                    ignored = true;
                    break;
                }
                dropper = r;
                dropWhy = why;
                break;
            }
        }
        // Its own state: an ancestor's, its key, or its State where its
        // ragdoll runs.
        bool own = false;
        SwitchWhy ownWhy = SwitchWhy::Rest;
        if (body.inherit) {
            ownWhy = SwitchWhy::Inherited;
            if (body.ancestor < i) {
                own = out[body.ancestor].active;
            } else if (body.ancestor < previous.size()) {
                own = previous[body.ancestor].active;
            }
        } else if (body.dynamic != nullptr) {
            own = sample(body.dynamic, seconds, 0.0f, true) >= 0.5f;
            ownWhy = SwitchWhy::Keyed;
        } else {
            own = body.rest;
        }
        const bool forced = dropper != kInvalidIndex;
        now.active = forced || own;
        if (!now.active) {
            now.why = ignored ? SwitchWhy::Exempt : (body.dynamic != nullptr ? SwitchWhy::Keyed : SwitchWhy::Still);
            continue;
        }
        // A body moving by its own switch keeps its own blend through its
        // ragdoll's blend-out; one moving because it was dropped takes the rig's.
        if (forced && !own) {
            now.blend = rigBlend(dropper, seconds);
            now.why = dropWhy;
        } else {
            now.blend = std::clamp(sample(body.blend, seconds, 1.0f, false), 0.0f, 1.0f);
            now.why = ownWhy;
        }
    }
}

void PhysicsSwitches(const Document& document, u32 model, u32 clip, f32 seconds, SwitchScope scope,
                     std::span<const BodySwitch> previous, std::vector<BodySwitch>& out) {
    SwitchReader(document, model, clip, scope).read(seconds, previous, out);
}

std::string BaseSequenceName(std::string_view name) {
    std::string base(name.substr(0, name.find('-')));
    const auto space = [&base](std::size_t i) { return std::isspace(static_cast<unsigned char>(base[i])) != 0; };
    std::size_t end = base.size();
    while (end > 0 && space(end - 1)) {
        --end;
    }
    std::size_t digits = end;
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(base[digits - 1]))) {
        --digits;
    }
    if (digits < end && digits > 0 && space(digits - 1)) {
        end = digits;
        while (end > 0 && space(end - 1)) {
            --end;
        }
    }
    std::size_t begin = 0;
    while (begin < end && space(begin)) {
        ++begin;
    }
    return base.substr(begin, end - begin);
}

bool IsDecayClipName(std::string_view name) {
    return NamedWord(name, "decay");
}

bool PlaysOnce(std::string_view name) {
    const std::vector<std::string> words = Words(BaseSequenceName(name));
    if (words.empty()) {
        return false;
    }
    const std::string& first = words.front();
    const std::string second = words.size() > 1 ? words[1] : std::string();
    if (first == "attack" || first == "death" || first == "decay" || first == "birth" || first == "dissipate" ||
        first == "morph") {
        return true;
    }
    if (first == "spell") {
        return second != "channel";
    }
    return first == "stand" && second == "hit";
}

bool BakeLoops(const Clip& clip) {
    const BakePlays plays = clip.physics.has_value() ? clip.physics->plays : BakePlays::FromClip;
    switch (plays) {
    case BakePlays::Loops:
        return true;
    case BakePlays::Once:
        return false;
    default:
        return clip.looping && !PlaysOnce(clip.name);
    }
}

u32 FindClipNamed(const Document& document, u32 model, std::string_view name) {
    for (u32 c = 0; c < document.clips.size(); ++c) {
        if (document.clips[c].model == model && document.clips[c].name == name) {
            return c;
        }
    }
    return kInvalidIndex;
}

BakeLeadIn BakeComesFrom(const Document& document, u32 clip) {
    BakeLeadIn out;
    if (clip >= document.clips.size()) {
        return out;
    }
    const Clip& owner = document.clips[clip];
    const BakeFrom from = owner.physics.has_value() ? owner.physics->comesFrom : BakeFrom::Auto;
    if (from == BakeFrom::Itself || (from == BakeFrom::Auto && BakeLoops(owner))) {
        out.kind = BakeFrom::Itself;
        return out;
    }
    if (from == BakeFrom::FirstFrame) {
        return out;
    }
    u32 found = kInvalidIndex;
    if (from == BakeFrom::Clip) {
        found = FindClipNamed(document, owner.model, owner.physics->comesFromClip);
    } else {
        // A corpse continues its death: Decay Bone after Decay Flesh, and
        // Decay Flesh, or a lone Decay, after the first Death.
        const std::vector<std::string> words = Words(BaseSequenceName(owner.name));
        if (IsDecayClipName(owner.name)) {
            if (words.size() > 1 && words[1] == "bone") {
                for (u32 c = 0; c < document.clips.size() && found == kInvalidIndex; ++c) {
                    const std::vector<std::string> other = Words(BaseSequenceName(document.clips[c].name));
                    if (document.clips[c].model == owner.model && c != clip && other.size() > 1 &&
                        other[0] == "decay" && other[1] == "flesh") {
                        found = c;
                    }
                }
            }
            for (u32 c = 0; c < document.clips.size() && found == kInvalidIndex; ++c) {
                if (document.clips[c].model == owner.model && IsDeathClipName(document.clips[c].name)) {
                    found = c;
                }
            }
        } else {
            found = FirstLoopNamed(document, owner.model, "Stand");
        }
    }
    if (found != kInvalidIndex && found != clip) {
        out.kind = BakeFrom::Clip;
        out.clip = found;
    }
    return out;
}

u32 BakeGoesTo(const Document& document, u32 clip) {
    if (clip >= document.clips.size()) {
        return kInvalidIndex;
    }
    const Clip& owner = document.clips[clip];
    const BakeTo to = owner.physics.has_value() ? owner.physics->goesTo : BakeTo::Auto;
    if (to == BakeTo::Nothing || BakeLoops(owner)) {
        return kInvalidIndex;
    }
    if (to == BakeTo::Clip) {
        const u32 found = FindClipNamed(document, owner.model, owner.physics->goesToClip);
        return found != clip ? found : kInvalidIndex;
    }
    const std::vector<std::string> words = Words(BaseSequenceName(owner.name));
    if (words.empty() || words[0] == "death" || words[0] == "decay" || words[0] == "dissipate" || !PlaysOnce(owner.name)) {
        return kInvalidIndex;
    }
    const u32 stand = FirstLoopNamed(document, owner.model, "Stand");
    return stand != clip ? stand : kInvalidIndex;
}

std::vector<u32> BakeChain(const Document& document, u32 clip, bool* cycle) {
    std::vector<u32> chain;
    if (cycle != nullptr) {
        *cycle = false;
    }
    for (u32 at = clip; chain.size() <= kMaxChain;) {
        const BakeLeadIn from = BakeComesFrom(document, at);
        if (from.kind != BakeFrom::Clip) {
            break;
        }
        if (from.clip == clip || std::find(chain.begin(), chain.end(), from.clip) != chain.end()) {
            if (cycle != nullptr) {
                *cycle = true;
            }
            break;
        }
        chain.push_back(from.clip);
        at = from.clip;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

} // namespace wem
} // namespace models
} // namespace whiteout
