// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/soft_modes.h>

#include <whiteout/models/wem/physics/soft_step.h>

#include "soft_linalg.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace whiteout {
namespace models {
namespace wem {

namespace {

using softdetail::Mat3;

constexpr f64 kTau = 6.283185307179586;
/// What *Hold shape* at full gives the neighbours' term, against the return's.
constexpr f64 kHold = 0.5;
/// How many times the return's stiffness *Keep volume* at full gives the one
/// direction that changes the volume.
constexpr f64 kVolume = 60.0;
/// The same for the whole body's lag under *Keep in place*: stiff enough that
/// it is followed and not simulated.
constexpr f64 kPlace = 2500.0;
/// Gravity at *Weight* 1, in region sizes a second squared: a body at two
/// swings a second then sags by three hundredths of its size.
constexpr f32 kGravity = 5.0f;
/// The quickest a mode swings, a second (§8.3): keys play nothing faster.
constexpr f64 kQuickest = 8.0;
/// The neighbours a point holds to, at most, and how far they are looked for,
/// in point spacings.
constexpr std::size_t kNeighbours = 8;
constexpr f32 kNeighbourReach = 2.5f;

/// `a . b` in the wider width. A sum of these is a matrix that must stay
/// positive: in the narrow one, a body with a direction that weighs almost
/// nothing rounds its way to a negative stiffness.
f64 Dot(const Vector3f& a, const Vector3f& b) {
    return static_cast<f64>(a.x) * b.x + static_cast<f64>(a.y) * b.y + static_cast<f64>(a.z) * b.z;
}

/// A point a lump moves: its share of the point, and its arm from the pivot.
struct Entry {
    u32 point = 0;
    f32 weight = 0.0f;
    Vector3f arm{0, 0, 0};
};

/// A point's motion per unit of one coordinate of a lump it rides.
Vector3f ColumnOf(u8 kind, const Vector3f& arm, f32 weight) {
    switch (kind) {
    case 0:
        return Vector3f{weight, 0, 0};
    case 1:
        return Vector3f{0, weight, 0};
    case 2:
        return Vector3f{0, 0, weight};
    // A small turn w moves a point at arm r by w x r.
    case 3:
        return Vector3f{0, -arm.z, arm.y} * weight;
    case 4:
        return Vector3f{arm.z, 0, -arm.x} * weight;
    case 5:
        return Vector3f{-arm.y, arm.x, 0} * weight;
    // A scale's logarithm s moves it by s times its arm along that axis.
    case 6:
        return Vector3f{arm.x * weight, 0, 0};
    case 7:
        return Vector3f{0, arm.y * weight, 0};
    default:
        return Vector3f{0, 0, arm.z * weight};
    }
}

/// The body's coordinates and what each does to each point.
struct Layout {
    std::vector<SoftModes::Coordinate> coordinates;
    std::vector<u32> firstOf;                ///< Per lump, its first coordinate, or none.
    std::vector<std::vector<Entry>> entries; ///< Per lump that swings, its points.
    std::vector<f32> reach;                  ///< Per lump, its farthest point's arm.
    std::vector<Vector3f> outward;           ///< Per lump, outward over its points; zero where they face every way.
    /// Per point, the coordinates that move it and how; and the share of the
    /// point each one's lump has.
    std::vector<u32> columnStart;
    std::vector<u32> columnIndex;
    std::vector<Vector3f> columns;
    std::vector<f32> columnShare;
};

Layout LayoutOf(const SoftRig& rig) {
    Layout out;
    const SoftBody& body = rig.body();
    const std::vector<SoftPoint>& points = rig.points().points;
    const u32 lumps = rig.lumpCount();
    out.firstOf.assign(lumps, kInvalidIndex);
    out.entries.resize(lumps);
    out.reach.assign(lumps, 0.0f);
    out.outward.assign(lumps, Vector3f{0, 0, 0});
    std::vector<f32> held(lumps, 0.0f);
    for (u32 p = 0; p < points.size(); ++p) {
        for (const SoftSlot& slot : rig.slotsOf(p)) {
            if (slot.lump < lumps && slot.weight > 0.0f && body.lumps[slot.lump].swings) {
                const Vector3f arm = points[p].at - body.lumps[slot.lump].rest;
                out.entries[slot.lump].push_back(Entry{p, slot.weight, arm});
                out.reach[slot.lump] = std::max(out.reach[slot.lump], arm.length());
                out.outward[slot.lump] = out.outward[slot.lump] + points[p].outward * (points[p].area * slot.weight);
                held[slot.lump] += points[p].area * slot.weight;
            }
        }
    }
    for (u32 k = 0; k < lumps; ++k) {
        if (out.entries[k].empty()) {
            continue;
        }
        const f32 length = out.outward[k].length();
        out.outward[k] = held[k] > 0.0f && length > 0.5f * held[k] ? out.outward[k] * (1.0f / length) : Vector3f{0, 0, 0};
        out.firstOf[k] = static_cast<u32>(out.coordinates.size());
        // A bone with bones below it has its turn alone; a rigid part's lump
        // has no scale (§6.5).
        for (u8 kind = 0; kind < 9; ++kind) {
            const bool has = kind < 3 ? rig.moves(k) : kind < 6 ? true : rig.scales(k);
            if (has) {
                out.coordinates.push_back(SoftModes::Coordinate{k, kind});
            }
        }
    }
    for (u32 p = 0; p < points.size(); ++p) {
        out.columnStart.push_back(static_cast<u32>(out.columnIndex.size()));
        for (const SoftSlot& slot : rig.slotsOf(p)) {
            if (slot.lump >= lumps || out.firstOf[slot.lump] == kInvalidIndex || !(slot.weight > 0.0f)) {
                continue;
            }
            const Vector3f arm = points[p].at - body.lumps[slot.lump].rest;
            for (u32 c = out.firstOf[slot.lump]; c < out.coordinates.size() && out.coordinates[c].lump == slot.lump; ++c) {
                out.columnIndex.push_back(c);
                out.columns.push_back(ColumnOf(out.coordinates[c].kind, arm, slot.weight));
                out.columnShare.push_back(slot.weight);
            }
        }
    }
    out.columnStart.push_back(static_cast<u32>(out.columnIndex.size()));
    return out;
}

/// `L y = b` in place over @p b, @p stride apart.
void Forward(const std::vector<f64>& l, std::size_t n, f64* b, std::size_t stride) {
    for (std::size_t i = 0; i < n; ++i) {
        f64 s = b[i * stride];
        const f64* row = &l[i * n];
        for (std::size_t k = 0; k < i; ++k) {
            s -= row[k] * b[k * stride];
        }
        b[i * stride] = s / row[i];
    }
}

/// `L^T x = b` in place over @p b.
void Backward(const std::vector<f64>& l, std::size_t n, f64* b) {
    for (std::size_t i = n; i-- > 0;) {
        f64 s = b[i];
        for (std::size_t k = i + 1; k < n; ++k) {
            s -= l[k * n + i] * b[k];
        }
        b[i] = s / l[i * n + i];
    }
}

/// `stiff += scale * v v^T / (v^T M^-1 v)`: one direction stiffened to
/// @p scale times the return's, whatever its mass.
void Stiffen(std::vector<f64>& stiff, const std::vector<f64>& factor, std::size_t n, const std::vector<f64>& v, f64 scale) {
    std::vector<f64> solved = v;
    softdetail::CholeskySolve(factor, n, solved.data());
    f64 denominator = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        denominator += v[i] * solved[i];
    }
    if (!(denominator > 1e-300)) {
        return;
    }
    const f64 c = scale / denominator;
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            stiff[i * n + j] += c * v[i] * v[j];
        }
    }
}

