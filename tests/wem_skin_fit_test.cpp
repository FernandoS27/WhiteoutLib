// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// A few bones fitted to dense motion (BONE_REDUCTION_RESEARCH.md §7.1).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

#include <whiteout/models/wem/skinning/fit.h>
#include <whiteout/models/wem/skinning/quantize.h>

using namespace whiteout;
namespace skinning = whiteout::models::wem::skinning;
namespace geom = whiteout::models::wem::geom;

namespace {

/// A rigid motion `v · M`: a turn of @p angle about z, then @p t.
Matrix44f Turn(f32 angle, const Vector3f& t) {
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = std::cos(angle), m.data[0][1] = std::sin(angle);
    m.data[1][0] = -std::sin(angle), m.data[1][1] = std::cos(angle);
    m.data[3][0] = t.x, m.data[3][1] = t.y, m.data[3][2] = t.z;
    return m;
}

/// A 12×12 sheet in x, y, at z = 0, 10 units across.
std::vector<Vector3f> Sheet() {
    std::vector<Vector3f> out;
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            out.push_back(Vector3f{x * 10.0f / 11.0f, y * 10.0f / 11.0f, 0.0f});
        }
    }
    return out;
}

/// The sheet bent as a wave: no few rigid bones carry it exactly.
std::vector<Vector3f> Wave(const std::vector<Vector3f>& rest, u32 frames) {
    std::vector<Vector3f> out;
    for (u32 f = 0; f < frames; ++f) {
        const f32 phase = static_cast<f32>(f) * 0.2f;
        for (const Vector3f& p : rest) {
            out.push_back(Vector3f{p.x, p.y, 2.0f * std::sin(0.6f * p.x + phase) * (p.y / 10.0f)});
        }
    }
    return out;
}

/// The sheet's diagonal.
const f32 kSize = std::sqrt(200.0f);

/// The RMS distance of @p weights on @p fit's bones from @p moved.
f32 RmsOf(const skinning::FitResult& fit, const std::vector<std::vector<geom::Influence>>& weights,
          const std::vector<Vector3f>& rest, const std::vector<Vector3f>& moved, u32 frames) {
    f64 sum = 0.0;
    for (u32 f = 0; f < frames; ++f) {
        for (std::size_t i = 0; i < rest.size(); ++i) {
            Vector3f at{0, 0, 0};
            for (const geom::Influence& influence : weights[i]) {
                at = at + transform_point(rest[i], fit.skinning[f * fit.bones + influence.bone]) * influence.weight;
            }
            sum += (at - moved[f * rest.size() + i]).length() * (at - moved[f * rest.size() + i]).length();
        }
    }
    return static_cast<f32>(std::sqrt(sum / (static_cast<f64>(frames) * rest.size())));
}

} // namespace

TEST_CASE("skin fit Horn's closed form recovers a turn", "[wem][skinning][fit]") {
    // Three points about the origin turned 0.5 rad about z and moved.
    const Matrix44f m = Turn(0.5f, Vector3f{1, 2, 3});
    const Vector3f p[3] = {{1, 0, 0}, {0, 2, 0}, {0, 0, 3}};
    f64 from[3] = {}, to[3] = {}, cross[3][3] = {};
    Vector3f q[3];
    for (int i = 0; i < 3; ++i) {
        q[i] = transform_point(p[i], m);
        from[0] += p[i].x / 3, from[1] += p[i].y / 3, from[2] += p[i].z / 3;
        to[0] += q[i].x / 3, to[1] += q[i].y / 3, to[2] += q[i].z / 3;
    }
    for (int i = 0; i < 3; ++i) {
        const f64 a[3] = {p[i].x - from[0], p[i].y - from[1], p[i].z - from[2]};
        const f64 b[3] = {q[i].x - to[0], q[i].y - to[1], q[i].z - to[2]};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                cross[r][c] += a[r] * b[c];
            }
        }
    }
    Quaternion rotation;
    Vector3f translation;
    REQUIRE(skinning::SolveRigid(from, to, cross, rotation, translation));
    for (int i = 0; i < 3; ++i) {
        const Vector3f back = rotation.rotate_vector(p[i]) + translation;
        CHECK((back - q[i]).length() < 1e-4f);
    }
}

