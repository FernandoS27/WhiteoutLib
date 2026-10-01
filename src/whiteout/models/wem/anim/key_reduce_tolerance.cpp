// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Tier 2 of the key reducer (EDIT_MODE_KEY_OPTIMIZE_DESIGN.md §4): keys whose
/// absence moves nothing of the model further than ε, judged in model space
/// through the skeleton, and checked afterwards against the skinned mesh.

#include <whiteout/models/wem/anim/key_reduce.h>

#include <whiteout/models/wem/anim/animator.h>
#include <whiteout/models/wem/optimize.h>
#include <whiteout/models/wem/skinning/deform.h>

#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <thread>

#include "key_reduce_common.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using namespace keys;

constexpr f64 kInfeasible = std::numeric_limits<f64>::infinity();

// ---- One key of any value type -------------------------------------------------------

using FValue = std::array<f32, 4>;

struct FKey {
    i32 ms = 0;
    FValue value{};
    FValue in{};
    FValue out{};
    i32 source = -1;
};

/// Floats in one element of @p type; none for an integer.
std::size_t kFloatsOf(geom::AttrType type) {
    return type == geom::AttrType::U32 ? 0 : geom::AttrTypeSize(type) / sizeof(f32);
}

template <class T>
FValue ToF(const T& v) {
    FValue out{};
    std::memcpy(out.data(), &v, sizeof(T));
    return out;
}

template <class T>
T FromF(const FValue& v) {
    T out{};
    std::memcpy(&out, v.data(), sizeof(T));
    return out;
}

template <class T>
Key<T> ToKey(const FKey& k) {
    Key<T> out;
    out.ms = k.ms;
    out.value = FromF<T>(k.value);
    out.in = FromF<T>(k.in);
    out.out = FromF<T>(k.out);
    out.source = k.source;
    return out;
}

/// How a track's error is measured (§4.1, §4.2).
enum class Kind : u8 {
    Node,     ///< A transform of a node of a single-layer clip: the skeleton.
    Colour,   ///< Alpha, colour, a blend weight: absolute, 0 and 1 exact.
    Uv,       ///< UV translation and scale.
    UvTurn,   ///< UV rotation.
    Relative, ///< Scalars in units of their own: of the clip's peak.
    Exact,    ///< Discrete, or a transform the layers blend: float noise only.
};

Kind KindOf(const AnimChannel& channel, bool layered) {
    const TrackTarget& target = channel.target;
    switch (target.channel) {
    case Channel::Translation:
    case Channel::Rotation:
    case Channel::Scale:
        return target.kind == TrackTarget::Kind::Node && !layered ? Kind::Node : Kind::Exact;
    case Channel::Alpha:
    case Channel::Color:
    case Channel::Weight:
        return Kind::Colour;
    case Channel::UvTranslate:
    case Channel::UvScale:
        return Kind::Uv;
    case Channel::UvRotate:
        return Kind::UvTurn;
    case Channel::Visibility:
    case Channel::TextureIndex:
    case Channel::StageWeight:
    case Channel::StageSourceWeight:
    case Channel::StageSourceEnabled:
        return Kind::Exact;
    default:
        return Kind::Relative;
    }
}

/// One sub-track of the clip being reduced, as its rule reads it.
struct Track {
    u32 container = 0;
    u32 index = 0;
    const AnimChannel* channel = nullptr;
    geom::AttrType type = geom::AttrType::F32;
    bool quat = false;
    ReadRule rule = ReadRule::Wc3;
    Interpolation interp = Interpolation::Linear; ///< Between two keys, as the rule reads it.
    Interpolation stored = Interpolation::Linear; ///< The sub-track's own.
    Kind kind = Kind::Exact;
    u32 node = kInvalidNode;
    bool reducible = false; ///< In scope, and on a channel whose clips agree.
    std::vector<FKey> orig; ///< The keys as they were.
    std::vector<FKey> cur;  ///< With any refitted tangents.
    std::vector<u8> fixed;
    std::vector<u8> kept;
    std::vector<u32> prev;
    std::vector<u32> next;
    std::vector<u32> version;
    f64 noise = 0.0;
    f64 scale = 1.0; ///< A non-node kind's tolerance, in its units.
    bool curved = false;
    bool refit = false; ///< Smooth vectors whose facing tangents may be refit.
};

FValue PairF(const Track& t, const FKey& a, const FKey& b, i32 ms) {
    FValue out{};
    WithType(t.type, [&]<class T>(bool) {
        out = ToF(Pair(t.rule, t.interp, ToKey<T>(a), ToKey<T>(b), SpanFraction(ms, a.ms, b.ms), false));
    });
    return out;
}

/// The original track's read at @p ms, inside its key range.
FValue OrigAt(const Track& t, i32 ms) {
    const auto it = std::upper_bound(t.orig.begin(), t.orig.end(), ms,
                                     [](i32 v, const FKey& k) { return v < k.ms; });
    if (it == t.orig.begin()) {
        return t.orig.front().value;
    }
    const std::size_t j = static_cast<std::size_t>(it - t.orig.begin()) - 1;
    if (j + 1 >= t.orig.size()) {
        return t.orig.back().value;
    }
    return PairF(t, t.orig[j], t.orig[j + 1], ms);
}

/// The two tangents facing a span refit to the original curve at @p samples by
/// least squares, per component; left as they are where the fit is singular.
void Refit(const Track& t, FKey& a, FKey& c, const std::vector<i32>& samples) {
    const std::size_t n = kFloatsOf(t.type);
    for (std::size_t comp = 0; comp < n; ++comp) {
        f64 s11 = 0, s12 = 0, s22 = 0, r1 = 0, r2 = 0;
        for (const i32 ms : samples) {
            const f64 s = SpanFraction(ms, a.ms, c.ms);
            f64 h00 = 0, h01 = 0, h10 = 0, h11 = 0;
            if (t.interp == Interpolation::Hermite) {
                h00 = 2 * s * s * s - 3 * s * s + 1;
                h01 = -2 * s * s * s + 3 * s * s;
                h10 = s * s * s - 2 * s * s + s;
                h11 = s * s * s - s * s;
            } else {
                const f64 i = 1.0 - s;
                h00 = i * i * i;
                h01 = s * s * s;
                h10 = 3 * i * i * s;
                h11 = 3 * i * s * s;
            }
            const f64 y = OrigAt(t, ms)[comp];
            const f64 r = y - h00 * a.value[comp] - h01 * c.value[comp];
            s11 += h10 * h10;
            s12 += h10 * h11;
            s22 += h11 * h11;
            r1 += h10 * r;
            r2 += h11 * r;
        }
        const f64 det = s11 * s22 - s12 * s12;
        if (!(std::fabs(det) > 1e-12 * std::max(1.0, s11 * s22))) {
            continue;
        }
        a.out[comp] = static_cast<f32>((r1 * s22 - r2 * s12) / det);
        c.in[comp] = static_cast<f32>((s11 * r2 - s12 * r1) / det);
    }
}

// ---- The skeleton (§4.2) --------------------------------------------------------------

