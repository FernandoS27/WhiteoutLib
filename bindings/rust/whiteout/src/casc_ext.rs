// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

//! CASC entry points that need hand-written marshalling.
//!
//! `Storage::openOnline` takes an options struct mixing an interface
//! pointer with a `std::function`, `Storage::readBatch` passes value
//! objects in both directions, and progress reporting is a callback — none
//! of those shapes is something the codegen can express, so they all cross
//! through the shims in `bindings/c/whiteout_casc_shims.cpp`.
//!
//! The methods are inherent `impl`s on [`crate::casc::Storage`], so they are
//! callable without importing anything from here. Only the request/result
//! types below need a `use`.

use core::ffi::{c_char, c_void};
use std::ffi::{CStr, CString};

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

    fn whiteout_casc_shim_openWithProgress(
        path: *const c_char,
        product: *const c_char,
        locale_mask: u32,
        flags: u32,
        progress: Option<ProgressFn>,
        user: *mut c_void,
        pool: *mut c_void,
    ) -> *mut c_void;

    fn whiteout_casc_shim_openOnlineWithProgress(
        product: *const c_char,
        region: *const c_char,
        build_key: *const c_char,
        http: *mut c_void,
        cache_dir: *const c_char,
        locale_mask: u32,
        flags: u32,
        progress: Option<ProgressFn>,
        user: *mut c_void,
        pool: *mut c_void,
    ) -> *mut c_void;

    fn whiteout_casc_shim_setProgressCallback(
        self_: *mut c_void,
        progress: Option<ProgressFn>,
        user: *mut c_void,
    );

    fn whiteout_casc_shim_progressStepName(step: i32) -> *const c_char;

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

// ── Progress reporting ──────────────────────────────────────────────────

/// Stage of the open sequence an event belongs to.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ProgressStep {
    /// Online: versions/cdns endpoint lookup.
    ResolvingVersion = 0,
    /// Build config (fetch or disk read + parse).
    LoadingBuildConfig = 1,
    /// CDN config (fetch or disk read + parse).
    LoadingCdnConfig = 2,
    /// Local `.idx` bucket files.
    LoadingIndexFiles = 3,
    /// Local `data.NNN` archives being memory-mapped.
    MappingArchives = 4,
    /// Online: per-archive `.index` fetches.
    LoadingArchiveIndexes = 5,
    /// ENCODING manifest decode + parse.
    LoadingEncodingTable = 6,
    /// TVFS sub-manifests (~870 on WoW retail).
    LoadingVfsManifests = 7,
    /// ROOT manifest decode + parse.
    LoadingRootManifest = 8,
    /// Storage is usable. Always the final event.
    Ready = 9,
}

impl ProgressStep {
    fn from_raw(v: i32) -> ProgressStep {
        match v {
            0 => ProgressStep::ResolvingVersion,
            1 => ProgressStep::LoadingBuildConfig,
            2 => ProgressStep::LoadingCdnConfig,
            3 => ProgressStep::LoadingIndexFiles,
            4 => ProgressStep::MappingArchives,
            5 => ProgressStep::LoadingArchiveIndexes,
            6 => ProgressStep::LoadingEncodingTable,
            7 => ProgressStep::LoadingVfsManifests,
            8 => ProgressStep::LoadingRootManifest,
            _ => ProgressStep::Ready,
        }
    }

    /// English label for this step, as the library spells it.
    pub fn name(self) -> &'static str {
        // SAFETY: the shim returns a static, NUL-terminated literal.
        unsafe {
            CStr::from_ptr(whiteout_casc_shim_progressStepName(self as i32))
                .to_str()
                .unwrap_or("Unknown")
        }
    }
}

/// Position of an event within its step's lifetime.
#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ProgressState {
    /// The step is starting. Always paired with an `End`.
    Begin = 0,
    /// Counters advanced. Throttled — samples may be dropped.
    Update = 1,
    /// The step finished; the counters hold the final tally.
    End = 2,
}

impl ProgressState {
    fn from_raw(v: i32) -> ProgressState {
        match v {
            0 => ProgressState::Begin,
            1 => ProgressState::Update,
            _ => ProgressState::End,
        }
    }
}

