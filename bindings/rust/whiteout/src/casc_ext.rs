// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! CASC entry points that need hand-written marshalling.
//!
//! `Storage::openOnline` takes an options struct mixing an interface
//! pointer with a `std::function`, and `Storage::readBatch` passes value
//! objects in both directions — neither shape is something the codegen can
//! express, so both cross through the shims in
//! `bindings/c/whiteout_casc_shims.cpp`.
//!
//! The methods are inherent `impl`s on [`crate::casc::Storage`], so they are
//! callable without importing anything from here. Only the request/result
//! types below need a `use`.

use core::ffi::{c_char, c_void};
use std::ffi::CString;

use crate::casc::{FileIdHint, Storage};
use crate::support::RawCString;

extern "C" {
    fn whiteout_casc_shim_openOnline(
        product: *const c_char,
        region: *const c_char,
        build_key: *const c_char,
        http: *mut c_void,
        cache_dir: *const c_char,
        locale_mask: u32,
        pool: *mut c_void,
    ) -> *mut c_void;

    fn whiteout_casc_shim_readBatch(
        self_: *const c_void,
        paths: *const *const c_char,
        file_data_ids: *const i32,
        hints: *const i32,
        count: usize,
    ) -> *mut c_void;

    fn whiteout_casc_shim_readBatch_count(snapshot: *mut c_void) -> usize;
    fn whiteout_casc_shim_readBatch_data_at(
        snapshot: *mut c_void,
        index: usize,
    ) -> crate::support::RawBytes;
    fn whiteout_casc_shim_readBatch_success_at(snapshot: *mut c_void, index: usize) -> i32;
    fn whiteout_casc_shim_readBatch_error_at(snapshot: *mut c_void, index: usize) -> RawCString;
    fn whiteout_casc_shim_readBatch_free(snapshot: *mut c_void);
}

/// Anything that can be handed to C++ as an `interfaces::HttpHandler*`.
///
/// Implemented for both the trampoline wrapper (your own Rust
/// implementation) and the library's built-in client, so either can drive
/// [`Storage::open_online`].
pub trait AsHttpHandler {
    /// The raw `interfaces::HttpHandler*` this value stands for.
    fn as_http_ptr(&self) -> *mut c_void;
}

impl AsHttpHandler for crate::interfaces::HostHttpHandler {
    fn as_http_ptr(&self) -> *mut c_void {
        self.as_ptr()
    }
}

impl AsHttpHandler for crate::host::SimpleHttpHandler {
    fn as_http_ptr(&self) -> *mut c_void {
        self.raw.as_ptr() as *mut c_void
    }
}

/// One file to read in a batch: either by CASC path, or by WoW-style
/// FileDataId.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BatchReadRequest {
    /// Read by CASC path, e.g. `"Base\\creatures\\beast\\beast.m2"`.
    Path(String),
    /// Read by FileDataId, with a sub-type hint for the lookup.
    FileId {
        /// The FileDataId to resolve.
        id: i32,
        /// Which entry to pick when the id maps to several.
        hint: FileIdHint,
    },
}

impl BatchReadRequest {
    /// Read by CASC path.
    pub fn path(path: impl Into<String>) -> Self {
        BatchReadRequest::Path(path.into())
    }

    /// Read by FileDataId, taking the primary entry.
    pub fn file_id(id: i32) -> Self {
        BatchReadRequest::FileId {
            id,
            hint: FileIdHint::None,
        }
    }
}

/// Result of a single file in a batch read, in request order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchReadResult {
    /// File contents, or `None` when the read failed.
    pub data: Option<Vec<u8>>,
    /// Diagnostic message; empty on success.
    pub error: String,
}

impl BatchReadResult {
    /// Whether this file was read successfully.
    pub fn is_ok(&self) -> bool {
        self.data.is_some()
    }
}

