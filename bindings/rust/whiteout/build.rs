// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Locate and link the native library. Three modes, in precedence order:
//
//   1. WHITEOUT_LIB_DIR      — link a prebuilt library from that directory.
//   2. `build-native` feature — build it from source via CMake (not yet
//                               wired; documented so the escape hatch is
//                               obvious rather than surprising).
//   3. pkg-config            — system install.
//
// docs.rs has no native library and no C++ toolchain, so the build is
// skipped there entirely; the crate still type-checks and documents.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=WHITEOUT_LIB_DIR");
    println!("cargo:rerun-if-env-changed=WHITEOUT_STATIC");

    // docs.rs builds documentation only — nothing to link against.
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    let static_link = env::var("WHITEOUT_STATIC")
        .map(|v| v != "0" && !v.eq_ignore_ascii_case("false"))
        .unwrap_or(false);

    if let Some(dir) = env::var_os("WHITEOUT_LIB_DIR") {
        let dir = PathBuf::from(dir);
        if !dir.is_dir() {
            panic!(
                "WHITEOUT_LIB_DIR points at {} which is not a directory",
                dir.display()
            );
        }
        println!("cargo:rustc-link-search=native={}", dir.display());
        if static_link {
            println!("cargo:rustc-link-lib=static=whiteout_native_static");
            link_cxx_runtime();
        } else {
            println!("cargo:rustc-link-lib=dylib=whiteout_native");
        }
        return;
    }

    if pkg_config_probe() {
        return;
    }

    panic!(
        "could not locate the whiteout native library.\n\
         \n\
         Set WHITEOUT_LIB_DIR to the directory containing whiteout_native\
         (add WHITEOUT_STATIC=1 for whiteout_native_static), e.g.\n\
         \n\
           cmake -S . -B build-rust -DWHITEOUT_BUILD_C_BINDINGS=ON\n\
           cmake --build build-rust --config Release --target whiteout_c\n\
           WHITEOUT_LIB_DIR=build-rust/c-dist/Release cargo test\n\
         \n\
         scripts/build-rust.ps1 does all of the above."
    );
}

fn pkg_config_probe() -> bool {
    // Deliberately shelling out rather than taking a pkg-config crate
    // dependency: this is a fallback path, and the crate's dependency
    // budget is better spent elsewhere.
    let out = std::process::Command::new("pkg-config")
        .args(["--libs", "--silence-errors", "whiteout"])
        .output();
    match out {
        Ok(o) if o.status.success() => {
            let flags = String::from_utf8_lossy(&o.stdout).to_string();
            for tok in flags.split_whitespace() {
                if let Some(p) = tok.strip_prefix("-L") {
                    println!("cargo:rustc-link-search=native={p}");
                } else if let Some(l) = tok.strip_prefix("-l") {
                    println!("cargo:rustc-link-lib=dylib={l}");
                }
            }
            true
        }
        _ => false,
    }
}

// A static whiteout_native still needs the C++ runtime it was compiled
// against. MSVC links it automatically from the object files' directives;
// the Unix linkers do not.
fn link_cxx_runtime() {
    let target = env::var("TARGET").unwrap_or_default();
    if target.contains("msvc") {
        return;
    }
    if target.contains("apple") {
        println!("cargo:rustc-link-lib=dylib=c++");
    } else {
        println!("cargo:rustc-link-lib=dylib=stdc++");
    }
}
