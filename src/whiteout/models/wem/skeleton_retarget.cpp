// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file skeleton_retarget.cpp
 * @brief `RetargetSkeleton` — restating a rig in another convention (§10.5).
 *
 * The contract and the algebra are in `retarget.h`. What is here is the three
 * things the algebra does not say:
 *
 * 1. **Matrices, not `Transform`s.** `Compose` is the usual TRS composition and
 *    is exact only where the parent scale is uniform; a conjugation is a matrix
 *    product and has to be one, so everything below composes `ToMatrix` and
 *    decomposes once at the end.
 * 2. **Two sampling passes, no key storage.** The first decides which nodes
 *    shear — which decides the tree — and the second writes. Holding every
 *    sampled key instead would be tens of megabytes on a heavy `.m3` and would
 *    buy one pass.
 * 3. **The pivot is free.** `T(-p) * S * R * T(p + t)` has translation row
 *    `-p*A + p + t`, so `t` absorbs any choice of `p`, and the rig may therefore
 *    store the bind position there — which is what `.mdx`'s `PIVT` means and
 *    what a bone-position query wants.
 */

#include <whiteout/models/wem/retarget.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

namespace {

std::string number(u64 value) {
    return std::to_string(value);
}

// ============================================================================
// Matrix helpers
//
// Row vectors throughout, the convention `ToMatrix` documents: the 3x3 is
// `data[0..2][0..2]`, the translation is `data[3][0..2]`, and a chain composes
// child-first.
// ============================================================================

Matrix44f Linear(const Matrix44f& m) {
    Matrix44f out = Matrix44f::identity();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.data[r][c] = m.data[r][c];
        }
    }
    return out;
}

Vector3f TranslationOf(const Matrix44f& m) {
    return Vector3f{m.data[3][0], m.data[3][1], m.data[3][2]};
}

void SetTranslation(Matrix44f& m, const Vector3f& t) {
    m.data[3][0] = t.x;
    m.data[3][1] = t.y;
    m.data[3][2] = t.z;
}

Vector3f Apply(const Matrix44f& m, const Vector3f& v) {
    return Vector3f{v.x * m.data[0][0] + v.y * m.data[1][0] + v.z * m.data[2][0] + m.data[3][0],
                    v.x * m.data[0][1] + v.y * m.data[1][1] + v.z * m.data[2][1] + m.data[3][1],
                    v.x * m.data[0][2] + v.y * m.data[1][2] + v.z * m.data[2][2] + m.data[3][2]};
}

f32 Distance(const Vector3f& a, const Vector3f& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) +
                     (a.z - b.z) * (a.z - b.z));
}

/// The orthogonal factor of the 3x3, by Newton iteration on `R <- (R + R^-T)/2`.
/// The polar rotation rather than Gram-Schmidt because it is the *nearest* one,
/// which is what makes both the projection and the split well behaved.
Matrix44f PolarRotation(const Matrix44f& m) {
    Matrix44f r = Linear(m);
    for (int step = 0; step < 24; ++step) {
        const Matrix44f inverseTranspose = Matrix44f::inverse(r).transpose();
        Matrix44f next = Matrix44f::identity();
        f32 delta = 0.0f;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                next.data[i][j] = 0.5f * (r.data[i][j] + inverseTranspose.data[i][j]);
                delta = std::fmax(delta, std::fabs(next.data[i][j] - r.data[i][j]));
            }
        }
        r = next;
        if (delta < 1e-7f) {
            break;
        }
    }
    return r;
}

/// The nearest `diag(s) * R`: the polar rotation, with each row's projection
/// onto it as the scale. Exact when the input already is one — the common case.
Matrix44f ProjectToScaleRotation(const Matrix44f& m) {
    const Matrix44f rotation = PolarRotation(m);
    Matrix44f out = Matrix44f::identity();
    for (int r = 0; r < 3; ++r) {
        // Row r of `A * R^T` is `s_r * e_r` when `A = diag(s) * R`, so its r-th
        // entry is the scale and the other two are the shear this discards.
        f32 scale = 0.0f;
        for (int k = 0; k < 3; ++k) {
            scale += m.data[r][k] * rotation.data[r][k];
        }
        for (int c = 0; c < 3; ++c) {
            out.data[r][c] = scale * rotation.data[r][c];
        }
    }
    return out;
}

/// How far @p m is from the nearest `diag(s) * R` — the shear a pivot node
/// cannot hold, as the largest entry-wise difference.
f32 ShearOf(const Matrix44f& m) {
    const Matrix44f held = ProjectToScaleRotation(m);
    f32 worst = 0.0f;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            worst = std::fmax(worst, std::fabs(m.data[r][c] - held.data[r][c]));
        }
    }
    return worst;
}

f32 Determinant3(const Matrix44f& m) {
    return m.data[0][0] * (m.data[1][1] * m.data[2][2] - m.data[1][2] * m.data[2][1]) -
           m.data[0][1] * (m.data[1][0] * m.data[2][2] - m.data[1][2] * m.data[2][0]) +
           m.data[0][2] * (m.data[1][0] * m.data[2][1] - m.data[1][1] * m.data[2][0]);
}

/// Jacobi eigendecomposition of a symmetric 3x3. Returns `Q` with the
/// eigenvectors as *rows*, so `Q * A * Q^T` is `diag(out)`.
Matrix44f JacobiEigen(const Matrix44f& symmetric, Vector3f& out) {
    f32 a[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            a[i][j] = symmetric.data[i][j];
        }
    }
    Matrix44f q = Matrix44f::identity();
    for (int sweep = 0; sweep < 32; ++sweep) {
        f32 off = 0.0f;
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 3; ++j) {
                off += a[i][j] * a[i][j];
            }
        }
        if (off < 1e-16f) {
            break;
        }
        for (int p = 0; p < 3; ++p) {
            for (int r = p + 1; r < 3; ++r) {
                if (std::fabs(a[p][r]) < 1e-12f) {
                    continue;
                }
                const f32 theta = (a[r][r] - a[p][p]) / (2.0f * a[p][r]);
                const f32 sign = theta >= 0.0f ? 1.0f : -1.0f;
                const f32 t = sign / (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
                const f32 c = 1.0f / std::sqrt(t * t + 1.0f);
                const f32 s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const f32 akp = a[k][p], akr = a[k][r];
                    a[k][p] = c * akp - s * akr;
                    a[k][r] = s * akp + c * akr;
                }
                for (int k = 0; k < 3; ++k) {
                    const f32 apk = a[p][k], ark = a[r][k];
                    a[p][k] = c * apk - s * ark;
                    a[r][k] = s * apk + c * ark;
                }
                for (int k = 0; k < 3; ++k) {
                    const f32 qpk = q.data[p][k], qrk = q.data[r][k];
                    q.data[p][k] = c * qpk - s * qrk;
                    q.data[r][k] = s * qpk + c * qrk;
                }
            }
        }
    }
    out = Vector3f{a[0][0], a[1][1], a[2][2]};
    return q;
}

