// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/m2_wc3_emitters.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "whiteout/models/m2/structures/base.h"
#include "whiteout/models/wem/anim/channel.h"
#include "whiteout/models/wem/anim/clip.h"
#include "whiteout/models/wem/meshes/remove.h"

namespace whiteout {
namespace models {
namespace cross {

namespace {

using namespace wem;

constexpr f32 kDegreesPerRadian = 57.2957795f;

bool Has(u32 flags, m2::ParticleFlag bit) {
    return (flags & static_cast<u32>(bit)) != 0;
}

/// The M2 material blend numbering the record states — the renderer's reading
/// of `blendingType` — as `PRE2`'s filter. There is no opaque particle filter;
/// the game's opaque particles draw blended too.
Wc3ParticleFilter FilterOf(u32 blend) {
    switch (blend) {
    case 1:
        return Wc3ParticleFilter::AlphaKey;
    case 3: // ONE, ONE
    case 4: // SRC_ALPHA, ONE
    case 7: // premultiplied add
        return Wc3ParticleFilter::Additive;
    case 5:
        return Wc3ParticleFilter::Modulate;
    case 6:
        return Wc3ParticleFilter::Modulate2x;
    default:
        return Wc3ParticleFilter::Blend;
    }
}

/// A lifetime curve at @p t, linear between keys.
template <class T, class Lerp>
T Sample(const std::vector<f32>& times, const std::vector<T>& values, f32 t, T fallback,
         Lerp lerp) {
    const std::size_t n = std::min(times.size(), values.size());
    if (n == 0) {
        return fallback;
    }
    if (t <= times[0]) {
        return values[0];
    }
    for (std::size_t k = 1; k < n; ++k) {
        if (t <= times[k]) {
            const f32 span = std::max(times[k] - times[k - 1], 1e-6f);
            return lerp(values[k - 1], values[k], (t - times[k - 1]) / span);
        }
    }
    return values[n - 1];
}

f32 LerpF(f32 a, f32 b, f32 s) {
    return a + (b - a) * s;
}

Vector3f LerpColor(const Vector3f& a, const Vector3f& b, f32 k) {
    return Vector3f{LerpF(a.x, b.x, k), LerpF(a.y, b.y, k), LerpF(a.z, b.z, k)};
}

Vector2f LerpSize(const Vector2f& a, const Vector2f& b, f32 k) {
    return Vector2f{LerpF(a.x, b.x, k), LerpF(a.y, b.y, k)};
}

/// The colour a particle shows on average over @p from..@p to of its life,
/// weighted by how visible it is there (its alpha).
Vector3f VisibleMeanColor(const M2ParticleEmitterPayload& m, f32 from, f32 to) {
    constexpr int kSteps = 64;
    Vector3f sum{0, 0, 0};
    f32 weight = 0.0f;
    for (int k = 0; k < kSteps; ++k) {
        const f32 t = LerpF(from, to, (static_cast<f32>(k) + 0.5f) / kSteps);
        const f32 a = std::max(Sample(m.alphaTimes, m.alphas, t, 1.0f, LerpF), 0.0f);
        const Vector3f c = Sample(m.colorTimes, m.colors, t, Vector3f{1, 1, 1}, LerpColor);
        sum = {sum.x + c.x * a, sum.y + c.y * a, sum.z + c.z * a};
        weight += a;
    }
    if (weight <= 1e-6f) {
        return Sample(m.colorTimes, m.colors, 0.5f * (from + to), Vector3f{1, 1, 1}, LerpColor);
    }
    return {sum.x / weight, sum.y / weight, sum.z / weight};
}

/// How far inside its end a run reads its last cell: a key standing exactly on
/// the end belongs to the next run.
constexpr f32 kRunEdge = 1e-3f;

/// The cells a cell track sweeps over [from, to): where it stands at the start
/// and just inside the end, which `PRE2` then plays through evenly. The client
/// interpolates between keys (`M2CellReader`), so a run spans every cell between
/// them — Dimensius' `0 4 12 30 63` is the whole 8x8 sheet, not five held cells.
Wc3ParticleInterval CellRun(const M2CellReader& cells, f32 from, f32 to) {
    Wc3ParticleInterval run;
    run.repeat = 1;
    if (cells.empty()) {
        return run;
    }
    run.start = static_cast<u32>(cells.at(from));
    run.end = static_cast<u32>(cells.at(std::max(std::min(to, 1.0f) - kRunEdge, from)));
    return run;
}

class Crossing {
public:
    Crossing(Document& document, u32 modelIndex, const M2EmitterOptions& options,
             M2EmitterReport& report)
        : document_(document), modelIndex_(modelIndex), model_(document.models[modelIndex]),
          options_(options), report_(report) {}