/// A smooth stop: itself up to six tenths, and never past 1.
f64 Stop(f64 u) {
    const f64 knee = 0.6;
    return u <= knee ? u : knee + (1.0 - knee) * std::tanh((u - knee) / (1.0 - knee));
}

/// The 2x2 a damped oscillator's state (where, how fast) is carried by over
/// @p dt with no push: exact, whatever its damping.
std::array<f64, 4> Carry(f64 omega, f64 zeta, f64 dt) {
    const f64 decay = std::exp(-zeta * omega * dt);
    if (std::abs(zeta - 1.0) < 1e-6) {
        return {decay * (1.0 + omega * dt), decay * dt, -decay * omega * omega * dt, decay * (1.0 - omega * dt)};
    }
    if (zeta < 1.0) {
        const f64 damped = omega * std::sqrt(1.0 - zeta * zeta);
        const f64 s = std::sin(damped * dt), c = std::cos(damped * dt);
        return {decay * (c + zeta * omega / damped * s), decay * s / damped, -decay * omega * omega * s / damped,
                decay * (c - zeta * omega / damped * s)};
    }
    const f64 damped = omega * std::sqrt(zeta * zeta - 1.0);
    const f64 s = std::sinh(damped * dt), c = std::cosh(damped * dt);
    return {decay * (c + zeta * omega / damped * s), decay * s / damped, -decay * omega * omega * s / damped,
            decay * (c - zeta * omega / damped * s)};
}

Mat3 LinearOf(const Matrix44f& m) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r][c] = m.data[r][c];
        }
    }
    return out;
}

/// What turns a move in the model's space into @p carrier's frame: its turn
/// undone, and its size, which is held between a quarter and four. A bone an
/// animation scales away to hide a part has no inverse worth the name, and a
/// push through one would be without bound. One with no frame left gives none.
Mat3 BackOf(const Matrix44f& carrier) {
    Vector3f rows[3] = {Vector3f{carrier.data[0][0], carrier.data[0][1], carrier.data[0][2]},
                        Vector3f{carrier.data[1][0], carrier.data[1][1], carrier.data[1][2]},
                        Vector3f{carrier.data[2][0], carrier.data[2][1], carrier.data[2][2]}};
    const f32 lengths[3] = {rows[0].length(), rows[1].length(), rows[2].length()};
    const f32 scale = (lengths[0] + lengths[1] + lengths[2]) / 3.0f;
    Mat3 out{};
    if (!(std::min({lengths[0], lengths[1], lengths[2]}) > 1e-4f * std::max(scale, 1e-6f)) || !(scale > 1e-6f)) {
        return out;
    }
    // The rows made square to each other, in turn.
    rows[0] = rows[0] * (1.0f / lengths[0]);
    rows[1] = rows[1] - rows[0] * rows[1].dot(rows[0]);
    const f32 second = rows[1].length();
    if (!(second > 1e-6f)) {
        return out;
    }
    rows[1] = rows[1] * (1.0f / second);
    rows[2] = cross(rows[0], rows[1]) * (cross(rows[0], rows[1]).dot(rows[2]) < 0.0f ? -1.0f : 1.0f);
    const f64 by = 1.0 / static_cast<f64>(std::clamp(scale, 0.25f, 4.0f));
    for (int r = 0; r < 3; ++r) {
        // The transpose: a turn's inverse.
        out[0][r] = static_cast<f64>(rows[r].x) * by;
        out[1][r] = static_cast<f64>(rows[r].y) * by;
        out[2][r] = static_cast<f64>(rows[r].z) * by;
    }
    return out;
}

Vector3f Times(const Vector3f& v, const Mat3& m) {
    const std::array<f64, 3> out = softdetail::RowTimes({v.x, v.y, v.z}, m);
    return Vector3f{static_cast<f32>(out[0]), static_cast<f32>(out[1]), static_cast<f32>(out[2])};
}

/// A lump's swing from its nine numbers: its move, its small turn, and the
/// logarithms of its scales.
Transform SwingOf(const Vector3f part[3]) {
    Transform swing;
    swing.translation = part[0];
    const f32 angle = part[1].length();
    if (angle > 1e-9f) {
        swing.rotation = Quaternion::from_axis_angle(part[1] * (1.0f / angle), angle).normalized();
    }
    swing.scale = Vector3f{std::exp(part[2].x), std::exp(part[2].y), std::exp(part[2].z)};
    return swing;
}

} // namespace

u64 SoftRigPrint(const SoftRig& rig) {
    u64 print = 1469598103934665603ull;
    const auto add = [&](const void* data, std::size_t size) {
        const u8* at = static_cast<const u8*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            print = (print ^ at[i]) * 1099511628211ull;
        }
    };
    const auto number = [&](f32 value) { add(&value, sizeof value); };
    const auto whole = [&](u32 value) { add(&value, sizeof value); };
    if (rig.empty()) {
        return print;
    }
    whole(rig.pointCount());
    whole(rig.lumpCount());
    for (u32 p = 0; p < rig.pointCount(); ++p) {
        const SoftPoint& point = rig.points().points[p];
        for (const f32 value : {point.at.x, point.at.y, point.at.z, point.area, point.outward.x, point.outward.y,
                                point.outward.z}) {
            number(value);
        }
        for (const SoftSlot& slot : rig.slotsOf(p)) {
            whole(slot.lump);
            number(slot.weight);
        }
    }
    for (u32 k = 0; k < rig.lumpCount(); ++k) {
        const SoftLump& lump = rig.body().lumps[k];
        for (const f32 value : {lump.rest.x, lump.rest.y, lump.rest.z}) {
            number(value);
        }
        whole((lump.swings ? 1u : 0u) | (rig.moves(k) ? 2u : 0u) | (rig.scales(k) ? 4u : 0u));
    }
    return print;
}

f32 SoftModes::rate(u32 mode, f32 looseness) const {
    return mode < stiffness.size() ? looseness * static_cast<f32>(std::sqrt(stiffness[mode])) : 0.0f;
}