struct Rep {
    Vector3f position;
    std::vector<geom::Influence> influences; ///< Normalised, as the renderer divides.
};

struct Skeleton {
    Animator::Composition order;
    std::vector<std::vector<u32>> subtree; ///< Each node and its descendants, parents first.
    std::vector<f64> reach;
    std::vector<f64> share; ///< The node's share of ε.
    std::vector<u8> floored; ///< Carries what the file does not describe.
    std::vector<Vector3f> centre;
    std::vector<std::vector<Rep>> reps; ///< The Mesh measure's extreme vertices.
};

bool CarriesUndescribed(NodeKind kind) {
    switch (kind) {
    case NodeKind::Attachment:
    case NodeKind::Light:
    case NodeKind::Camera:
    case NodeKind::ParticleEmitter:
    case NodeKind::RibbonEmitter:
    case NodeKind::CollisionShape:
    case NodeKind::Wc3ParticleEmitter1:
    case NodeKind::Wc3ParticleEmitter2:
    case NodeKind::Wc3RibbonEmitter:
    case NodeKind::Sc2ParticleEmitter:
    case NodeKind::Sc2RibbonEmitter:
    case NodeKind::Wc3CornEmitter:
    case NodeKind::M2ParticleEmitter:
        return true;
    default:
        return false;
    }
}

std::vector<geom::Influence> Normalised(std::span<const geom::Influence> influences) {
    std::vector<geom::Influence> out(influences.begin(), influences.end());
    f32 sum = 0.0f;
    for (const geom::Influence& i : out) {
        sum += i.weight;
    }
    for (geom::Influence& i : out) {
        i.weight = sum > 0.0f ? i.weight / sum : 0.0f;
    }
    return out;
}

/// Each vertex of @p mesh with what binds it: its influences, or the rigid
/// section's node at weight one.
std::vector<std::vector<geom::Influence>> BindingsOf(const Mesh& mesh) {
    const u32 count = mesh.vertexCount();
    std::vector<std::vector<geom::Influence>> out(count);
    if (!mesh.skin.empty()) {
        for (u32 v = 0; v < count && v + 1 < mesh.skin.offsets.size(); ++v) {
            out[v] = Normalised(std::span<const geom::Influence>(
                mesh.skin.influences.data() + mesh.skin.offsets[v],
                mesh.skin.offsets[v + 1] - mesh.skin.offsets[v]));
        }
    }
    const geom::FaceSet& faces = mesh.faceSet();
    const std::span<const u32> sectionOf = mesh.faceSections();
    std::size_t corner = 0;
    for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
        const u32 section = f < sectionOf.size() ? sectionOf[f] : kInvalidIndex;
        const std::optional<u32> rigid =
            section < mesh.sections.size() ? mesh.sections[section].rigidNode : std::nullopt;
        for (u32 k = 0; k < faces.faceValence[f]; ++k, ++corner) {
            const u32 v = faces.cornerVertex[corner];
            if (rigid && v < count && out[v].empty()) {
                out[v] = {geom::Influence{*rigid, 1.0f}};
            }
        }
    }
    return out;
}

Skeleton SkeletonOf(const Document& document, u32 model, const Animator& animator, f64 height,
                    const KeyReduceOptions& options) {
    const Model& owner = document.models[model];
    const NodeTree& tree = owner.nodes;
    const u32 count = tree.size();
    const bool pivoted = tree.rig == RigConvention::PivotRelative;
    Skeleton out;
    out.order = animator.composition();
    out.reach.assign(count, 0.0);
    out.share.assign(count, 1.0);
    out.floored.assign(count, 0);
    out.centre.resize(count);
    out.reps.resize(count);
    out.subtree.resize(count);
    for (u32 n = 0; n < count; ++n) {
        out.centre[n] = pivoted ? tree.nodes[n].pivot : tree.worldBind(n).translation;
        const Node& node = tree.nodes[n];
        if (node.kind == NodeKind::Attachment || IsWarcraftEngineBoneName(node.name)) {
            out.share[n] = std::clamp<f64>(options.importantShare, 0.01, 1.0);
        }
    }
    // Descendants, parents first: the order `compose` places them in.
    std::vector<std::vector<u32>> children(count);
    for (const u32 n : out.order.order) {
        if (out.order.parent[n] < count) {
            children[out.order.parent[n]].push_back(n);
        }
    }
    for (u32 n = 0; n < count; ++n) {
        std::vector<u32>& list = out.subtree[n];
        list.push_back(n);
        for (std::size_t i = 0; i < list.size(); ++i) {
            for (const u32 child : children[list[i]]) {
                list.push_back(child);
            }
        }
    }
    // Reach and the extreme vertices, over what each node carries directly:
    // every vertex it weighs on at all, since a blend moves with each bone.
    std::vector<std::vector<Rep>> carried(count);
    for (const Mesh& mesh : owner.meshes) {
        const auto positions =
            mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        const std::vector<std::vector<geom::Influence>> bindings = BindingsOf(mesh);
        for (std::size_t v = 0; v < positions.size() && v < bindings.size(); ++v) {
            const Vector3f& p = positions[v];
            if (bindings[v].empty() || !std::isfinite(p.x + p.y + p.z)) {
                continue;
            }
            for (const geom::Influence& i : bindings[v]) {
                if (i.bone >= count || !(i.weight > 0.0f)) {
                    continue;
                }
                const Vector3f d = p - out.centre[i.bone];
                out.reach[i.bone] = std::max<f64>(out.reach[i.bone], std::sqrt(d.dot(d)));
                carried[i.bone].push_back(Rep{p, bindings[v]});
            }
        }
    }
    static const std::array<Vector3f, 15> kDirections = {
        Vector3f{1, 0, 0},  Vector3f{-1, 0, 0},  Vector3f{0, 1, 0},  Vector3f{0, -1, 0},
        Vector3f{0, 0, 1},  Vector3f{0, 0, -1},  Vector3f{1, 1, 1},  Vector3f{1, 1, -1},
        Vector3f{1, -1, 1}, Vector3f{1, -1, -1}, Vector3f{-1, 1, 1}, Vector3f{-1, 1, -1},
        Vector3f{-1, -1, 1}, Vector3f{-1, -1, -1}, Vector3f{0, 0, 0}};
    for (u32 n = 0; n < count; ++n) {
        std::array<f32, 15> score;
        std::array<i64, 15> pick;
        score.fill(-std::numeric_limits<f32>::infinity());
        pick.fill(-1);
        for (std::size_t r = 0; r < carried[n].size(); ++r) {
            const Vector3f d = carried[n][r].position - out.centre[n];
            for (std::size_t k = 0; k < kDirections.size(); ++k) {
                // The last direction stands for the farthest in any.
                const f32 s = k + 1 == kDirections.size() ? d.dot(d) : d.dot(kDirections[k]);
                if (s > score[k]) {
                    score[k] = s;
                    pick[k] = static_cast<i64>(r);
                }
            }
        }
        std::sort(pick.begin(), pick.end());
        for (std::size_t k = 0; k < pick.size(); ++k) {
            if (pick[k] >= 0 && (k == 0 || pick[k] != pick[k - 1])) {
                out.reps[n].push_back(std::move(carried[n][static_cast<std::size_t>(pick[k])]));
            }
        }
        if (CarriesUndescribed(tree.nodes[n].kind)) {
            out.reach[n] = std::max<f64>(out.reach[n], options.floorShare * height);
            out.floored[n] = 1;
        }
    }
    return out;
}