    void run() {
        for (u32 n = 0; n < model_.nodes.size(); ++n) {
            Node& node = model_.nodes.nodes[n];
            if (node.kind == NodeKind::M2ParticleEmitter && !node.removed) {
                cross(n, node);
            }
        }
    }

private:
    void note(u32 node, const std::string& what) {
        report_.diagnostics.info(DiagCode::LossyKindConversion,
                                 "particle emitter '" + model_.nodes.nodes[node].name + "': " + what,
                                 ElementRef(ElementKind::Node, node));
    }

    void cross(u32 n, Node& node) {
        const M2ParticleEmitterPayload m = std::get<M2ParticleEmitterPayload>(node.payload);
        Wc3ParticleEmitter2Payload p;

        const bool plane = m.emitterType == 1;
        const bool sphere = m.emitterType == 2;
        // An implosion: a sphere shooting its particles INWARD (a negative
        // speed) and killing each one as it passes the centre -- Dimensius'
        // eye gathers its needles so. PRE2 only shoots outward, and draws the
        // time reverse instead: a burst from the centre, living as long as the
        // inward flight took, over the curves read backwards. The picture of
        // a particle at radius r is the same either way; only its direction
        // of travel flips.
        const bool implodes = sphere && m.speed < 0.0f &&
                              Has(m.flags, m2::ParticleFlag::ImplosionFilter);
        p.speed = m.speed;
        p.variation = m.speedVariation;
        p.latitude = std::clamp(m.verticalRange * kDegreesPerRadian, 0.0f, 180.0f);
        p.gravity = -m.gravity.z; // Warcraft III's gravity pulls down, and only down
        p.lifespan = m.lifespan;
        p.emissionRate = m.emissionRate;
        if (plane) {
            // The record's X is the node's Y: WoW turns an emitter a quarter
            // about Z before placing it, and PRE2 lays width along X.
            p.width = m.length;
            p.length = m.width;
        } else if (implodes) {
            p.width = 0.0f;
            p.length = 0.0f;
            p.speed = -m.speed;
            p.latitude = 180.0f;
            // Out to the outer radius, which is how far the inward cloud reaches.
            p.lifespan = std::min(m.lifespan, std::max(m.width, m.length) / p.speed);
            note(n, "an imploding sphere is drawn as its time reverse, a burst from the centre");
        } else if (sphere) {
            // The two floats are a sphere's min and max radius; the square
            // around it, shot every way (straight up when the record says so).
            const f32 across = 2.0f * std::max(m.width, m.length);
            p.width = across;
            p.length = across;
            p.latitude = Has(m.flags, m2::ParticleFlag::HemisphereUpDirection) ? 0.0f : 180.0f;
            note(n, "a sphere emitter is drawn as the square around it");
        } else {
            note(n, "a spline or bone emitter is drawn as a point at its node");
        }
        if (m.zSource > 1e-3f) {
            note(n, "its particles aimed away from a point on its axis; PRE2 aims them in a cone");
        }

        p.filter = FilterOf(m.blend);
        p.rows = std::max<u32>(m.rows, 1);
        p.columns = std::max<u32>(m.columns, 1);
        const bool head = Has(m.flags, m2::ParticleFlag::HeadStyle);
        const bool tail = Has(m.flags, m2::ParticleFlag::TailStyle);
        p.headOrTail = head && tail ? Wc3ParticleHeadOrTail::Both
                       : tail       ? Wc3ParticleHeadOrTail::Tail
                                    : Wc3ParticleHeadOrTail::Head;
        p.tailLength = m.tailLength;

        p.time = M2EmitterMiddleTime(m);
        // Twinkle is a size multiplier the game always applies, uniform over
        // its [min, max], and a share of the particles it blinks out; PRE2 has
        // neither, so the means go into the size and the alpha.
        const f32 twinkleSize = 0.5f * (m.twinkleScale.x + m.twinkleScale.y);
        const f32 twinkleShown = std::clamp(m.twinklePercent, 0.0f, 1.0f);
        Wc3ParticleSegment* segments[3] = {&p.start, &p.middle, &p.end};
        // Where on the WoW life each PRE2 segment reads: the same point, or,
        // reversed, the inward flight's arrival back to its start.
        const f32 flight = implodes && m.lifespan > 0.0f ? p.lifespan / m.lifespan : 1.0f;
        const auto source = [&](f32 t) { return implodes ? (1.0f - t) * flight : t; };
        if (implodes) {
            // The middle key, reversed, while it falls inside the flight.
            p.time = p.time < flight ? std::clamp(1.0f - p.time / flight, 0.01f, 0.99f) : 0.5f;
        }
        const f32 at[3] = {source(0.0f), source(p.time), source(1.0f)};
        Vector2f sizes[3];
        for (int s = 0; s < 3; ++s) {
            segments[s]->color = Sample(m.colorTimes, m.colors, at[s], Vector3f{1, 1, 1}, LerpColor);
            const f32 alpha = Sample(m.alphaTimes, m.alphas, at[s], 1.0f, LerpF) * twinkleShown;
            segments[s]->alpha = static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
            const Vector2f size = Sample(m.scaleTimes, m.scales, at[s], Vector2f{1, 1}, LerpSize);
            sizes[s] = {twinkleSize * size.x, twinkleSize * size.y};
            // A PRE2 quad is square. The side that keeps the particle's area
            // keeps its light: Dimensius' eye flare is four times wider than
            // tall, and drawn by its width it was a red square over his face.
            segments[s]->scaling = std::sqrt(std::max(sizes[s].x * sizes[s].y, 0.0f));
        }
        if (m.colors.size() > 3) {
            // A colour the three segments cannot pass through is lost at the
            // middle key: the eye flare runs pale blue to red, and its middle
            // sample kept only the red. The middle takes what the particle
            // shows on average while it is visible instead.
            segments[1]->color = implodes ? VisibleMeanColor(m, 0.0f, flight)
                                          : VisibleMeanColor(m, 0.0f, 1.0f);
        }
        if (m.colors.size() > 3 || m.alphas.size() > 3 || m.scales.size() > 3) {
            note(n, "its colour, alpha or size curve has more keys than PRE2's three segments");
        }

        // A WoW particle can lie along its velocity (0x4) at any aspect, where a
        // PRE2 head is a square facing the camera. PRE2's tail is the quad that
        // lies along the velocity: as long as the particle is (its speed times
        // the tail length, a constant, so the middle segment's length) and as
        // wide. Dimensius' needles are thirteen times longer than wide, and as
        // square heads they were wide sparkles all around him.
        const f32 speed = std::abs(p.speed);
        const bool needle = !tail && Has(m.flags, m2::ParticleFlag::VelocityOrient) &&
                            speed > 1e-3f && sizes[1].x > 2.0f * sizes[1].y;
        if (needle) {
            p.headOrTail = Wc3ParticleHeadOrTail::Tail;
            p.tailLength = 2.0f * sizes[1].x / speed;
            for (int s = 0; s < 3; ++s) {
                segments[s]->scaling = sizes[s].y;
            }
            note(n, "a particle lying along its velocity is drawn as a PRE2 tail");
        }

        // A random cell per particle has no PRE2 spelling; playing the whole
        // sheet over the life keeps the variety, as a flicker. The game draws
        // one only where no cell curve is keyed: a keyed curve wins.
        const u32 cells = p.rows * p.columns;
        if (cells > 1 && m.headCells.empty() &&
            (Has(m.flags, m2::ParticleFlag::ChooseRandomTexture) ||
             Has(m.flags, m2::ParticleFlag::RandFlipbookStart))) {
            p.headLife = {0, cells - 1, 1};
            p.headDecay = {0, cells - 1, 1};
            p.tailLife = p.headLife;
            p.tailDecay = p.headDecay;
            note(n, "a random cell per particle becomes the sheet played over its life");
        } else {
            const M2CellReader headCells = M2CellReader::Head(m);
            const M2CellReader tailCells = M2CellReader::Tail(m);
            p.headLife = CellRun(headCells, 0.0f, p.time);
            p.headDecay = CellRun(headCells, p.time, 1.01f);
            p.tailLife = CellRun(tailCells, 0.0f, p.time);
            p.tailDecay = CellRun(tailCells, p.time, 1.01f);
            if (needle) {
                // The head's cells, drawn by the tail.
                p.tailLife = p.headLife;
                p.tailDecay = p.headDecay;
            }
            // The client masks a cell into the sheet; a PRE2 cell past it samples the next tile.
            for (Wc3ParticleInterval* run : {&p.headLife, &p.headDecay, &p.tailLife, &p.tailDecay}) {
                run->start %= cells;
                run->end %= cells;
            }
        }

        p.texture = m.texture;
        if (Has(m.flags, m2::ParticleFlag::MultiTexture)) {
            if (foldMultiTexture(m, segments)) {
                note(n, "a multi-texture particle keeps its first texture, and the other two "
                        "fold in as their mean");
            } else {
                note(n, "a multi-texture particle keeps its first texture");
            }
        }
        if (Has(m.flags, m2::ParticleFlag::Refraction)) {
            note(n, "a refraction particle is drawn as a plain one");
        }
        p.squirt = Has(m.flags, m2::ParticleFlag::Squirt);
        p.priorityPlane = m.priorityPlane;
        p.unshaded = Has(m.flags, m2::ParticleFlag::Unlit) ||
                     Has(m.flags, m2::ParticleFlag::MultiTexture) ||
                     Has(m.flags, m2::ParticleFlag::Refraction);
        p.unfogged = Has(m.flags, m2::ParticleFlag::Unfogged);
        p.sortPrimsFarZ = Has(m.flags, m2::ParticleFlag::SortParticles);
        p.xyQuad = Has(m.flags, m2::ParticleFlag::XYQuad);
        if (m.spin != 0 || m.baseSpin != 0 || m.drag != 0 || m.twinkleScale.x != m.twinkleScale.y ||
            m.twinklePercent < 1 || m.wind.x != 0 || m.wind.y != 0 || m.wind.z != 0 ||
            m.lifespanVariation != 0 || m.emissionRateVariation != 0) {
            note(n, "spin, drag, a twinkle's blink, wind and lifespan or rate variation have no "
                    "PRE2 field");
        }

        // File bit 0x10 is what makes a WoW particle ride its emitter, which is
        // PRE2's model space (0x80000). WEM reads that bit as "the local is the
        // world", so the local becomes the pivot, as an `.mdx` import writes one.
        if (Has(m.flags, m2::ParticleFlag::WorldSpace)) {
            node.flags = node.flags | NodeFlags::ModelSpace;
            node.local.translation = node.pivot;
            for (Transform& pose : node.poses) {
                pose.translation = node.pivot;
            }
        }

        node.kind = NodeKind::Wc3ParticleEmitter2;
        node.payload = p;
        retargetChannels(n, sphere, implodes);
        ++report_.particles;
    }