/// Layout of `whiteout_casc_ProgressInfo` (bindings/c/whiteout_casc_progress.h).
#[repr(C)]
struct RawProgressInfo {
    size: u32,
    step: i32,
    state: i32,
    _pad: i32,
    object: *const c_char,
    current: u64,
    total: u64,
    bytes_done: u64,
    bytes_total: u64,
    step_index: u32,
    step_count: u32,
    elapsed_ms: f64,
    overall_fraction: f64,
}

/// A single progress event.
///
/// `object` borrows from the native event, so it is only valid inside the
/// callback — copy it out if you need to keep it.
#[derive(Clone, Copy, Debug)]
pub struct ProgressInfo<'a> {
    /// Which phase this event belongs to.
    pub step: ProgressStep,
    /// Where in the phase's lifetime the event sits.
    pub state: ProgressState,
    /// What is being worked on: an archive key, a filename, `"ENCODING"`.
    pub object: &'a str,
    /// Items processed in this step.
    pub current: u64,
    /// Items in this step; 0 when unknown.
    pub total: u64,
    /// Bytes transferred in this step; 0 when untracked.
    pub bytes_done: u64,
    /// Expected bytes for this step; 0 when unknown.
    pub bytes_total: u64,
    /// Position of `step` in the planned sequence.
    pub step_index: u32,
    /// Steps planned for this operation.
    pub step_count: u32,
    /// Milliseconds since the operation started.
    pub elapsed_ms: f64,
    /// Completion of the whole operation, in `0.0..=1.0`.
    pub overall_fraction: f64,
}

type ProgressFn = extern "C" fn(user: *mut c_void, info: *const RawProgressInfo) -> i32;

/// Boxed closure plus the panic it may have raised. An `extern "C"` frame
/// must not unwind, so a panicking callback is caught here, turned into a
/// cancel, and resumed once the native call has returned.
struct ProgressCtx<'f> {
    handler: &'f mut dyn FnMut(&ProgressInfo) -> bool,
    panic: Option<Box<dyn core::any::Any + Send>>,
}

extern "C" fn progress_trampoline(user: *mut c_void, info: *const RawProgressInfo) -> i32 {
    if user.is_null() || info.is_null() {
        return 1;
    }
    // SAFETY: `user` is the ProgressCtx we passed to the shim, which outlives
    // the native call, and `info` is valid for the duration of this call.
    let ctx = unsafe { &mut *(user as *mut ProgressCtx) };
    if ctx.panic.is_some() {
        return 0; // already unwinding — stop asking
    }
    let raw = unsafe { &*info };
    let object = if raw.object.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(raw.object) }.to_str().unwrap_or("")
    };
    let event = ProgressInfo {
        step: ProgressStep::from_raw(raw.step),
        state: ProgressState::from_raw(raw.state),
        object,
        current: raw.current,
        total: raw.total,
        bytes_done: raw.bytes_done,
        bytes_total: raw.bytes_total,
        step_index: raw.step_index,
        step_count: raw.step_count,
        elapsed_ms: raw.elapsed_ms,
        overall_fraction: raw.overall_fraction,
    };

    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (ctx.handler)(&event)));
    match result {
        Ok(keep_going) => i32::from(keep_going),
        Err(payload) => {
            ctx.panic = Some(payload);
            0
        }
    }
}

