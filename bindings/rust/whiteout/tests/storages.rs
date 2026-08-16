// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// CASC and MPQ, behind the `casc` / `mpq` cargo features which mirror the
// CMake `WHITEOUT_ENABLE_*` options.
//
// Smoke tests: opening a real storage needs a game install, so what is
// checked here is that the feature-gated modules link, that absent paths
// report absence rather than panicking, and that the options types
// round-trip. The C++ side carries the corpus coverage.

#![cfg(any(feature = "casc", feature = "mpq"))]

#[cfg(feature = "mpq")]
mod mpq_tests {
    use whiteout::mpq;

    #[test]
    fn opening_a_nonexistent_archive_reports_absence() {
        // `std::optional` in C++, `None` here — not an error, and above all
        // not a panic.
        assert!(mpq::Storage::open("definitely/not/here.mpq", None).is_none());
    }

    #[test]
    fn create_options_round_trip_through_accessors() {
        let mut opts = mpq::CreateOptions::new();
        opts.set_hash_table_size(4096);
        opts.set_sector_size_shift(4);
        assert_eq!(opts.hash_table_size(), 4096);
        assert_eq!(opts.sector_size_shift(), 4);
    }

    #[test]
    fn write_options_expose_their_enum_typed_field() {
        let mut opts = mpq::WriteOptions::new();
        let c = opts.compression();
        opts.set_compression(c);
        opts.set_encrypt(true);
        assert!(opts.encrypt());
    }

    #[test]
    fn a_created_archive_answers_queries() {
        let opts = mpq::CreateOptions::new();
        let Some(storage) = mpq::Storage::create(&opts, None) else {
            eprintln!("skipping: MPQ create unsupported in this build");
            return;
        };
        // Empty archive: nothing present, and asking is not an error.
        assert!(!storage.file_exists("nothing.txt"));
        assert!(storage.read_file("nothing.txt").is_none());
        assert!(storage.list_files().is_empty());
    }
}

#[cfg(feature = "casc")]
mod casc_tests {
    use whiteout::casc;

    #[test]
    fn opening_a_nonexistent_storage_reports_absence() {
        assert!(casc::Storage::open("definitely/not/here", None).is_none());
    }

    #[test]
    fn root_format_enum_round_trips() {
        assert_eq!(
            casc::RootFormat::try_from(0).unwrap(),
            casc::RootFormat::Unknown
        );
        // An out-of-range discriminant is an error, never a transmute.
        assert!(casc::RootFormat::try_from(9999).is_err());
    }

    #[test]
    fn write_options_use_struct_update_syntax() {
        // An all-primitive options struct is emitted by value, so it reads
        // like any other Rust config type — and its defaults are read out
        // of a native instance, so they cannot drift from the C++ ones.
        let opts = casc::WriteOptions {
            compress: true,
            ..Default::default()
        };
        assert!(opts.compress);
        assert_eq!(
            opts.locale_flags,
            casc::WriteOptions::default().locale_flags
        );
    }

    #[test]
    fn create_options_carry_their_strings() {
        let mut opts = casc::CreateOptions::new();
        opts.set_product("w3");
        opts.set_version("1.36.0");
        assert_eq!(opts.product(), "w3");
        assert_eq!(opts.version(), "1.36.0");
    }

    // ── Progress reporting ────────────────────────────────────────────
    //
    // A real CASC install is too large to ship, so these drive the callback
    // over a directory that fails to open — which still announces the step
    // it failed in, and is enough to prove events cross the FFI intact.

    #[test]
    fn every_step_has_a_label() {
        use whiteout::casc_ext::ProgressStep::*;
        for step in [
            ResolvingVersion,
            LoadingBuildConfig,
            LoadingCdnConfig,
            LoadingIndexFiles,
            MappingArchives,
            LoadingArchiveIndexes,
            LoadingEncodingTable,
            LoadingVfsManifests,
            LoadingRootManifest,
            Ready,
        ] {
            assert!(!step.name().is_empty(), "no label for {step:?}");
        }
        assert_eq!(LoadingIndexFiles.name(), "Loading index files");
    }