/// How far anything @p node carries can have moved, from the skinning matrix
/// it had to the one it has now: `|c·ΔS| + R·‖ΔA‖`, the Frobenius norm bounding
/// the linear part's.
f64 BoundError(const Skeleton& sk, u32 node, const Matrix44f& was, const Matrix44f& now) {
    const Vector3f& c = sk.centre[node];
    f64 frob = 0.0;
    f64 point[3] = {0, 0, 0};
    for (int j = 0; j < 3; ++j) {
        f64 p = static_cast<f64>(was.data[3][j]) - now.data[3][j];
        for (int i = 0; i < 3; ++i) {
            const f64 d = static_cast<f64>(was.data[i][j]) - now.data[i][j];
            frob += d * d;
            p += (i == 0 ? c.x : i == 1 ? c.y : c.z) * d;
        }
        point[j] = p;
    }
    return std::sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]) +
           sk.reach[node] * std::sqrt(frob);
}

Vector3f Skin(const Rep& rep, std::span<const Matrix44f> palette) {
    return skinning::DeformPosition(rep.position, rep.influences, palette);
}

// ---- One clip ------------------------------------------------------------------------

struct Context {
    const Skeleton* sk = nullptr;
    const Animator* animator = nullptr;
    f64 epsilon = 0.0;
    KeyReduceOptions::Measure measure = KeyReduceOptions::Measure::Skeleton;
};

struct ClipWork {
    u32 clip = 0;
    i32 origin = 0;
    std::vector<Track> tracks;
    std::vector<i32> checkMs; ///< Clip-local.
    std::vector<std::vector<Matrix44f>> origSkin;
    std::vector<Pose> cur;
    // Scratch for a candidate's trial composition.
    std::vector<Matrix44f> savedFrame;
    std::vector<Matrix44f> savedSkin;
};

void SetLocal(Transform& local, const Track& t, const FValue& v) {
    switch (t.channel->target.channel) {
    case Channel::Translation:
        local.translation = FromF<Vector3f>(v);
        break;
    case Channel::Rotation:
        local.rotation = FromF<Quaternion>(v);
        break;
    default:
        local.scale = t.type == geom::AttrType::F32 ? Vector3f{v[0], v[0], v[0]} : FromF<Vector3f>(v);
        break;
    }
}

/// The check times inside `[from, to)` on the track's own clock.
std::pair<std::size_t, std::size_t> CheckRange(const ClipWork& w, i32 from, i32 to) {
    const auto lo = std::lower_bound(w.checkMs.begin(), w.checkMs.end(), from - w.origin);
    const auto hi = std::lower_bound(w.checkMs.begin(), w.checkMs.end(), to - w.origin);
    return {static_cast<std::size_t>(lo - w.checkMs.begin()),
            static_cast<std::size_t>(hi - w.checkMs.begin())};
}

/// The worst share of its ε any node under @p t's node uses at the check
/// times in `[a, c)`, with @p t read as @p a then @p c. With @p commit the
/// poses keep the change.
f64 NodeCost(ClipWork& w, const Context& ctx, const Track& t, const FKey& a, const FKey& c, bool commit) {
    const Skeleton& sk = *ctx.sk;
    const std::vector<u32>& under = sk.subtree[t.node];
    const auto [lo, hi] = CheckRange(w, a.ms, c.ms);
    f64 cost = 0.0;
    w.savedFrame.resize(under.size());
    w.savedSkin.resize(under.size());
    for (std::size_t i = lo; i < hi; ++i) {
        Pose& pose = w.cur[i];
        const Transform savedLocal = pose.local[t.node];
        if (!commit) {
            for (std::size_t k = 0; k < under.size(); ++k) {
                w.savedFrame[k] = pose.frame[under[k]];
                w.savedSkin[k] = pose.skinning[under[k]];
            }
        }
        SetLocal(pose.local[t.node], t, PairF(t, a, c, w.checkMs[i] + w.origin));
        for (const u32 d : under) {
            ctx.animator->composeNode(pose, d, sk.order.parent[d]);
        }
        for (const u32 d : under) {
            f64 error = 0.0;
            if (ctx.measure == KeyReduceOptions::Measure::Mesh && !sk.reps[d].empty()) {
                for (const Rep& rep : sk.reps[d]) {
                    const Vector3f moved = Skin(rep, w.origSkin[i]) - Skin(rep, pose.skinning);
                    error = std::max<f64>(error, std::sqrt(moved.dot(moved)));
                }
                // What it carries past its vertices still answers to the bound.
                if (sk.floored[d] != 0) {
                    error = std::max(error, BoundError(sk, d, w.origSkin[i][d], pose.skinning[d]));
                }
            } else {
                error = BoundError(sk, d, w.origSkin[i][d], pose.skinning[d]);
            }
            cost = std::max(cost, error / (ctx.epsilon * sk.share[d]));
        }
        if (!commit) {
            pose.local[t.node] = savedLocal;
            for (std::size_t k = 0; k < under.size(); ++k) {
                pose.frame[under[k]] = w.savedFrame[k];
                pose.skinning[under[k]] = w.savedSkin[k];
            }
            if (cost > 1.0) {
                return cost; // Over already: how far over no queue needs.
            }
        }
    }
    return cost;
}

/// @p knots and, between each two, samples at 120 Hz, at most 16 of them:
/// between two key times every track is one segment, which 16 samples follow,
/// and a WoW global loop's twenty-minute gap is not 150,000 of them.
std::vector<i32> Between(std::vector<i32> knots) {
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    std::vector<i32> out;
    for (std::size_t i = 0; i < knots.size(); ++i) {
        out.push_back(knots[i]);
        if (i + 1 == knots.size()) {
            break;
        }
        const i64 gap = static_cast<i64>(knots[i + 1]) - knots[i];
        const i64 count = std::clamp<i64>((gap * 120 + 999) / 1000, 1, 16);
        for (i64 k = 1; k < count; ++k) {
            out.push_back(knots[i] + static_cast<i32>(gap * k / count));
        }
    }
    return out;
}