/// Runs `body` with a trampoline bound to `handler`, resuming any panic the
/// handler raised once the native call is over.
fn with_progress_ctx<R>(
    handler: Option<&mut dyn FnMut(&ProgressInfo) -> bool>,
    body: impl FnOnce(Option<ProgressFn>, *mut c_void) -> R,
) -> R {
    match handler {
        None => body(None, core::ptr::null_mut()),
        Some(handler) => {
            let mut ctx = ProgressCtx {
                handler,
                panic: None,
            };
            let out = body(
                Some(progress_trampoline),
                &mut ctx as *mut ProgressCtx as *mut c_void,
            );
            if let Some(payload) = ctx.panic.take() {
                std::panic::resume_unwind(payload);
            }
            out
        }
    }
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

    /// Open a local storage, reporting progress as it goes.
    ///
    /// `progress` is called for every event until it returns `false`, which
    /// cancels the open — the storage then comes back as `None` with
    /// `last_error()` reporting cancellation. It may be called from worker
    /// threads during the parallel phases, but never from two at once, and it
    /// never blocks one: a slow handler costs dropped `Update` samples rather
    /// than throughput.
    ///
    /// - `product` — optional product code selecting a build from a
    ///   multi-product `.build.info`, e.g. `"w3"` vs `"w3t"`.
    /// - `flags` — `StorageFeatureFlags` bitmask; 0 loads everything eagerly.
    pub fn open_with_progress(
        path: &str,
        product: Option<&str>,
        locale_mask: u32,
        flags: u32,
        pool: Option<&crate::interfaces::HostWorkerPool>,
        progress: &mut dyn FnMut(&ProgressInfo) -> bool,
    ) -> Option<Storage> {
        let path_cstr = CString::new(path).unwrap_or_default();
        let product_cstr = CString::new(product.unwrap_or("")).unwrap_or_default();
        let pool_ptr = pool.map_or(core::ptr::null_mut(), |p| p.as_ptr());

        with_progress_ctx(Some(progress), |cb, user| {
            // SAFETY: every pointer is live for the duration of the call, and
            // the shim either returns a fresh Storage* or null.
            unsafe {
                Storage::from_raw(whiteout_casc_shim_openWithProgress(
                    path_cstr.as_ptr(),
                    product_cstr.as_ptr(),
                    locale_mask,
                    flags,
                    cb,
                    user,
                    pool_ptr,
                ) as *mut _)
            }
        })
    }

    /// Open a CDN-backed storage, reporting progress as it goes.
    ///
    /// Same reporting and cancellation rules as [`Storage::open_with_progress`].
    /// `flags` is a `StorageFeatureFlags` bitmask; pass 0 to keep the online
    /// default (fully lazy).
    pub fn open_online_with_progress(
        product: &str,
        region: &str,
        http: &dyn AsHttpHandler,
        build_key: Option<&str>,
        cache_dir: Option<&str>,
        locale_mask: u32,
        flags: u32,
        pool: Option<&crate::interfaces::HostWorkerPool>,
        progress: &mut dyn FnMut(&ProgressInfo) -> bool,
    ) -> Option<Storage> {
        let product_cstr = CString::new(product).unwrap_or_default();
        let region_cstr =
            CString::new(if region.is_empty() { "us" } else { region }).unwrap_or_default();
        let build_key_cstr = CString::new(build_key.unwrap_or("")).unwrap_or_default();
        let cache_dir_cstr = CString::new(cache_dir.unwrap_or("")).unwrap_or_default();
        let http_ptr = http.as_http_ptr();
        let pool_ptr = pool.map_or(core::ptr::null_mut(), |p| p.as_ptr());

        with_progress_ctx(Some(progress), |cb, user| {
            // SAFETY: as above.
            unsafe {
                Storage::from_raw(whiteout_casc_shim_openOnlineWithProgress(
                    product_cstr.as_ptr(),
                    region_cstr.as_ptr(),
                    build_key_cstr.as_ptr(),
                    http_ptr,
                    cache_dir_cstr.as_ptr(),
                    locale_mask,
                    flags,
                    cb,
                    user,
                    pool_ptr,
                ) as *mut _)
            }
        })
    }

    /// Report progress for work that happens after open — the deferred load a
    /// `LoadOnDemand` storage does on first access, and `prefetch()`.
    ///
    /// Scoped rather than a plain setter: the callback is installed for the
    /// duration of `body` and cleared afterwards, so its borrow can't outlive
    /// what the native side holds.
    ///
    /// ```no_run
    /// # use whiteout::casc::Storage;
    /// # let mut storage: Storage = unimplemented!();
    /// storage.with_progress(&mut |info| {
    ///     println!("{} {:.0}%", info.step.name(), info.overall_fraction * 100.0);
    ///     true
    /// }, |s| s.prefetch());
    /// ```
    pub fn with_progress<R>(
        &mut self,
        progress: &mut dyn FnMut(&ProgressInfo) -> bool,
        body: impl FnOnce(&mut Storage) -> R,
    ) -> R {
        let handle = self.raw.as_ptr() as *mut c_void;
        with_progress_ctx(Some(progress), |cb, user| {
            // SAFETY: the callback is cleared before this scope ends, so the
            // native side never holds a pointer to a dead context.
            unsafe { whiteout_casc_shim_setProgressCallback(handle, cb, user) };
            let out = body(self);
            unsafe {
                whiteout_casc_shim_setProgressCallback(handle, None, core::ptr::null_mut())
            };
            out
        })
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
