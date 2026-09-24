// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/rigging/ik.h"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// Below this a length is a point, and a direction from it is noise.
constexpr f32 kTiny = 1e-6f;

// ---- Auto IK, in doubles ---------------------------------------------------------
//
// A drag taken back to the press has to give the press back to 1e-5, and the
// bend comes out of an arccosine that is ill-conditioned exactly where a limb
// is nearly straight. Doubles put that error far below anything a key keeps.

struct D3 {
    f64 x = 0, y = 0, z = 0;
};

D3 operator+(const D3& a, const D3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
D3 operator-(const D3& a, const D3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
D3 operator*(const D3& a, f64 s) {
    return {a.x * s, a.y * s, a.z * s};
}
f64 Dot(const D3& a, const D3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
D3 Cross(const D3& a, const D3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 Len(const D3& a) {
    return std::sqrt(Dot(a, a));
}
D3 Unit(const D3& a) {
    const f64 l = Len(a);
    return l > 0.0 ? a * (1.0 / l) : D3{};
}
D3 ToD(const Vector3f& v) {
    return {v.x, v.y, v.z};
}
Vector3f ToF(const D3& v) {
    return Vector3f{static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z)};
}

/// A rotation, Hamilton's convention: `a * b` turns by `b`, then by `a`.
struct Qd {
    f64 x = 0, y = 0, z = 0, w = 1;
};

Qd operator*(const Qd& a, const Qd& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Qd Conj(const Qd& q) {
    return {-q.x, -q.y, -q.z, q.w};
}
Qd AxisAngle(const D3& unitAxis, f64 angle) {
    const f64 s = std::sin(angle * 0.5);
    return {unitAxis.x * s, unitAxis.y * s, unitAxis.z * s, std::cos(angle * 0.5)};
}
D3 Turn(const Qd& q, const D3& v) {
    const D3 u{q.x, q.y, q.z};
    const D3 t = Cross(u, v) * 2.0;
    return v + t * q.w + Cross(u, t);
}
Qd ToD(const Quaternion& q) {
    const f64 l = std::sqrt(static_cast<f64>(q.x) * q.x + static_cast<f64>(q.y) * q.y +
                            static_cast<f64>(q.z) * q.z + static_cast<f64>(q.w) * q.w);
    if (l <= 0.0)
        return {};
    return {q.x / l, q.y / l, q.z / l, q.w / l};
}
Quaternion ToF(const Qd& q) {
    return Quaternion{static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z),
                      static_cast<f32>(q.w)};
}

/// The shortest turn taking direction @p from onto direction @p to.
Qd BetweenD(const D3& from, const D3& to) {
    const D3 u = Unit(from);
    const D3 v = Unit(to);
    const D3 axis = Cross(u, v);
    const f64 sine = Len(axis);
    const f64 cosine = Dot(u, v);
    if (sine < 1e-12) {
        if (cosine > 0.0)
            return {};
        const D3 seed = std::abs(u.x) < 0.9 ? D3{1, 0, 0} : D3{0, 1, 0};
        return AxisAngle(Unit(Cross(u, seed)), 3.14159265358979323846);
    }
    return AxisAngle(axis * (1.0 / sine), std::atan2(sine, cosine));
}

/// @p angle brought into (-pi, pi].
f64 Wrap(f64 angle) {
    constexpr f64 kTwoPi = 6.28318530717958647692;
    angle = std::fmod(angle, kTwoPi);
    if (angle > kTwoPi * 0.5)
        angle -= kTwoPi;
    if (angle <= -kTwoPi * 0.5)
        angle += kTwoPi;
    return angle;
}

/// Soft reach (§5): a target @p dist from the Upper becomes the distance the End
/// is sent. Nothing changes below the soft zone; inside it the reach eases
/// toward @p length and never arrives. The zone starts at `L − s` or at the
/// press's own reach @p pressed, whichever is further, so a limb pressed nearly
/// straight does not pull in the moment it is grabbed.
f64 SoftReach(f64 dist, f64 pressed, f64 length) {
    const f64 knee = std::max(length * (1.0 - kSoftReach), std::min(pressed, length));
    if (dist <= knee)
        return dist;
    const f64 soft = length - knee;
    if (soft <= length * 1e-9)
        return std::min(dist, length);
    return knee + soft * (1.0 - std::exp(-(dist - knee) / soft));
}

/// Below this fraction of a unit the hinge's direction across the aim line is
/// too short to steer by, and the swivel correction fades out rather than
/// spinning the limb about a line that is nearly the hinge itself.
constexpr f64 kHingeFade = 0.25;

} // namespace

Vector3f Rotate(const Quaternion& q, const Vector3f& v) {
    const Vector3f u{q.x, q.y, q.z};
    const Vector3f t = ::whiteout::cross(u, v) * 2.0f;
    return v + t * q.w + ::whiteout::cross(u, t);
}

// ---- Auto IK ---------------------------------------------------------------------

LimbSolve SolveLimb(const LimbPress& press, const LimbSetup& setup, const IkGoal& goal) {
    LimbSolve solve;
    const D3 upper = ToD(press.upper);
    const D3 lower = ToD(press.lower);
    // A three-joint leg's Hock keeps its bend, so the Lower to the End is one
    // rigid segment and the two-joint solve places it whole (§5).
    const D3 u = lower - upper;
    const D3 v = ToD(press.end) - lower;
    const f64 a = Len(u);
    const f64 b = Len(v);
    const D3 hinge = Unit(ToD(setup.hinge));
    if (a < kTiny || b < kTiny || Len(hinge) < 0.5)
        return solve;
    const f64 length = a + b;

    // Where the End is going, from the Upper. A target on the Upper keeps the
    // way the limb already points: the least surprising non-answer.
    const D3 pressed = u + v;
    const D3 toward = ToD(goal.position) - upper;
    const f64 dist = Len(toward);
    const D3 aim = dist > length * 1e-9 ? toward * (1.0 / dist)
                                        : Unit(Len(pressed) > length * 1e-9 ? pressed : u);
    const f64 reach = SoftReach(dist, Len(pressed), length);

    // The bend. The Lower turned by phi about the hinge puts the End
    // `|u + R(phi) v|` from the Upper, whose square is
    // `a² + b² + 2(along + across·cos phi + side·sin phi)`: solved for the
    // reach, on the branch nearest no turn, which is the side the limb already
    // bends to — and that side is set by the press alone, so it never flips.
    const D3 vAlong = hinge * Dot(v, hinge);
    const f64 along = Dot(u, vAlong);
    const f64 across = Dot(u, v - vAlong);
    const f64 side = Dot(u, Cross(hinge, v));
    const f64 amplitude = std::hypot(across, side);
    f64 phi = 0.0;
    if (amplitude > a * b * 1e-9) {
        const f64 needed = (reach * reach - a * a - b * b) * 0.5 - along;
        const f64 straightest = std::atan2(side, across);
        const f64 spread = std::acos(std::clamp(needed / amplitude, -1.0, 1.0));
        const f64 one = Wrap(straightest + spread);
        const f64 other = Wrap(straightest - spread);
        // A tie is a limb pressed straight, and that bends positively.
        if (std::abs(std::abs(one) - std::abs(other)) < 1e-12)
            phi = std::max(one, other);
        else
            phi = std::abs(one) < std::abs(other) ? one : other;
    }
    if (setup.bendLimitsDeg.has_value()) {
        constexpr f64 kDeg = 3.14159265358979323846 / 180.0;
        const f64 now = setup.bendDeg * kDeg;
        f64 low = setup.bendLimitsDeg->first * kDeg;
        f64 high = setup.bendLimitsDeg->second * kDeg;
        if (low > high)
            std::swap(low, high);
        // A bend the clip already has outside the limits stays; the limits only
        // stop the drag taking it further out.
        low = std::min(low, now);
        high = std::max(high, now);
        phi = std::clamp(now + phi, low, high) - now;
    }
    const Qd bend = AxisAngle(hinge, phi);
    const D3 bent = u + Turn(bend, v);

    // The Upper aims the End with the shortest arc, then turns about the aim
    // line until the hinge sits as near its press direction as the line lets
    // it: that is the swivel kept. Both are measured across the aim line, and
    // as the line nears the hinge itself that measure shrinks to nothing, so
    // the correction fades out with it instead of spinning the limb.
    const Qd aimTurn = BetweenD(bent, aim);
    Qd upperTurn = aimTurn;
    const D3 carried = Turn(aimTurn, hinge);
    const D3 want = hinge - aim * Dot(hinge, aim);
    const D3 have = carried - aim * Dot(carried, aim);
    const f64 wantLength = Len(want);
    const f64 haveLength = Len(have);
    if (wantLength > 1e-9 && haveLength > 1e-9) {
        const f64 psi = std::atan2(Dot(aim, Cross(have, want)), Dot(have, want));
        const f64 weight = std::min(1.0, std::min(wantLength, haveLength) / kHingeFade);
        upperTurn = AxisAngle(aim, psi * weight) * aimTurn;
    }

    // The Lower's turn is the bend, about the hinge where the Upper's turn left
    // it; everything below the Lower then turned by `limb`.
    const Qd limb = upperTurn * bend;
    solve.upperDelta = ToF(upperTurn);
    solve.lowerDelta = ToF(upperTurn * bend * Conj(upperTurn));
    const D3 lowerAt = upper + Turn(upperTurn, u);
    const D3 endAt = lowerAt + Turn(limb, v);
    solve.lower = ToF(lowerAt);
    solve.end = ToF(endAt);
    if (press.hock.has_value())
        solve.hock = ToF(lowerAt + Turn(limb, ToD(*press.hock) - lower));
    if (goal.holdOrientation)
        solve.endDelta = ToF(ToD(goal.orientation) * Conj(limb * ToD(press.endWorld)));
    solve.reached = Len(endAt - ToD(goal.position)) <= length * 1e-4;
    solve.ok = true;
    return solve;
}

f32 AskedReach(f32 wanted, f32 pressed, f32 length) {
    // `SoftReach` backwards: the same knee, the same zone.
    const f64 l = length;
    const f64 knee = std::max(l * (1.0 - kSoftReach), std::min<f64>(pressed, l));
    if (wanted <= knee)
        return wanted;
    const f64 soft = l - knee;
    if (soft <= l * 1e-9)
        return static_cast<f32>(std::min<f64>(wanted, l));
    // e^-6.9 is 1e-3 of the zone, 5e-5 of the length when the zone is the
    // full 5 %: inside the 1e-4 `SolveLimb` calls reached.
    constexpr f64 kZones = 6.9;
    const f64 share = (wanted - knee) / soft;
    const f64 t = share >= 1.0 ? kZones : std::min(kZones, -std::log(1.0 - share));
    return static_cast<f32>(knee + soft * t);
}

LimbSolve SwivelLimb(const LimbPress& press, f32 angleRad) {
    LimbSolve solve;
    const D3 upper = ToD(press.upper);
    const D3 lower = ToD(press.lower);
    const D3 line = ToD(press.end) - upper;
    if (Len(lower - upper) < kTiny || Len(ToD(press.end) - lower) < kTiny || Len(line) < kTiny)
        return solve;
    // The End is on the line, so it stays; its rotation is turned back.
    const Qd turn = AxisAngle(Unit(line), angleRad);
    solve.upperDelta = ToF(turn);
    solve.endDelta = ToF(Conj(turn));
    solve.lower = ToF(upper + Turn(turn, lower - upper));
    solve.end = ToF(upper + Turn(turn, line));
    if (press.hock.has_value())
        solve.hock = ToF(upper + Turn(turn, ToD(*press.hock) - upper));
    solve.ok = true;
    solve.reached = true;
    return solve;
}

Vector3f FallbackHinge(const LimbPress& press, const LimbPress& rest, const Vector3f& forward,
                       bool bendsForward) {
    for (const LimbPress* at : {&press, &rest}) {
        const D3 u = ToD(at->lower) - ToD(at->upper);
        const D3 v = ToD(at->end) - ToD(at->lower);
        const D3 n = Cross(u, v);
        // Bent by more than a few hundredths of a degree: a plane worth taking.
        if (Len(n) > 1e-3 * Len(u) * Len(v))
            return ToF(Unit(n));
    }
    const D3 limb = ToD(press.end) - ToD(press.upper);
    if (Len(limb) < kTiny)
        return Vector3f{0, 0, 0};
    const D3 dir = Unit(limb);
    D3 across = Cross(dir, Unit(ToD(forward)));
    if (Len(across) < 1e-6) {
        // A limb pointing along forward: any side will do.
        const D3 seed = std::abs(dir.x) < 0.9 ? D3{1, 0, 0} : D3{0, 1, 0};
        across = Cross(dir, seed);
    }
    // `limb × -forward` bends the Lower toward forward: a positive turn about it
    // swings the End back, and the aim then carries the Lower out in front.
    return ToF(Unit(bendsForward ? across * -1.0 : across));
}

ChainSolve SolveChain(std::span<const Vector3f> press, const Quaternion& endWorld,
                      const IkGoal& goal) {
    ChainSolve solve;
    const std::size_t count = press.size();
    if (count < 2)
        return solve;
    std::vector<D3> was(count);
    for (std::size_t i = 0; i < count; ++i)
        was[i] = ToD(press[i]);
    std::vector<f64> lengths(count - 1);
    f64 total = 0.0;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        lengths[i] = Len(was[i + 1] - was[i]);
        total += lengths[i];
    }
    if (total < kTiny)
        return solve;

    const D3 root = was[0];
    const D3 target = ToD(goal.position);
    const f64 tolerance = total * kChainTolerance;
    std::vector<D3> points = was;
    // A point @p length from @p from toward @p toward; a segment with no
    // direction left keeps the one it had at the press.
    const auto place = [](const D3& from, const D3& toward, f64 length, const D3& pressed) {
        const D3 dir = toward - from;
        const f64 l = Len(dir);
        return from + (l > 1e-12 ? dir * (1.0 / l) : Unit(pressed)) * length;
    };
    if (Len(target - root) >= total) {
        // Out of reach: straight toward the target, every length kept.
        const D3 dir = Unit(target - root);
        f64 run = 0.0;
        for (std::size_t i = 1; i < count; ++i) {
            run += lengths[i - 1];
            points[i] = root + dir * run;
        }
    } else {
        for (u32 pass = 0; pass < kChainIterations && Len(points[count - 1] - target) > tolerance;
             ++pass) {
            points[count - 1] = target;
            for (std::size_t i = count - 1; i-- > 0;)
                points[i] = place(points[i + 1], points[i], lengths[i], was[i] - was[i + 1]);
            points[0] = root;
            for (std::size_t i = 0; i + 1 < count; ++i)
                points[i + 1] = place(points[i], points[i + 1], lengths[i], was[i + 1] - was[i]);
        }
    }

    // One turn per joint, root down, each from where the turns above left its
    // segment: the shortest arc, so none of them twists a bone about itself.
    Qd carried{};
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const D3 from = Turn(carried, was[i + 1] - was[i]);
        const D3 to = points[i + 1] - points[i];
        const Qd turn = Len(from) > 1e-12 && Len(to) > 1e-12 ? BetweenD(from, to) : Qd{};
        solve.deltas.push_back(ToF(turn));
        carried = turn * carried;
    }
    for (const D3& point : points)
        solve.points.push_back(ToF(point));
    if (goal.holdOrientation)
        solve.endDelta = ToF(ToD(goal.orientation) * Conj(carried * ToD(endWorld)));
    solve.reached = Len(points[count - 1] - target) <= tolerance;
    solve.ok = true;
    return solve;
}

} // namespace wem
} // namespace models
} // namespace whiteout