std::vector<Transform> SoftRandomWobble(const SoftRig& rig, const SoftModes& modes, f32 speed, u32 seed,
                                        std::span<const f32> amount, u32 steps, f32 dt, f32 duration, bool loops) {
    const u32 lumps = rig.lumpCount();
    const std::size_t frames = static_cast<std::size_t>(steps) + 1;
    std::vector<Transform> out(frames * lumps, Transform{});
    if (rig.empty() || modes.empty() || steps == 0 || amount.size() < frames || !(dt > 0.0f)) {
        return out;
    }
    const Layout layout = LayoutOf(rig);
    const std::size_t n = layout.coordinates.size();
    const std::size_t m = modes.count();
    if (n != modes.coordinates.size() || modes.shapes.size() != m * n) {
        return out;
    }
    // The slowest that move a point. A coordinate no point feels (a scale
    // along an axis a flat region lies across) is a mode too, and the slowest
    // there is: mixed in, it would take a place and move nothing.
    f64 widest = 0.0;
    for (std::size_t mode = 0; mode < m; ++mode) {
        widest = std::max(widest, modes.span[mode]);
    }
    std::vector<std::size_t> live;
    for (std::size_t mode = 0; mode < m && live.size() < kSoftRandomModes; ++mode) {
        if (modes.span[mode] > 1e-6 * widest && modes.span[mode] > 1e-12) {
            live.push_back(mode);
        }
    }
    const std::size_t mixed = live.size();
    // A mode's mass is 1: over the whole skin its points move one over the
    // root of the skin's area, by the root of their mean square.
    f64 skin = 0.0;
    for (u32 p = 0; p < rig.pointCount(); ++p) {
        skin += rig.points().points[p].area;
    }
    const f64 even = std::sqrt(std::max(skin, 1e-30));
    // Each mode's rate, its phase, and how much of it moves the body one
    // unit: its points as a whole, and its farthest no more than
    // `kSoftRandomFar`.
    std::vector<f64> rate(mixed), phase(mixed), unit(mixed);
    for (std::size_t at = 0; at < mixed; ++at) {
        const std::size_t mode = live[at];
        const f64 quicker = std::min(std::sqrt(std::max(modes.stiffness[mode], 1.0)), 4.0);
        rate[at] = SoftWholeCycles(static_cast<f32>(std::clamp(speed, 0.01f, 8.0f) * quicker), duration, loops);
        // The golden angle apart, mode after mode.
        phase[at] = 2.3999632 * (static_cast<f64>(seed % 1024u) + 13.0 * static_cast<f64>(at));
        unit[at] = std::min(even, static_cast<f64>(kSoftRandomFar) / modes.span[mode]) /
                   std::sqrt(static_cast<f64>(mixed));
    }
    std::vector<f64> coordinates(n);
    for (std::size_t i = 0; i < frames; ++i) {
        const f64 far = amount[i];
        if (far == 0.0) {
            continue;
        }
        const f64 seconds = static_cast<f64>(loops ? i % steps : i) * dt;
        std::fill(coordinates.begin(), coordinates.end(), 0.0);
        for (std::size_t at = 0; at < mixed; ++at) {
            const f64 x = far * unit[at] * std::sin(kTau * rate[at] * seconds + phase[at]);
            const f64* shape = modes.shapes.data() + live[at] * n;
            for (std::size_t c = 0; c < n; ++c) {
                coordinates[c] += x * shape[c];
            }
        }
        for (u32 k = 0; k < lumps; ++k) {
            if (layout.firstOf[k] == kInvalidIndex) {
                continue;
            }
            Vector3f part[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            for (u32 c = layout.firstOf[k]; c < n && layout.coordinates[c].lump == k; ++c) {
                const u8 kind = layout.coordinates[c].kind;
                const f32 value = static_cast<f32>(coordinates[c]);
                (kind % 3 == 0 ? part[kind / 3].x : kind % 3 == 1 ? part[kind / 3].y : part[kind / 3].z) = value;
            }
            out[i * lumps + k] = SwingOf(part);
        }
    }
    return out;
}

Vector3f SoftWindAt(const SoftWorld& world, f32 seconds) {
    Vector3f out{0, 0, 0};
    const f32 g = kGravity * std::max(world.size, 1e-6f);
    const f32 degrees = static_cast<f32>(kTau / 360.0);
    for (const BakeWorldForce& force : world.forces) {
        if (force.kind != BakeWorldForce::Kind::Wind || seconds < force.start || seconds > force.end ||
            force.end <= force.start) {
            continue;
        }
        f32 ramp = 1.0f;
        if (force.rampIn > 0.0f) {
            ramp = std::min(ramp, (seconds - force.start) / force.rampIn);
        }
        if (force.rampOut > 0.0f) {
            ramp = std::min(ramp, (force.end - seconds) / force.rampOut);
        }
        if (!(ramp > 0.0f)) {
            continue;
        }
        // One wave of gusts, in whole cycles a loop so that its end is its start.
        const f32 rate = SoftWholeCycles(std::max(force.gustSpeed, 0.0f), world.duration, world.looping);
        const f32 gust =
            std::max(1.0f + force.gusts * std::sin(static_cast<f32>(kTau) * rate * (seconds - force.start)), 0.0f);
        const f32 heading = force.heading * degrees, rise = force.rise * degrees;
        const Vector3f along{std::cos(rise) * std::cos(heading), std::cos(rise) * std::sin(heading), std::sin(rise)};
        out = out + along * (g * force.strength * ramp * gust);
    }
    return out;
}

Vector3f SoftBlastAt(const SoftWorld& world, f32 seconds, f32 dt, const Vector3f& at) {
    Vector3f out{0, 0, 0};
    if (!(dt > 0.0f)) {
        return out;
    }
    const f32 g = kGravity * std::max(world.size, 1e-6f);
    const Vector3f box = world.upper - world.lower;
    const f32 height = std::max({box.z, box.x * 0.5f, box.y * 0.5f, 1e-3f});
    const Vector3f middle = (world.lower + world.upper) * 0.5f;
    // The step a blast goes off in is the one whose number holds its moment,
    // as a kick's: two times compared lost one to rounding and gave another
    // to two steps. A loop's last frame is its first.
    const i64 step = std::llround(seconds / dt);
    const i64 steps = world.looping && world.duration > 0.0f ? std::max<i64>(1, std::llround(world.duration / dt)) : 0;
    for (std::size_t i = 0; i < world.forces.size(); ++i) {
        const BakeWorldForce& force = world.forces[i];
        if (force.kind != BakeWorldForce::Kind::Blast) {
            continue;
        }
        const i64 lands = static_cast<i64>(std::floor(std::max(force.start, 0.0f) / dt + 1e-3f));
        if ((steps > 0 ? lands % steps : lands) != step) {
            continue;
        }
        Vector3f centre{middle.x, middle.y, world.lower.z + force.height * height};
        if (force.centreNode != kInvalidNode && i < world.centres.size()) {
            centre = world.centres[i];
        }
        const f32 radius = std::max(force.radius, 0.01f) * height;
        const Vector3f away = at - centre;
        const f32 distance = away.length();
        if (distance >= radius) {
            continue;
        }
        const Vector3f way = distance > 1e-6f * radius ? away * (1.0f / distance) : Vector3f{0, 0, 1};
        out = out + way * (g * force.strength * kSoftBlastSeconds * (1.0f - distance / radius) / dt);
    }
    return out;
}

f32 SoftKickShare(f32 distance, f32 size) {
    const f32 u = distance / (kSoftKickReach * std::max(size, 1e-6f));
    return u < 1.0f ? (1.0f - u) * (1.0f - u) : 0.0f;
}

f32 SoftKickPush(f32 strength, f32 size, f32 dt) {
    return dt > 0.0f ? kGravity * std::max(size, 1e-6f) * strength * kSoftBlastSeconds / dt : 0.0f;
}

f32 SoftDampingOf(f32 bounce) {
    // 1 at no bounce; a fifth at Fat's 35 %; six hundredths at full, which is
    // a dozen swings down to a hundredth. Between them by ratio, so each tenth
    // of the slider adds about as many swings as the last.
    const f32 b = std::clamp(bounce, 0.0f, 1.0f);
    return std::pow(0.06f, std::pow(b, 0.532f));
}

SoftModes MakeSoftModes(const SoftRig& rig, const SoftVolume& volume, const SoftJiggle& settings) {
    SoftModes modes;
    if (rig.empty()) {
        return modes;
    }
    modes.size = rig.points().size;
    const Layout layout = LayoutOf(rig);
    const std::size_t n = layout.coordinates.size();
    if (n == 0) {
        return modes;
    }
    const std::vector<SoftPoint>& points = rig.points().points;
    const f64 size = std::max(modes.size, 1e-6f);

    // Mass: each point's area, through what the coordinates do to it. And the
    // return: every point pulled back to its goal by each of its lumps, which
    // is the mass with each lump's part of a point taken by itself. Where a
    // point's lumps move it alike the two are one, and the body lagging as a
    // whole swings at *Looseness*. Where they pull it apart the return is the
    // larger: two lumps that cancel at the bind have no mass as a pair there,
    // and would otherwise swing free, and wildly once a pose turns their bones
    // apart.
    std::vector<f64> mass(n * n, 0.0), stiff(n * n, 0.0);
    for (u32 p = 0; p < points.size(); ++p) {
        const f64 area = points[p].area;
        for (u32 a = layout.columnStart[p]; a < layout.columnStart[p + 1]; ++a) {
            for (u32 b = layout.columnStart[p]; b < layout.columnStart[p + 1]; ++b) {
                const u32 ca = layout.columnIndex[a], cb = layout.columnIndex[b];
                const f64 dot = area * Dot(layout.columns[a], layout.columns[b]);
                mass[ca * n + cb] += dot;
                if (layout.coordinates[ca].lump == layout.coordinates[cb].lump) {
                    stiff[ca * n + cb] += dot / static_cast<f64>(layout.columnShare[a]);
                }
            }
        }
    }
    f64 trace = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        trace += mass[i * n + i];
    }
    // A coordinate no point feels (a scale along an axis its points lie flat
    // across) still needs a mass to have a rate.
    const f64 least = std::max(trace / static_cast<f64>(n), 1e-30) * 1e-7;
    for (std::size_t i = 0; i < n; ++i) {
        mass[i * n + i] += least;
        stiff[i * n + i] += least;
    }

    // Neighbours move alike: found through space, since two layers of one body
    // share no edge.
    if (settings.holdShape > 0.0f && points.size() > 1) {
        std::vector<f32> spacing(points.size());
        std::vector<f32> sorted;
        for (u32 p = 0; p < points.size(); ++p) {
            spacing[p] = std::sqrt(std::max(points[p].area, 0.0f));
            sorted.push_back(spacing[p]);
        }
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const f32 cell = std::max(sorted[sorted.size() / 2] * kNeighbourReach, static_cast<f32>(size) * 1e-4f);
        const Vector3f low = rig.points().low;
        const auto keyOf = [&](const Vector3f& at) {
            const auto axis = [&](f32 v, f32 from) { return static_cast<u64>(std::max(0.0f, (v - from) / cell)) & 0x1FFFFFu; };
            return axis(at.x, low.x) | (axis(at.y, low.y) << 21) | (axis(at.z, low.z) << 42);
        };
        std::map<u64, std::vector<u32>> cells;
        for (u32 p = 0; p < points.size(); ++p) {
            cells[keyOf(points[p].at)].push_back(p);
        }
        const f64 hold = kHold * static_cast<f64>(settings.holdShape) * static_cast<f64>(settings.holdShape);
        std::vector<std::pair<f32, u32>> close;
        std::vector<u32> index;
        std::vector<Vector3f> difference;
        for (u32 p = 0; p < points.size(); ++p) {
            close.clear();
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dz = -1; dz <= 1; ++dz) {
                        const Vector3f probe = points[p].at + Vector3f{static_cast<f32>(dx), static_cast<f32>(dy),
                                                                      static_cast<f32>(dz)} * cell;
                        if (probe.x < low.x - cell || probe.y < low.y - cell || probe.z < low.z - cell) {
                            continue;
                        }
                        const auto found = cells.find(keyOf(probe));
                        if (found == cells.end()) {
                            continue;
                        }
                        for (const u32 q : found->second) {
                            const f32 d = (points[q].at - points[p].at).length();
                            if (q != p && d <= cell) {
                                close.emplace_back(d, q);
                            }
                        }
                    }
                }
            }
            std::sort(close.begin(), close.end());
            close.erase(std::unique(close.begin(), close.end()), close.end());
            if (close.size() > kNeighbours) {
                close.resize(kNeighbours);
            }
            for (const auto& [d, q] : close) {
                // What the two points' motions differ by, per coordinate.
                index.clear();
                difference.clear();
                for (u32 a = layout.columnStart[p]; a < layout.columnStart[p + 1]; ++a) {
                    index.push_back(layout.columnIndex[a]);
                    difference.push_back(layout.columns[a]);
                }
                for (u32 b = layout.columnStart[q]; b < layout.columnStart[q + 1]; ++b) {
                    const auto same = std::find(index.begin(), index.end(), layout.columnIndex[b]);
                    if (same != index.end()) {
                        difference[static_cast<std::size_t>(same - index.begin())] =
                            difference[static_cast<std::size_t>(same - index.begin())] - layout.columns[b];
                    } else {
                        index.push_back(layout.columnIndex[b]);
                        difference.push_back(layout.columns[b] * -1.0f);
                    }
                }
                // As a strain: over the distance, coincident points held a
                // quarter of a spacing apart.
                const f64 apart = std::max(d, 0.25f * std::max(spacing[p], 1e-12f));
                const f64 weight = hold * static_cast<f64>(points[p].area) / static_cast<f64>(close.size()) * size * size /
                                   (apart * apart);
                for (std::size_t a = 0; a < index.size(); ++a) {
                    for (std::size_t b = 0; b < index.size(); ++b) {
                        stiff[index[a] * n + index[b]] += weight * Dot(difference[a], difference[b]);
                    }
                }
            }
        }
    }

    std::vector<f64> factor = mass;
    if (!softdetail::Cholesky(factor, n)) {
        return SoftModes{};
    }
    // The one direction that changes the volume, stiffened.
    if (settings.keepVolume > 0.0f && volume.encloses) {
        std::vector<Vector3f> at(points.size());
        for (u32 p = 0; p < points.size(); ++p) {
            at[p] = points[p].at;
        }
        const std::vector<Vector3f> gradient = SoftVolumeGradient(volume, at);
        std::vector<f64> direction(n, 0.0);
        for (u32 p = 0; p < points.size(); ++p) {
            for (u32 a = layout.columnStart[p]; a < layout.columnStart[p + 1]; ++a) {
                direction[layout.columnIndex[a]] += Dot(gradient[p], layout.columns[a]);
            }
        }
        Stiffen(stiff, factor, n, direction, kVolume * static_cast<f64>(settings.keepVolume) * settings.keepVolume);
    }
    // *Keep in place*: the swing in which the whole body lags as one.
    if (settings.keepInPlace) {
        for (int axis = 0; axis < 3; ++axis) {
            std::vector<f64> direction(n, 0.0);
            for (u32 p = 0; p < points.size(); ++p) {
                for (u32 a = layout.columnStart[p]; a < layout.columnStart[p + 1]; ++a) {
                    const Vector3f& column = layout.columns[a];
                    direction[layout.columnIndex[a]] +=
                        static_cast<f64>(points[p].area) * (axis == 0 ? column.x : axis == 1 ? column.y : column.z);
                }
            }
            Stiffen(stiff, factor, n, direction, kPlace);
        }
    }

    // The generalised problem through the mass's factor: L^-1 K L^-T.
    for (std::size_t column = 0; column < n; ++column) {
        Forward(factor, n, stiff.data() + column, n);
    }
    for (std::size_t row = 0; row < n; ++row) {
        Forward(factor, n, stiff.data() + row * n, 1);
    }
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            const f64 mean = 0.5 * (stiff[i * n + j] + stiff[j * n + i]);
            stiff[i * n + j] = stiff[j * n + i] = mean;
        }
    }
    std::vector<f64> values;
    if (!softdetail::SymmetricEigen(stiff, n, values) || !(values.front() > 0.0)) {
        return SoftModes{};
    }
    // A Random Wobble gives each of the slowest its own phase: which shape is
    // which, and which way it goes, must be the body's and not the solver's.
    softdetail::SettleEigenBasis(stiff, n, values);
    modes.coordinates = layout.coordinates;
    modes.stiffness.resize(n);
    modes.shapes = std::move(stiff);
    for (std::size_t m = 0; m < n; ++m) {
        modes.stiffness[m] = values[m] / values.front();
        Backward(factor, n, modes.shapes.data() + m * n);
    }
    // How far a unit of each mode moves the point it moves most.
    modes.span.assign(n, 0.0);
    for (u32 p = 0; p < points.size(); ++p) {
        for (std::size_t m = 0; m < n; ++m) {
            const f64* shape = modes.shapes.data() + m * n;
            f64 x = 0.0, y = 0.0, z = 0.0;
            for (u32 a = layout.columnStart[p]; a < layout.columnStart[p + 1]; ++a) {
                const f64 q = shape[layout.columnIndex[a]];
                x += q * layout.columns[a].x;
                y += q * layout.columns[a].y;
                z += q * layout.columns[a].z;
            }
            modes.span[m] = std::max(modes.span[m], std::sqrt(x * x + y * y + z * z));
        }
    }
    return modes;
}

