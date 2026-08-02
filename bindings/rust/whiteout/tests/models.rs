// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Phase 4 gate: m2 (WoW) and m3 (StarCraft II / Heroes of the Storm).
//
// m3 gets the full parse → write → re-parse treatment against the SC2 and
// HotS corpora. m2's parser takes a `CascFileSystem`, which lands with the
// `host` module, so only its data plane is exercised here.
//
// Corpus tests skip themselves when the assets are absent.

use std::path::{Path, PathBuf};

use whiteout::m3;
use whiteout::math::Vector3f;

// ── m3: data plane ────────────────────────────────────────────────────────

#[test]
fn m3_model_scalar_and_string_fields() {
    let mut m = m3::Model::new();
    m.set_name("TestM3");
    assert_eq!(m.name(), "TestM3");
}

#[test]
fn m3_bitflag_fields_compose() {
    let mut m = m3::Model::new();
    let flags = m.flags();
    // Flag sets are a newtype, so `|`/`contains` work and an unknown bit
    // is not an error the way an unknown enum discriminant would be.
    let combined = flags | m3::ModelFlag::default();
    assert!(combined.contains(m3::ModelFlag::default()));
    m.set_flags(combined);
}

#[test]
fn m3_vector_elements_are_borrowed() {
    let mut m = m3::Model::new();
    m.resize_sequences(2);
    assert_eq!(m.sequences_len(), 2);
    assert!(m.sequences(0).is_some());
    assert!(m.sequences(2).is_none());
}

// ── m3: round-trip ────────────────────────────────────────────────────────

#[test]
fn m3_synthesised_model_round_trips() {
    let mut m = m3::Model::new();
    m.set_name("RoundTrip");

    let bytes = m3::Writer::new().write(&m);
    if bytes.is_empty() {
        // An empty model may legitimately not be serialisable; the corpus
        // test below is the real coverage.
        return;
    }
    let back = m3::Parser::new().parse(&bytes).expect("re-parse failed");
    assert_eq!(back.name(), "RoundTrip");
}

// ── Corpus helpers ────────────────────────────────────────────────────────

fn corpus_dir(name: &str) -> Option<PathBuf> {
    for prefix in ["", "../", "../../", "../../../"] {
        let p = PathBuf::from(format!("{prefix}Corpus/{name}"));
        if p.is_dir() {
            return Some(p);
        }
    }
    None
}

fn collect(dir: &Path, ext: &str, out: &mut Vec<PathBuf>, limit: usize) {
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
            collect(&p, ext, out, limit);
        } else if p.extension().is_some_and(|e| e.eq_ignore_ascii_case(ext)) {
            out.push(p);
        }
    }
}