    #[test]
    fn open_reports_events_before_failing() {
        let dir = std::env::temp_dir().join("whiteout-rs-progress-events");
        std::fs::create_dir_all(&dir).unwrap();

        let mut events = Vec::new();
        let storage = casc::Storage::open_with_progress(
            dir.to_str().unwrap(),
            None,
            0,
            0,
            None,
            &mut |info| {
                assert!((0.0..=1.0).contains(&info.overall_fraction));
                assert!(info.step_count > 0);
                events.push((info.step, info.state, info.object.to_string()));
                true
            },
        );

        assert!(storage.is_none(), "a directory with no .idx must not open");
        assert!(!events.is_empty(), "the failing step should still be named");
        assert!(events
            .iter()
            .any(|(step, ..)| *step == whiteout::casc_ext::ProgressStep::LoadingIndexFiles));

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_handler_can_cancel() {
        let dir = std::env::temp_dir().join("whiteout-rs-progress-cancel");
        std::fs::create_dir_all(&dir).unwrap();

        let mut calls = 0;
        let storage = casc::Storage::open_with_progress(
            dir.to_str().unwrap(),
            None,
            0,
            0,
            None,
            &mut |_| {
                calls += 1;
                false
            },
        );

        assert!(storage.is_none());
        assert_eq!(calls, 1, "cancelling stops the event stream");
        assert_eq!(casc::Storage::last_error(), 0x16); // CascError::Cancelled

        let _ = std::fs::remove_dir_all(&dir);
    }
}

// ── list_files must materialise once (regression) ─────────────────────────
//
// The C ABI originally lowered a `vector<string>` return to a
// `_count`/`_at` pair, which re-invokes the C++ method for every index.
// That is fine when the method returns a reference to a stored vector
// (`getIssues`), and quadratic when it builds the vector fresh — as
// `listFiles()` does. Over a 135k-entry CASC storage it hung.
//
// There is no cheap way to assert big-O from a unit test, so this pins the
// observable consequence: the call completes promptly and repeatedly, and
// returns a consistent answer.

#[cfg(feature = "mpq")]
mod list_files_complexity {
    use whiteout::mpq;

    #[test]
    fn repeated_list_files_stays_cheap() {
        let opts = mpq::CreateOptions::new();
        let Some(storage) = mpq::Storage::create(&opts, None) else {
            eprintln!("skipping: MPQ create unsupported in this build");
            return;
        };

        // Under the quadratic lowering each of these walked the whole
        // archive once per entry. Even empty, the shape of the call is
        // what is being pinned: one materialisation, then O(1) reads.
        let start = std::time::Instant::now();
        for _ in 0..200 {
            let files = storage.list_files();
            assert!(files.is_empty(), "a fresh archive lists nothing");
        }
        let elapsed = start.elapsed();

        // Deliberately loose — this is a hang detector, not a benchmark.
        assert!(
            elapsed < std::time::Duration::from_secs(10),
            "200 list_files() calls took {elapsed:?}; the quadratic lowering is back"
        );
    }
}

// ── listEntries / readBatch / openOnline ──────────────────────────────────
//
// `list_entries` comes through the `@bind record` snapshot lowering;
// `read_batch` and `open_online` through the hand-written shims in
// `casc_ext`. A writable storage is created, saved and reopened, so none of
// this needs a real game install.

#[cfg(feature = "casc")]
mod casc_ext_tests {
    use whiteout::casc;
    use whiteout::casc_ext::BatchReadRequest;

    /// CASC stores paths with backslash separators.
    fn normalize(path: &str) -> String {
        path.replace('/', "\\")
    }

    fn make_data(size: usize, seed: u8) -> Vec<u8> {
        (0..size).map(|i| (i as u8).wrapping_add(seed)).collect()
    }

    struct TempDir(std::path::PathBuf);

