// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Phase 5 gate: host-implemented interfaces.
//
// The load-bearing tests here are the panic-safety ones. The C ABI is
// compiled `-fno-exceptions`, so a Rust panic unwinding into it is
// undefined behaviour; every thunk contains it. These tests panic inside
// each thunk category and assert the process survives with a sane result —
// the mitigation for R1, verified rather than documented.

use std::collections::HashMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use whiteout::interfaces::{CascFileSystem, FileSystem, HostCascFileSystem, HostFileSystem};

// ── A trivial in-memory implementation ────────────────────────────────────

#[derive(Default)]
struct MemFs {
    files: Mutex<HashMap<String, Vec<u8>>>,
    reads: AtomicUsize,
}

impl FileSystem for MemFs {
    fn read_file(&self, path: &str) -> Option<Vec<u8>> {
        self.reads.fetch_add(1, Ordering::Relaxed);
        self.files.lock().unwrap().get(path).cloned()
    }

    fn write_file(&self, path: &str, data: &[u8]) -> bool {
        self.files
            .lock()
            .unwrap()
            .insert(path.to_string(), data.to_vec());
        true
    }

    fn file_exists(&self, path: &str) -> bool {
        self.files.lock().unwrap().contains_key(path)
    }
}

#[test]
fn cpp_calls_back_into_a_rust_filesystem() {
    let fs = MemFs::default();
    fs.write_file("units/arthas.mdx", b"MDLX-ish bytes");

    let host = HostFileSystem::new(fs);

    // Read back *through the C++ interface*, which is the path library
    // code takes — not through the Rust trait directly.
    let got = host.read_through_native("units/arthas.mdx");
    assert_eq!(got.as_deref(), Some(&b"MDLX-ish bytes"[..]));

    assert!(host.exists_through_native("units/arthas.mdx"));
    assert!(!host.exists_through_native("nope.mdx"));
}

#[test]
fn a_missing_file_reads_back_as_none() {
    let host = HostFileSystem::new(MemFs::default());
    assert_eq!(host.read_through_native("absent"), None);
}

#[test]
fn large_buffers_survive_the_round_trip() {
    // Exercises the length-prefixed allocation used to hand buffers over:
    // C++ copies and immediately frees, with only the pointer to go on.
    let fs = MemFs::default();
    let big: Vec<u8> = (0..64 * 1024).map(|i| (i % 251) as u8).collect();
    fs.write_file("big.bin", &big);

    let host = HostFileSystem::new(fs);
    for _ in 0..64 {
        let got = host.read_through_native("big.bin").expect("missing");
        assert_eq!(got.len(), big.len());
        assert_eq!(got, big);
    }
}

#[test]
fn non_ascii_paths_cross_intact() {
    let fs = MemFs::default();
    fs.write_file("模型/Ünïcødé.mdx", b"ok");
    let host = HostFileSystem::new(fs);
    assert_eq!(
        host.read_through_native("模型/Ünïcødé.mdx").as_deref(),
        Some(&b"ok"[..])
    );
}

// ── Panic safety (R1) ─────────────────────────────────────────────────────

struct PanickingFs {
    on_read: bool,
    on_exists: bool,
}

impl FileSystem for PanickingFs {
    fn read_file(&self, _path: &str) -> Option<Vec<u8>> {
        if self.on_read {
            panic!("deliberate panic inside read_file");
        }
        Some(b"fine".to_vec())
    }

    fn file_exists(&self, _path: &str) -> bool {
        if self.on_exists {
            panic!("deliberate panic inside file_exists");
        }
        true
    }
}

#[test]
fn a_panic_in_read_file_is_contained_at_the_boundary() {
    // Without catch_unwind in the thunk this unwinds into `-fno-exceptions`
    // C++ and the process dies (or worse). It must instead surface as a
    // plain "no data".
    let host = HostFileSystem::new(PanickingFs {
        on_read: true,
        on_exists: false,
    });
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {})); // keep the expected panic quiet
    let got = host.read_through_native("anything");
    std::panic::set_hook(prev);

    assert_eq!(got, None, "a contained panic must read back as absent");
    // Still usable afterwards — the boundary is not poisoned.
    assert!(host.exists_through_native("anything"));
}

#[test]
fn a_panic_in_file_exists_is_contained() {
    let host = HostFileSystem::new(PanickingFs {
        on_read: false,
        on_exists: true,
    });
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let exists = host.exists_through_native("anything");
    std::panic::set_hook(prev);

    assert!(!exists, "a contained panic must report false");
}

// ── CascFileSystem ────────────────────────────────────────────────────────

#[derive(Default)]
struct MemCasc {
    files: Mutex<HashMap<u32, Vec<u8>>>,
}

impl CascFileSystem for MemCasc {
    fn read_file(&self, file_id: u32) -> Option<Vec<u8>> {
        self.files.lock().unwrap().get(&file_id).cloned()
    }

    fn reserve_file_id(&self, path: &str) -> Option<u32> {
        Some(path.len() as u32)
    }

    fn write_file(&self, file_id: u32, data: &[u8]) -> bool {
        self.files.lock().unwrap().insert(file_id, data.to_vec());
        true
    }

    fn file_exists(&self, file_id: u32) -> bool {
        self.files.lock().unwrap().contains_key(&file_id)
    }
}

#[test]
fn cpp_calls_back_into_a_rust_casc_filesystem() {
    let fs = MemCasc::default();
    fs.write_file(1234, b"payload");

    let host = HostCascFileSystem::new(fs);
    assert_eq!(
        host.read_through_native(1234).as_deref(),
        Some(&b"payload"[..])
    );
    assert!(host.exists_through_native(1234));
    assert!(!host.exists_through_native(9999));
    assert_eq!(host.read_through_native(9999), None);
}

// ── Threading ─────────────────────────────────────────────────────────────

#[test]
fn a_host_filesystem_is_usable_from_many_threads() {
    // The C++ side documents that these interfaces may be called
    // concurrently from worker threads, which is why the traits require
    // `Send + Sync`. This is the check that the wrapper honours it.
    let fs = MemFs::default();
    for i in 0..16 {
        fs.write_file(&format!("f{i}"), format!("data{i}").as_bytes());
    }
    let host = Arc::new(HostFileSystem::new(fs));

    let mut handles = Vec::new();
    for t in 0..8 {
        let host = Arc::clone(&host);
        handles.push(std::thread::spawn(move || {
            for i in 0..16 {
                let want = format!("data{i}");
                let got = host.read_through_native(&format!("f{i}")).expect("missing");
                assert_eq!(got, want.as_bytes(), "thread {t}");
            }
        }));
    }
    for h in handles {
        h.join().expect("worker thread panicked");
    }
}

#[test]
fn dropping_the_host_wrapper_releases_both_sides() {
    // Repeated create/drop under ASan is the leak check for the boxed
    // trait object and the C++ subclass alike.
    for _ in 0..256 {
        let fs = MemFs::default();
        fs.write_file("x", b"y");
        let host = HostFileSystem::new(fs);
        assert!(host.exists_through_native("x"));
    }
}