/**
 * @brief Factors a linear part into two a pivot node CAN hold.
 *
 * Always possible, and exact: polar gives `A = P * R` with `P` symmetric, and
 * `P = Q^T * D * Q`, so `A = Q^T * (D * Q * R)` — a pure rotation followed by a
 * scale-rotation. @p firstOut is the child's factor and @p secondOut the helper
 * parent's, and that order is forced: with row vectors the child's factor is the
 * left one, and there is no factorisation with the rotation on the right.
 *
 * **@p previous is what makes a sequence of keys usable.** The eigenbasis is
 * defined only up to the order of its vectors and their signs, so two adjacent
 * keys of a slowly turning bone can come back with two axes swapped — each
 * factorisation exact on its own, and the interpolation between them a tumble
 * through nothing. Aligning every key to the one before costs six dot products,
 * and it is the difference between an error that shrinks when keys are added and
 * one that does not: unaligned, subdividing a ProtossBuildingBirth bone four
 * times moved the worst error from 3037 units to 3174.
 */
void SplitIntoTwo(const Matrix44f& m, const Matrix44f* previous, Matrix44f& firstOut,
                  Matrix44f& secondOut) {
    const Matrix44f rotation = PolarRotation(m);
    const Matrix44f stretch = Linear(m) * rotation.transpose(); // symmetric
    Vector3f eigen;
    Matrix44f q = JacobiEigen(stretch, eigen);

    // `stretch` is `q^T * diag(eigen) * q`, and both a row permutation (carried
    // along by `eigen`) and a row negation leave that product alone — which is
    // exactly the freedom to spend on continuity.
    int order[3] = {0, 1, 2};
    const Matrix44f prior = previous != nullptr ? previous->transpose() : Matrix44f::identity();
    const auto align = [&prior](const Matrix44f& basis, int row, int against) {
        return basis.data[row][0] * prior.data[against][0] +
               basis.data[row][1] * prior.data[against][1] +
               basis.data[row][2] * prior.data[against][2];
    };
    if (previous != nullptr) {
        static const int kPerms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                         {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        f32 best = -1.0f;
        for (const auto& perm : kPerms) {
            f32 score = 0.0f;
            for (int i = 0; i < 3; ++i) {
                score += std::fabs(align(q, perm[i], i));
            }
            if (score > best) {
                best = score;
                order[0] = perm[0];
                order[1] = perm[1];
                order[2] = perm[2];
            }
        }
    } else {
        // No history: descending eigenvalue is at least deterministic.
        if (eigen.data[order[0]] < eigen.data[order[1]]) std::swap(order[0], order[1]);
        if (eigen.data[order[1]] < eigen.data[order[2]]) std::swap(order[1], order[2]);
        if (eigen.data[order[0]] < eigen.data[order[1]]) std::swap(order[0], order[1]);
    }

    Matrix44f permuted = Matrix44f::identity();
    Vector3f values;
    for (int i = 0; i < 3; ++i) {
        values.data[i] = eigen.data[order[i]];
        for (int j = 0; j < 3; ++j) {
            permuted.data[i][j] = q.data[order[i]][j];
        }
    }
    q = permuted;
    eigen = values;

    if (previous != nullptr) {
        f32 alignment[3];
        for (int i = 0; i < 3; ++i) {
            alignment[i] = align(q, i, i);
            if (alignment[i] < 0.0f) {
                for (int j = 0; j < 3; ++j) {
                    q.data[i][j] = -q.data[i][j];
                }
                alignment[i] = -alignment[i];
            }
        }
        // A permutation or a sign flip can leave a reflection, and `firstOut` has
        // to stay a rotation; the row to give up is the one the previous key
        // constrains least.
        if (Determinant3(q) < 0.0f) {
            int weakest = 0;
            for (int i = 1; i < 3; ++i) {
                if (alignment[i] < alignment[weakest]) {
                    weakest = i;
                }
            }
            for (int j = 0; j < 3; ++j) {
                q.data[weakest][j] = -q.data[weakest][j];
            }
        }
    } else if (Determinant3(q) < 0.0f) {
        for (int j = 0; j < 3; ++j) {
            q.data[2][j] = -q.data[2][j];
        }
    }

    firstOut = q.transpose();
    firstOut.data[3][0] = firstOut.data[3][1] = firstOut.data[3][2] = 0.0f;
    firstOut.data[3][3] = 1.0f;

    const Matrix44f rest = q * rotation;
    secondOut = Matrix44f::identity();
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            secondOut.data[i][j] = eigen.data[i] * rest.data[i][j];
        }
    }
}

/// The TRS a pivot node holds so `T(-p) * S * R * T(p + t)` equals @p wanted
/// with its linear part replaced by @p held.
Transform PivotedTrs(const Matrix44f& wanted, const Matrix44f& held, const Vector3f& pivot) {
    Transform out = FromMatrix(held);
    // `-p*held + p + t` is the composition's translation row, so `t` is the
    // target row plus `p*held - p`. The pivot cancels, which is why any pivot
    // works and the rig is free to keep the bind position in it.
    const Vector3f moved = Apply(held, pivot);
    const Vector3f target = TranslationOf(wanted);
    out.translation = Vector3f{target.x + moved.x - pivot.x, target.y + moved.y - pivot.y,
                               target.z + moved.z - pivot.z};
    return out;
}

