// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/fracture/seeds.h>

#include <algorithm>
#include <cmath>

#include <whiteout/models/wem/geometry/bvh.h>

#include "parallel.h"

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

namespace {

f64 Dot(const Vector3d& a, const Vector3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3d GrainOf(Grain grain, f64 stretch) {
    const f64 shrink = stretch > 0.0 ? 1.0 / stretch : 1.0;
    switch (grain) {
    case Grain::X:
        return Vector3d(shrink, 1, 1);
    case Grain::Y:
        return Vector3d(1, shrink, 1);
    case Grain::Z:
        return Vector3d(1, 1, shrink);
    default:
        return Vector3d(1, 1, 1);
    }
}

f64 GrainDistance(const Vector3d& a, const Vector3d& b, const Vector3d& grain) {
    const Vector3d r((a.x - b.x) * grain.x, (a.y - b.y) * grain.y, (a.z - b.z) * grain.z);
    return Dot(r, r);
}

} // namespace

u64 FractureHash(u32 seed, u32 target, u64 index) {
    u64 z = ((static_cast<u64>(seed) << 32) | target) ^ (index * 0xD1B54A32D192ED03ull);
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

f64 HashUnit(u64 hash) {
    return static_cast<f64>(hash >> 11) * (1.0 / 9007199254740992.0);
}

std::vector<Vector3d> FractureSeeds(std::span<const SeedTarget> targets,
                                    const SeedOptions& options) {
    std::vector<Vector3d> seeds;
    const Vector3d grain = GrainOf(options.grain, options.stretch);
    for (u32 ti = 0; ti < targets.size(); ++ti) {
        const SeedTarget& target = targets[ti];
        if (target.pieces < 2 || target.winding == nullptr) {
            continue;
        }
        const Extent& box = target.winding->bounds;
        if (!box.valid()) {
            continue;
        }
        const Vector3d low(box.minimum.x, box.minimum.y, box.minimum.z);
        const Vector3d size(box.maximum.x - low.x, box.maximum.y - low.y, box.maximum.z - low.z);
        const f64 band = 0.01 * std::sqrt(Dot(size, size));
        const u32 wanted = 8 * target.pieces;
        const u32 batch = std::max<u32>(256, wanted);
        const u64 maxDraws = static_cast<u64>(wanted) * 64;

        std::vector<Vector3d> candidates;
        std::vector<Vector3d> drawn(batch);
        std::vector<u8> kept(batch);
        for (u64 first = 0; first < maxDraws && candidates.size() < wanted; first += batch) {
            Parallel(batch, options.threads, [&](std::size_t i) {
                const u64 index = 4 * (first + i);
                const Vector3d p(low.x + HashUnit(FractureHash(options.seed, ti, index)) * size.x,
                                 low.y + HashUnit(FractureHash(options.seed, ti, index + 1)) * size.y,
                                 low.z + HashUnit(FractureHash(options.seed, ti, index + 2)) * size.z);
                drawn[i] = p;
                kept[i] = 0;
                if (options.blastRadius > 0.0 && options.nearBlast > 0.0) {
                    const Vector3d r = p - options.blastCentre;
                    const f64 near = std::max(0.0, 1.0 - std::sqrt(Dot(r, r)) / options.blastRadius);
                    const f64 chance = (1.0 - options.nearBlast) + options.nearBlast * near * near;
                    if (HashUnit(FractureHash(options.seed, ti, index + 3)) >= chance) {
                        return;
                    }
                }
                if (target.surface != nullptr && !target.surface->empty()) {
                    const PointHit hit = target.surface->closestPoint(
                        Vector3f(static_cast<f32>(p.x), static_cast<f32>(p.y),
                                 static_cast<f32>(p.z)),
                        static_cast<f32>(band));
                    if (hit.hit()) {
                        kept[i] = 1;
                        return;
                    }
                }
                if (WindingNumber(*target.winding, p) >= 0.5) {
                    kept[i] = 1;
                }
            });
            for (u32 i = 0; i < batch && candidates.size() < wanted; ++i) {
                if (kept[i] != 0) {
                    candidates.push_back(drawn[i]);
                }
            }
        }

        const u32 pieces = std::min<u32>(target.pieces, static_cast<u32>(candidates.size()));
        std::vector<Vector3d> mine(candidates.begin(), candidates.begin() + pieces);
        const u32 rounds = static_cast<u32>(std::lround(4.0 * std::clamp(options.even, 0.0, 1.0)));
        std::vector<Vector3d> sum(pieces);
        std::vector<u32> members(pieces);
        for (u32 round = 0; round < rounds && pieces > 0; ++round) {
            std::fill(sum.begin(), sum.end(), Vector3d(0, 0, 0));
            std::fill(members.begin(), members.end(), 0u);
            for (const Vector3d& c : candidates) {
                u32 best = 0;
                f64 bestDistance = GrainDistance(c, mine[0], grain);
                for (u32 s = 1; s < pieces; ++s) {
                    const f64 distance = GrainDistance(c, mine[s], grain);
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        best = s;
                    }
                }
                sum[best] += c;
                ++members[best];
            }
            for (u32 s = 0; s < pieces; ++s) {
                if (members[s] > 0) {
                    const f64 inverse = 1.0 / static_cast<f64>(members[s]);
                    mine[s] = Vector3d(sum[s].x * inverse, sum[s].y * inverse, sum[s].z * inverse);
                }
            }
        }
        seeds.insert(seeds.end(), mine.begin(), mine.end());
    }
    return seeds;
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