/// The samples a non-node track is judged at in `[a, c)`: the original keys,
/// the midpoints between them, and quarters on a curve.
std::vector<i32> ChannelSamples(const Track& t, i32 from, i32 to) {
    std::vector<i32> out{from};
    std::vector<i32> knots{from};
    for (const FKey& k : t.orig) {
        if (k.ms > from && k.ms < to) {
            knots.push_back(k.ms);
        }
    }
    knots.push_back(to);
    for (std::size_t i = 0; i + 1 < knots.size(); ++i) {
        const i32 a = knots[i];
        const i32 b = knots[i + 1];
        if (i > 0) {
            out.push_back(a);
        }
        for (i32 q = 1; q < 4; ++q) {
            const i32 m = a + (b - a) * q / 4;
            if ((t.curved || q == 2) && m > a && m < b) {
                out.push_back(m);
            }
        }
    }
    // A refit span is judged where no key is, as the node's are (`CheckTimes`).
    if (t.refit) {
        const std::vector<i32> grid = Between(knots);
        out.insert(out.end(), grid.begin(), grid.end());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

f64 TurnDegrees(const FValue& a, const FValue& b) {
    const Quaternion x = FromF<Quaternion>(a);
    const Quaternion y = FromF<Quaternion>(b);
    const f64 la = std::sqrt(static_cast<f64>(x.dot(x)));
    const f64 lb = std::sqrt(static_cast<f64>(y.dot(y)));
    if (!(la > 0.0) || !(lb > 0.0)) {
        return la == lb ? 0.0 : kInfeasible;
    }
    const f64 cosine = std::min(std::fabs(static_cast<f64>(x.dot(y))) / (la * lb), 1.0);
    return 2.0 * std::acos(cosine) * 57.29577951308232;
}

f64 ChannelCost(const Track& t, const FKey& a, const FKey& c) {
    f64 cost = 0.0;
    const std::size_t n = kFloatsOf(t.type);
    for (const i32 ms : ChannelSamples(t, a.ms, c.ms)) {
        const FValue o = OrigAt(t, ms);
        const FValue v = PairF(t, a, c, ms);
        if (t.type == geom::AttrType::U32) {
            if (std::memcmp(o.data(), v.data(), 4) != 0) {
                return kInfeasible;
            }
            continue;
        }
        f64 worst = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const f64 d = std::fabs(static_cast<f64>(o[i]) - v[i]);
            if (!(d <= std::numeric_limits<f64>::max())) {
                return kInfeasible;
            }
            if (t.kind == Kind::Colour && (o[i] == 0.0f || o[i] == 1.0f) && d > t.noise) {
                return kInfeasible;
            }
            worst = std::max(worst, d);
        }
        switch (t.kind) {
        case Kind::UvTurn:
            worst = t.type == geom::AttrType::Quat ? TurnDegrees(o, v) : worst * 57.29577951308232;
            break;
        case Kind::Exact:
            if (worst > t.noise) {
                return kInfeasible;
            }
            worst = 0.0;
            break;
        default:
            break;
        }
        cost = std::max(cost, worst / t.scale);
    }
    return cost;
}

/// Keys @p a and @p c with the tangents facing their span refit, when the
/// track allows it.
std::pair<FKey, FKey> Facing(ClipWork& w, const Track& t, u32 a, u32 c) {
    FKey from = t.cur[a];
    FKey to = t.cur[c];
    if (t.refit) {
        std::vector<i32> samples;
        if (t.kind == Kind::Node) {
            const auto [lo, hi] = CheckRange(w, from.ms, to.ms);
            for (std::size_t i = lo; i < hi; ++i) {
                samples.push_back(w.checkMs[i] + w.origin);
            }
        } else {
            samples = ChannelSamples(t, from.ms, to.ms);
        }
        samples.push_back(to.ms);
        Refit(t, from, to, samples);
    }
    return {from, to};
}

/// The cost of @p t read with keys @p a and @p c adjacent.
f64 SpanCost(ClipWork& w, const Context& ctx, const Track& t, u32 a, u32 c) {
    const auto [from, to] = Facing(w, t, a, c);
    return t.kind == Kind::Node ? NodeCost(w, ctx, t, from, to, false) : ChannelCost(t, from, to);
}

/// Makes @p a and @p c adjacent in @p t: the keys between go, the facing
/// tangents take their refit, and a node's poses follow.
void CommitSpan(ClipWork& w, const Context& ctx, Track& t, u32 a, u32 c) {
    const auto [from, to] = Facing(w, t, a, c);
    for (u32 k = t.next[a]; k != c && k < t.kept.size(); k = t.next[k]) {
        t.kept[k] = 0;
    }
    t.next[a] = c;
    t.prev[c] = a;
    t.cur[a].out = from.out;
    t.cur[c].in = to.in;
    if (t.kind == Kind::Node) {
        NodeCost(w, ctx, t, from, to, true);
    }
}

// ---- The searches (§4.5) --------------------------------------------------------------

struct Entry {
    f64 cost = 0.0;
    u32 track = 0;
    u32 key = 0;
    u32 version = 0;
    bool operator>(const Entry& other) const {
        return cost > other.cost;
    }
};

using Queue = std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>>;

bool Removable(const Track& t, u32 k) {
    return t.reducible && t.kept[k] != 0 && t.fixed[k] == 0;
}

void Push(Queue& queue, ClipWork& w, const Context& ctx, u32 index, u32 k) {
    Track& t = w.tracks[index];
    if (!Removable(t, k)) {
        return;
    }
    const f64 cost = SpanCost(w, ctx, t, t.prev[k], t.next[k]);
    if (cost <= 1.0) {
        queue.push(Entry{cost, index, k, ++t.version[k]});
    }
}

/// Balanced: the cheapest removal anywhere first, re-costed when popped.
void Balanced(ClipWork& w, const Context& ctx) {
    Queue queue;
    for (u32 i = 0; i < w.tracks.size(); ++i) {
        for (u32 k = 0; k < w.tracks[i].kept.size(); ++k) {
            Push(queue, w, ctx, i, k);
        }
    }
    while (!queue.empty()) {
        const Entry top = queue.top();
        queue.pop();
        Track& t = w.tracks[top.track];
        if (!Removable(t, top.key) || top.version != t.version[top.key]) {
            continue;
        }
        const f64 cost = SpanCost(w, ctx, t, t.prev[top.key], t.next[top.key]);
        if (cost > 1.0) {
            continue;
        }
        // Dearer than it was and than the next: back in line.
        if (cost > top.cost + 1e-12 && !queue.empty() && cost > queue.top().cost) {
            queue.push(Entry{cost, top.track, top.key, ++t.version[top.key]});
            continue;
        }
        const u32 a = t.prev[top.key];
        const u32 c = t.next[top.key];
        CommitSpan(w, ctx, t, a, c);
        Push(queue, w, ctx, top.track, a);
        Push(queue, w, ctx, top.track, c);
    }
}

/// Maximum: per track, nodes parents first, the fewest keys whose spans all
/// fit — a span depends only on its two ends, the rest as it stands.
void Maximum(ClipWork& w, const Context& ctx) {
    std::vector<u32> order;
    std::vector<u32> rank(ctx.sk->order.parent.size(), 0);
    for (std::size_t i = 0; i < ctx.sk->order.order.size(); ++i) {
        rank[ctx.sk->order.order[i]] = static_cast<u32>(i);
    }
    for (u32 i = 0; i < w.tracks.size(); ++i) {
        if (w.tracks[i].reducible) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        const Track& x = w.tracks[a];
        const Track& y = w.tracks[b];
        const u32 rx = x.kind == Kind::Node ? rank[x.node] : ~0u;
        const u32 ry = y.kind == Kind::Node ? rank[y.node] : ~0u;
        return rx < ry;
    });
    for (const u32 index : order) {
        Track& t = w.tracks[index];
        std::vector<u32> alive;
        for (u32 k = 0; k < t.kept.size(); ++k) {
            if (t.kept[k] != 0) {
                alive.push_back(k);
            }
        }
        const std::size_t n = alive.size();
        if (n < 3) {
            continue;
        }
        std::vector<u32> best(n, ~0u);
        std::vector<std::size_t> from(n, 0);
        best[0] = 0;
        for (std::size_t j = 1; j < n; ++j) {
            u32 misses = 0;
            for (std::size_t i = j; i-- > 0;) {
                if (best[i] != ~0u && (i + 1 == j || SpanCost(w, ctx, t, alive[i], alive[j]) <= 1.0)) {
                    if (best[i] + 1 < best[j]) {
                        best[j] = best[i] + 1;
                        from[j] = i;
                    }
                    misses = 0;
                } else if (++misses >= 4) {
                    break;
                }
                // A fixed key is never jumped.
                if (t.fixed[alive[i]] != 0) {
                    break;
                }
            }
        }
        std::vector<std::size_t> chain{n - 1};
        while (chain.back() != 0) {
            chain.push_back(from[chain.back()]);
        }
        for (std::size_t k = chain.size(); k-- > 1;) {
            if (chain[k - 1] != chain[k] + 1) {
                CommitSpan(w, ctx, t, alive[chain[k]], alive[chain[k - 1]]);
            }
        }
    }
}