/// `T(-p) * S * R * T(p + t)` — what the target rig will compose from what
/// `PivotedTrs` returned, and the residual check's other half.
Matrix44f PivotComposition(const Transform& trs, const Vector3f& pivot) {
    Matrix44f out = ToMatrix(Transform{Vector3f{0, 0, 0}, trs.rotation, trs.scale});
    const Vector3f moved = Apply(out, Vector3f{-pivot.x, -pivot.y, -pivot.z});
    SetTranslation(out, Vector3f{moved.x + pivot.x + trs.translation.x,
                                 moved.y + pivot.y + trs.translation.y,
                                 moved.z + pivot.z + trs.translation.z});
    return out;
}

/// Whether @p node composes onto its parent at all. `ModelSpace` says its local
/// IS its world — `worldBind` stops the walk there and so must a conjugation,
/// which would otherwise cancel a `B` the runtime never applied. WC3 particle
/// emitters are where this shows up: five of 77 corpus `.mdx` files ended 120
/// units out until the flag was honoured.
bool DetachedFromParent(const Node& node) {
    return hasFlag(node.flags, NodeFlags::ModelSpace);
}

bool NearlyIdentity(const Matrix44f& m, f32 tolerance) {
    const Matrix44f id = Matrix44f::identity();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(m.data[r][c] - id.data[r][c]) > tolerance) {
                return false;
            }
        }
    }
    return true;
}

// ============================================================================
// Track sampling
// ============================================================================

/// @p track's value at @p time, as @p count floats. Honours `Step`; `Hermite`
/// and `Bezier` are sampled on the value and lose their tangents, which is what
/// `AnimTrackApproximated` reports.
void SampleTrack(const SubTrack& track, geom::AttrType type, f32 time, f32* out, u32 count) {
    const u32 components = geom::AttrTypeComponents(type);
    const u32 stride = ValuesPerKey(track.interp) * components;
    const std::size_t keys = track.times.size();
    if (keys == 0 || track.values.size() < keys * stride * sizeof(f32)) {
        return;
    }
    const f32* values = reinterpret_cast<const f32*>(track.values.data());
    const u32 wanted = std::min(count, components);

    std::size_t after = 0;
    while (after < keys && track.times[after] <= time) {
        ++after;
    }
    if (after == 0) {
        for (u32 c = 0; c < wanted; ++c) {
            out[c] = values[c];
        }
        return;
    }
    const std::size_t before = after - 1;
    if (after >= keys || track.interp == Interpolation::Step) {
        for (u32 c = 0; c < wanted; ++c) {
            out[c] = values[before * stride + c];
        }
        return;
    }
    const f32 span = track.times[after] - track.times[before];
    const f32 alpha = span > 0.0f ? (time - track.times[before]) / span : 0.0f;
    const f32* a = values + before * stride;
    const f32* b = values + after * stride;
    if (type == geom::AttrType::Quat && wanted == 4) {
        // Shortest arc — what `Slerp` means, and what a componentwise lerp of
        // two keys on opposite hemispheres would get wrong by half a turn.
        const f32 dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
        f32 length = 0.0f;
        for (u32 c = 0; c < 4; ++c) {
            out[c] = a[c] + alpha * (sign * b[c] - a[c]);
            length += out[c] * out[c];
        }
        length = std::sqrt(length);
        if (length > 0.0f) {
            for (u32 c = 0; c < 4; ++c) {
                out[c] /= length;
            }
        }
        return;
    }
    for (u32 c = 0; c < wanted; ++c) {
        out[c] = a[c] + alpha * (b[c] - a[c]);
    }
}

/// The three node channels a retarget rewrites, in the order the code wants.
constexpr Channel kNodeChannels[3] = {Channel::Translation, Channel::Rotation, Channel::Scale};

struct ChannelSlot {
    geom::AttrType type = geom::AttrType::F32x3;
    const SubTrack* track = nullptr;
    const AnimChannel* channel = nullptr;
    Interpolation interp = Interpolation::Linear;
};

/// The channel driving (@p node, @p channel) with `sub == 0`, or null.
const AnimChannel* FindNodeChannel(const AnimChannelTable& table, u32 node, Channel channel) {
    for (const AnimChannel& entry : table.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            return &entry;
        }
    }
    return nullptr;
}

/// The id of that channel, declaring it — or restating its value type — as
/// needed. Invalidates every `AnimChannel*` the caller holds.
u32 EnsureNodeChannel(AnimChannelTable& table, u32 node, Channel channel, geom::AttrType type) {
    for (AnimChannel& entry : table.channels) {
        if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == node &&
            entry.target.channel == channel && entry.target.sub == 0) {
            // Every container's tracks for this channel are rewritten in the
            // same pass, so widening a source that keyed one float (D3's scale)
            // into the three the target holds cannot leave one behind.
            if (entry.valueType != type) {
                entry.valueType = type;
                entry.initValue.clear();
            }
            return entry.id;
        }
    }
    AnimChannel fresh;
    fresh.id = table.nextFreeId();
    fresh.target.kind = TrackTarget::Kind::Node;
    fresh.target.node = node;
    fresh.target.channel = channel;
    fresh.valueType = type;
    return table.add(fresh);
}

void CollectSlots(const AnimChannelTable& table, const SubTrackContainer& container, u32 node,
                  ChannelSlot slots[3]) {
    for (int c = 0; c < 3; ++c) {
        slots[c] = ChannelSlot{};
        const AnimChannel* channel = FindNodeChannel(table, node, kNodeChannels[c]);
        if (channel == nullptr) {
            continue;
        }
        slots[c].type = channel->valueType;
        slots[c].channel = channel;
        slots[c].track = container.find(channel->id);
        if (slots[c].track != nullptr) {
            slots[c].interp = slots[c].track->interp;
        }
    }
}

/// Whether @p slot holds a curve that changes between its keys. A slot with no
/// track is constant, which is step-compatible.
bool StepLike(const ChannelSlot& slot) {
    return slot.track == nullptr || slot.interp == Interpolation::Step;
}