    /// The second and third textures of a multi-texture particle, which PRE2
    /// cannot sample, as the factor they multiply the first by on average: the
    /// client's `texColor = t0*t1 (*t2) * 2 (or 4)`, `texAlpha = t0.a*t1.a*t2.a
    /// * 2 (or 4)`. Dimensius' needles multiply a white and a mid-grey map in
    /// at 4x, and without them drew at under half their light.
    bool foldMultiTexture(const M2ParticleEmitterPayload& m, Wc3ParticleSegment* segments[3]) {
        if (!options_.textureMean) {
            return false;
        }
        if (m.texture2 >= document_.textures.size() || m.texture3 >= document_.textures.size()) {
            return false;
        }
        const std::optional<Vector4f> t1 = options_.textureMean(document_.textures[m.texture2]);
        const std::optional<Vector4f> t2 = options_.textureMean(document_.textures[m.texture3]);
        if (!t1 || !t2) {
            return false;
        }
        const bool threeColors = Has(m.flags, m2::ParticleFlag::MultitexUse3Colors);
        const f32 colorGain = threeColors ? 4.0f : 2.0f;
        const Vector3f rgb{colorGain * t1->x * (threeColors ? t2->x : 1.0f),
                           colorGain * t1->y * (threeColors ? t2->y : 1.0f),
                           colorGain * t1->z * (threeColors ? t2->z : 1.0f)};
        const f32 alpha = (Has(m.flags, m2::ParticleFlag::MultitexUseModx4) ? 4.0f : 2.0f) *
                          t1->w * t2->w;
        for (int s = 0; s < 3; ++s) {
            Vector3f& c = segments[s]->color;
            c = {std::min(c.x * rgb.x, 1.0f), std::min(c.y * rgb.y, 1.0f),
                 std::min(c.z * rgb.z, 1.0f)};
            segments[s]->alpha =
                static_cast<u8>(std::clamp(segments[s]->alpha * alpha, 0.0f, 255.0f) + 0.5f);
        }
        return true;
    }