impl Storage {
    /// Open a CDN-backed (online) storage.
    ///
    /// The returned [`Storage`] exposes the same read API as a local one.
    ///
    /// - `product` — product code, e.g. `"wow"`, `"w3"`, `"d3"`, `"fenris"`.
    /// - `region` — region for version lookup; empty defaults to `"us"`.
    /// - `http` — HTTP transport; required.
    /// - `build_key` — optional hex build-config key; `None` takes the
    ///   latest active build.
    /// - `cache_dir` — optional on-disk cache; `None` keeps everything in
    ///   memory.
    /// - `locale_mask` — locale filter, 0 accepts all.
    pub fn open_online(
        product: &str,
        region: &str,
        http: &dyn AsHttpHandler,
        build_key: Option<&str>,
        cache_dir: Option<&str>,
        locale_mask: u32,
        pool: Option<&crate::interfaces::HostWorkerPool>,
    ) -> Option<Storage> {
        let product_cstr = CString::new(product).unwrap_or_default();
        let region_cstr = CString::new(if region.is_empty() { "us" } else { region })
            .unwrap_or_default();
        let build_key_cstr = CString::new(build_key.unwrap_or("")).unwrap_or_default();
        let cache_dir_cstr = CString::new(cache_dir.unwrap_or("")).unwrap_or_default();

        // SAFETY: every pointer is live for the duration of the call, and
        // the shim either returns a fresh Storage* or null.
        unsafe {
            Storage::from_raw(whiteout_casc_shim_openOnline(
                product_cstr.as_ptr(),
                region_cstr.as_ptr(),
                build_key_cstr.as_ptr(),
                http.as_http_ptr(),
                cache_dir_cstr.as_ptr(),
                locale_mask,
                pool.map_or(core::ptr::null_mut(), |p| p.as_ptr()),
            ) as *mut _)
        }
    }

    /// Read every requested file in one native call.
    ///
    /// Results come back in request order; an individual failure yields a
    /// result with `data == None` and does not affect the others. When the
    /// storage was opened with a worker pool, resolution / raw read / BLTE
    /// decode overlap across files — considerably faster than reading one
    /// file at a time.
    pub fn read_batch(&self, requests: &[BatchReadRequest]) -> Vec<BatchReadResult> {
        if requests.is_empty() {
            return Vec::new();
        }

        // The CStrings must outlive the call, so they are kept alongside
        // the pointer array rather than being built inline.
        let mut owned: Vec<Option<CString>> = Vec::with_capacity(requests.len());
        let mut ids: Vec<i32> = Vec::with_capacity(requests.len());
        let mut hints: Vec<i32> = Vec::with_capacity(requests.len());
        for r in requests {
            match r {
                BatchReadRequest::Path(p) => {
                    owned.push(Some(CString::new(p.as_str()).unwrap_or_default()));
                    ids.push(-1);
                    hints.push(0);
                }
                BatchReadRequest::FileId { id, hint } => {
                    owned.push(None);
                    ids.push(*id);
                    hints.push(*hint as i32);
                }
            }
        }
        let ptrs: Vec<*const c_char> = owned
            .iter()
            .map(|o| o.as_ref().map_or(core::ptr::null(), |c| c.as_ptr()))
            .collect();

        // SAFETY: the parallel arrays are all `requests.len()` long and stay
        // alive across the call; the snapshot is freed before returning.
        unsafe {
            let snap = whiteout_casc_shim_readBatch(
                self.raw.as_ptr() as *const c_void,
                ptrs.as_ptr(),
                ids.as_ptr(),
                hints.as_ptr(),
                requests.len(),
            );
            if snap.is_null() {
                return Vec::new();
            }
            let n = whiteout_casc_shim_readBatch_count(snap);
            let mut out = Vec::with_capacity(n);
            for i in 0..n {
                let ok = whiteout_casc_shim_readBatch_success_at(snap, i) != 0;
                // Borrowed views into the snapshot (`owner` is null), so
                // copy them out rather than taking ownership.
                let data = if ok {
                    let raw = whiteout_casc_shim_readBatch_data_at(snap, i);
                    if raw.data.is_null() {
                        Some(Vec::new())
                    } else {
                        Some(core::slice::from_raw_parts(raw.data, raw.size).to_vec())
                    }
                } else {
                    None
                };
                let raw_err = whiteout_casc_shim_readBatch_error_at(snap, i);
                let error = if raw_err.chars.is_null() {
                    String::new()
                } else {
                    String::from_utf8_lossy(core::slice::from_raw_parts(
                        raw_err.chars as *const u8,
                        raw_err.length,
                    ))
                    .into_owned()
                };
                out.push(BatchReadResult { data, error });
            }
            whiteout_casc_shim_readBatch_free(snap);
            out
        }
    }
}