/**
 * @brief The interpolation a rewritten channel takes.
 *
 * A conjugation is a constant matrix on each side, so the rewritten curve steps
 * exactly where every source curve it reads steps -- and emitting `Linear` over
 * a source that stepped turns a held pose into a ramp, which is the whole of the
 * inter-key error the first sweep measured. The output's linear part reads the
 * source's rotation and scale; its translation reads all three, because
 * `B`'s own translation rides the source's linear part.
 */
Interpolation InterpFor(const ChannelSlot slots[3], Channel channel) {
    const bool linearHeld = StepLike(slots[1]) && StepLike(slots[2]);
    const bool held = channel == Channel::Translation
                          ? linearHeld && StepLike(slots[0])
                          : linearHeld;
    if (held) {
        return Interpolation::Step;
    }
    return channel == Channel::Rotation ? Interpolation::Slerp : Interpolation::Linear;
}

/// The union of the three tracks' key times, ascending and deduplicated.
std::vector<f32> KeyTimes(const ChannelSlot slots[3]) {
    std::vector<f32> times;
    for (int c = 0; c < 3; ++c) {
        if (slots[c].track != nullptr) {
            times.insert(times.end(), slots[c].track->times.begin(), slots[c].track->times.end());
        }
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

/// The source node's local transform at @p time under the container's layering
/// rules: the sub-track where there is one, then the channel's declared rest,
/// then the node's own.
Matrix44f SampleLocal(const Transform& rest, f32 time, const ChannelSlot slots[3]) {
    Transform local = rest;
    for (int c = 0; c < 3; ++c) {
        const ChannelSlot& slot = slots[c];
        const u32 components = geom::AttrTypeComponents(slot.type);
        f32 buffer[4] = {0, 0, 0, 1};
        if (slot.track != nullptr) {
            SampleTrack(*slot.track, slot.type, time, buffer, 4);
        } else if (slot.channel != nullptr && slot.channel->hasInitValue()) {
            const f32* init = reinterpret_cast<const f32*>(slot.channel->initValue.data());
            for (u32 i = 0; i < components && i < 4; ++i) {
                buffer[i] = init[i];
            }
        } else {
            continue;
        }
        switch (kNodeChannels[c]) {
        case Channel::Translation:
            local.translation = Vector3f{buffer[0], buffer[1], buffer[2]};
            break;
        case Channel::Rotation:
            local.rotation = Quaternion{buffer[0], buffer[1], buffer[2], buffer[3]};
            break;
        case Channel::Scale:
            // A source that keys one float scales uniformly — D3 is the case.
            local.scale = components == 1 ? Vector3f{buffer[0], buffer[0], buffer[0]}
                                          : Vector3f{buffer[0], buffer[1], buffer[2]};
            break;
        default:
            break;
        }
    }
    return ToMatrix(local);
}

void PushVec3(std::vector<f32>& into, const Vector3f& v) {
    into.push_back(v.x);
    into.push_back(v.y);
    into.push_back(v.z);
}

void PushQuat(std::vector<f32>& into, const Quaternion& q) {
    into.push_back(q.x);
    into.push_back(q.y);
    into.push_back(q.z);
    into.push_back(q.w);
}

SubTrack MakeTrack(u32 channel, Interpolation interp, const std::vector<f32>& times,
                   const std::vector<f32>& values) {
    SubTrack track;
    track.channel = channel;
    track.interp = interp;
    track.times = times;
    track.values.resize(values.size() * sizeof(f32));
    if (!values.empty()) {
        std::memcpy(track.values.data(), values.data(), values.size() * sizeof(f32));
    }
    return track;
}

/// Everything the two passes need about one source node, computed once.
struct NodePlan {
    Matrix44f conjugate = Matrix44f::identity();        ///< `B`
    Matrix44f inverseConjugate = Matrix44f::identity(); ///< `inverse(B)`
    Vector3f pivot{0, 0, 0};                            ///< The bind position.
    /**
     * @brief The pivot the composition actually uses — @ref pivot, or zero on a
     *        split node.
     *
     * The pivot is free, and on a split node zero is the only good choice: with
     * any other, the child's translation track has to hold `p*U - p` for the
     * composed translation to come out where the helper needs it, and that is a
     * ROTATED POINT sampled linearly against a rotation that is slerped. The two
     * agree at every key and diverge by `|p|` times the slerp-lerp gap between
     * them, which measured a whole unit on a bone two units off the origin.
     * Zero makes the child's translation the constant zero and leaves the helper
     * carrying the target translation unmodified.
     */
    Vector3f composePivot{0, 0, 0};
    Matrix44f rest = Matrix44f::identity();             ///< The conjugated rest node transform.
    Transform sourceLocal;                              ///< The source rest, kept because
                                                        ///< `local` is overwritten with the
                                                        ///< pivot chain before pass two.
    u32 parent = kInvalidNode;                          ///< The SOURCE parent.
    bool restIsIdentity = true;
    bool sheared = false;
    bool needsTracks = false;
    bool detached = false; ///< `ModelSpace`: the parent contributes nothing.

    /// `inverse(B(parent))`, or the identity where there is no parent to cancel.
    Matrix44f parentConjugateInverse(const std::vector<NodePlan>& plan) const {
        if (detached || parent == kInvalidNode || parent >= plan.size()) {
            return Matrix44f::identity();
        }
        return plan[parent].inverseConjugate;
    }
};

/// Restates one model as a pivot rig. Everything below the `want` switch, kept
/// out of `RetargetSkeleton` because it is the whole of the hard direction.
void ToPivotRelative(Model& model, std::vector<Clip*>& clips, const ElementRef& where,
                     const SkeletonRetargetOptions& options, SkeletonRetargetResult& result) {
    const u32 sourceCount = model.nodes.size();

    // --- the conjugation, per source node ------------------------------------
    std::vector<NodePlan> plan(sourceCount);
    for (u32 n = 0; n < sourceCount; ++n) {
        const Node& node = model.nodes.nodes[n];
        plan[n].parent = node.parent;
        plan[n].sourceLocal = node.local;
        if (node.kind == NodeKind::Bone) {
            plan[n].conjugate = model.nodes.inverseBindMatrix(n);
            plan[n].inverseConjugate = Matrix44f::inverse(plan[n].conjugate);
            plan[n].pivot = TranslationOf(plan[n].inverseConjugate);
        } else {
            // A non-bone has no bind to preserve, only its animated frame; `B = I`
            // keeps it, and the conjugation on the parent's side is what stops
            // that frame moving when the bones' do not.
            plan[n].pivot = model.nodes.worldBind(n).translation;
        }
    }
    for (u32 n = 0; n < sourceCount; ++n) {
        plan[n].detached = DetachedFromParent(model.nodes.nodes[n]);
        const Matrix44f parentInverse = plan[n].parentConjugateInverse(plan);
        plan[n].rest =
            plan[n].conjugate * ToMatrix(model.nodes.nodes[n].local) * parentInverse;
        plan[n].restIsIdentity = NearlyIdentity(plan[n].rest, 1e-5f);
        plan[n].sheared = ShearOf(plan[n].rest) > options.shearTolerance;
        plan[n].needsTracks = !plan[n].restIsIdentity;
        plan[n].composePivot = plan[n].pivot;
    }

    // --- pass one: which nodes shear, over every key the source holds ---------
    u32 smoothTracks = 0;
    for (const Clip* clip : clips) {
        for (const SubTrackContainer& container : clip->containers) {
            for (u32 n = 0; n < sourceCount; ++n) {
                ChannelSlot slots[3];
                CollectSlots(model.animChannels, container, n, slots);
                const std::vector<f32> times = KeyTimes(slots);
                if (times.empty()) {
                    continue;
                }
                plan[n].needsTracks = true;
                for (int c = 0; c < 3; ++c) {
                    if (slots[c].track != nullptr &&
                        (slots[c].interp == Interpolation::Hermite ||
                         slots[c].interp == Interpolation::Bezier)) {
                        ++smoothTracks;
                    }
                }
                if (plan[n].sheared) {
                    continue;
                }
                const Matrix44f parentInverse = plan[n].parentConjugateInverse(plan);
                const Transform& rest = plan[n].sourceLocal;
                for (const f32 time : times) {
                    const Matrix44f target =
                        plan[n].conjugate * SampleLocal(rest, time, slots) * parentInverse;
                    if (ShearOf(target) > options.shearTolerance) {
                        plan[n].sheared = true;
                        break;
                    }
                }
            }
        }
    }

    // Which nodes split is settled, so the pivots they compose about are too.
    const bool split = options.splitShearedNodes;
    if (split) {
        for (u32 n = 0; n < sourceCount; ++n) {
            if (plan[n].sheared) {
                plan[n].composePivot = Vector3f{0, 0, 0};
            }
        }
    }

    // --- the tree ------------------------------------------------------------
    //
    // Rebuilt in one pass rather than inserted into, so a helper lands
    // immediately before the node it stretches and "parents precede children"
    // survives without a sort. The new index of a source node is its own plus
    // the helpers before it, which needs no parent information — so the map is
    // built first and the parents resolved after, because `.mdx` does not
    // guarantee parents precede children and reading a not-yet-assigned `remap`
    // entry would orphan the node.
    std::vector<u32> remap(sourceCount, kInvalidNode);
    std::vector<u32> helperOf(sourceCount, kInvalidNode);
    u32 next = 0;
    for (u32 n = 0; n < sourceCount; ++n) {
        if (plan[n].sheared) {
            ++result.shearedNodes;
            if (split) {
                helperOf[n] = next++;
                ++result.nodesInserted;
            }
        }
        remap[n] = next++;
    }

    std::vector<Node> rebuilt;
    rebuilt.reserve(next);
    for (u32 n = 0; n < sourceCount; ++n) {
        const u32 parent = plan[n].parent != kInvalidNode && plan[n].parent < sourceCount
                               ? remap[plan[n].parent]
                               : kInvalidNode;
        if (helperOf[n] != kInvalidNode) {
            Node helper;
            helper.name = model.nodes.nodes[n].name + "_stretch";
            helper.kind = NodeKind::Helper;
            helper.resetPayloadForKind();
            helper.parent = parent;
            rebuilt.push_back(std::move(helper));
        }
        Node moved = model.nodes.nodes[n];
        moved.parent = helperOf[n] != kInvalidNode ? helperOf[n] : parent;
        moved.pivot = plan[n].composePivot;
        // `local` keeps the bind position whatever the composition pivot is, so
        // `worldBind` still answers where the bone is even on a split node whose
        // pivot is zero. Only a round trip back out through `PIVT` loses that,
        // and only for those nodes.
        const Vector3f parentPivot = plan[n].parent != kInvalidNode &&
                                             plan[n].parent < sourceCount
                                         ? plan[plan[n].parent].pivot
                                         : Vector3f{0, 0, 0};
        moved.local = Transform::identity();
        moved.local.translation = Vector3f{plan[n].pivot.x - parentPivot.x,
                                           plan[n].pivot.y - parentPivot.y,
                                           plan[n].pivot.z - parentPivot.z};
        moved.poses.clear();
        moved.poseMatrices.clear();
        rebuilt.push_back(std::move(moved));
    }
    const bool grew = rebuilt.size() != sourceCount;
    model.nodes.nodes = std::move(rebuilt);
    model.nodes.invalidateHierarchy();

    // A helper composes `T(-p) * S * R * T(p + t)` like any other node, and the
    // split wants it to contribute the stretch and NOTHING else -- so its pivot
    // is zero, which makes that composition exactly `S * R`. Its `local` is the
    // identity for the reason the pivot chain needs it to be: the child's own
    // `local` already spans from the real parent's pivot to its own.
    for (u32 n = 0; n < sourceCount; ++n) {
        if (helperOf[n] == kInvalidNode) {
            continue;
        }
        Node& helper = model.nodes.nodes[helperOf[n]];
        helper.pivot = Vector3f{0, 0, 0};
        helper.local = Transform::identity();
    }

    // The §10.6 referencer table — the same four rows `CompactNodes` walks.
    if (grew) {
        for (Mesh& mesh : model.meshes) {
            for (geom::Influence& influence : mesh.skin.influences) {
                if (influence.bone < remap.size()) {
                    influence.bone = remap[influence.bone];
                }
            }
            for (MeshSection& section : mesh.sections) {
                if (section.rigidNode.has_value() && *section.rigidNode < remap.size()) {
                    section.rigidNode = remap[*section.rigidNode];
                }
            }
        }
        for (AnimChannel& channel : model.animChannels.channels) {
            if (channel.target.kind == TrackTarget::Kind::Node &&
                channel.target.node < remap.size()) {
                channel.target.node = remap[channel.target.node];
            }
        }
        for (Clip* clip : clips) {
            for (ClipEvent& event : clip->events) {
                if (event.node < remap.size()) {
                    event.node = remap[event.node];
                }
            }
        }
    }

    // --- pass two: rewrite the tracks -----------------------------------------
    //
    // The channel table is read through a snapshot: declaring the target's
    // channels changes value types -- D3 keys ONE scale float where a pivot rig
    // wants three -- and would otherwise make the sampler read the source's keys
    // with the target's stride.
    const AnimChannelTable sourceChannels = model.animChannels;
    struct NodeChannels {
        u32 translation = kInvalidIndex;
        u32 rotation = kInvalidIndex;
        u32 scale = kInvalidIndex;
        u32 helperTranslation = kInvalidIndex;
        u32 helperRotation = kInvalidIndex;
        u32 helperScale = kInvalidIndex;
    };
    std::vector<NodeChannels> declared(sourceCount);
    for (u32 n = 0; n < sourceCount; ++n) {
        if (!plan[n].needsTracks) {
            continue;
        }
        const u32 node = remap[n];
        declared[n].translation = EnsureNodeChannel(model.animChannels, node, Channel::Translation,
                                                    geom::AttrType::F32x3);
        declared[n].rotation =
            EnsureNodeChannel(model.animChannels, node, Channel::Rotation, geom::AttrType::Quat);
        declared[n].scale =
            EnsureNodeChannel(model.animChannels, node, Channel::Scale, geom::AttrType::F32x3);
        if (helperOf[n] != kInvalidNode) {
            declared[n].helperTranslation = EnsureNodeChannel(
                model.animChannels, helperOf[n], Channel::Translation, geom::AttrType::F32x3);
            declared[n].helperRotation = EnsureNodeChannel(model.animChannels, helperOf[n],
                                                           Channel::Rotation, geom::AttrType::Quat);
            declared[n].helperScale = EnsureNodeChannel(model.animChannels, helperOf[n],
                                                        Channel::Scale, geom::AttrType::F32x3);
        }
    }

    for (Clip* clip : clips) {
        for (SubTrackContainer& container : clip->containers) {
            // Read from a copy: the channel table grows underneath as helper
            // channels are declared, and the container is rebuilt from scratch.
            const SubTrackContainer source = container;
            std::vector<SubTrack> written;
            for (const SubTrack& track : source.subTracks) {
                const AnimChannel* channel = sourceChannels.find(track.channel);
                const bool nodeTrs =
                    channel != nullptr && channel->target.kind == TrackTarget::Kind::Node &&
                    channel->target.sub == 0 &&
                    (channel->target.channel == Channel::Translation ||
                     channel->target.channel == Channel::Rotation ||
                     channel->target.channel == Channel::Scale);
                if (!nodeTrs) {
                    written.push_back(track);
                }
            }

            for (u32 n = 0; n < sourceCount; ++n) {
                if (!plan[n].needsTracks) {
                    continue;
                }
                ChannelSlot slots[3];
                // The snapshot was taken AFTER the referencer remap, so its
                // channels name the new index even though their keys are still
                // the source's.
                CollectSlots(sourceChannels, source, remap[n], slots);
                std::vector<f32> times = KeyTimes(slots);
                const bool keyed = !times.empty();
                if (keyed && options.refineKeys != 0 && plan[n].sheared && split) {
                    // A split node's two factors interpolate independently, and
                    // the product of two interpolants is not the interpolant of
                    // the products -- the one error the rewrite cannot remove at
                    // a key because it lives BETWEEN keys. Subdividing shrinks it
                    // where it is worst and nowhere else.
                    std::vector<f32> dense;
                    dense.reserve(times.size() * (options.refineKeys + 1));
                    for (std::size_t i = 0; i + 1 < times.size(); ++i) {
                        dense.push_back(times[i]);
                        const f32 span = times[i + 1] - times[i];
                        for (u32 k = 1; k <= options.refineKeys; ++k) {
                            dense.push_back(times[i] +
                                            span * f32(k) / f32(options.refineKeys + 1));
                        }
                    }
                    dense.push_back(times.back());
                    times = std::move(dense);
                }
                if (!keyed) {
                    // A pivot rig rests at the identity and has no other way to
                    // say otherwise, so a rest the conjugation moved off it has
                    // to become a key.
                    if (plan[n].restIsIdentity) {
                        continue;
                    }
                    times.push_back(0.0f);
                }

                const Matrix44f parentInverse = plan[n].parentConjugateInverse(plan);
                // `local` was overwritten with the pivot chain above the tree
                // rebuild, so the source rest comes from the plan.
                const Transform& sourceRest = plan[n].sourceLocal;

                std::vector<f32> translation, rotation, scale;
                std::vector<f32> helperTranslation, helperRotation, helperScale;
                // The previous key, so this one can be aligned to it.
                Matrix44f previousFirst = Matrix44f::identity();
                bool havePrevious = false;
                translation.reserve(times.size() * 3);
                rotation.reserve(times.size() * 4);
                scale.reserve(times.size() * 3);

                for (const f32 time : times) {
                    const Matrix44f target =
                        plan[n].conjugate * SampleLocal(sourceRest, time, slots) * parentInverse;

                    Matrix44f rebuiltNode;
                    if (plan[n].sheared && split) {
                        Matrix44f first, second;
                        SplitIntoTwo(target, havePrevious ? &previousFirst : nullptr, first,
                                     second);
                        previousFirst = first;
                        havePrevious = true;
                        // The translation rides the HELPER, not the child. Both
                        // placements reproduce `target` at a key, but the child's
                        // would have to hold `c * inverse(second)` -- a division
                        // by the stretch, which on a hit-test bone is 0.03 -- and
                        // the next key multiplies it back by a slightly different
                        // stretch. That amplified a per-key rounding into 8 units
                        // between keys. With the translation above the stretch,
                        // the helper interpolates `c` exactly as an unsplit node
                        // would.
                        const Transform stretch = FromMatrix(second);
                        PushVec3(helperTranslation, TranslationOf(target));
                        PushQuat(helperRotation, stretch.rotation);
                        PushVec3(helperScale, stretch.scale);

                        // The child's composed translation is therefore zero.
                        const Transform trs = PivotedTrs(Matrix44f::identity(), Linear(first),
                                                         plan[n].composePivot);
                        PushVec3(translation, trs.translation);
                        PushQuat(rotation, trs.rotation);
                        PushVec3(scale, trs.scale);

                        Matrix44f helperMatrix = ToMatrix(
                            Transform{Vector3f{0, 0, 0}, stretch.rotation, stretch.scale});
                        SetTranslation(helperMatrix, TranslationOf(target));
                        rebuiltNode = PivotComposition(trs, plan[n].composePivot) * helperMatrix;
                    } else {
                        // Unsplit: the linear part as it stands, or the nearest
                        // scale-rotation when the caller declined the split.
                        const Matrix44f held = plan[n].sheared ? ProjectToScaleRotation(target)
                                                               : Linear(target);
                        const Transform trs = PivotedTrs(target, held, plan[n].composePivot);
                        PushVec3(translation, trs.translation);
                        PushQuat(rotation, trs.rotation);
                        PushVec3(scale, trs.scale);
                        rebuiltNode = PivotComposition(trs, plan[n].composePivot);
                    }

                    const Vector3f probe{plan[n].pivot.x + 1.0f, plan[n].pivot.y, plan[n].pivot.z};
                    result.worstResidual = std::fmax(
                        result.worstResidual,
                        Distance(Apply(target, plan[n].pivot), Apply(rebuiltNode, plan[n].pivot)));
                    result.worstResidual =
                        std::fmax(result.worstResidual,
                                  Distance(Apply(target, probe), Apply(rebuiltNode, probe)));
                }

                written.push_back(MakeTrack(declared[n].translation,
                                            InterpFor(slots, Channel::Translation), times,
                                            translation));
                written.push_back(MakeTrack(declared[n].rotation,
                                            InterpFor(slots, Channel::Rotation), times, rotation));
                written.push_back(MakeTrack(declared[n].scale, InterpFor(slots, Channel::Scale),
                                            times, scale));
                if (keyed) {
                    ++result.nodesRewritten;
                } else {
                    ++result.restKeysAdded;
                }

                if (!helperRotation.empty()) {
                    written.push_back(MakeTrack(declared[n].helperTranslation,
                                                InterpFor(slots, Channel::Translation), times,
                                                helperTranslation));
                    written.push_back(MakeTrack(declared[n].helperRotation,
                                                InterpFor(slots, Channel::Rotation), times,
                                                helperRotation));
                    written.push_back(MakeTrack(declared[n].helperScale,
                                                InterpFor(slots, Channel::Scale), times,
                                                helperScale));
                }
            }

            container.subTracks = std::move(written);
        }
    }

    // A channel's `initValue` is what an opaque container contributes where it
    // holds no sub-track, and under an explicit bind that is the bone's ABSOLUTE
    // local. A pivot rig's un-keyed channel contributes the identity instead, so
    // leaving the value would re-apply the source's whole rest chain on top of a
    // pivot composition -- which is what made an unkeyed root scale twice.
    for (AnimChannel& channel : model.animChannels.channels) {
        if (channel.target.kind == TrackTarget::Kind::Node &&
            (channel.target.channel == Channel::Translation ||
             channel.target.channel == Channel::Rotation ||
             channel.target.channel == Channel::Scale)) {
            channel.initValue.clear();
        }
    }

    model.nodes.poseSchema.assign(1, PoseSchema{});
    model.nodes.authoritativePose = 0;
    model.nodes.rig = RigConvention::PivotRelative;
    for (Node& node : model.nodes.nodes) {
        // The value the two pivot importers write, so a document that came
        // through here and one imported natively compare the same way.
        node.poses.assign(1, node.local);
        node.poseMatrices.clear();
    }

    if (result.shearedNodes != 0) {
        if (split) {
            result.diagnostics.warn(DiagCode::BoneShearSplit,
                                    number(result.shearedNodes) +
                                        " nodes carried shear a pivot node cannot hold; " +
                                        number(result.nodesInserted) +
                                        " helper parents hold the stretch",
                                    where);
        } else {
            result.diagnostics.warn(DiagCode::BoneShearProjected,
                                    number(result.shearedNodes) +
                                        " nodes had shear projected away; the pose is approximate",
                                    where);
        }
    }
    u32 suppressed = 0;
    for (u32 n = 0; n < sourceCount; ++n) {
        const NodeFlags flags = model.nodes.nodes[remap[n]].flags;
        if (hasFlag(flags, NodeFlags::DontInheritTranslation) ||
            hasFlag(flags, NodeFlags::DontInheritRotation) ||
            hasFlag(flags, NodeFlags::DontInheritScale)) {
            ++suppressed;
        }
    }
    if (suppressed != 0) {
        result.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                number(suppressed) +
                                    " nodes suppress an inherited component; the rewrite composes "
                                    "the chain unconditionally",
                                where);
    }
    if (smoothTracks != 0) {
        result.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                number(smoothTracks) +
                                    " tracks were Hermite or Bezier and were resampled linearly",
                                where);
    }
}

