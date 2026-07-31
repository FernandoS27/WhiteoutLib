// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Two things are under test here:
//
//   1. The layout contract — that the by-value ABI actually lines up with
//      the C++ types in the library we linked against.
//   2. Parity — that every operation implemented natively in Rust agrees
//      with the C++ implementation it replaced. Without this, the native
//      fast paths are free to silently drift.

use whiteout::math::{self, ffi, Matrix44f, Quaternion, Vector2f, Vector3f, Vector4f};

const EPS: f32 = 1e-5;

fn close(a: f32, b: f32) -> bool {
    (a - b).abs() <= EPS * a.abs().max(b.abs()).max(1.0)
}

fn v3_close(a: Vector3f, b: Vector3f) -> bool {
    close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z)
}

fn q_close(a: Quaternion, b: Quaternion) -> bool {
    close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z) && close(a.w, b.w)
}

// ── Layout ────────────────────────────────────────────────────────────────

#[test]
fn abi_layout_matches_linked_library() {
    math::check_abi().expect("linked whiteout_native disagrees with this crate's layouts");
}

#[test]
fn value_types_have_no_padding() {
    assert_eq!(core::mem::size_of::<Vector2f>(), 8);
    assert_eq!(core::mem::size_of::<Vector3f>(), 12);
    assert_eq!(core::mem::size_of::<Vector4f>(), 16);
    assert_eq!(core::mem::size_of::<Quaternion>(), 16);
    assert_eq!(core::mem::size_of::<Matrix44f>(), 64);
}

// ── Parity: native Rust vs the C++ implementation ─────────────────────────

#[test]
fn native_vector3_arithmetic_matches_cpp() {
    let a = Vector3f::new(1.5, -2.25, 3.0);
    let b = Vector3f::new(0.5, 4.0, -1.25);

    unsafe {
        assert_eq!(a + b, ffi::whiteout_v_Vector3f_add(a, b));
        assert_eq!(a - b, ffi::whiteout_v_Vector3f_sub(a, b));
        assert_eq!(a * b, ffi::whiteout_v_Vector3f_mul(a, b));
        assert_eq!(a / b, ffi::whiteout_v_Vector3f_div(a, b));
        assert_eq!(-a, ffi::whiteout_v_Vector3f_negate(a));
        assert_eq!(a * 2.5, ffi::whiteout_v_Vector3f_mul_scalar(a, 2.5));
        assert_eq!(a / 2.5, ffi::whiteout_v_Vector3f_div_scalar(a, 2.5));
        assert!(close(a.dot(b), ffi::whiteout_v_Vector3f_dot(a, b)));
        assert!(close(a.length(), ffi::whiteout_v_Vector3f_length(a)));
        assert!(close(
            a.length_squared(),
            ffi::whiteout_v_Vector3f_length_squared(a)
        ));
    }
}

#[test]
fn native_vector2_and_4_arithmetic_match_cpp() {
    let a2 = Vector2f::new(3.0, -1.5);
    let b2 = Vector2f::new(-0.5, 2.0);
    let a4 = Vector4f::new(1.0, 2.0, 3.0, 4.0);
    let b4 = Vector4f::new(-1.0, 0.5, 2.0, -3.0);

    unsafe {
        assert_eq!(a2 + b2, ffi::whiteout_v_Vector2f_add(a2, b2));
        assert_eq!(a2 * b2, ffi::whiteout_v_Vector2f_mul(a2, b2));
        assert!(close(a2.dot(b2), ffi::whiteout_v_Vector2f_dot(a2, b2)));

        assert_eq!(a4 + b4, ffi::whiteout_v_Vector4f_add(a4, b4));
        assert_eq!(a4 * b4, ffi::whiteout_v_Vector4f_mul(a4, b4));
        assert!(close(a4.length(), ffi::whiteout_v_Vector4f_length(a4)));
    }
}

#[test]
fn quaternion_addition_is_native_but_multiplication_is_not() {
    let a = Quaternion::new(0.1, 0.2, 0.3, 0.9);
    let b = Quaternion::new(-0.3, 0.4, 0.1, 0.85);

    // Component-wise add is native.
    unsafe {
        assert_eq!(a + b, ffi::whiteout_v_Quaternion_add(a, b));
    }

    // `*` is the Hamilton product — it goes through C++, and it must NOT
    // equal the component-wise product. This is the regression test for the
    // one place where an "obviously component-wise" native implementation
    // would have been wrong.
    let hamilton = a * b;
    let componentwise = Quaternion::new(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w);
    assert!(!q_close(hamilton, componentwise));

    // Sanity-check the Hamilton product against the closed form.
    let expected = Quaternion::new(
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    );
    assert!(q_close(hamilton, expected), "{hamilton:?} vs {expected:?}");
}

