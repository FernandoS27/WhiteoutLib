// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Phase 3 gate: the data plane. Fields, borrowed element access, Tier A
// slices, and a byte-identical round-trip over the war3.w3mod corpus.
//
// The corpus sweep skips itself when Corpus/MDL/war3.w3mod is absent, so
// the suite still runs on a checkout without game assets. Everything above
// it is self-contained.

use std::path::{Path, PathBuf};

use whiteout::math::Vector3f;
use whiteout::mdx::{MDLXFormat, MdlFormat, Model, Parser, Writer};

fn parse(bytes: &[u8]) -> Option<Model> {
    Parser::new().parse(bytes, MDLXFormat::MDX)
}

fn write(m: &Model) -> whiteout::Bytes {
    Writer::new().write(m, MDLXFormat::MDX, MdlFormat::WarcraftIII)
}

// ── Fields: scalars, strings, enums ───────────────────────────────────────

#[test]
fn model_scalar_fields_round_trip_through_accessors() {
    let mut m = Model::new();
    m.set_model_name("TestModel");
    m.set_version(1000);
    assert_eq!(m.model_name(), "TestModel");
    assert_eq!(m.version(), 1000);
}

#[test]
fn strings_survive_non_ascii() {
    let mut m = Model::new();
    m.set_model_name("Ünïcødé");
    assert_eq!(m.model_name(), "Ünïcødé");
}

// ── Tier A: zero-copy POD vectors ─────────────────────────────────────────

#[test]
fn geoset_vertex_positions_are_a_borrowed_slice() {
    let mut m = Model::new();
    m.resize_geosets(1);
    let mut g = m.geosets_mut(0).expect("geoset 0");

    g.resize_vertex_positions(3);
    {
        // Writes land straight in the C++ vector — no marshalling.
        let vs = g.vertex_positions_mut();
        assert_eq!(vs.len(), 3);
        vs[0] = Vector3f::new(1.0, 2.0, 3.0);
        vs[1] = Vector3f::new(4.0, 5.0, 6.0);
        vs[2] = Vector3f::new(-1.0, 0.0, 0.5);
    }

    let vs = g.vertex_positions();
    assert_eq!(vs.len(), 3);
    assert_eq!(vs[0], Vector3f::new(1.0, 2.0, 3.0));
    assert_eq!(vs[2], Vector3f::new(-1.0, 0.0, 0.5));
}

#[test]
fn faces_are_a_u16_slice() {
    let mut m = Model::new();
    m.resize_geosets(1);
    let mut g = m.geosets_mut(0).expect("geoset 0");

    g.set_faces(&[0u16, 1, 2, 2, 1, 3]);
    assert_eq!(g.faces(), &[0u16, 1, 2, 2, 1, 3]);

    g.resize_faces(3);
    assert_eq!(g.faces().len(), 3);
}

#[test]
fn assigning_a_slice_copies_it_in() {
    let mut m = Model::new();
    m.resize_geosets(1);
    let mut g = m.geosets_mut(0).expect("geoset 0");

    let src = vec![Vector3f::new(9.0, 8.0, 7.0); 4];
    g.set_vertex_positions(&src);
    assert_eq!(g.vertex_positions().len(), 4);
    assert_eq!(g.vertex_positions()[3], Vector3f::new(9.0, 8.0, 7.0));
}

// ── Borrowed element access ───────────────────────────────────────────────

#[test]
fn vector_elements_are_borrowed_in_place() {
    let mut m = Model::new();
    m.resize_bones(2);
    assert_eq!(m.bones_len(), 2);

    // A mutation through the borrowed element must be visible through a
    // fresh borrow — proving we edited the model, not a copy.
    m.bones_mut(0).expect("bone 0").set_geoset_id(42);
    assert_eq!(m.bones(0).expect("bone 0").geoset_id(), 42);

    assert!(m.bones(2).is_none(), "out-of-range index must be None");
}

#[test]
fn iterating_borrows_each_element() {
    let mut m = Model::new();
    m.resize_sequences(3);
    for i in 0..3 {
        m.sequences_mut(i)
            .expect("seq")
            .set_interval_start(i as u32 * 100);
    }

    let starts: Vec<u32> = m.sequences_iter().map(|s| s.interval_start()).collect();
    assert_eq!(starts, vec![0, 100, 200]);
    assert_eq!(m.sequences_iter().len(), 3);
}

#[test]
fn nested_struct_fields_borrow_without_copying() {
    let mut m = Model::new();
    // `modelExtent` is a nested Extent held by value inside Model; the
    // borrow edits it in place rather than round-tripping a copy.
    m.model_extent_mut().set_bounds_radius(12.5);
    assert_eq!(m.model_extent().bounds_radius(), 12.5);
}

#[test]
fn math_typed_fields_cross_by_value() {
    let mut m = Model::new();
    let mut e = m.model_extent_mut();
    e.set_minimum(Vector3f::new(-1.0, -2.0, -3.0));
    assert_eq!(e.minimum(), Vector3f::new(-1.0, -2.0, -3.0));
}

// ── Round-trip: build, write, parse ───────────────────────────────────────