TEST_CASE("skin fit two rigid halves come back as two bones", "[wem][skinning][fit]") {
    const std::vector<Vector3f> rest = Sheet();
    const u32 frames = 40;
    std::vector<Vector3f> moved;
    for (u32 f = 0; f < frames; ++f) {
        // The left half turns one way, the right half the other, each about
        // its own centre.
        const f32 a = 0.03f * static_cast<f32>(f);
        for (const Vector3f& p : rest) {
            const bool left = p.x < 5.0f;
            const Vector3f centre{left ? 2.5f : 7.5f, 5.0f, 0.0f};
            const Matrix44f m = Turn(left ? a : -a, centre + Vector3f{0, 0, left ? 0.1f * f : -0.1f * f});
            moved.push_back(transform_point(p - centre, m));
        }
    }
    skinning::FitOptions options;
    options.maxBones = 2;
    const skinning::FitResult fit =
        skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
    REQUIRE(fit.bones == 2);
    CHECK(fit.rms < 1e-3f * kSize);
    CHECK(fit.worst < 1e-2f * kSize);
    REQUIRE(fit.skinning.size() == frames * 2u);
    // Each point leans on one bone, and the halves on different ones.
    const u32 leftBone = fit.weights.front().front().bone;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        REQUIRE_FALSE(fit.weights[i].empty());
        CHECK(fit.weights[i].front().weight > 0.99f);
        CHECK((fit.weights[i].front().bone == leftBone) == (rest[i].x < 5.0f));
    }
}

TEST_CASE("skin fit more bones carry a wave closer", "[wem][skinning][fit]") {
    const std::vector<Vector3f> rest = Sheet();
    const u32 frames = 30;
    const std::vector<Vector3f> moved = Wave(rest, frames);
    f32 last = 1e30f;
    for (const u32 bones : {2u, 6u, 16u}) {
        skinning::FitOptions options;
        options.maxBones = bones;
        const skinning::FitResult fit =
            skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
        CHECK(fit.bones <= bones);
        CHECK(fit.rms < last);
        last = fit.rms;
        for (const auto& influences : fit.weights) {
            CHECK(influences.size() <= 4);
            f32 sum = 0.0f;
            for (const auto& influence : influences) {
                CHECK(influence.weight >= 0.0f);
                sum += influence.weight;
            }
            CHECK(std::fabs(sum - 1.0f) < 1e-4f);
        }
    }
    // Sixteen bones on a 2-unit wave: within a tenth of a unit.
    CHECK(last < 0.1f);
}

TEST_CASE("skin fit a point a given bone carries leans on it", "[wem][skinning][fit]") {
    const std::vector<Vector3f> rest = Sheet();
    const u32 frames = 30;
    std::vector<Vector3f> moved = Wave(rest, frames);
    // The bottom row rides a fixed bone that slides along x.
    std::vector<Matrix44f> fixedMotion;
    for (u32 f = 0; f < frames; ++f) {
        fixedMotion.push_back(Turn(0.0f, Vector3f{0.2f * static_cast<f32>(f), 0, 0}));
        for (std::size_t i = 0; i < 12; ++i) {
            moved[f * rest.size() + i] = transform_point(rest[i], fixedMotion.back());
        }
    }
    std::vector<std::vector<u32>> allowed(rest.size());
    for (std::size_t i = 0; i < 24; ++i) {
        allowed[i] = {0};
    }
    skinning::FitOptions options;
    options.maxBones = 6;
    const skinning::FitResult fit = skinning::FitBones(
        {rest, moved, frames}, skinning::FitFixedBones{fixedMotion, 1, allowed}, options);
    for (std::size_t i = 0; i < 12; ++i) {
        REQUIRE_FALSE(fit.weights[i].empty());
        CHECK(fit.weights[i].front().bone == fit.bones);
        CHECK(fit.weights[i].front().weight > 0.9f);
    }
    for (std::size_t i = 24; i < rest.size(); ++i) {
        for (const auto& influence : fit.weights[i]) {
            CHECK(influence.bone < fit.bones);
        }
    }
}

