// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file matrix_inverse_test.cpp
 * @brief `Matrix44f::inverse` against the two things it used to get wrong.
 *
 * The affine fast path transposed the 3x3, which inverts a rotation and nothing
 * else, and it was gated on a test that read `data[3]` — the row this layout
 * keeps the *translation* in. So the fast path fired exactly when a matrix had
 * no translation, and then returned a transpose: `inverse(scaling(2,2,2))` came
 * back as scale 2. Every case below multiplies the result by the original and
 * asks for the identity, which is the only property that matters and the one a
 * transpose fails.
 */

#include <catch2/catch_all.hpp>

#include <whiteout/vector_types.h>

#include <cmath>

using namespace whiteout;

namespace {

f32 OffIdentity(const Matrix44f& m) {
    f32 worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            worst = std::fmax(worst, std::fabs(m.data[r][c] - (r == c ? 1.0f : 0.0f)));
        }
    }
    return worst;
}

/// The round trip, both ways: an inverse that only happens to work on one side
/// is not one.
void CheckInverts(const Matrix44f& m, f32 tolerance = 1e-4f) {
    const Matrix44f inv = Matrix44f::inverse(m);
    CHECK(OffIdentity(inv * m) < tolerance);
    CHECK(OffIdentity(m * inv) < tolerance);
}

Matrix44f WithTranslation(Matrix44f m, const Vector3f& t) {
    m.data[3][0] = t.x;
    m.data[3][1] = t.y;
    m.data[3][2] = t.z;
    return m;
}

} // namespace

TEST_CASE("matrix inverse handles rigid transforms", "[math][matrix]") {
    CheckInverts(Matrix44f::identity());
    CheckInverts(Matrix44f::rotation_z(0.7f));
    CheckInverts(Matrix44f::translation({3, -4, 5}));
    CheckInverts(WithTranslation(Matrix44f::rotation_x(-1.1f), {3, -4, 5}));
}

TEST_CASE("matrix inverse handles scale without a translation", "[math][matrix]") {
    // The regression: no translation is what used to select the transpose path.
    CheckInverts(Matrix44f::scaling({2, 2, 2}));
    CheckInverts(Matrix44f::scaling({2, 3, 4}));
    CheckInverts(Matrix44f::scaling({-1, 1, 1}));

    const Matrix44f scaled = Matrix44f::scaling({2, 2, 2});
    const Matrix44f inv = Matrix44f::inverse(scaled);
    // Stated outright, because "off identity by 3" is easy to read past: the
    // transpose returned the scale unchanged instead of its reciprocal.
    CHECK(inv.data[0][0] == Catch::Approx(0.5f).margin(1e-6f));
    CHECK(inv.data[1][1] == Catch::Approx(0.5f).margin(1e-6f));
    CHECK(inv.data[2][2] == Catch::Approx(0.5f).margin(1e-6f));
}

TEST_CASE("matrix inverse handles shear", "[math][matrix]") {
    Matrix44f shear = Matrix44f::identity();
    shear.data[0][1] = 0.4f;
    CheckInverts(shear);
    CheckInverts(WithTranslation(shear, {1, 2, 3}));

    // The shape an `.m3` IREF takes: shear on a rotated, non-uniformly scaled
    // basis with a translation.
    Matrix44f iref = Matrix44f::scaling({1.5f, 0.5f, 2.0f}) * Matrix44f::rotation_y(0.9f);
    iref.data[2][0] += 0.3f;
    CheckInverts(WithTranslation(iref, {-7, 11, 0.25f}));
}

TEST_CASE("matrix inverse composes an inverse bind pose", "[math][matrix]") {
    // The use this was found through: `IREF * local * inverse(parent IREF)`
    // composed down a chain has to reproduce `IREF * world`.
    const Matrix44f parentIref = WithTranslation(
        Matrix44f::scaling({2, 3, 1}) * Matrix44f::rotation_z(0.4f), {1, 0, -2});
    const Matrix44f local = WithTranslation(Matrix44f::rotation_x(0.6f), {0, 4, 0});
    const Matrix44f childIref =
        WithTranslation(Matrix44f::scaling({1, 0.5f, 0.5f}) * Matrix44f::rotation_y(-0.2f), {3, 3, 3});

    const Matrix44f parentWorld = Matrix44f::translation({5, 5, 5});
    const Matrix44f childWorld = local * parentWorld;

    const Matrix44f node = childIref * local * Matrix44f::inverse(parentIref);
    const Matrix44f chained = node * (parentIref * parentWorld);
    const Matrix44f wanted = childIref * childWorld;

    f32 worst = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            worst = std::fmax(worst, std::fabs(chained.data[r][c] - wanted.data[r][c]));
        }
    }
    CHECK(worst < 1e-4f);
}
