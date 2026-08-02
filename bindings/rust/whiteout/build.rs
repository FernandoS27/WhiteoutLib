// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Locate or build the native library, in this order:
//
//   1. WHITEOUT_LIB_DIR      — an explicit prebuilt library. Always wins,
//                              so a packager can override anything below.
//   2. `vendored` feature     — build the bundled C++ with CMake. On by
//                              default, because `whiteout_native` is not a
//                              library anyone has installed system-wide.
//   3. pkg-config            — a system install, for distro packagers.
//
// docs.rs has no C++ toolchain, so the whole thing is skipped there; the
// crate still type-checks and documents.
//
// This crate declares `links = "whiteout_native"`, which makes it the only
// crate in a build graph allowed to provide the native library — and lets
// the keys below reach dependent build scripts, which read them as
// environment variables. They are a compatibility surface: adding is fine,
// renaming or removing breaks dependents.
//
//   cargo:lib_dir=<path>   DEP_WHITEOUT_NATIVE_LIB_DIR
//                          directory holding whiteout_native_static
//   cargo:include=<path>   DEP_WHITEOUT_NATIVE_INCLUDE
//                          include root for the C++ headers (<whiteout/…>).
//                          Omitted when a prebuilt library was given with no
//                          headers alongside it and no WHITEOUT_INCLUDE_DIR.
//   cargo:has_casc=0|1     DEP_WHITEOUT_NATIVE_HAS_CASC
//   cargo:has_mpq=0|1      DEP_WHITEOUT_NATIVE_HAS_MPQ
//                          the configuration this library was actually built
//                          with, so a dependent can match it rather than
//                          guess from its own features.
//
// Emitted on every path that yields a usable library. A dependent that only
// calls into Rust needs none of it — linking this crate's rlib already
// pulls the native library in. It is for a dependent with its own C++ that
// must compile against the same headers and link the same archive.

use std::env;
use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-env-changed=WHITEOUT_LIB_DIR");
    println!("cargo:rerun-if-env-changed=WHITEOUT_INCLUDE_DIR");
    println!("cargo:rerun-if-env-changed=WHITEOUT_STATIC");

    // docs.rs builds documentation only — nothing to link against.
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    if let Some(dir) = env::var_os("WHITEOUT_LIB_DIR") {
        link_prebuilt(PathBuf::from(dir));
        return;
    }

    // Exactly one of these survives cfg expansion, so neither needs a
    // trailing `return` — and adding one would be the last statement in
    // `main` under the vendored build, which clippy rejects.
    #[cfg(feature = "vendored")]
    build_vendored();

    #[cfg(not(feature = "vendored"))]
    {
        if pkg_config_probe() {
            return;
        }
        panic!(
            "could not locate the whiteout native library.\n\
             \n\
             The `vendored` feature is disabled, so this crate expects a\n\
             prebuilt library. Either:\n\
             \n\
               * re-enable the default `vendored` feature to build the\n\
                 bundled C++ sources (needs CMake and a C++20 compiler), or\n\
               * set WHITEOUT_LIB_DIR to a directory containing\n\
                 whiteout_native (add WHITEOUT_STATIC=1 for the static\n\
                 archive), or\n\
               * install the library so pkg-config can find it."
        );
    }
}

/// Tell dependent build scripts how to link this library instead of
/// building their own. See the header for the key list.
fn publish(lib_dir: &Path, include_dir: Option<&Path>) {
    println!("cargo:lib_dir={}", lib_dir.display());
    if let Some(inc) = include_dir {
        println!("cargo:include={}", inc.display());
    }
    println!("cargo:has_casc={}", u8::from(cfg!(feature = "casc")));
    println!("cargo:has_mpq={}", u8::from(cfg!(feature = "mpq")));
}

/// Where the headers live for a prebuilt library: stated outright, or in the
/// usual spot next to a `lib/` directory.
fn prebuilt_include(lib_dir: &Path) -> Option<PathBuf> {
    if let Some(dir) = env::var_os("WHITEOUT_INCLUDE_DIR") {
        return Some(PathBuf::from(dir));
    }
    [lib_dir.join("..").join("include"), lib_dir.join("include")]
        .into_iter()
        .find(|p| p.join("whiteout").is_dir())
        .map(simplify)
}

fn want_static() -> bool {
    env::var("WHITEOUT_STATIC")
        .map(|v| v != "0" && !v.eq_ignore_ascii_case("false"))
        .unwrap_or(false)
}

fn link_prebuilt(dir: PathBuf) {
    if !dir.is_dir() {
        panic!(
            "WHITEOUT_LIB_DIR points at {} which is not a directory",
            dir.display()
        );
    }
    println!("cargo:rustc-link-search=native={}", dir.display());
    if want_static() {
        println!("cargo:rustc-link-lib=static=whiteout_native_static");
        link_cxx_runtime();
    } else {
        println!("cargo:rustc-link-lib=dylib=whiteout_native");
    }
    let include = prebuilt_include(&dir);
    publish(&dir, include.as_deref());
}

/// The bundled C++ tree: `native/` in a published crate, or the repository
/// root when building from a checkout.
#[allow(dead_code)]
fn vendored_sources() -> Option<PathBuf> {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").ok()?);
    let candidates = [
        manifest.join("native"),
        // bindings/rust/whiteout -> repository root
        manifest.join("..").join("..").join(".."),
    ];
    candidates
        .into_iter()
        .find(|p| p.join("CMakeLists.txt").is_file() && p.join("include").join("whiteout").is_dir())
        .map(simplify)
}