SoftJiggleResult RunSoftJiggle(const SoftRig& rig, const SoftModes& modes, const SoftJiggle& settings,
                               const SoftJiggleDrive& drive, SoftCeiling ceiling, std::span<const f64> start,
                               std::vector<f64>* end) {
    SoftJiggleResult out;
    const u32 lumps = rig.lumpCount();
    const u32 count = rig.pointCount();
    const u32 steps = drive.steps;
    const std::size_t frames = static_cast<std::size_t>(steps) + 1;
    out.swing.assign(frames * lumps, Transform{});
    if (end != nullptr) {
        end->clear();
    }
    if (rig.empty() || modes.empty() || steps == 0 || drive.driven.size() < frames * count ||
        drive.carriers.size() < frames * lumps || !(drive.dt > 0.0f)) {
        return out;
    }
    const Layout layout = LayoutOf(rig);
    const std::size_t n = layout.coordinates.size();
    const std::size_t m = modes.count();
    // The modes are a remake's or a slider's old ones: nothing to swing by.
    if (n != modes.coordinates.size() || modes.shapes.size() != m * n) {
        return out;
    }
    const std::vector<SoftPoint>& points = rig.points().points;
    const f64 size = std::max(modes.size, 1e-6f);
    const f64 dt = drive.dt;
    const f64 limit = std::clamp(settings.limit, 0.01f, 4.0f) * size;
    // The world's push, where one is given for every step.
    const bool blown = drive.pushed.size() >= frames * count;
    const auto level = [&](std::size_t i) { return i < drive.level.size() ? std::clamp(drive.level[i], 0.0f, 4.0f) : 1.0f; };

    // 1. The push, step by step, as each mode feels it: the points' own
    // acceleration turned into each lump's carrier's frame, and gravity apart,
    // which the ceiling leaves alone.
    std::vector<f64> inertial(frames * m, 0.0), heavy(frames * m, 0.0);
    std::vector<Mat3> back(frames * lumps);
    {
        std::vector<f64> pushed(n), weighed(n);
        const Vector3f gravity{0, 0, -kGravity * static_cast<f32>(size) * std::clamp(settings.weight, 0.0f, 4.0f)};
        const f32 perStep = 1.0f / static_cast<f32>(dt * dt);
        for (std::size_t i = 0; i < frames; ++i) {
            // A loop's last frame is its first; a clip played once is taken
            // to start and to stop at rest.
            const bool edge = !drive.loops && (i == 0 || i == steps);
            const std::size_t before = drive.loops ? (i + steps - 1) % steps : i - (edge ? 0 : 1);
            const std::size_t after = drive.loops ? (i + 1) % steps : i + (edge ? 0 : 1);
            const std::size_t here = drive.loops ? i % steps : i;
            std::fill(pushed.begin(), pushed.end(), 0.0);
            std::fill(weighed.begin(), weighed.end(), 0.0);
            for (u32 k = 0; k < lumps; ++k) {
                back[i * lumps + k] = BackOf(drive.carriers[i * lumps + k]);
                if (layout.firstOf[k] == kInvalidIndex) {
                    continue;
                }
                const Mat3& inverse = back[i * lumps + k];
                Vector3f move{0, 0, 0}, turn{0, 0, 0}, scale{0, 0, 0};
                Vector3f held{0, 0, 0};
                f32 share = 0.0f;
                for (const Entry& entry : layout.entries[k]) {
                    const Vector3f acceleration = (drive.driven[after * count + entry.point] -
                                                   drive.driven[here * count + entry.point] * 2.0f +
                                                   drive.driven[before * count + entry.point]) *
                                                  perStep;
                    const f32 weight = points[entry.point].area * entry.weight;
                    // In the carrier's frame, the point is thrown the other
                    // way; a wind or a blast pushes it the way it blows.
                    const Vector3f world = blown ? drive.pushed[here * count + entry.point] : Vector3f{0, 0, 0};
                    const Vector3f felt = Times(acceleration * -1.0f + world, inverse);
                    move = move + felt * weight;
                    turn = turn + cross(entry.arm, felt) * weight;
                    scale = scale + Vector3f{entry.arm.x * felt.x, entry.arm.y * felt.y, entry.arm.z * felt.z} * weight;
                    held = held + entry.arm * weight;
                    share += weight;
                }
                const Vector3f down = Times(gravity, inverse);
                const Vector3f sag[3] = {down * share, cross(held, down),
                                         Vector3f{held.x * down.x, held.y * down.y, held.z * down.z}};
                const Vector3f push[3] = {move, turn, scale};
                for (u32 c = layout.firstOf[k]; c < n && layout.coordinates[c].lump == k; ++c) {
                    const u8 kind = layout.coordinates[c].kind;
                    const Vector3f& from = push[kind / 3];
                    const Vector3f& pull = sag[kind / 3];
                    pushed[c] = kind % 3 == 0 ? from.x : kind % 3 == 1 ? from.y : from.z;
                    weighed[c] = kind % 3 == 0 ? pull.x : kind % 3 == 1 ? pull.y : pull.z;
                }
            }
            for (std::size_t mode = 0; mode < m; ++mode) {
                const f64* shape = modes.shapes.data() + mode * n;
                f64 a = 0.0, b = 0.0;
                for (std::size_t c = 0; c < n; ++c) {
                    a += shape[c] * pushed[c];
                    b += shape[c] * weighed[c];
                }
                inertial[i * m + mode] = a;
                heavy[i * m + mode] = b;
            }
        }
    }

    // 2. Each mode's rate and damping. Faster modes die sooner. One too quick
    // for keys to play is slowed to the quickest that are and damped fully,
    // its push scaled so that its resting answer stays its own: it follows the
    // push and does not ring. The cut is a rate, not a share of the step rate,
    // so what a mode does is the same at any.
    const f64 looseness = std::clamp(settings.looseness, 0.05f, 60.0f);
    const f64 damping = SoftDampingOf(settings.bounce);
    std::vector<f64> omega(m), zeta(m);
    for (std::size_t mode = 0; mode < m; ++mode) {
        const f64 ratio = std::sqrt(std::max(modes.stiffness[mode], 1.0));
        omega[mode] = kTau * looseness * ratio;
        zeta[mode] = std::min(damping * ratio, 4.0);
        const f64 quickest = kTau * kQuickest;
        if (omega[mode] > quickest) {
            const f64 slowed = quickest * quickest / (omega[mode] * omega[mode]);
            for (std::size_t i = 0; i < frames; ++i) {
                inertial[i * m + mode] *= slowed;
                heavy[i * m + mode] *= slowed;
            }
            omega[mode] = quickest;
            zeta[mode] = std::max(zeta[mode], 1.0);
        }
    }

    // 3. The solve, a mode at a time. Each is a damped oscillator, and over a
    // step in which the push changes along a straight line its state has a
    // closed form. A loop is run once from rest; the state it maps onto itself
    // is then two numbers, solved; and it is run again from there.
    const auto run = [&](std::size_t mode, const f64* push, f64 x0, f64 v0, std::vector<f64>& x, std::vector<f64>& v) {
        const f64 w = omega[mode], w2 = w * w;
        const std::array<f64, 4> carry = Carry(w, zeta[mode], dt);
        const auto pass = [&](f64 px, f64 pv) {
            x[0] = px;
            v[0] = pv;
            for (std::size_t i = 0; i + 1 < frames; ++i) {
                const f64 p0 = push[i], p1 = push[i + 1];
                const f64 slope = (p1 - p0) / dt;
                // The answer that follows the straight line, and what is left
                // of the state once it is taken off, carried.
                const f64 shift = 2.0 * zeta[mode] * slope / (w2 * w);
                const f64 rx = px - (p0 / w2 - shift), rv = pv - slope / w2;
                px = carry[0] * rx + carry[1] * rv + (p1 / w2 - shift);
                pv = carry[2] * rx + carry[3] * rv + slope / w2;
                x[i + 1] = px;
                v[i + 1] = pv;
            }
        };
        if (!drive.loops) {
            pass(x0, v0);
            return;
        }
        pass(0.0, 0.0);
        std::array<f64, 4> loop{1, 0, 0, 1};
        for (u32 i = 0; i < steps; ++i) {
            loop = {loop[0] * carry[0] + loop[1] * carry[2], loop[0] * carry[1] + loop[1] * carry[3],
                    loop[2] * carry[0] + loop[3] * carry[2], loop[2] * carry[1] + loop[3] * carry[3]};
        }
        // (I - loop) z = what a loop from rest ends with.
        const f64 a = 1.0 - loop[0], b = -loop[1], c = -loop[2], d = 1.0 - loop[3];
        const f64 det = a * d - b * c;
        if (std::abs(det) > 1e-300) {
            pass((d * x[steps] - b * v[steps]) / det, (-c * x[steps] + a * v[steps]) / det);
        }
    };

    // 4. Each mode's swing, in three parts.
    //  - Where gravity alone would rest it at each step: followed, not swung,
    //    and never more than half the *Limit*.
    //  - Its answer to the body's own motion, and to that rest moving under it:
    //    the part the ceiling limits (§8.3, D22). It is the push that is limited
    //    and never the state, so the swing stays a linear answer to a function
    //    of time and a loop still closes.
    //  - What is left of the state the clip began with.
    std::vector<f64> where(frames * m, 0.0);
    std::vector<f64> thrown(frames * m, 0.0);
    std::vector<f64> state(2 * m, 0.0), thrownEnd(2 * m, 0.0);
    const bool begun = !drive.loops && start.size() == 2 * m;
    {
        std::vector<f64> push(frames), x(frames), v(frames), need(frames), gain(frames), sag(frames), sagSpeed(frames);
        for (std::size_t mode = 0; mode < m; ++mode) {
            const f64 w = omega[mode], w2 = w * w;
            const f64 scale = modes.span[mode] / limit;
            for (std::size_t i = 0; i < frames; ++i) {
                sag[i] = heavy[i * m + mode] / w2;
                if (scale > 0.0) {
                    const f64 u = 2.0 * std::abs(sag[i]) * scale;
                    sag[i] *= u > 0.0 ? Stop(u) / u : 1.0;
                }
            }
            for (std::size_t i = 0; i < frames; ++i) {
                // A clip played once is taken to start and to stop with the
                // rest still, as with the body's own motion.
                const bool edge = !drive.loops && (i == 0 || i == steps);
                const std::size_t before = drive.loops ? (i % steps + steps - 1) % steps : i - (edge ? 0 : 1);
                const std::size_t after = drive.loops ? (i + 1) % steps : i + (edge ? 0 : 1);
                sagSpeed[i] = (sag[after] - sag[before]) / (2.0 * dt);
                const f64 quickens = (sag[after] - 2.0 * sag[i] + sag[before]) / (dt * dt);
                // Measured from a rest that moves, the mode is pushed by how
                // that rest quickens and by the drag of its speed.
                push[i] = inertial[i * m + mode] - (quickens + 2.0 * zeta[mode] * w * sagSpeed[i]);
            }
            run(mode, push.data(), 0.0, 0.0, x, v);
            for (std::size_t i = 0; i < frames; ++i) {
                out.over = std::max(out.over, static_cast<f32>(std::sqrt(x[i] * x[i] + v[i] * v[i] / w2) * scale));
            }
            if (ceiling == SoftCeiling::Swing && scale > 0.0) {
                // Where the swing would pass the *Limit*, the pushes that throw
                // it there are scaled down: each by what the swing is over by
                // at any later step, as far as that push is still felt there.
                const f64 fade = std::exp(-zeta[mode] * w * dt);
                const std::size_t felt = std::min<std::size_t>(
                    steps, static_cast<std::size_t>(std::ceil(4.0 / std::max(zeta[mode] * w * dt, 1e-9))));
                for (int round = 0; round < 2; ++round) {
                    f64 top = 0.0;
                    for (std::size_t i = 0; i < frames; ++i) {
                        // The swing's size there, whatever its phase.
                        const f64 swing = std::sqrt(x[i] * x[i] + v[i] * v[i] / w2) * scale;
                        need[i] = swing > 1.0 ? 1.0 / swing : 1.0;
                        top = std::max(top, swing);
                    }
                    if (!(top > 1.0)) {
                        break;
                    }
                    for (std::size_t i = 0; i < frames; ++i) {
                        f64 g = 1.0, share = 1.0;
                        for (std::size_t j = 0; j <= felt; ++j, share *= fade) {
                            if (!drive.loops && i + j > steps) {
                                break;
                            }
                            const std::size_t at = drive.loops ? (i + j) % steps : i + j;
                            g = std::min(g, 1.0 - share * (1.0 - need[at]));
                        }
                        gain[i] = g;
                    }
                    for (std::size_t i = 0; i < frames; ++i) {
                        push[i] *= gain[i];
                    }
                    run(mode, push.data(), 0.0, 0.0, x, v);
                }
                // What is still over is taken off as one gain: exact, since
                // the answer is linear in the push.
                f64 peak = 0.0;
                for (std::size_t i = 0; i < frames; ++i) {
                    peak = std::max(peak, std::abs(x[i]) * scale);
                }
                if (peak > 1.0) {
                    for (std::size_t i = 0; i < frames; ++i) {
                        x[i] /= peak;
                        v[i] /= peak;
                    }
                }
            }
            for (std::size_t i = 0; i < frames; ++i) {
                thrown[i * m + mode] = x[i];
                where[i * m + mode] = sag[i];
            }
            thrownEnd[2 * mode] = x[steps];
            thrownEnd[2 * mode + 1] = v[steps];
            state[2 * mode] = sag[steps];
            state[2 * mode + 1] = sagSpeed[steps];
            if (drive.loops) {
                out.seam = std::max(out.seam, static_cast<f32>(std::abs(x[steps] - x[0]) * modes.span[mode] / size));
            }
            if (begun) {
                std::fill(push.begin(), push.end(), 0.0);
                run(mode, push.data(), start[2 * mode] - sag[0], start[2 * mode + 1] - sagSpeed[0], x, v);
                for (std::size_t i = 0; i < frames; ++i) {
                    where[i * m + mode] += x[i];
                }
                state[2 * mode] += x[steps];
                state[2 * mode + 1] += v[steps];
            }
        }
    }
    // `Clip`: one gain for the clip, from what its worst mode is over by.
    f64 one = 1.0;
    if (ceiling == SoftCeiling::Clip) {
        f64 peak = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t mode = 0; mode < m; ++mode) {
                peak = std::max(peak, std::abs(thrown[i * m + mode]) * modes.span[mode] / limit);
            }
        }
        if (peak > 1.0) {
            one = 1.0 / peak;
            out.gain = static_cast<f32>(one);
        }
    }
    {
        f64 peak = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t mode = 0; mode < m; ++mode) {
                where[i * m + mode] += one * thrown[i * m + mode];
                peak = std::max(peak, std::abs(where[i * m + mode]) * modes.span[mode] / limit);
            }
        }
        out.peak = static_cast<f32>(peak);
        for (std::size_t s = 0; s < 2 * m; ++s) {
            state[s] += one * thrownEnd[s];
        }
        // By the level the clip ends at, as what comes out is: a span eased
        // out to nothing ends with the body where its animation has it.
        const f64 ends = level(steps);
        for (std::size_t mode = 0; mode < m && !drive.loops; ++mode) {
            const f64 x = thrownEnd[2 * mode], v = thrownEnd[2 * mode + 1] / omega[mode];
            out.atEnd =
                std::max(out.atEnd, static_cast<f32>(ends * one * std::sqrt(x * x + v * v) * modes.span[mode] / limit));
        }
    }
    if (end != nullptr) {
        *end = state;
    }

    // How long after the last push the body takes to lie still.
    {
        f64 largest = 0.0;
        std::vector<f64> pushAt(frames, 0.0), away(frames, 0.0);
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t mode = 0; mode < m; ++mode) {
                pushAt[i] = std::max(pushAt[i], std::abs(inertial[i * m + mode]));
                away[i] = std::max(away[i], std::abs(thrown[i * m + mode]) * one * modes.span[mode]);
            }
            largest = std::max(largest, pushAt[i]);
        }
        std::size_t last = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            if (pushAt[i] > 0.01 * largest) {
                last = i;
            }
        }
        for (std::size_t i = frames; i-- > last;) {
            if (away[i] > 0.01 * limit) {
                if (i + 1 < frames) {
                    out.settles = static_cast<f32>(static_cast<f64>(i + 1 - last) * dt);
                }
                break;
            }
            if (i == last) {
                out.settles = 0.0f;
            }
        }
    }

    // 5. What comes out: each lump's move, turn and scales from the modes, by
    // the level, with a smooth stop at the *Limit* that has no memory.
    std::vector<f64> coordinates(n);
    std::vector<Vector3f> moved(count);
    // Per point, its share on the lumps a floor can raise.
    std::vector<f32> lifts(count, 0.0f);
    for (u32 k = 0; k < lumps; ++k) {
        if (layout.firstOf[k] != kInvalidIndex && rig.moves(k)) {
            for (const Entry& entry : layout.entries[k]) {
                lifts[entry.point] += entry.weight;
            }
        }
    }
    // Where the swing as it stands at step @p i moves each point, model space.
    const auto displace = [&](std::size_t i) {
        std::fill(moved.begin(), moved.end(), Vector3f{0, 0, 0});
        for (u32 k = 0; k < lumps; ++k) {
            if (layout.firstOf[k] == kInvalidIndex) {
                continue;
            }
            const Vector3f& pivot = rig.body().lumps[k].rest;
            const Matrix44f departed = SoftDepartureMatrix(pivot, out.swing[i * lumps + k]);
            const Mat3 forward = LinearOf(drive.carriers[i * lumps + k]);
            for (const Entry& entry : layout.entries[k]) {
                const Vector3f local = transform_point(pivot + entry.arm, departed) - (pivot + entry.arm);
                moved[entry.point] = moved[entry.point] + Times(local, forward) * entry.weight;
            }
        }
    };
    const bool meets = drive.colliderCount != 0 && drive.colliders.size() >= frames * drive.colliderCount;
    for (std::size_t i = 0; i < frames; ++i) {
        std::fill(coordinates.begin(), coordinates.end(), 0.0);
        for (std::size_t mode = 0; mode < m; ++mode) {
            const f64 x = where[i * m + mode] * level(i);
            if (x == 0.0) {
                continue;
            }
            const f64* shape = modes.shapes.data() + mode * n;
            for (std::size_t c = 0; c < n; ++c) {
                coordinates[c] += x * shape[c];
            }
        }
        for (u32 k = 0; k < lumps; ++k) {
            if (layout.firstOf[k] == kInvalidIndex) {
                continue;
            }
            Vector3f part[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            for (u32 c = layout.firstOf[k]; c < n && layout.coordinates[c].lump == k; ++c) {
                const u8 kind = layout.coordinates[c].kind;
                const f32 value = static_cast<f32>(coordinates[c]);
                (kind % 3 == 0 ? part[kind / 3].x : kind % 3 == 1 ? part[kind / 3].y : part[kind / 3].z) = value;
            }
            // The farthest it moves a point of its own: its scale, then its turn
            // of the scaled arm, then its move. Scaled down, each is under its
            // share of this, so the stop holds on what is posed.
            const f32 reach = layout.reach[k];
            const f32 grown = std::max({std::abs(part[2].x), std::abs(part[2].y), std::abs(part[2].z)});
            const f32 travel = part[0].length() + reach * (std::exp(grown) * part[1].length() + std::expm1(grown));
            if (travel > 0.0f) {
                // Half as far inward as outward.
                const f32 inward = part[0].length() > 1e-12f
                                       ? std::max(0.0f, -part[0].dot(layout.outward[k]) / part[0].length())
                                       : 0.0f;
                const f64 allowed = limit * (1.0 - 0.5 * inward);
                const f64 u = travel / allowed;
                const f32 kept = static_cast<f32>(Stop(u) / u);
                for (Vector3f& each : part) {
                    each = each * kept;
                }
            }
            out.swing[i * lumps + k] = SwingOf(part);
        }
        // The floor: no point the swing moved is left under it that what
        // drives the body had over it. A lump is raised by what its worst point
        // needs of the lumps that can be; never by more than the *Limit*.
        if (std::isfinite(drive.floor) && settings.meetsFloor) {
            for (int pass = 0; pass < 2; ++pass) {
                displace(i);
                bool raised = false;
                for (u32 k = 0; k < lumps; ++k) {
                    if (layout.firstOf[k] == kInvalidIndex || !rig.moves(k)) {
                        continue;
                    }
                    f32 under = 0.0f;
                    for (const Entry& entry : layout.entries[k]) {
                        const f32 driven = drive.driven[(drive.loops ? i % steps : i) * count + entry.point].z;
                        const f32 low = std::min(drive.floor, driven) - (driven + moved[entry.point].z);
                        under = std::max(under, low / lifts[entry.point]);
                    }
                    under = std::min(under, static_cast<f32>(limit));
                    if (under > 0.0f) {
                        Transform& swing = out.swing[i * lumps + k];
                        swing.translation = swing.translation + Times(Vector3f{0, 0, under}, back[i * lumps + k]);
                        raised = true;
                    }
                }
                if (!raised) {
                    break;
                }
            }
        }
        // The colliders, the same way: no point the swing moved is left deeper
        // in one than what drives the body had it. A lump is moved out, the
        // way its worst point needs, by what that point needs. A point the
        // animation has outside leaves by the side it came in by, which is
        // the way out where it is driven: the nearest side of a small collider
        // may be its far one.
        if (meets) {
            const std::size_t here = drive.loops ? i % steps : i;
            const SoftCollider* shapes = drive.colliders.data() + here * drive.colliderCount;
            for (int pass = 0; pass < 3; ++pass) {
                displace(i);
                bool cleared = false;
                for (u32 k = 0; k < lumps; ++k) {
                    if (layout.firstOf[k] == kInvalidIndex || !rig.moves(k)) {
                        continue;
                    }
                    Vector3f way{0, 0, 0};
                    f32 most = 0.0f;
                    for (const Entry& entry : layout.entries[k]) {
                        const Vector3f& driven = drive.driven[here * count + entry.point];
                        const Vector3f now = driven + moved[entry.point];
                        for (u32 c = 0; c < drive.colliderCount; ++c) {
                            Vector3f outOf{0, 0, 0}, was{0, 0, 0};
                            const f32 depth = SoftColliderDepth(shapes[c], now, outOf);
                            if (!(depth > 0.0f)) {
                                continue;
                            }
                            const f32 before = SoftColliderDepth(shapes[c], driven, was);
                            const f32 need = (before > 0.0f ? depth - before : SoftColliderExit(shapes[c], now, was)) /
                                             lifts[entry.point];
                            if (need > most) {
                                most = need;
                                way = before > 0.0f ? outOf : was;
                            }
                        }
                    }
                    most = std::min(most, static_cast<f32>(limit));
                    if (most > 0.0f) {
                        Transform& swing = out.swing[i * lumps + k];
                        swing.translation = swing.translation + Times(way * most, back[i * lumps + k]);
                        cleared = true;
                    }
                }
                if (!cleared) {
                    break;
                }
            }
        }
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