/// Align to poses: a clip time goes from every node track keyed there at once,
/// while the whole model stays within ε; the tracks are reduced afterwards.
void AlignPoses(ClipWork& w, const Context& ctx) {
    std::map<i32, std::vector<std::pair<u32, u32>>> at; // time -> (track, key)
    for (u32 i = 0; i < w.tracks.size(); ++i) {
        const Track& t = w.tracks[i];
        if (t.kind != Kind::Node) {
            continue;
        }
        for (u32 k = 0; k < t.kept.size(); ++k) {
            if (Removable(t, k)) {
                at[t.cur[k].ms].push_back({i, k});
            }
        }
    }
    const Skeleton& sk = *ctx.sk;
    // Whether removing every key at one time keeps the model within ε. With
    // @p commit the removal stays.
    const auto attempt = [&](const std::vector<std::pair<u32, u32>>& removed, bool commit) {
        i32 from = std::numeric_limits<i32>::max();
        i32 to = std::numeric_limits<i32>::min();
        for (const auto& [i, k] : removed) {
            const Track& t = w.tracks[i];
            if (!Removable(t, k)) {
                return false;
            }
            from = std::min(from, t.cur[t.prev[k]].ms);
            to = std::max(to, t.cur[t.next[k]].ms);
        }
        const auto [lo, hi] = CheckRange(w, from, to);
        bool fits = true;
        for (std::size_t c = lo; c < hi && fits; ++c) {
            Pose trial = w.cur[c];
            for (const auto& [i, k] : removed) {
                const Track& t = w.tracks[i];
                const auto [a, b] = Facing(w, t, t.prev[k], t.next[k]);
                const i32 ms = w.checkMs[c] + w.origin;
                if (ms >= a.ms && ms < b.ms) {
                    SetLocal(trial.local[t.node], t, PairF(t, a, b, ms));
                }
            }
            for (const u32 n : sk.order.order) {
                ctx.animator->composeNode(trial, n, sk.order.parent[n]);
            }
            for (u32 d = 0; d < trial.skinning.size() && fits; ++d) {
                fits = BoundError(sk, d, w.origSkin[c][d], trial.skinning[d]) <= ctx.epsilon * sk.share[d];
            }
        }
        if (fits && commit) {
            for (const auto& [i, k] : removed) {
                Track& t = w.tracks[i];
                CommitSpan(w, ctx, t, t.prev[k], t.next[k]);
            }
        }
        return fits;
    };
    for (const auto& [time, removed] : at) {
        if (removed.size() > 1 && attempt(removed, false)) {
            attempt(removed, true);
        }
    }
}

// ---- Building and writing back ----------------------------------------------------------

/// The clip-local times the skeleton is checked at: every node key of the clip
/// and the midpoints between them.
std::vector<i32> CheckTimes(const ClipWork& w) {
    std::vector<i32> keys;
    for (const Track& t : w.tracks) {
        if (t.kind == Kind::Node) {
            for (const FKey& k : t.orig) {
                keys.push_back(k.ms - w.origin);
            }
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::vector<i32> out;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        out.push_back(keys[i]);
        if (i + 1 < keys.size() && keys[i + 1] - keys[i] > 1) {
            out.push_back((keys[i] + keys[i + 1]) / 2);
        }
    }
    // A refit span is a cubic of the fit's own making, which can bulge where
    // no key is: judged, and fitted, on the final check's 120 Hz grid too.
    const bool refits = std::any_of(w.tracks.begin(), w.tracks.end(),
                                    [](const Track& t) { return t.kind == Kind::Node && t.refit; });
    if (refits && !keys.empty()) {
        const std::vector<i32> grid = Between(keys);
        out.insert(out.end(), grid.begin(), grid.end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](i32 t) { return t < 0; }), out.end());
    return out;
}

Pose PoseAt(const Animator& animator, u32 clip, i32 ms) {
    Mix mix;
    mix.plays.push_back(Play{clip, static_cast<f32>(ms) / 1000.0f, 1.0f, false});
    mix.globals = false;
    Pose pose;
    animator.sample(mix, pose, true);
    animator.compose(pose);
    return pose;
}

bool InList(const std::vector<u32>& list, u32 value) {
    return list.empty() || std::find(list.begin(), list.end(), value) != list.end();
}