/// Windows `canonicalize` yields an extended-length path (the `\\?\C:\…`
/// form). MSVC cannot open source files spelled that way once they land in
/// a generated `.vcxproj`, so strip the prefix. Elsewhere this is a no-op.
#[allow(dead_code)]
fn simplify(p: PathBuf) -> PathBuf {
    let full = p.canonicalize().unwrap_or(p);
    let s = full.to_string_lossy().into_owned();
    match s.strip_prefix(r"\\?\") {
        Some(rest) => PathBuf::from(rest),
        None => PathBuf::from(s),
    }
}

#[cfg(feature = "vendored")]
fn build_vendored() {
    let src = vendored_sources().unwrap_or_else(|| {
        panic!(
            "the `vendored` feature is enabled but the bundled C++ sources \
             are missing.\n\
             Expected `native/CMakeLists.txt` beside this crate, or a \
             repository checkout above it.\n\
             If you are consuming a published crate this is a packaging \
             bug — please report it."
        )
    });
    println!(
        "cargo:rerun-if-changed={}",
        src.join("CMakeLists.txt").display()
    );

    let mut cfg = cmake::Config::new(&src);
    // Always Release, regardless of the Rust profile. The `cmake` crate
    // would otherwise mirror a debug Rust build, and on MSVC that links
    // the debug CRT while rustc links the release one — an unresolved
    // `__imp__CrtDbgReport` at link time. A debug native library buys
    // nothing here anyway.
    cfg.profile("Release");
    cfg.define("WHITEOUT_BUILD_C_BINDINGS", "ON")
        .define("WHITEOUT_BUILD_C_STATIC", "ON")
        .define("WHITEOUT_BUILD_TESTS", "OFF")
        .define("WHITEOUT_BUILD_EXAMPLES", "OFF")
        // zlib-ng lives in a submodule we do not vendor; the library
        // falls back to its own inflate when it is absent.
        .define("WHITEOUT_USE_ZLIBNG", "OFF")
        .define(
            "WHITEOUT_ENABLE_CASC",
            if cfg!(feature = "casc") { "ON" } else { "OFF" },
        )
        .define(
            "WHITEOUT_ENABLE_MPQ",
            if cfg!(feature = "mpq") { "ON" } else { "OFF" },
        )
        .build_target("whiteout_c_static");

    let dst = cfg.build();

    // The C target writes into `c-dist`, under a per-config subdirectory
    // on multi-config generators.
    let base = dst.join("build").join("c-dist");
    let dir = ["Release", "RelWithDebInfo", "Debug", ""]
        .iter()
        .map(|c| {
            if c.is_empty() {
                base.clone()
            } else {
                base.join(c)
            }
        })
        .find(|d| d.is_dir())
        .unwrap_or(base);

    println!("cargo:rustc-link-search=native={}", dir.display());
    println!("cargo:rustc-link-lib=static=whiteout_native_static");
    link_static_deps(&dst);
    link_cxx_runtime();
    // The headers come from the tree we just built, not from `dst`: the C
    // target is built directly, without an install step.
    publish(&dir, Some(&src.join("include")));
}

/// Static archives the C target depends on but does not absorb.
#[allow(dead_code)]
fn link_static_deps(dst: &Path) {
    let build = dst.join("build");
    for sub in ["", "Release", "RelWithDebInfo", "Debug"] {
        let dir = if sub.is_empty() {
            build.clone()
        } else {
            build.join(sub)
        };
        if dir.is_dir() {
            println!("cargo:rustc-link-search=native={}", dir.display());
        }
    }
    // Dependents first: GNU ld resolves static archives in command-line
    // order, so whiteout_lib has to come after the archives that reference
    // it. (MSVC and lld do not care, which is why the previous order
    // survived local testing.)
    let mut libs = Vec::new();
    if cfg!(feature = "casc") {
        libs.push("whiteout_casc");
    }
    if cfg!(feature = "mpq") {
        libs.push("whiteout_mpq");
    }
    libs.push("whiteout_lib");
    for lib in libs {
        println!("cargo:rustc-link-lib=static={lib}");
    }
}

#[allow(dead_code)]
fn pkg_config_probe() -> bool {
    // Deliberately shelling out rather than taking a pkg-config crate
    // dependency: this is a fallback path.
    let out = std::process::Command::new("pkg-config")
        .args(["--libs", "--silence-errors", "whiteout"])
        .output();
    match out {
        Ok(o) if o.status.success() => {
            let flags = String::from_utf8_lossy(&o.stdout).to_string();
            let mut lib_dir = None;
            for tok in flags.split_whitespace() {
                if let Some(p) = tok.strip_prefix("-L") {
                    println!("cargo:rustc-link-search=native={p}");
                    lib_dir.get_or_insert_with(|| PathBuf::from(p));
                } else if let Some(l) = tok.strip_prefix("-l") {
                    println!("cargo:rustc-link-lib=dylib={l}");
                }
            }
            if let Some(dir) = lib_dir {
                let include = prebuilt_include(&dir);
                publish(&dir, include.as_deref());
            }
            true
        }
        _ => false,
    }
}

// A static whiteout_native still needs the C++ runtime and the platform
// libraries it was compiled against. Linking the shared library pulled
// these in transitively; a static archive records no such dependency, and
// rustc's MSVC link line carries only a minimal default set.
fn link_cxx_runtime() {
    let target = env::var("TARGET").unwrap_or_default();
    if target.contains("msvc") {
        for lib in [
            "advapi32", // registry — BlizzardGameFinder
            "winhttp",  // SimpleHttpHandler
            "ole32",    // known-folder lookups
            "shell32", "user32",
        ] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
        return;
    }
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
}
