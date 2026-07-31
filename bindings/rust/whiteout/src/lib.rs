// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! Rust bindings for [WhiteoutLib](https://github.com/Sahmkow/WhiteoutLib) —
//! parsers and writers for Blizzard model and texture formats.
//!
//! # Layout contract
//!
//! Value types such as [`math::Vector3f`] are `#[repr(C)]` mirrors of their
//! C++ counterparts and cross the FFI boundary with no conversion. Their
//! sizes are pinned by `const` assertions at compile time, and
//! [`math::check_abi`] re-verifies them against the library actually linked:
//!
//! ```no_run
//! whiteout::math::check_abi().expect("native library layout mismatch");
//! ```
//!
//! # Errors
//!
//! The underlying C++ library does not throw, and signals absence with
//! `std::optional`. This binding follows that: operations that can simply
//! find nothing return [`Option`], and [`Result`] is reserved for the few
//! calls that produce a real diagnostic.
//!
//! # Zero-copy pixel access
//!
//! [`textures::Texture::data`] and `data_mut` borrow the C++ buffer
//! directly — nothing is copied in either direction. That is safe because
//! the slice borrows the texture, so the compiler rejects any use that
//! could dangle. This must not compile:
//!
//! ```compile_fail
//! use whiteout::textures::{PixelFormat, Texture};
//! let mut tex = Texture::create_2d(PixelFormat::RGBA8, 4, 4, 1).unwrap();
//! let pixels = tex.data_mut();
//! drop(tex);            // owner released while `pixels` is still alive
//! pixels[0] = 1;
//! ```
//!
//! Neither may a shared and a mutable view coexist:
//!
//! ```compile_fail
//! use whiteout::textures::{PixelFormat, Texture};
//! let mut tex = Texture::create_2d(PixelFormat::RGBA8, 4, 4, 1).unwrap();
//! let shared = tex.data();
//! let unique = tex.data_mut();   // second borrow, one of them mutable
//! let _ = (shared[0], unique[0]);
//! ```
//!
//! C# and C++ can only document these hazards; here they are compile
//! errors, which is what makes handing out the raw buffer reasonable.

#![deny(unsafe_op_in_unsafe_fn)]
#![warn(missing_debug_implementations)]

pub mod math;
pub mod mdx;
mod support;
pub mod textures;

pub use support::{BorrowedSlice, Bytes, Ref, RefMut};

use core::fmt;

/// Errors that can cross the binding boundary.
///
/// Deliberately small: the C++ library reports "not found" through
/// `std::optional`, which this binding surfaces as [`Option`] rather than
/// as an error.
#[non_exhaustive]
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    /// A `std::string*` diagnostic from the native side.
    Native(String),
    /// The linked library's layout disagrees with what this crate was
    /// generated against — see [`math::check_abi`].
    Layout {
        what: &'static str,
        expected: usize,
        actual: usize,
    },
    /// A cargo feature is enabled but the linked library was built without
    /// the matching `WHITEOUT_ENABLE_*`.
    FeatureDisabled(&'static str),
    /// The native library produced an enum discriminant this crate does not
    /// know. Indicates version skew rather than bad input.
    UnknownEnum { name: &'static str, value: i32 },
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Native(msg) => write!(f, "{msg}"),
            Error::Layout {
                what,
                expected,
                actual,
            } => write!(
                f,
                "native layout mismatch for {what}: this crate expects {expected}, \
                 the linked library reports {actual}"
            ),
            Error::FeatureDisabled(feat) => write!(
                f,
                "the `{feat}` feature is enabled but the linked whiteout_native \
                 was built without it"
            ),
            Error::UnknownEnum { name, value } => write!(
                f,
                "the native library returned {value} for {name}, which this \
                 crate does not know — the linked library is newer"
            ),
        }
    }
}

impl std::error::Error for Error {}