/// Restates one model as an explicit-bind rig. Exact, and needs no extra nodes:
/// `B = T(rest position)` leaves the linear part alone, so no shear can appear
/// and the whole operation is a constant added to every translation key.
void ToExplicitBind(Model& model, std::vector<Clip*>& clips, const ElementRef& where,
                    SkeletonRetargetResult& result) {
    const u32 count = model.nodes.size();

    // `pivot`, not `worldBind` and not the `local` chain: the pivot IS the point
    // the composition turns about, so it is the only value `B = T(position)` can
    // take and have the two cancel. It is also absolute model space by
    // definition, which sidesteps `local` entirely — and `local` is not to be
    // trusted here, because `.mdx` does not guarantee parents precede children
    // (`SEAltarOfStars` parents node 5 to node 35) and the importer that
    // subtracts the parent pivot reads a zero for one it has not reached.
    std::vector<Vector3f> position(count);
    u32 suppressed = 0;
    for (u32 n = 0; n < count; ++n) {
        position[n] = model.nodes.nodes[n].pivot;
        // `ModelSpace` is handled below; the three partial suppressions are not,
        // because they modify the parent frame per component and no single
        // constant `B` cancels that.
        const NodeFlags flags = model.nodes.nodes[n].flags;
        if (hasFlag(flags, NodeFlags::DontInheritTranslation) ||
            hasFlag(flags, NodeFlags::DontInheritRotation) ||
            hasFlag(flags, NodeFlags::DontInheritScale)) {
            ++suppressed;
        }
    }

    for (u32 n = 0; n < count; ++n) {
        Node& node = model.nodes.nodes[n];
        const Vector3f parent =
            !DetachedFromParent(node) && node.parent != kInvalidNode && node.parent < count
                ? position[node.parent]
                : Vector3f{0, 0, 0};
        const Vector3f delta{position[n].x - parent.x, position[n].y - parent.y,
                             position[n].z - parent.z};

        AnimChannel* channel = nullptr;
        for (AnimChannel& entry : model.animChannels.channels) {
            if (entry.target.kind == TrackTarget::Kind::Node && entry.target.node == n &&
                entry.target.channel == Channel::Translation && entry.target.sub == 0) {
                channel = &entry;
                break;
            }
        }
        if (channel != nullptr && channel->valueType == geom::AttrType::F32x3) {
            const u32 id = channel->id;
            // The rest an un-keyed container contributes moves with the keys.
            if (channel->hasInitValue()) {
                f32* init = reinterpret_cast<f32*>(channel->initValue.data());
                init[0] += delta.x;
                init[1] += delta.y;
                init[2] += delta.z;
            }
            for (Clip* clip : clips) {
                for (SubTrackContainer& container : clip->containers) {
                    for (SubTrack& track : container.subTracks) {
                        if (track.channel != id) {
                            continue;
                        }
                        f32* values = reinterpret_cast<f32*>(track.values.data());
                        const std::size_t floats = track.values.size() / sizeof(f32);
                        // The VALUE of each key only. A `Hermite` key is
                        // `{value, inTan, outTan}` in the same block, and a
                        // tangent is a derivative — shifting it bends the curve
                        // instead of moving it, which is what put a WC3 bone 444
                        // units out.
                        const std::size_t stride = ValuesPerKey(track.interp) * 3;
                        for (std::size_t v = 0; v + 2 < floats; v += stride) {
                            values[v + 0] += delta.x;
                            values[v + 1] += delta.y;
                            values[v + 2] += delta.z;
                        }
                        ++result.keysOffset;
                    }
                }
            }
        }

        // The rest is the pivot chain, which is what `local` already held; a
        // rotation or scale left there was never part of the pivot composition
        // and does not survive into a convention that reads it.
        node.local = Transform::identity();
        node.local.translation = delta;
        node.pivot = Vector3f{0, 0, 0};
    }

    PoseSchema schema;
    schema.name = "bind";
    schema.space = PoseSpace::Model;
    schema.inverse = true;
    schema.storage = PoseStorage::Matrix;
    model.nodes.poseSchema.assign(1, schema);
    model.nodes.authoritativePose = 0;
    model.nodes.rig = RigConvention::ExplicitBind;
    // Every node, not only the bones. A kind that carries no pose falls back to
    // `poseOf`, which derives one from `worldBind` — and `worldBind` honours the
    // inherit flags while the conjugation does not, so an attachment under a
    // bone that suppresses inherited translation would be handed a bind matrix
    // the rest of this operation never used. Storing the value settles it: an
    // `.m2` event under such a bone was landing 1.4 units off.
    for (u32 n = 0; n < count; ++n) {
        Node& node = model.nodes.nodes[n];
        Matrix44f bind = Matrix44f::identity();
        SetTranslation(bind, Vector3f{-position[n].x, -position[n].y, -position[n].z});
        node.poseMatrices.assign(1, bind);
        node.poses.assign(1, FromMatrix(bind));
    }

    if (suppressed != 0) {
        result.diagnostics.warn(DiagCode::AnimTrackApproximated,
                                number(suppressed) +
                                    " nodes suppress an inherited component; the rewrite composes "
                                    "the chain unconditionally",
                                where);
    }
    result.diagnostics.info(DiagCode::RigConventionChanged,
                            std::string("model '") + model.name + "': " + number(count) +
                                " nodes restated as explicit_bind",
                            where);
}

} // namespace