ClipWork BuildClip(const Document& document, u32 model, u32 clip, const Animator& animator,
                   const std::set<u32>& agreeing, const KeyReduceOptions& options) {
    const Clip& source = document.clips[clip];
    ClipWork w;
    w.clip = clip;
    w.origin = source.readRule == ReadRule::Sc2 ? static_cast<i32>(source.native.value("startFrame", 0)) : 0;
    const bool layered = source.containers.size() != 1;
    const SampleWindow window = ClipWindow(source, 0, -1);
    for (u32 k = 0; k < source.containers.size(); ++k) {
        const auto& tracks = source.containers[k].subTracks;
        for (u32 i = 0; i < tracks.size(); ++i) {
            const SubTrack& sub = tracks[i];
            const AnimChannel* channel = Eligible(document, model, sub);
            if (channel == nullptr) {
                continue;
            }
            Track t;
            t.container = k;
            t.index = i;
            t.channel = channel;
            t.type = channel->valueType;
            t.kind = KindOf(*channel, layered);
            t.node = channel->target.node;
            t.stored = sub.interp;
            bool read = false;
            WithType(t.type, [&]<class T>(bool quat) {
                const PreparedTrack<T> p = Prepare<T>(source, sub, quat);
                if (!p.valid || p.keys.size() < 2) {
                    return;
                }
                read = true;
                t.quat = quat;
                t.rule = p.rule;
                t.interp = PairInterpolation(p);
                for (const Key<T>& key : p.keys) {
                    FKey f;
                    f.ms = key.ms;
                    f.value = ToF(key.value);
                    f.in = ToF(key.in);
                    f.out = ToF(key.out);
                    f.source = key.source;
                    t.orig.push_back(f);
                    t.fixed.push_back(key.source < 0 || !ReadAt(source, key, window) ? 1 : 0);
                }
                t.noise = Noise(Magnitude(p.keys));
            });
            if (!read) {
                continue;
            }
            const std::size_t n = t.orig.size();
            for (std::size_t k2 = 1; k2 < n; ++k2) {
                if (t.orig[k2].ms <= t.orig[k2 - 1].ms) {
                    t.fixed.assign(n, 1); // Two keys on one millisecond: no span between.
                }
            }
            std::size_t first = n;
            std::size_t last = n;
            for (std::size_t k2 = 0; k2 < n; ++k2) {
                if (t.fixed[k2] == 0) {
                    first = first == n ? k2 : first;
                    last = k2;
                }
            }
            if (first < n) {
                t.fixed[first] = t.fixed[last] = 1;
            }
            t.fixed[0] = t.fixed[n - 1] = 1;
            for (const KeptKeys& keep : options.kept) {
                if (keep.clip != clip || keep.channel != sub.channel) {
                    continue;
                }
                for (std::size_t k2 = 0; k2 < n; ++k2) {
                    const i32 at = t.orig[k2].source;
                    if (at >= 0 && static_cast<std::size_t>(at) < sub.times.size() &&
                        std::find(keep.times.begin(), keep.times.end(), sub.times[static_cast<std::size_t>(at)]) !=
                            keep.times.end()) {
                        t.fixed[k2] = 1;
                    }
                }
            }
            t.cur = t.orig;
            t.kept.assign(n, 1);
            t.version.assign(n, 0);
            t.prev.resize(n);
            t.next.resize(n);
            for (u32 k2 = 0; k2 < n; ++k2) {
                t.prev[k2] = k2 == 0 ? 0 : k2 - 1;
                t.next[k2] = k2 + 1 < n ? k2 + 1 : k2;
            }
            const bool smooth = t.interp == Interpolation::Hermite || t.interp == Interpolation::Bezier;
            t.curved = (t.quat && t.rule != ReadRule::Sc2) || smooth;
            t.refit = options.refitTangents && smooth && !t.quat;
            t.reducible = agreeing.count(sub.channel) != 0 && InList(options.channels, sub.channel);
            if (t.kind == Kind::Node && t.node >= document.models[model].nodes.size()) {
                t.kind = Kind::Exact;
            }
            f64 peak = 0.0;
            for (const FKey& key : t.orig) {
                for (std::size_t c = 0; c < kFloatsOf(t.type); ++c) {
                    if (std::isfinite(key.value[c])) {
                        peak = std::max(peak, static_cast<f64>(std::fabs(key.value[c])));
                    }
                }
            }
            switch (t.kind) {
            case Kind::Colour:
                t.scale = options.companions.colour;
                break;
            case Kind::Uv:
                t.scale = options.companions.uv;
                break;
            case Kind::UvTurn:
                t.scale = options.companions.uvTurnDegrees;
                break;
            case Kind::Relative:
                t.scale = std::max(options.companions.relative * peak, t.noise);
                break;
            default:
                t.scale = 1.0;
                break;
            }
            w.tracks.push_back(std::move(t));
        }
    }
    w.checkMs = CheckTimes(w);
    w.cur.reserve(w.checkMs.size());
    w.origSkin.reserve(w.checkMs.size());
    for (const i32 ms : w.checkMs) {
        Pose pose = PoseAt(animator, clip, ms);
        w.origSkin.push_back(pose.skinning);
        w.cur.push_back(std::move(pose));
    }
    return w;
}

void WriteBack(Document& document, const ClipWork& w) {
    Clip& clip = document.clips[w.clip];
    for (const Track& t : w.tracks) {
        SubTrack& sub = clip.containers[t.container].subTracks[t.index];
        std::vector<u8> keep(sub.times.size(), 1);
        const std::size_t size = geom::AttrTypeSize(t.type);
        const std::size_t perKey = ValuesPerKey(sub.interp);
        for (std::size_t k = 0; k < t.cur.size(); ++k) {
            const i32 source = t.cur[k].source;
            if (source < 0 || static_cast<std::size_t>(source) >= keep.size()) {
                continue;
            }
            if (t.kept[k] == 0) {
                keep[static_cast<std::size_t>(source)] = 0;
            } else if (perKey == 3) {
                // A refit tangent: only the side facing a removed span changed.
                u8* at = sub.values.data() + static_cast<std::size_t>(source) * perKey * size;
                if (std::memcmp(t.cur[k].in.data(), t.orig[k].in.data(), size) != 0) {
                    std::memcpy(at + size, t.cur[k].in.data(), size);
                }
                if (std::memcmp(t.cur[k].out.data(), t.orig[k].out.data(), size) != 0) {
                    std::memcpy(at + 2 * size, t.cur[k].out.data(), size);
                }
            }
        }
        Filter(sub, t.type, keep);
    }
}

// ---- The mesh measure (§4.3) ------------------------------------------------------------

/// Every mesh of the model with what the measure needs of it, found once.
struct MeshData {
    const Mesh* mesh = nullptr;
    std::vector<std::vector<geom::Influence>> bindings;
    std::vector<f32> area;     ///< Each vertex's share of face area.
    std::vector<u32> dominant; ///< The node weighing most on each vertex.
};

std::vector<MeshData> MeshesOf(const Model& model) {
    std::vector<MeshData> out;
    for (const Mesh& mesh : model.meshes) {
        MeshData data;
        data.mesh = &mesh;
        data.bindings = BindingsOf(mesh);
        const auto positions =
            mesh.attributes.get<const Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
        data.area.assign(positions.size(), 0.0f);
        const geom::FaceSet& faces = mesh.faceSet();
        std::size_t corner = 0;
        for (std::size_t f = 0; f < faces.faceValence.size(); ++f) {
            const u32 valence = faces.faceValence[f];
            f32 face = 0.0f;
            for (u32 k = 1; k + 1 < valence; ++k) {
                const u32 a = faces.cornerVertex[corner];
                const u32 b = faces.cornerVertex[corner + k];
                const u32 c = faces.cornerVertex[corner + k + 1];
                if (a < positions.size() && b < positions.size() && c < positions.size()) {
                    const Vector3f e1 = positions[b] - positions[a];
                    const Vector3f e2 = positions[c] - positions[a];
                    const Vector3f n = cross(e1, e2);
                    face += 0.5f * std::sqrt(n.dot(n));
                }
            }
            for (u32 k = 0; k < valence; ++k) {
                const u32 v = faces.cornerVertex[corner + k];
                if (v < data.area.size() && std::isfinite(face)) {
                    data.area[v] += face / static_cast<f32>(valence);
                }
            }
            corner += valence;
        }
        data.dominant.assign(positions.size(), kInvalidNode);
        for (std::size_t v = 0; v < data.bindings.size() && v < data.dominant.size(); ++v) {
            f32 best = 0.0f;
            for (const geom::Influence& i : data.bindings[v]) {
                if (i.weight > best) {
                    best = i.weight;
                    data.dominant[v] = i.bone;
                }
            }
        }
        out.push_back(std::move(data));
    }
    return out;
}