#[test]
fn synthesised_model_round_trips() {
    let mut m = Model::new();
    m.set_model_name("RoundTrip");
    m.set_version(800);
    m.resize_geosets(1);
    {
        let mut g = m.geosets_mut(0).expect("geoset 0");
        g.set_vertex_positions(&[
            Vector3f::new(0.0, 0.0, 0.0),
            Vector3f::new(1.0, 0.0, 0.0),
            Vector3f::new(0.0, 1.0, 0.0),
        ]);
        g.set_faces(&[0, 1, 2]);
    }

    let bytes = write(&m);
    assert!(!bytes.is_empty(), "writer produced nothing");

    let back = parse(&bytes).expect("re-parse failed");
    assert_eq!(back.model_name(), "RoundTrip");
    assert_eq!(back.geosets_len(), 1);
    let g = back.geosets(0).expect("geoset 0");
    assert_eq!(g.vertex_positions().len(), 3);
    assert_eq!(g.vertex_positions()[1], Vector3f::new(1.0, 0.0, 0.0));
    assert_eq!(g.faces(), &[0u16, 1, 2]);
}

// ── Corpus sweep ──────────────────────────────────────────────────────────

fn corpus_dir() -> Option<PathBuf> {
    for base in [
        "Corpus/MDL/war3.w3mod",
        "../Corpus/MDL/war3.w3mod",
        "../../Corpus/MDL/war3.w3mod",
        "../../../Corpus/MDL/war3.w3mod",
    ] {
        let p = Path::new(base);
        if p.is_dir() {
            return Some(p.to_path_buf());
        }
    }
    None
}

fn collect_mdx(dir: &Path, out: &mut Vec<PathBuf>, limit: usize) {
    if out.len() >= limit {
        return;
    }
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    let mut entries: Vec<_> = entries.flatten().map(|e| e.path()).collect();
    entries.sort(); // deterministic subset across runs
    for p in entries {
        if out.len() >= limit {
            return;
        }
        if p.is_dir() {
            collect_mdx(&p, out, limit);
        } else if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("mdx")) {
            out.push(p);
        }
    }
}

/// Parse → write → re-parse over real game assets, asserting the second
/// serialisation is byte-identical to the first.
///
/// The first write is the baseline rather than the on-disk file: the C++
/// library is not required to reproduce every source file byte-for-byte
/// (that is `mdx_mdl_roundtrip_test.cpp`'s job). What this pins down is
/// that *the binding* neither loses nor corrupts anything on the way
/// through Rust.
#[test]
fn corpus_round_trips_byte_identically() {
    let Some(dir) = corpus_dir() else {
        eprintln!("skipping: Corpus/MDL/war3.w3mod not present");
        return;
    };

    let limit: usize = std::env::var("WHITEOUT_CORPUS_LIMIT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(300);

    let mut files = Vec::new();
    collect_mdx(&dir, &mut files, limit);
    assert!(!files.is_empty(), "corpus directory held no .mdx files");

    let mut checked = 0usize;
    let mut skipped = 0usize;
    for path in &files {
        let Ok(src) = std::fs::read(path) else {
            continue;
        };
        let Some(model) = parse(&src) else {
            // Lenient parsing returning None is a library-level judgement,
            // not a binding failure.
            skipped += 1;
            continue;
        };

        let first = write(&model);
        if first.is_empty() {
            skipped += 1;
            continue;
        }

        let reparsed = parse(&first)
            .unwrap_or_else(|| panic!("{}: re-parse of our own output failed", path.display()));
        let second = write(&reparsed);

        assert_eq!(
            first.as_ref(),
            second.as_ref(),
            "{}: write→parse→write was not byte-identical ({} vs {} bytes)",
            path.display(),
            first.len(),
            second.len()
        );
        checked += 1;
    }

    eprintln!(
        "corpus: {checked} round-tripped, {skipped} skipped, {} total",
        files.len()
    );
    assert!(
        checked > 0,
        "no corpus file survived a round-trip ({skipped} skipped)"
    );
}

/// Every model in the sample must be reachable through the accessors
/// without panicking — the broad smoke test for field binding.
#[test]
fn corpus_fields_are_readable() {
    let Some(dir) = corpus_dir() else {
        eprintln!("skipping: Corpus/MDL/war3.w3mod not present");
        return;
    };

    let mut files = Vec::new();
    collect_mdx(&dir, &mut files, 100);

    let mut total_verts = 0usize;
    for path in &files {
        let Ok(src) = std::fs::read(path) else {
            continue;
        };
        let Some(m) = parse(&src) else { continue };

        let _ = m.model_name();
        let _ = m.version();
        for g in m.geosets_iter() {
            total_verts += g.vertex_positions().len();
            // Faces index vertices, so this is a real invariant.
            let nverts = g.vertex_positions().len();
            if nverts > 0 {
                for &f in g.faces() {
                    assert!(
                        (f as usize) < nverts,
                        "{}: face index {f} exceeds {nverts} vertices",
                        path.display()
                    );
                }
            }
        }
        for b in m.bones_iter() {
            let _ = b.geoset_id();
        }
    }
    eprintln!(
        "corpus: read {total_verts} vertices across {} files",
        files.len()
    );
}