// ============================================================================

SkeletonRetargetResult RetargetSkeleton(Document& document, ProfileId to,
                                        const SkeletonRetargetOptions& options) {
    SkeletonRetargetResult result;
    if (static_cast<u32>(to) >= static_cast<u32>(ProfileId::Count)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried, "unknown target profile");
        return result;
    }
    const RigConvention want = Profile(to).rig;
    result.ok = true;

    for (std::size_t m = 0; m < document.models.size(); ++m) {
        Model& model = document.models[m];
        const ElementRef where(ElementKind::Document, static_cast<u32>(m));
        if (model.nodes.empty()) {
            continue;
        }
        if (model.nodes.rig == want) {
            result.diagnostics.info(DiagCode::RigConventionChanged,
                                    std::string("model '") + model.name + "' is already " +
                                        ToString(want),
                                    where);
            continue;
        }

        // The clips driving THIS model; one for another names another model's
        // channel ids. An unset `model` is only this one's when there is no
        // other it could mean.
        std::vector<Clip*> clips;
        for (Clip& clip : document.clips) {
            if (clip.model == static_cast<u32>(m) ||
                (clip.model == kInvalidIndex && document.models.size() == 1)) {
                clips.push_back(&clip);
            }
        }

        if (want == RigConvention::ExplicitBind) {
            ToExplicitBind(model, clips, where, result);
        } else {
            ToPivotRelative(model, clips, where, options, result);
            result.diagnostics.info(DiagCode::RigConventionChanged,
                                    std::string("model '") + model.name + "': " +
                                        number(model.nodes.size()) +
                                        " nodes restated as pivot_relative",
                                    where);
        }
    }

    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