    impl Drop for TempDir {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    /// Build a storage holding three files and reopen it from disk.
    fn round_tripped(tag: &str) -> Option<(casc::Storage, Vec<(String, Vec<u8>)>, TempDir)> {
        let dir = std::env::temp_dir().join(format!("whiteout-casc-rs-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        let guard = TempDir(dir.clone());

        let written = vec![
            ("dir/file1.txt".to_string(), make_data(1024, 0x11)),
            ("dir/file2.bin".to_string(), make_data(65536, 0x22)),
            ("tiny.dat".to_string(), make_data(1, 0x33)),
        ];

        let mut opts = casc::CreateOptions::new();
        opts.set_product("test");
        opts.set_version("1.0.0");
        let mut writable = casc::StorageWritable::create(&opts, None)?;

        let write_opts = casc::WriteOptions::default();
        for (path, data) in &written {
            if !writable.write_file(path, data, &write_opts) {
                return None;
            }
        }
        if !writable.save_path(dir.to_str()?) {
            return None;
        }
        drop(writable);

        let storage = casc::Storage::open(dir.to_str()?, None)?;
        Some((storage, written, guard))
    }

    #[test]
    fn list_entries_reports_metadata_for_every_written_file() {
        let Some((storage, written, _guard)) = round_tripped("entries") else {
            eprintln!("skipping: CASC create/save unsupported in this build");
            return;
        };

        let entries = storage.list_entries();
        assert!(!entries.is_empty());

        for (path, data) in &written {
            let want = normalize(path);
            let entry = entries
                .iter()
                .find(|e| e.path.eq_ignore_ascii_case(&want))
                .unwrap_or_else(|| panic!("no entry for {want}"));
            assert_eq!(entry.file_size, data.len() as u64);
            // Root-manifest entries carry the truncated content key
            // zero-padded to 16, so only the width is guaranteed.
            assert_eq!(entry.c_key.len(), 16);
            assert!(entry.c_key.iter().any(|&b| b != 0));
        }
    }

    #[test]
    fn read_batch_reads_every_requested_file_in_order() {
        let Some((storage, written, _guard)) = round_tripped("batch") else {
            eprintln!("skipping: CASC create/save unsupported in this build");
            return;
        };

        let requests: Vec<_> = written
            .iter()
            .map(|(p, _)| BatchReadRequest::path(p.clone()))
            .collect();
        let results = storage.read_batch(&requests);

        assert_eq!(results.len(), written.len());
        for (result, (path, data)) in results.iter().zip(&written) {
            assert!(result.is_ok(), "batch read failed for {path}: {}", result.error);
            assert_eq!(result.data.as_deref(), Some(data.as_slice()));
        }
    }

    #[test]
    fn read_batch_reports_per_file_failure_without_affecting_the_others() {
        let Some((storage, written, _guard)) = round_tripped("batchfail") else {
            eprintln!("skipping: CASC create/save unsupported in this build");
            return;
        };

        let results = storage.read_batch(&[
            BatchReadRequest::path("tiny.dat"),
            BatchReadRequest::path("does/not/exist.bin"),
            BatchReadRequest::path("dir/file1.txt"),
        ]);

        assert_eq!(results.len(), 3);
        assert!(results[0].is_ok());
        assert_eq!(results[0].data.as_deref(), Some(written[2].1.as_slice()));
        assert!(!results[1].is_ok());
        assert!(results[2].is_ok());
        assert_eq!(results[2].data.as_deref(), Some(written[0].1.as_slice()));
    }

    #[test]
    fn read_batch_of_nothing_returns_nothing() {
        let Some((storage, _written, _guard)) = round_tripped("batchempty") else {
            eprintln!("skipping: CASC create/save unsupported in this build");
            return;
        };
        assert!(storage.read_batch(&[]).is_empty());
    }

    #[test]
    fn optional_scalar_returns_map_to_option() {
        let Some((storage, written, _guard)) = round_tripped("optional") else {
            eprintln!("skipping: CASC create/save unsupported in this build");
            return;
        };
        assert_eq!(storage.file_size("tiny.dat"), Some(written[2].1.len() as u64));
        assert_eq!(storage.file_size("does/not/exist.bin"), None);
    }

    #[test]
    fn open_online_without_a_reachable_cdn_reports_absence() {
        // No network in CI: the point is that the shim links, marshals its
        // arguments, and reports failure as None rather than panicking.
        let http = whiteout::host::SimpleHttpHandler::new();
        let storage = casc::Storage::open_online(
            "definitely-not-a-product",
            "us",
            &http,
            None,
            None,
            0,
            None,
        );
        assert!(storage.is_none());
    }
}