/// The grid a clip is measured on: its keys, and 120 Hz between them
/// (`Between`), on to its last key when a track runs past its end.
std::vector<i32> DenseTimes(const Clip& clip) {
    i32 end = static_cast<i32>(ClipMs(clip));
    if (clip.readRule != ReadRule::Wc3) {
        for (const SubTrackContainer& container : clip.containers) {
            for (const SubTrack& track : container.subTracks) {
                if (!track.times.empty()) {
                    end = std::max(end, static_cast<i32>(std::lround(track.times.back() * 1000.0f)));
                }
            }
        }
    }
    std::vector<i32> knots{0, end};
    for (const SubTrackContainer& container : clip.containers) {
        for (const SubTrack& track : container.subTracks) {
            for (const f32 t : track.times) {
                const i32 ms = static_cast<i32>(std::lround(t * 1000.0f));
                if (ms > 0 && ms < end) {
                    knots.push_back(ms);
                }
            }
        }
    }
    return Between(std::move(knots));
}

struct Measured {
    f32 maxError = 0.0f;
    f64 squared = 0.0; ///< Area-weighted sum of squares.
    f64 weight = 0.0;
    std::vector<f32> nodeMax;
    std::vector<f64> nodeSquared;
    std::vector<f64> nodeWeight;
    std::vector<std::vector<f32>> vertexMax; ///< Per mesh.
};

/// Palettes of @p clip at @p times.
std::vector<std::vector<Matrix44f>> Palettes(const Animator& animator, u32 clip, const std::vector<i32>& times) {
    std::vector<std::vector<Matrix44f>> out;
    out.reserve(times.size());
    for (const i32 ms : times) {
        out.push_back(PoseAt(animator, clip, ms).skinning);
    }
    return out;
}

Measured Compare(const std::vector<MeshData>& meshes, u32 nodes,
                 const std::vector<std::vector<Matrix44f>>& was,
                 const std::vector<std::vector<Matrix44f>>& now) {
    Measured out;
    out.nodeMax.assign(nodes, 0.0f);
    out.nodeSquared.assign(nodes, 0.0);
    out.nodeWeight.assign(nodes, 0.0);
    out.vertexMax.resize(meshes.size());
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        out.vertexMax[m].assign(meshes[m].area.size(), 0.0f);
    }
    for (std::size_t i = 0; i < was.size() && i < now.size(); ++i) {
        for (std::size_t m = 0; m < meshes.size(); ++m) {
            const MeshData& data = meshes[m];
            const std::vector<Vector3f> a = skinning::DeformMesh(*data.mesh, was[i]);
            const std::vector<Vector3f> b = skinning::DeformMesh(*data.mesh, now[i]);
            for (std::size_t v = 0; v < a.size() && v < b.size(); ++v) {
                const Vector3f d = a[v] - b[v];
                const f32 e = std::sqrt(d.dot(d));
                if (!std::isfinite(e)) {
                    continue; // A source that poses to NaN, alike either way.
                }
                const f64 w = v < data.area.size() ? data.area[v] : 0.0;
                out.maxError = std::max(out.maxError, e);
                if (v < out.vertexMax[m].size()) {
                    out.vertexMax[m][v] = std::max(out.vertexMax[m][v], e);
                }
                out.squared += w * e * e;
                out.weight += w;
                const u32 node = v < data.dominant.size() ? data.dominant[v] : kInvalidNode;
                if (node < nodes) {
                    out.nodeMax[node] = std::max(out.nodeMax[node], e);
                    out.nodeSquared[node] += w * e * e;
                    out.nodeWeight[node] += w;
                }
            }
        }
    }
    return out;
}

/// Keys per clip and per node of @p document's @p clips: what a report
/// compares before and after.
struct KeyCounts {
    std::vector<u64> clips;
    std::vector<u64> nodes;
};

KeyCounts CountsOf(const Document& document, u32 model, const std::vector<u32>& clips) {
    KeyCounts out;
    out.nodes.assign(document.models[model].nodes.size(), 0);
    for (const u32 c : clips) {
        out.clips.push_back(CountKeys(document, model, {c}));
        for (const SubTrackContainer& container : document.clips[c].containers) {
            for (const SubTrack& track : container.subTracks) {
                const AnimChannel* channel = document.models[model].animChannels.find(track.channel);
                if (channel != nullptr && channel->target.kind == TrackTarget::Kind::Node &&
                    channel->target.node < out.nodes.size()) {
                    out.nodes[channel->target.node] += track.times.size();
                }
            }
        }
    }
    return out;
}

void Rows(const KeyCounts& before, const KeyCounts& after, const std::vector<u32>& clips,
          const std::vector<Measured>& measured, KeyReduceReport& report) {
    const std::size_t nodes = before.nodes.size();
    std::vector<KeyErrorRow> nodeRows(nodes);
    std::vector<f64> nodeSquared(nodes, 0.0);
    std::vector<f64> nodeWeight(nodes, 0.0);
    f64 squared = 0.0;
    f64 weight = 0.0;
    for (std::size_t n = 0; n < nodes; ++n) {
        nodeRows[n].index = static_cast<u32>(n);
        nodeRows[n].keysBefore = before.nodes[n];
        nodeRows[n].keysAfter = n < after.nodes.size() ? after.nodes[n] : 0;
    }
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const Measured& m = measured[i];
        KeyErrorRow row;
        row.index = clips[i];
        row.keysBefore = before.clips[i];
        row.keysAfter = after.clips[i];
        row.maxError = m.maxError;
        row.rmsError = m.weight > 0.0 ? static_cast<f32>(std::sqrt(m.squared / m.weight)) : 0.0f;
        report.clips.push_back(row);
        report.keysBefore += row.keysBefore;
        report.keysAfter += row.keysAfter;
        report.maxError = std::max(report.maxError, m.maxError);
        squared += m.squared;
        weight += m.weight;
        for (std::size_t n = 0; n < nodes && n < m.nodeMax.size(); ++n) {
            nodeRows[n].maxError = std::max(nodeRows[n].maxError, m.nodeMax[n]);
            nodeSquared[n] += m.nodeSquared[n];
            nodeWeight[n] += m.nodeWeight[n];
        }
    }
    report.rmsError = weight > 0.0 ? static_cast<f32>(std::sqrt(squared / weight)) : 0.0f;
    for (const Measured& m : measured) {
        report.vertexError.resize(std::max(report.vertexError.size(), m.vertexMax.size()));
        for (std::size_t k = 0; k < m.vertexMax.size(); ++k) {
            std::vector<f32>& into = report.vertexError[k];
            into.resize(std::max(into.size(), m.vertexMax[k].size()), 0.0f);
            for (std::size_t v = 0; v < m.vertexMax[k].size(); ++v) {
                into[v] = std::max(into[v], m.vertexMax[k][v]);
            }
        }
    }
    for (std::size_t n = 0; n < nodes; ++n) {
        KeyErrorRow& row = nodeRows[n];
        row.rmsError =
            nodeWeight[n] > 0.0 ? static_cast<f32>(std::sqrt(nodeSquared[n] / nodeWeight[n])) : 0.0f;
        if (row.keysBefore != row.keysAfter || row.maxError > 0.0f) {
            report.nodes.push_back(row);
        }
    }
}