    /// The node's M2 property channels as PRE2's: renamed, restated in PRE2's
    /// units, or removed where PRE2 has no such property.
    void retargetChannels(u32 n, bool sphere, bool implodes) {
        using M = M2ParticleProperty;
        using W = Wc3Particle2Property;
        std::vector<u32> dropped;
        for (AnimChannel& channel : model_.animChannels.channels) {
            TrackTarget& target = channel.target;
            if (target.kind != TrackTarget::Kind::Node || target.node != n ||
                target.channel != Channel::EmitterProperty) {
                continue;
            }
            const M from = static_cast<M>(EmitterPropertyOf(target.sub));
            W to = W::Count;
            switch (from) {
            case M::Speed:
                to = W::Speed;
                if (implodes) {
                    scaleValues(channel, -1.0f); // the reverse flies outward
                }
                break;
            case M::SpeedVariation:
                to = W::Variation;
                break;
            case M::VerticalRange:
                to = W::Latitude;
                scaleValues(channel, kDegreesPerRadian);
                break;
            case M::Gravity:
                to = W::Gravity;
                downwardOnly(channel);
                break;
            case M::EmissionRate:
                to = W::EmissionRate;
                break;
            case M::Width:
                to = sphere ? W::Count : W::Length; // the quarter turn
                break;
            case M::Length:
                to = sphere ? W::Count : W::Width;
                break;
            default:
                break;
            }
            if (to == W::Count) {
                dropped.push_back(channel.id);
                continue;
            }
            target.sub = EmitterPropertySub(static_cast<u32>(to));
            ++report_.keyedChannels;
        }
        if (!dropped.empty()) {
            note(n, std::to_string(dropped.size()) +
                        " keyed propert(ies) with no PRE2 track (lifespan, azimuth, aim point "
                        "or a sphere's radii) were dropped");
        }
        EraseChannels(document_, modelIndex_, dropped);
    }

