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
}