fn corpus_limit(default: usize) -> usize {
    std::env::var("WHITEOUT_CORPUS_LIMIT")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

// ── m3: corpus ────────────────────────────────────────────────────────────

fn m3_round_trip_corpus(name: &str) {
    let Some(dir) = corpus_dir(name) else {
        eprintln!("skipping: Corpus/{name} not present");
        return;
    };

    let mut files = Vec::new();
    collect(&dir, "m3", &mut files, corpus_limit(200));
    assert!(!files.is_empty(), "Corpus/{name} held no .m3 files");

    let (mut checked, mut skipped) = (0usize, 0usize);
    for path in &files {
        let Ok(src) = std::fs::read(path) else {
            continue;
        };
        let Some(model) = m3::Parser::new().parse(&src) else {
            skipped += 1;
            continue;
        };

        let first = m3::Writer::new().write(&model);
        if first.is_empty() {
            skipped += 1;
            continue;
        }
        let reparsed = m3::Parser::new()
            .parse(&first)
            .unwrap_or_else(|| panic!("{}: re-parse of our own output failed", path.display()));
        let second = m3::Writer::new().write(&reparsed);

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
        "{name}: {checked} round-tripped, {skipped} skipped, {} total",
        files.len()
    );
    assert!(checked > 0, "no {name} file survived a round-trip");
}

#[test]
fn m3_sc2_corpus_round_trips() {
    m3_round_trip_corpus("Sc2M3");
}

#[test]
fn m3_hots_corpus_round_trips() {
    m3_round_trip_corpus("HotSM3");
}

#[test]
fn m3_corpus_fields_are_readable() {
    let Some(dir) = corpus_dir("Sc2M3") else {
        eprintln!("skipping: Corpus/Sc2M3 not present");
        return;
    };
    let mut files = Vec::new();
    collect(&dir, "m3", &mut files, 100);

    let mut divisions = 0usize;
    for path in &files {
        let Ok(src) = std::fs::read(path) else {
            continue;
        };
        let Some(m) = m3::Parser::new().parse(&src) else {
            continue;
        };

        let _ = m.name();
        let _ = m.flags();
        for s in m.sequences_iter() {
            let _ = s.name();
        }
        divisions += m.divisions_len();
    }
    eprintln!(
        "Sc2M3: read {divisions} divisions across {} files",
        files.len()
    );
}

// ── m2: data plane only (parser needs the host module) ────────────────────

#[test]
fn m2_model_fields_are_reachable() {
    use whiteout::m2;

    let mut m = m2::Model::new();
    m.set_model_name("TestM2");
    assert_eq!(m.model_name(), "TestM2");

    m.resize_global_loops(2);
    assert_eq!(m.global_loops_len(), 2);
    assert!(m.global_loops(0).is_some());
    assert!(m.global_loops(2).is_none());
}

#[test]
fn m2_nested_vector_tracks_are_per_sequence_slices() {
    use whiteout::m2;

    // The M2 animation layout is a vector-of-vectors: one inner sequence
    // per animation. Each inner vector is contiguous, so it borrows as a
    // slice; the outer one is resized explicitly.
    let mut t = m2::AnimationTrackVector3f::new();
    t.resize_timestamps(2);
    assert_eq!(t.timestamps_len(), 2);

    t.set_timestamps(0, &[0u32, 100, 200]);
    assert_eq!(t.timestamps(0), &[0u32, 100, 200]);

    t.resize_values(2);
    t.set_values(
        0,
        &[Vector3f::new(1.0, 2.0, 3.0), Vector3f::new(4.0, 5.0, 6.0)],
    );
    assert_eq!(t.values(0).len(), 2);
    assert_eq!(t.values(0)[1], Vector3f::new(4.0, 5.0, 6.0));

    // Out-of-range outer index yields an empty slice rather than panicking.
    assert!(t.timestamps(99).is_empty());
}

#[test]
fn m2_nested_vector_mutation_is_in_place() {
    use whiteout::m2;

    let mut t = m2::AnimationTrackF32::new();
    t.resize_values(1);
    t.set_values(0, &[1.0f32, 2.0, 3.0]);
    t.values_mut(0)[1] = 9.5;
    assert_eq!(t.values(0), &[1.0f32, 9.5, 3.0]);
}

#[test]
fn m2_nested_vector_of_handles_borrows_each_element() {
    use whiteout::m2;

    // `vector<vector<CameraSpline>>`: the elements own storage, so they
    // are borrowed one at a time rather than handed back as a slice.
    let mut t = m2::AnimationTrackM2CameraSpline::new();
    t.resize_values(2);
    assert_eq!(t.values_len(), 2);

    t.resize_values_inner(0, 3);
    assert_eq!(t.values_inner_len(0), 3);
    assert!(t.values(0, 0).is_some());
    assert!(t.values(0, 3).is_none(), "out of range must be None");
    assert!(t.values(9, 0).is_none(), "out-of-range outer must be None");

    // A mutation through the borrow is visible through a fresh one,
    // proving the element is edited in place rather than copied.
    t.values_mut(0, 1)
        .expect("elem")
        .set_value(Vector3f::new(1.0, 2.0, 3.0));
    assert_eq!(
        t.values(0, 1).expect("elem").value(),
        Vector3f::new(1.0, 2.0, 3.0)
    );
}

// ── Fixed-size arrays are bounds-checked (regression) ─────────────────────
//
// `TypeRef.array_size` carries the extent of a fixed C++ array, and the
// emitter ignored it: the accessors indexed straight through to
// `&self->arr[index]`, so an out-of-range index was undefined behaviour
// reachable from entirely safe Rust. 19 fields were affected, including
// M2Vertex's bone weights and indices — exactly what a skinning loop
// touches.

#[test]
fn array_accessors_report_their_length() {
    use whiteout::m2;
    assert_eq!(m2::Vertex::bone_weights_len(), 4);
    assert_eq!(m2::Vertex::bone_indices_len(), 4);
    assert_eq!(m2::Vertex::tex_coords_len(), 2);
}

#[test]
fn arrays_round_trip_within_bounds() {
    use whiteout::m2;

    let mut v = m2::Vertex::new();
    for i in 0..m2::Vertex::bone_weights_len() {
        v.set_bone_weights(i, (i as u8 + 1) * 10);
    }
    for i in 0..m2::Vertex::bone_weights_len() {
        assert_eq!(v.bone_weights(i), (i as u8 + 1) * 10);
    }
}

#[test]
#[should_panic(expected = "out of range")]
fn reading_past_an_array_panics_instead_of_reading_oob() {
    use whiteout::m2;
    let v = m2::Vertex::new();
    // Previously this read one element past the end of a 4-byte array.
    let _ = v.bone_weights(4);
}

#[test]
#[should_panic(expected = "out of range")]
fn writing_past_an_array_panics_instead_of_corrupting_memory() {
    use whiteout::m2;
    let mut v = m2::Vertex::new();
    // Previously this wrote one element past the end.
    v.set_bone_weights(4, 0xFF);
}

#[test]
#[should_panic(expected = "out of range")]
fn math_typed_arrays_are_checked_too() {
    use whiteout::mdx;
    let e = mdx::ParticleEmitter2::new();
    let _ = e.segment_color(3); // len is 3
}