    template <class F>
    void forEachSubTrack(u32 channel, F&& f) {
        for (Clip& clip : document_.clips) {
            if (clip.model != modelIndex_) {
                continue;
            }
            for (SubTrackContainer& container : clip.containers) {
                for (SubTrack& track : container.subTracks) {
                    if (track.channel == channel) {
                        f(track);
                    }
                }
            }
        }
    }

    /// Every F32 value of the channel (tangents included) times @p factor.
    void scaleValues(AnimChannel& channel, f32 factor) {
        const auto scale = [factor](std::vector<u8>& bytes) {
            for (std::size_t at = 0; at + sizeof(f32) <= bytes.size(); at += sizeof(f32)) {
                f32 value = 0;
                std::memcpy(&value, bytes.data() + at, sizeof(f32));
                value *= factor;
                std::memcpy(bytes.data() + at, &value, sizeof(f32));
            }
        };
        scale(channel.initValue);
        forEachSubTrack(channel.id, [&](SubTrack& track) { scale(track.values); });
    }

    /// F32x3 gravity vectors to PRE2's scalar pull: the downward component.
    void downwardOnly(AnimChannel& channel) {
        const auto collapse = [](std::vector<u8>& bytes) {
            std::vector<u8> out;
            for (std::size_t at = 0; at + 3 * sizeof(f32) <= bytes.size(); at += 3 * sizeof(f32)) {
                f32 z = 0;
                std::memcpy(&z, bytes.data() + at + 2 * sizeof(f32), sizeof(f32));
                const f32 down = -z;
                const u8* raw = reinterpret_cast<const u8*>(&down);
                out.insert(out.end(), raw, raw + sizeof(f32));
            }
            bytes = std::move(out);
        };
        collapse(channel.initValue);
        channel.valueType = geom::AttrType::F32;
        forEachSubTrack(channel.id, [&](SubTrack& track) { collapse(track.values); });
    }