std::vector<u32> ClipsOf(const Document& document, u32 model, const std::vector<u32>& wanted) {
    std::vector<u32> out;
    for (u32 c = 0; c < document.clips.size(); ++c) {
        if (document.clips[c].model == model && InList(wanted, c)) {
            out.push_back(c);
        }
    }
    return out;
}

template <class F>
void Parallel(std::size_t count, u32 threads, F&& work) {
    const u32 wanted = threads != 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
    const u32 used = static_cast<u32>(std::min<std::size_t>(wanted, count));
    std::atomic<std::size_t> nextItem{0};
    const auto run = [&] {
        for (std::size_t i = nextItem++; i < count; i = nextItem++) {
            work(i);
        }
    };
    if (used <= 1) {
        run();
        return;
    }
    std::vector<std::thread> pool;
    for (u32 t = 0; t < used; ++t) {
        pool.emplace_back(run);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

/// Reduces one clip at @p epsilon; its keys only, from the document as it is.
void ReduceClip(Document& document, u32 model, u32 clip, const Skeleton& sk, f64 epsilon,
                const std::set<u32>& agreeing, const KeyReduceOptions& options) {
    const Animator animator(document, model);
    ClipWork w = BuildClip(document, model, clip, animator, agreeing, options);
    Context ctx;
    ctx.sk = &sk;
    ctx.animator = &animator;
    ctx.epsilon = epsilon;
    ctx.measure = options.measure;
    if (options.alignPoses) {
        AlignPoses(w, ctx);
    }
    if (options.search == KeyReduceOptions::Search::Maximum) {
        Maximum(w, ctx);
    } else {
        Balanced(w, ctx);
    }
    WriteBack(document, w);
}

} // namespace

KeyReduceReport ReduceKeys(Document& document, u32 model, const KeyReduceOptions& options) {
    KeyReduceReport report;
    if (model >= document.models.size()) {
        return report;
    }
    const Model& owner = document.models[model];
    const f64 height = ModelHeight(owner);
    const f64 epsilon = options.tolerance > 0.0f   ? static_cast<f64>(options.tolerance)
                        : options.heightShare > 0.0f ? options.heightShare * height
                                                     : 1e-3 * height;
    report.tolerance = static_cast<f32>(epsilon);
    const std::vector<u32> clips = ClipsOf(document, model, options.clips);
    if (clips.empty()) {
        return report;
    }
    const KeyCounts before = CountsOf(document, model, clips);

    // What no rule reads goes first, and a constant is one key: tier 1's own.
    ExactKeyReport exact;
    DropUnreadKeys(document, model, clips, options.channels, exact);
    CollapseConstantTracks(document, model, clips, options.channels, exact);
    const std::set<u32> agreeing = AgreeingChannels(document, model);

    // Everything the threads read and nothing writes, found here: a face set
    // regenerates on first read.
    const std::vector<MeshData> meshes = MeshesOf(owner);
    const Animator shape(document, model);
    const Skeleton sk = SkeletonOf(document, model, shape, height, options);
    const u32 nodes = owner.nodes.size();

    std::vector<Measured> measured(clips.size());
    std::vector<u8> repaired(clips.size(), 0);
    std::vector<u8> kept(clips.size(), 0);
    std::mutex progressLock;
    u32 done = 0;
    Parallel(clips.size(), options.threads, [&](std::size_t i) {
        const u32 c = clips[i];
        if (options.cancelled && options.cancelled()) {
            kept[i] = 1;
            return;
        }
        const std::vector<i32> dense = DenseTimes(document.clips[c]);
        std::vector<std::vector<Matrix44f>> was;
        {
            const Animator animator(document, model);
            was = Palettes(animator, c, dense);
        }
        const std::vector<SubTrackContainer> original = document.clips[c].containers;
        const auto measure = [&] {
            const Animator animator(document, model);
            return Compare(meshes, nodes, was, Palettes(animator, c, dense));
        };
        const f64 slack = epsilon * (1.0 + 1e-3) + 1e-6 * height;
        ReduceClip(document, model, c, sk, epsilon, agreeing, options);
        measured[i] = measure();
        // Again from the clip's own keys: the Mesh measure's sampled vertices
        // missed one, so the bound over all of them; then the bound missed a
        // stretch between its check times, so half the room.
        KeyReduceOptions retry = options;
        retry.measure = KeyReduceOptions::Measure::Skeleton;
        for (const f64 share : {options.measure == KeyReduceOptions::Measure::Mesh ? 1.0 : 0.5, 0.5}) {
            if (measured[i].maxError <= slack) {
                break;
            }
            document.clips[c].containers = original;
            ReduceClip(document, model, c, sk, epsilon * share, agreeing, retry);
            measured[i] = measure();
            repaired[i] = 1;
            if (share == 0.5) {
                break;
            }
        }
        if (measured[i].maxError > slack) {
            document.clips[c].containers = original;
            measured[i] = measure();
            kept[i] = 1;
        }
        if (options.progress) {
            const std::lock_guard<std::mutex> lock(progressLock);
            options.progress(++done, static_cast<u32>(clips.size()));
        }
    });
    for (std::size_t i = 0; i < clips.size(); ++i) {
        if (repaired[i] != 0 && kept[i] == 0) {
            report.repaired.push_back(clips[i]);
        }
        if (kept[i] != 0) {
            report.kept.push_back(clips[i]);
        }
    }
    Rows(before, CountsOf(document, model, clips), clips, measured, report);
    return report;
}

KeyReduceReport MeasureKeyError(const Document& before, const Document& after, u32 model,
                                const std::vector<u32>& wanted) {
    KeyReduceReport report;
    if (model >= before.models.size() || model >= after.models.size()) {
        return report;
    }
    const std::vector<u32> clips = ClipsOf(before, model, wanted);
    const std::vector<MeshData> meshes = MeshesOf(before.models[model]);
    const u32 nodes = before.models[model].nodes.size();
    std::vector<Measured> measured(clips.size());
    Parallel(clips.size(), 0, [&](std::size_t i) {
        const u32 c = clips[i];
        const std::vector<i32> dense = DenseTimes(before.clips[c]);
        const Animator a(before, model);
        const Animator b(after, model);
        measured[i] = Compare(meshes, nodes, Palettes(a, c, dense), Palettes(b, c, dense));
    });
    report.tolerance = 1e-3f * ModelHeight(before.models[model]);
    Rows(CountsOf(before, model, clips), CountsOf(after, model, clips), clips, measured, report);
    return report;
}

} // namespace wem
} // namespace models
} // namespace whiteout