TEST_CASE("skin fit gives the same bones on any number of threads", "[wem][skinning][fit]") {
    const std::vector<Vector3f> rest = Sheet();
    const u32 frames = 20;
    const std::vector<Vector3f> moved = Wave(rest, frames);
    skinning::FitOptions options;
    options.maxBones = 8;
    options.threads = 1;
    const skinning::FitResult one = skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
    options.threads = 8;
    const skinning::FitResult many = skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
    REQUIRE(one.bones == many.bones);
    REQUIRE(one.skinning.size() == many.skinning.size());
    CHECK(std::memcmp(one.skinning.data(), many.skinning.data(), one.skinning.size() * sizeof(Matrix44f)) == 0);
    REQUIRE(one.weights.size() == many.weights.size());
    for (std::size_t i = 0; i < one.weights.size(); ++i) {
        REQUIRE(one.weights[i].size() == many.weights[i].size());
        for (std::size_t k = 0; k < one.weights[i].size(); ++k) {
            CHECK(one.weights[i][k].bone == many.weights[i][k].bone);
            CHECK(one.weights[i][k].weight == many.weights[i][k].weight);
        }
    }
    CHECK(one.rms == many.rms);
}

TEST_CASE("skin fit rounds the weights as the file holds them and refits the bones", "[wem][skinning][fit]") {
    const std::vector<Vector3f> rest = Sheet();
    const u32 frames = 30;
    const std::vector<Vector3f> moved = Wave(rest, frames);
    skinning::FitOptions options;
    options.maxBones = 6;
    const skinning::FitResult exact = skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);

    // A classic group: the heaviest few at one share each, as the quantizer
    // would snap the exact weights; the bones then take up part of the loss.
    options.weights = skinning::FitWeights::EqualShares;
    const skinning::FitResult equal = skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
    REQUIRE(equal.bones == exact.bones);
    std::vector<std::vector<geom::Influence>> snapped(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::vector<std::vector<geom::Influence>> one(1, exact.weights[i]);
        skinning::ClassicLimits limits;
        limits.maxBones = 4;
        const skinning::ClassicSkin classic = skinning::QuantizeClassic(one, {}, limits);
        for (const u32 bone : classic.groups[classic.groupOf.front()]) {
            snapped[i].push_back(geom::Influence{bone, 1.0f / classic.groups[classic.groupOf.front()].size()});
        }
        REQUIRE(equal.weights[i].size() == snapped[i].size());
        for (const geom::Influence& influence : equal.weights[i]) {
            CHECK(influence.weight == equal.weights[i].front().weight);
        }
    }
    const f32 roundedOnly = RmsOf(exact, snapped, rest, moved, frames);
    CHECK(roundedOnly > exact.rms);
    CHECK(equal.rms < roundedOnly);
    CHECK(std::fabs(RmsOf(equal, equal.weights, rest, moved, frames) - equal.rms) < 1e-4f);

    // Bytes summing to 255.
    options.weights = skinning::FitWeights::Bytes;
    const skinning::FitResult bytes = skinning::FitBones({rest, moved, frames}, skinning::FitFixedBones{}, options);
    for (const auto& influences : bytes.weights) {
        u32 total = 0;
        for (const geom::Influence& influence : influences) {
            const f32 byte = influence.weight * 255.0f;
            CHECK(std::fabs(byte - std::round(byte)) < 1e-3f);
            total += static_cast<u32>(std::lround(byte));
        }
        CHECK(total == 255);
    }
    CHECK(bytes.rms < exact.rms + 0.01f);
}