    Document& document_;
    u32 modelIndex_;
    Model& model_;
    const M2EmitterOptions& options_;
    M2EmitterReport& report_;
};

} // namespace

M2EmitterReport CrossM2Emitters(wem::Document& staged, const M2EmitterOptions& options) {
    M2EmitterReport report;
    for (u32 m = 0; m < staged.models.size(); ++m) {
        Crossing(staged, m, options, report).run();
    }
    return report;
}

f32 M2EmitterMiddleTime(const wem::M2ParticleEmitterPayload& emitter) {
    // Three segments out of curves of any length: birth, the middle key,
    // death. The curves' middle keys need not agree, and alpha's decides how
    // much the particle shows: TitanArgus' fire peaks at 0.79 a sixth into its
    // life, which the colour's middle key (0.46) sampled as 0.52.
    for (const std::vector<f32>* times :
         {&emitter.alphaTimes, &emitter.colorTimes, &emitter.scaleTimes}) {
        if (times->size() == 3) {
            return std::clamp((*times)[1], 0.01f, 0.99f);
        }
    }
    return 0.5f;
}

i32 M2CellAt(const std::vector<f32>& times, const std::vector<u32>& cells, f32 t) {
    const std::size_t n = std::min(times.size(), cells.size());
    if (n == 0) {
        return 0;
    }
    if (n == 1) {
        return static_cast<i32>(cells[0]);
    }
    // FindTrackKeys (0x141962160): two keys are taken to span the life, three
    // split at the middle one, and more search for the last key at or before t
    // short of the final one. None of the three clamps.
    std::size_t key = 0;
    f32 pct = t;
    if (n == 3) {
        const f32 middle = times[1];
        if (t >= middle) {
            key = 1;
            pct = middle < 1.0f ? (t - middle) / (1.0f - middle) : 0.0f;
        } else {
            pct = middle > 0.0f ? t / middle : 0.0f;
        }
    } else if (n > 3) {
        while (key + 2 < n && t >= times[key + 1]) {
            ++key;
        }
        const f32 span = times[key + 1] - times[key];
        pct = span != 0.0f ? (t - times[key]) / span : 0.0f;
    }
    // In the client's order: the integer difference, scaled, then the base.
    const i32 from = static_cast<i32>(cells[key]);
    const i32 to = static_cast<i32>(cells[key + 1]);
    const f32 value = static_cast<f32>(to - from) * pct + static_cast<f32>(from);
    // cvtss2si under the default MXCSR: nearest, ties to even.
    return static_cast<i32>(std::nearbyint(value));
}

namespace {

/// A key time as the client stores it: WEM reads `u16 / 32767`.
i32 KeyTime(f32 t) {
    return static_cast<i32>(std::lround(t * 32767.0f));
}

} // namespace

bool M2IsSimple(const wem::M2ParticleEmitterPayload& e) {
    const auto oneToThree = [](std::size_t n) { return n >= 1 && n <= 3; };
    if (!oneToThree(e.colorTimes.size()) || !oneToThree(e.alphaTimes.size()) ||
        !oneToThree(e.scaleTimes.size())) {
        return false;
    }
    for (const std::vector<f32>* cells : {&e.headCellTimes, &e.tailCellTimes}) {
        if (cells->size() > 4) {
            return false;
        }
        if (cells->size() == 4 && KeyTime((*cells)[1]) != KeyTime((*cells)[2])) {
            return false;
        }
    }
    if (e.lifespanVariation != 0.0f) {
        return false;
    }
    if (e.headCellTimes.empty() && Has(e.flags, m2::ParticleFlag::ChooseRandomTexture)) {
        return false;
    }
    if (e.scaleVariation.x != 0.0f || e.scaleVariation.y != 0.0f) {
        return false;
    }
    for (const Vector2f& scale : e.scales) {
        if (std::fabs(scale.x - scale.y) >= 2.3841858e-7f) {
            return false;
        }
    }
    // Every three-key track splits where the first one does.
    std::optional<i32> middle;
    const auto agrees = [&middle](const std::vector<f32>& times, bool isThree) {
        if (!isThree) {
            return true;
        }
        const i32 at = KeyTime(times[1]);
        if (!middle) {
            middle = at;
        }
        return *middle == at;
    };
    return agrees(e.colorTimes, e.colorTimes.size() == 3) &&
           agrees(e.alphaTimes, e.alphaTimes.size() == 3) &&
           agrees(e.scaleTimes, e.scaleTimes.size() == 3) &&
           agrees(e.headCellTimes, e.headCellTimes.size() >= 3) &&
           agrees(e.tailCellTimes, e.tailCellTimes.size() >= 3);
}

f32 M2SimpleMiddle(const wem::M2ParticleEmitterPayload& e) {
    const std::pair<const std::vector<f32>*, std::size_t> tracks[] = {
        {&e.colorTimes, 3}, {&e.alphaTimes, 3}, {&e.scaleTimes, 3},
        {&e.headCellTimes, 3}, {&e.tailCellTimes, 3}};
    for (std::size_t k = 0; k < std::size(tracks); ++k) {
        const std::vector<f32>& times = *tracks[k].first;
        const bool cells = k >= 3;
        if (cells ? times.size() >= 3 : times.size() == 3) {
            return static_cast<f32>(KeyTime(times[1])) * 0.000030518509f;
        }
    }
    return 0.0f;
}

M2SimpleCellKeys M2SimpleCells(const std::vector<u32>& cells, f32 middle) {
    M2SimpleCellKeys keys;
    // 16.16, truncated (cvttss2si), as InterpolateAllTracks_Simple takes it.
    keys.middle = static_cast<i32>(middle * 65536.0f);
    if (cells.empty()) {
        return keys;
    }
    const auto c = [&cells](std::size_t k) { return static_cast<i32>(cells[k] & 0xFFFF); };
    keys.start[0] = static_cast<i32>(static_cast<i16>(cells[0] & 0xFFFF)) * 65536;
    switch (cells.size()) {
    case 1:
        break;
    case 2: {
        // cvtss2si: nearest, ties to even.
        const f32 span = static_cast<f32>(c(1) - c(0));
        keys.slope[0] = static_cast<i32>(std::nearbyint(span * middle));
        keys.slope[1] = static_cast<i32>(std::nearbyint((1.0f - middle) * span));
        break;
    }
    case 3:
        keys.slope[0] = c(1) - c(0);
        keys.slope[1] = c(2) - c(1);
        break;
    default:
        keys.slope[0] = c(1) - c(0);
        keys.slope[1] = c(3) - c(2);
        break;
    }
    keys.start[1] = keys.start[0] + static_cast<i32>(static_cast<i16>(keys.slope[0])) * 65536;
    return keys;
}

i32 M2SimpleCellAt(const M2SimpleCellKeys& keys, f32 t) {
    constexpr i64 kOne = 65536;
    const i64 at = std::clamp<i64>(static_cast<i64>(t * 65536.0f), 0, kOne);
    const bool second = at >= keys.middle;
    // A split at the very end leaves segment 2 no length; the client divides
    // by zero there, and no shipped emitter has one.
    const i64 span = second ? kOne - keys.middle : keys.middle;
    const i64 fraction = span <= 0 ? 0 : ((second ? at - keys.middle : at) * kOne) / span;
    const i32 k = second ? 1 : 0;
    // 32-bit, as the client multiplies and shifts (sar: floored).
    const i32 value = static_cast<i32>(static_cast<u32>(keys.start[k]) +
                                       static_cast<u32>(keys.slope[k]) * static_cast<u32>(fraction));
    return value >> 16;
}

M2CellReader::M2CellReader(const wem::M2ParticleEmitterPayload& emitter,
                           const std::vector<f32>& times, const std::vector<u32>& cells)
    : simple_(M2IsSimple(emitter)), times_(times), cells_(cells) {
    if (simple_) {
        simpleKeys_ = M2SimpleCells(cells_, M2SimpleMiddle(emitter));
    }
}

M2CellReader M2CellReader::Head(const wem::M2ParticleEmitterPayload& emitter) {
    return M2CellReader(emitter, emitter.headCellTimes, emitter.headCells);
}

M2CellReader M2CellReader::Tail(const wem::M2ParticleEmitterPayload& emitter) {
    return M2CellReader(emitter, emitter.tailCellTimes, emitter.tailCells);
}

i32 M2CellReader::at(f32 t) const {
    if (simple_) {
        return M2SimpleCellAt(simpleKeys_, t);
    }
    return cells_.empty() ? 0 : M2CellAt(times_, cells_, t);
}

} // namespace cross
} // namespace models
} // namespace whiteout
