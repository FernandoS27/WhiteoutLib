// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/anim/curve_keys.h>

#include <whiteout/models/wem/anim/curves.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace whiteout {
namespace models {
namespace wem {

namespace {

bool Smooth(Interpolation interp) {
    return interp == Interpolation::Hermite || interp == Interpolation::Bezier;
}

/// An integer is held from key to key, whatever it says (CURVE §6.1, §8): no
/// span of it moves, and it has no tangents.
template <class T>
constexpr bool kHeld = std::is_same_v<T, u32>;

// ---- Component-wise arithmetic on any of the four value types ----------------

template <class T>
constexpr std::size_t kComponents = sizeof(T) / sizeof(f32);

template <class T, class Fn>
T Combine(const T& a, const T& b, Fn fn) {
    f32 x[kComponents<T>], y[kComponents<T>], out[kComponents<T>];
    std::memcpy(x, &a, sizeof(T));
    std::memcpy(y, &b, sizeof(T));
    for (std::size_t i = 0; i < kComponents<T>; ++i) {
        out[i] = fn(x[i], y[i]);
    }
    T result{};
    std::memcpy(&result, out, sizeof(T));
    return result;
}

template <class T>
T Scaled(const T& a, f32 k) {
    return Combine(a, a, [k](f32 x, f32) { return x * k; });
}

template <class T>
T Lerped(const T& a, const T& b, f32 u) {
    return Combine(a, b, [u](f32 x, f32 y) { return x + (y - x) * u; });
}

// ---- The renderer's curves, per value type ------------------------------------

f32 Linear(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
Vector3f Linear(const Vector3f& a, const Vector3f& b, f32 t) {
    return Vector3f::lerp(a, b, t);
}
Vector4f Linear(const Vector4f& a, const Vector4f& b, f32 t) {
    return Vector4f::lerp(a, b, t);
}
Quaternion Linear(const Quaternion& a, const Quaternion& b, f32 t) {
    return Wc3Slerp(a, b, t);
}

template <class T>
T Curve(Interpolation interp, const T& a, const T& outA, const T& inB, const T& b, f32 t) {
    f32 pa[kComponents<T>], po[kComponents<T>], pi[kComponents<T>], pb[kComponents<T>], out[kComponents<T>];
    std::memcpy(pa, &a, sizeof(T));
    std::memcpy(po, &outA, sizeof(T));
    std::memcpy(pi, &inB, sizeof(T));
    std::memcpy(pb, &b, sizeof(T));
    for (std::size_t i = 0; i < kComponents<T>; ++i) {
        out[i] = interp == Interpolation::Hermite ? HermiteInterp(pa[i], po[i], pi[i], pb[i], t)
                                                  : BezierInterp(pa[i], po[i], pi[i], pb[i], t);
    }
    T result{};
    std::memcpy(&result, out, sizeof(T));
    return result;
}
template <>
Quaternion Curve(Interpolation, const Quaternion& a, const Quaternion& outA, const Quaternion& inB,
                 const Quaternion& b, f32 t) {
    return Wc3Squad(a, outA, inB, b, t);
}

/// A Hermite span's derivative with respect to its own parameter, which MDX
/// runs from 0 to 1 over the span whatever its length.
template <class T>
T HermiteSlope(const CurveKey<T>& a, const CurveKey<T>& b, f32 t) {
    const f32 t2 = t * t;
    const f32 da = 6.0f * t2 - 6.0f * t;
    const f32 db = -6.0f * t2 + 6.0f * t;
    const f32 dm0 = 3.0f * t2 - 4.0f * t + 1.0f;
    const f32 dm1 = 3.0f * t2 - 2.0f * t;
    f32 pa[kComponents<T>], pb[kComponents<T>], m0[kComponents<T>], m1[kComponents<T>], out[kComponents<T>];
    std::memcpy(pa, &a.value, sizeof(T));
    std::memcpy(pb, &b.value, sizeof(T));
    std::memcpy(m0, &a.out, sizeof(T));
    std::memcpy(m1, &b.in, sizeof(T));
    for (std::size_t i = 0; i < kComponents<T>; ++i) {
        out[i] = da * pa[i] + db * pb[i] + dm0 * m0[i] + dm1 * m1[i];
    }
    T result{};
    std::memcpy(&result, out, sizeof(T));
    return result;
}

Quaternion Aligned(const Quaternion& to, const Quaternion& q) {
    return to.dot(q) < 0.0f ? Quaternion{-q.x, -q.y, -q.z, -q.w} : q;
}

/// `wa · a + wb · b` on the vector parts of two logs, which is what a tangent
/// in a rotation's log space is.
Quaternion LogBlend(const Quaternion& a, f32 wa, const Quaternion& b, f32 wb) {
    return Quaternion{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, 0.0f};
}

/// The four Kochanek–Bartels weights for one key (§3.3): the previous and next
/// differences' shares of its incoming and outgoing tangents.
struct TcbWeights {
    f32 inPrev, inNext, outPrev, outNext;
};
TcbWeights WeightsOf(const Vector3f& tcb) {
    const f32 t = 1.0f - tcb.x;
    const f32 c = tcb.y;
    const f32 b = tcb.z;
    return {t * (1.0f - c) * (1.0f + b) * 0.5f, t * (1.0f + c) * (1.0f - b) * 0.5f,
            t * (1.0f + c) * (1.0f + b) * 0.5f, t * (1.0f - c) * (1.0f - b) * 0.5f};
}

} // namespace

template <class T>
std::vector<CurveKey<T>> DecodeKeys(const SubTrack& track) {
    const u32 perKey = ValuesPerKey(track.interp);
    std::vector<CurveKey<T>> keys;
    if (track.values.size() != track.times.size() * perKey * sizeof(T)) {
        return keys;
    }
    keys.resize(track.times.size());
    for (std::size_t k = 0; k < keys.size(); ++k) {
        const u8* at = track.values.data() + k * perKey * sizeof(T);
        keys[k].time = track.times[k];
        std::memcpy(&keys[k].value, at, sizeof(T));
        if (perKey == 3) {
            std::memcpy(&keys[k].in, at + sizeof(T), sizeof(T));
            std::memcpy(&keys[k].out, at + 2 * sizeof(T), sizeof(T));
        } else {
            keys[k].in = keys[k].out = keys[k].value;
        }
        if (track.tcb.size() == 3 * keys.size()) {
            keys[k].tcb = Vector3f{track.tcb[3 * k], track.tcb[3 * k + 1], track.tcb[3 * k + 2]};
        }
    }
    return keys;
}

template <class T>
void EncodeKeys(SubTrack& track, const std::vector<CurveKey<T>>& keys) {
    // A TCB sub-track's tangents are the parameters', whatever the caller wrote
    // into them.
    const bool tcb = !track.tcb.empty() && track.interp == Interpolation::Hermite;
    std::vector<CurveKey<T>> derived;
    if (tcb) {
        derived = keys;
        TcbTangents(derived);
    }
    const std::vector<CurveKey<T>>& source = tcb ? derived : keys;
    const u32 perKey = ValuesPerKey(track.interp);
    track.times.resize(source.size());
    track.values.assign(source.size() * perKey * sizeof(T), 0);
    for (std::size_t k = 0; k < source.size(); ++k) {
        u8* at = track.values.data() + k * perKey * sizeof(T);
        track.times[k] = source[k].time;
        std::memcpy(at, &source[k].value, sizeof(T));
        if (perKey == 3) {
            std::memcpy(at + sizeof(T), &source[k].in, sizeof(T));
            std::memcpy(at + 2 * sizeof(T), &source[k].out, sizeof(T));
        }
    }
    if (tcb) {
        track.tcb.resize(3 * source.size());
        for (std::size_t k = 0; k < source.size(); ++k) {
            track.tcb[3 * k] = source[k].tcb.x;
            track.tcb[3 * k + 1] = source[k].tcb.y;
            track.tcb[3 * k + 2] = source[k].tcb.z;
        }
    }
}

template <class T>
T SpanValue(Interpolation interp, const CurveKey<T>& a, const CurveKey<T>& b, f32 u) {
    if constexpr (kHeld<T>) {
        return a.value;
    } else if (Smooth(interp)) {
        return Curve(interp, a.value, a.out, b.in, b.value, u);
    }
    if (interp == Interpolation::Step) {
        return a.value;
    }
    return Linear(a.value, b.value, u);
}

template <class T>
CurveKey<T> SplitSpan(Interpolation interp, CurveKey<T>& a, CurveKey<T>& b, f32 u) {
    CurveKey<T> key;
    key.value = SpanValue(interp, a, b, u);
    key.in = key.out = key.value;
    if constexpr (kHeld<T>) {
        return key;
    } else {
        if (!Smooth(interp) || std::is_same_v<T, Quaternion>) {
            return key;
        }
        if (interp == Interpolation::Hermite) {
            // The same curve over each half, re-parameterised to run 0..1 over
            // it: each derivative scales by that half's share of the span.
            const T slope = HermiteSlope(a, b, u);
            key.in = Scaled(slope, u);
            key.out = Scaled(slope, 1.0f - u);
            a.out = Scaled(a.out, u);
            b.in = Scaled(b.in, 1.0f - u);
            return key;
        }
        // Bezier: de Casteljau. The handles are positions, so the halves'
        // control points are the construction's own.
        const T q0 = Lerped(a.value, a.out, u);
        const T q1 = Lerped(a.out, b.in, u);
        const T q2 = Lerped(b.in, b.value, u);
        const T r0 = Lerped(q0, q1, u);
        const T r1 = Lerped(q1, q2, u);
        key.value = Lerped(r0, r1, u);
        key.in = r0;
        key.out = r1;
        a.out = q0;
        b.in = q2;
        return key;
    }
}

template <class T>
std::pair<CurveKey<T>, CurveKey<T>> SubSpan(Interpolation interp, const CurveKey<T>& a, const CurveKey<T>& b,
                                            f32 f0, f32 f1) {
    CurveKey<T> from;
    CurveKey<T> to;
    from.value = SpanValue(interp, a, b, f0);
    to.value = SpanValue(interp, a, b, f1);
    from.in = from.out = from.value;
    to.in = to.out = to.value;
    if constexpr (kHeld<T>) {
        return {from, to};
    } else {
        if (!Smooth(interp) || std::is_same_v<T, Quaternion>) {
            return {from, to};
        }
        if (interp == Interpolation::Hermite) {
            // The derivative of the same cubic, over a parameter that runs 0..1
            // across `[f0, f1]` instead.
            from.out = Scaled(HermiteSlope(a, b, f0), f1 - f0);
            to.in = Scaled(HermiteSlope(a, b, f1), f1 - f0);
            from.in = from.out;
            to.out = to.in;
            return {from, to};
        }
        // Bezier: the sub-curve's control points are the blossom at (f0, f0,
        // f0), (f0, f0, f1), (f0, f1, f1) and (f1, f1, f1) -- de Casteljau with
        // a different parameter at each level.
        const auto blossom = [&](f32 x, f32 y, f32 z) {
            const T q0 = Lerped(a.value, a.out, x);
            const T q1 = Lerped(a.out, b.in, x);
            const T q2 = Lerped(b.in, b.value, x);
            return Lerped(Lerped(q0, q1, y), Lerped(q1, q2, y), z);
        };
        from.value = blossom(f0, f0, f0);
        from.out = blossom(f0, f0, f1);
        to.in = blossom(f0, f1, f1);
        to.value = blossom(f1, f1, f1);
        from.in = from.value;
        to.out = to.value;
        return {from, to};
    }
}

template <class T>
T FlatTangent(Interpolation interp, const T& value) {
    if (interp == Interpolation::Hermite && !std::is_same_v<T, Quaternion>) {
        return T{};
    }
    return value;
}

template <class T>
void SmoothTangents(Interpolation interp, std::vector<CurveKey<T>>& keys, std::size_t i) {
    if (i >= keys.size() || !Smooth(interp)) {
        return;
    }
    if constexpr (kHeld<T>) {
        return;
    } else if constexpr (std::is_same_v<T, Quaternion>) {
        DeriveSquadPoints(keys, i);
    } else {
        const std::size_t n = keys.size();
        T in{};
        T out{};
        if (n >= 2) {
            if (i == 0) {
                out = Combine(keys[1].value, keys[0].value, [](f32 b, f32 a) { return b - a; });
                in = out;
            } else if (i + 1 == n) {
                in = Combine(keys[i].value, keys[i - 1].value, [](f32 b, f32 a) { return b - a; });
                out = in;
            } else {
                const f32 before = keys[i].time - keys[i - 1].time;
                const f32 after = keys[i + 1].time - keys[i].time;
                const f32 total = before + after;
                const T chord = Combine(keys[i + 1].value, keys[i - 1].value, [](f32 b, f32 a) { return b - a; });
                in = Scaled(chord, total > 0.0f ? before / total : 0.5f);
                out = Scaled(chord, total > 0.0f ? after / total : 0.5f);
            }
        }
        if (interp == Interpolation::Hermite) {
            keys[i].in = in;
            keys[i].out = out;
        } else {
            keys[i].in = Combine(keys[i].value, in, [](f32 v, f32 t) { return v - t / 3.0f; });
            keys[i].out = Combine(keys[i].value, out, [](f32 v, f32 t) { return v + t / 3.0f; });
        }
    }
}

void DeriveSquadPoints(std::vector<CurveKey<Quaternion>>& keys, std::size_t i) {
    if (i >= keys.size()) {
        return;
    }
    const Quaternion q = keys[i].value.normalized();
    const Quaternion prev = Aligned(q, i > 0 ? keys[i - 1].value.normalized() : q);
    const Quaternion next = Aligned(q, i + 1 < keys.size() ? keys[i + 1].value.normalized() : q);
    const Quaternion inverse = q.conjugate();
    const Quaternion toNext = (inverse * next).normalized().log();
    const Quaternion toPrev = (inverse * prev).normalized().log();
    const Quaternion sum{-(toNext.x + toPrev.x) * 0.25f, -(toNext.y + toPrev.y) * 0.25f,
                         -(toNext.z + toPrev.z) * 0.25f, 0.0f};
    const Quaternion point = (q * sum.exp()).normalized();
    keys[i].in = point;
    keys[i].out = point;
}

template <class T>
void TcbTangents(std::vector<CurveKey<T>>& keys) {
    const std::size_t n = keys.size();
    if constexpr (kHeld<T>) {
        return;
    } else if constexpr (std::is_same_v<T, Quaternion>) {
        // In each key's own frame: A toward the next key and P toward the
        // previous, so the difference arriving from the previous is -P. A
        // missing neighbour is the key itself, as squad's inner points take it.
        for (std::size_t i = 0; i < n; ++i) {
            const Quaternion q = keys[i].value.normalized();
            const Quaternion prev = Aligned(q, i > 0 ? keys[i - 1].value.normalized() : q);
            const Quaternion next = Aligned(q, i + 1 < n ? keys[i + 1].value.normalized() : q);
            const Quaternion inverse = q.conjugate();
            const Quaternion toNext = (inverse * next).normalized().log();
            const Quaternion fromPrev = LogBlend((inverse * prev).normalized().log(), -1.0f, toNext, 0.0f);
            const TcbWeights w = WeightsOf(keys[i].tcb);
            const Quaternion incoming = LogBlend(fromPrev, w.inPrev, toNext, w.inNext);
            const Quaternion outgoing = LogBlend(fromPrev, w.outPrev, toNext, w.outNext);
            // Squad's control points from the log tangents: q·exp((TS − A)/2)
            // out of the key, q·exp((−P − TD)/2) into it.
            keys[i].out = (q * LogBlend(outgoing, 0.5f, toNext, -0.5f).exp()).normalized();
            keys[i].in = (q * LogBlend(fromPrev, 0.5f, incoming, -0.5f).exp()).normalized();
        }
    } else {
        const auto minus = [](f32 a, f32 b) { return a - b; };
        const auto plus = [](f32 a, f32 b) { return a + b; };
        for (std::size_t i = 0; i < n; ++i) {
            const f32 tension = 1.0f - keys[i].tcb.x;
            if (n < 2) {
                keys[i].in = keys[i].out = T{};
            } else if (i == 0) {
                keys[i].out = Scaled(Combine(keys[1].value, keys[0].value, minus), tension);
                keys[i].in = keys[i].out;
            } else if (i + 1 == n) {
                keys[i].in = Scaled(Combine(keys[i].value, keys[i - 1].value, minus), tension);
                keys[i].out = keys[i].in;
            } else {
                const T before = Combine(keys[i].value, keys[i - 1].value, minus);
                const T after = Combine(keys[i + 1].value, keys[i].value, minus);
                const f32 dIn = keys[i].time - keys[i - 1].time;
                const f32 dOut = keys[i + 1].time - keys[i].time;
                const f32 total = dIn + dOut;
                const f32 inScale = total > 0.0f ? 2.0f * dIn / total : 1.0f;
                const f32 outScale = total > 0.0f ? 2.0f * dOut / total : 1.0f;
                const TcbWeights w = WeightsOf(keys[i].tcb);
                keys[i].in = Scaled(Combine(Scaled(before, w.inPrev), Scaled(after, w.inNext), plus), inScale);
                keys[i].out = Scaled(Combine(Scaled(before, w.outPrev), Scaled(after, w.outNext), plus), outScale);
            }
        }
    }
}

#define WEM_CURVE_TYPES(T)                                                                                  \
    template std::vector<CurveKey<T>> DecodeKeys<T>(const SubTrack&);                                     \
    template void EncodeKeys<T>(SubTrack&, const std::vector<CurveKey<T>>&);                              \
    template T SpanValue<T>(Interpolation, const CurveKey<T>&, const CurveKey<T>&, f32);                  \
    template CurveKey<T> SplitSpan<T>(Interpolation, CurveKey<T>&, CurveKey<T>&, f32);                    \
    template std::pair<CurveKey<T>, CurveKey<T>> SubSpan<T>(Interpolation, const CurveKey<T>&,            \
                                                            const CurveKey<T>&, f32, f32);                \
    template T FlatTangent<T>(Interpolation, const T&);                                                   \
    template void SmoothTangents<T>(Interpolation, std::vector<CurveKey<T>>&, std::size_t);               \
    template void TcbTangents<T>(std::vector<CurveKey<T>>&);

WEM_CURVE_TYPES(f32)
WEM_CURVE_TYPES(Vector3f)
WEM_CURVE_TYPES(Vector4f)
WEM_CURVE_TYPES(Quaternion)
WEM_CURVE_TYPES(u32)

#undef WEM_CURVE_TYPES

} // namespace wem
} // namespace models
} // namespace whiteout