// ── Behaviour that lives in C++ ───────────────────────────────────────────

#[test]
fn normalized_produces_unit_length() {
    let v = Vector3f::new(3.0, 4.0, 12.0);
    assert!(close(v.length(), 13.0));
    assert!(close(v.normalized().length(), 1.0));
}

#[test]
fn mutating_normalize_writes_back_through_the_pointer() {
    // `normalize` takes `&mut self` and crosses the ABI by pointer; this is
    // the test that the write-back actually lands in the caller's value.
    let mut v = Vector3f::new(0.0, 5.0, 0.0);
    v.normalize();
    assert!(v3_close(v, Vector3f::new(0.0, 1.0, 0.0)), "{v:?}");
}

#[test]
fn quaternion_round_trips_through_axis_angle() {
    let axis = Vector3f::new(0.0, 1.0, 0.0);
    let q = Quaternion::from_axis_angle(axis, core::f32::consts::FRAC_PI_2);
    let rotated = q.rotate_vector(Vector3f::new(1.0, 0.0, 0.0));
    // 90° about +Y takes +X to -Z.
    assert!(
        v3_close(rotated, Vector3f::new(0.0, 0.0, -1.0)),
        "{rotated:?}"
    );
}

#[test]
fn slerp_endpoints_are_the_inputs() {
    let a = Quaternion::identity();
    let b = Quaternion::from_axis_angle(Vector3f::new(0.0, 0.0, 1.0), 1.0);
    assert!(q_close(Quaternion::slerp(a, b, 0.0), a));
    assert!(q_close(Quaternion::slerp(a, b, 1.0), b));
}

#[test]
fn matrix_inverse_round_trips() {
    let m = Matrix44f::compose(
        Vector3f::new(1.0, 2.0, 3.0),
        Quaternion::from_axis_angle(Vector3f::new(0.0, 1.0, 0.0), 0.7),
        Vector3f::new(2.0, 2.0, 2.0),
    );
    let identity = m * Matrix44f::inverse(m);
    for r in 0..4 {
        for c in 0..4 {
            let want = if r == c { 1.0 } else { 0.0 };
            assert!(
                close(identity[(r, c)], want),
                "[{r}][{c}] = {}",
                identity[(r, c)]
            );
        }
    }
}

#[test]
fn matrix_indexing_is_row_major_and_native() {
    let mut m = Matrix44f::identity();
    m[(1, 3)] = 7.5;
    assert_eq!(m[(1, 3)], 7.5);
    assert_eq!(m.data[1][3], 7.5);
    // Row-major: element [1][3] must not alias [3][1].
    assert_eq!(m[(3, 1)], 0.0);
}

#[test]
fn translation_matrix_moves_a_point() {
    let m = Matrix44f::translation(Vector3f::new(10.0, 0.0, -5.0));
    let p = math::transform_point(Vector3f::new(1.0, 1.0, 1.0), m);
    assert!(v3_close(p, Vector3f::new(11.0, 1.0, -4.0)), "{p:?}");
}

// ── Ergonomics ────────────────────────────────────────────────────────────

#[test]
fn values_are_copy_and_compose_without_allocation() {
    // The point of the by-value ABI: this whole expression allocates
    // nothing and needs no cleanup, and `a` is still usable afterwards.
    let a = Vector3f::new(1.0, 0.0, 0.0);
    let b = Vector3f::new(0.0, 2.0, 0.0);
    let c = (a + b) * 2.0 - a;
    assert_eq!(c, Vector3f::new(1.0, 4.0, 0.0));
    assert_eq!(a, Vector3f::new(1.0, 0.0, 0.0));

    let arr = [a, b, c];
    assert_eq!(arr.iter().copied().reduce(|x, y| x + y).unwrap().y, 6.0);
}

#[test]
fn cross_product_follows_right_hand_rule() {
    let x = Vector3f::new(1.0, 0.0, 0.0);
    let y = Vector3f::new(0.0, 1.0, 0.0);
    assert!(v3_close(math::cross(x, y), Vector3f::new(0.0, 0.0, 1.0)));
}
