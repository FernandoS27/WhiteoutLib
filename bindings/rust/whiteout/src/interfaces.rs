// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Host-implemented interfaces: Rust traits the C++ library calls *into*.
// Mirrors <whiteout/interfaces.h>. The generated `host` module is the other
// direction — concrete implementations the library provides.
//
// Hand-written rather than generated. The C ABI side is a function-pointer
// table plus a `void* userdata` (see `bindings/c/whiteout_host_shims.cpp`),
// which maps onto a boxed trait object almost exactly — but the thunks need
// panic containment and per-method marshalling that is not worth teaching a
// generator for a handful of interfaces.

use core::ffi::{c_char, c_void};
use core::panic::AssertUnwindSafe;

use crate::support::{RawBytes, RawCString};

// ── Panic containment ─────────────────────────────────────────────────────
//
// The C ABI is compiled `-fno-exceptions`. A Rust panic unwinding into it is
// undefined behaviour, so every thunk that can reach user code stops
// unwinding at the boundary. This is a hazard neither the C# nor the Java
// binding has, and it belongs in code rather than in a doc comment.

fn guard<T>(fallback: T, f: impl FnOnce() -> T) -> T {
    match std::panic::catch_unwind(AssertUnwindSafe(f)) {
        Ok(v) => v,
        Err(_) => {
            // The payload has already gone to the panic hook.
            eprintln!(
                "whiteout: a panic in a host-implemented interface was contained \
                 at the FFI boundary; the operation reports failure"
            );
            fallback
        }
    }
}

/// Hand a Rust-allocated buffer to C++, which copies it and immediately
/// calls [`free_buffer`] with the same pointer — and *only* the pointer.
///
/// Rust needs the length to deallocate, so the allocation carries a length
/// header and the shim sees a pointer just past it. A thread-local would
/// also "work" given the copy-then-free-immediately contract, but it would
/// break the moment two buffers were in flight; this cannot.
const BUF_HEADER: usize = 16; // keeps the payload 16-byte aligned

fn leak_buffer(data: Vec<u8>, out_data: *mut *mut u8, out_size: *mut usize) {
    // SAFETY: both out-pointers come from the C++ caller's stack.
    unsafe {
        *out_data = core::ptr::null_mut();
        *out_size = 0;
    }
    if data.is_empty() {
        return;
    }
    let len = data.len();
    let Ok(layout) = std::alloc::Layout::from_size_align(BUF_HEADER + len, BUF_HEADER) else {
        return;
    };
    // SAFETY: non-zero size, valid alignment.
    let base = unsafe { std::alloc::alloc(layout) };
    if base.is_null() {
        return;
    }
    // SAFETY: `base` owns `BUF_HEADER + len` bytes, and the header is
    // aligned for `usize` because the block is 16-byte aligned.
    unsafe {
        (base as *mut usize).write(len);
        core::ptr::copy_nonoverlapping(data.as_ptr(), base.add(BUF_HEADER), len);
        *out_data = base.add(BUF_HEADER);
        *out_size = len;
    }
}

unsafe extern "C" fn free_buffer(data: *mut u8) {
    if data.is_null() {
        return;
    }
    // SAFETY: `data` is exactly what `leak_buffer` produced, so the header
    // sits `BUF_HEADER` bytes below it and records the payload length.
    unsafe {
        let base = data.sub(BUF_HEADER);
        let len = (base as *const usize).read();
        let layout = std::alloc::Layout::from_size_align_unchecked(BUF_HEADER + len, BUF_HEADER);
        std::alloc::dealloc(base, layout);
    }
}

/// # Safety
/// `p`/`len` must describe a byte run valid for `'a`.
unsafe fn str_of<'a>(p: *const c_char, len: usize) -> &'a str {
    if p.is_null() || len == 0 {
        return "";
    }
    // SAFETY: the C++ side passes a `std::string`'s data and size.
    let bytes = unsafe { core::slice::from_raw_parts(p as *const u8, len) };
    core::str::from_utf8(bytes).unwrap_or("")
}

/// # Safety
/// `data`/`size` must describe a byte run valid for `'a`.
unsafe fn bytes_of<'a>(data: *const u8, size: usize) -> &'a [u8] {
    if data.is_null() || size == 0 {
        return &[];
    }
    // SAFETY: contract above.
    unsafe { core::slice::from_raw_parts(data, size) }
}

// ── VirtualPathFileSystem ─────────────────────────────────────────────────

/// A file system the library resolves by path.
///
/// `Send + Sync` is mandatory, not conservative: the C++ header documents
/// that these methods may be called concurrently from worker threads.
pub trait FileSystem: Send + Sync {
    fn read_file(&self, path: &str) -> Option<Vec<u8>>;

    fn write_file(&self, _path: &str, _data: &[u8]) -> bool {
        false
    }

    fn file_exists(&self, path: &str) -> bool {
        self.read_file(path).is_some()
    }
}

#[repr(C)]
struct VfsFnTable {
    read_file: unsafe extern "C" fn(*mut c_void, *const c_char, usize, *mut *mut u8, *mut usize),
    free_buffer: unsafe extern "C" fn(*mut u8),
    write_file: unsafe extern "C" fn(*mut c_void, *const c_char, usize, *const u8, usize) -> i32,
    file_exists: unsafe extern "C" fn(*mut c_void, *const c_char, usize) -> i32,
}

/// # Safety
/// `userdata` must be the pointer `HostFileSystem::new` created.
unsafe fn vfs_of<'a>(userdata: *mut c_void) -> &'a dyn FileSystem {
    // SAFETY: contract above.
    unsafe { &**(userdata as *const Box<dyn FileSystem>) }
}

unsafe extern "C" fn vfs_read_file(
    userdata: *mut c_void,
    path: *const c_char,
    path_len: usize,
    out_data: *mut *mut u8,
    out_size: *mut usize,
) {
    let data = guard(Vec::new(), || {
        // SAFETY: contracts above.
        let fs = unsafe { vfs_of(userdata) };
        let path = unsafe { str_of(path, path_len) };
        fs.read_file(path).unwrap_or_default()
    });
    leak_buffer(data, out_data, out_size);
}

unsafe extern "C" fn vfs_write_file(
    userdata: *mut c_void,
    path: *const c_char,
    path_len: usize,
    data: *const u8,
    size: usize,
) -> i32 {
    guard(0, || {
        // SAFETY: contracts above.
        let fs = unsafe { vfs_of(userdata) };
        let path = unsafe { str_of(path, path_len) };
        let bytes = unsafe { bytes_of(data, size) };
        i32::from(fs.write_file(path, bytes))
    })
}

unsafe extern "C" fn vfs_file_exists(
    userdata: *mut c_void,
    path: *const c_char,
    path_len: usize,
) -> i32 {
    guard(0, || {
        // SAFETY: contracts above.
        let fs = unsafe { vfs_of(userdata) };
        let path = unsafe { str_of(path, path_len) };
        i32::from(fs.file_exists(path))
    })
}

extern "C" {
    fn whiteout_hostimpl_VirtualPathFileSystem_create(
        userdata: *mut c_void,
        fns: *const VfsFnTable,
    ) -> *mut c_void;
    fn whiteout_hostimpl_VirtualPathFileSystem_delete(handle: *mut c_void);

    fn whiteout_hostimpl_test_VirtualPathFileSystem_readFile(
        handle: *mut c_void,
        path: *const c_char,
    ) -> RawBytes;
    fn whiteout_hostimpl_test_VirtualPathFileSystem_fileExists(
        handle: *mut c_void,
        path: *const c_char,
    ) -> i32;
}

/// A [`FileSystem`] handed to the library.
///
/// Owns both the boxed trait object and the C++ subclass that forwards into
/// it, so it must outlive every library call that uses it.
pub struct HostFileSystem {
    handle: *mut c_void,
    userdata: *mut Box<dyn FileSystem>,
}

impl HostFileSystem {
    pub fn new<F: FileSystem + 'static>(fs: F) -> Self {
        let boxed: Box<dyn FileSystem> = Box::new(fs);
        let userdata = Box::into_raw(Box::new(boxed));
        let table = VfsFnTable {
            read_file: vfs_read_file,
            free_buffer,
            write_file: vfs_write_file,
            file_exists: vfs_file_exists,
        };
        // SAFETY: the shim copies the table, and `userdata` lives until drop.
        let handle = unsafe {
            whiteout_hostimpl_VirtualPathFileSystem_create(userdata as *mut c_void, &table)
        };
        HostFileSystem { handle, userdata }
    }

    /// Raw `interfaces::VirtualPathFileSystem*`, for library calls that
    /// take one.
    pub fn as_ptr(&self) -> *mut c_void {
        self.handle
    }

    /// Read back *through the C++ interface* — the path library code takes.
    pub fn read_through_native(&self, path: &str) -> Option<Vec<u8>> {
        let c = std::ffi::CString::new(path).ok()?;
        // SAFETY: `handle` is live for `&self`.
        let raw = unsafe {
            whiteout_hostimpl_test_VirtualPathFileSystem_readFile(self.handle, c.as_ptr())
        };
        // SAFETY: the invoker transfers ownership when non-empty.
        unsafe { crate::support::Bytes::from_raw(raw) }.map(|b| b.to_vec())
    }

    pub fn exists_through_native(&self, path: &str) -> bool {
        let Ok(c) = std::ffi::CString::new(path) else {
            return false;
        };
        // SAFETY: `handle` is live for `&self`.
        unsafe {
            whiteout_hostimpl_test_VirtualPathFileSystem_fileExists(self.handle, c.as_ptr()) != 0
        }
    }
}

impl Drop for HostFileSystem {
    fn drop(&mut self) {
        // SAFETY: both pointers were produced in `new` and are freed once.
        unsafe {
            whiteout_hostimpl_VirtualPathFileSystem_delete(self.handle);
            drop(Box::from_raw(self.userdata));
        }
    }
}

impl core::fmt::Debug for HostFileSystem {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HostFileSystem").finish_non_exhaustive()
    }
}

// SAFETY: `FileSystem` requires `Send + Sync`; the handle is a plain heap
// pointer with no thread affinity.
unsafe impl Send for HostFileSystem {}
unsafe impl Sync for HostFileSystem {}

// ── CascFileSystem ────────────────────────────────────────────────────────

/// A file system the library resolves by numeric data ID.
///
/// Required by the M2 parser and by CASC-backed asset loading.
pub trait CascFileSystem: Send + Sync {
    fn read_file(&self, file_id: u32) -> Option<Vec<u8>>;

    fn reserve_file_id(&self, _path: &str) -> Option<u32> {
        None
    }

    fn write_file(&self, _file_id: u32, _data: &[u8]) -> bool {
        false
    }

    fn file_exists(&self, file_id: u32) -> bool {
        self.read_file(file_id).is_some()
    }
}

#[repr(C)]
struct CascFsFnTable {
    read_file: unsafe extern "C" fn(*mut c_void, u32, *mut *mut u8, *mut usize),
    free_buffer: unsafe extern "C" fn(*mut u8),
    reserve_file_id: unsafe extern "C" fn(*mut c_void, *const c_char, usize, *mut u32) -> i32,
    write_file: unsafe extern "C" fn(*mut c_void, u32, *const u8, usize) -> i32,
    file_exists: unsafe extern "C" fn(*mut c_void, u32) -> i32,
}

/// # Safety
/// `userdata` must be the pointer `HostCascFileSystem::new` created.
unsafe fn casc_of<'a>(userdata: *mut c_void) -> &'a dyn CascFileSystem {
    // SAFETY: contract above.
    unsafe { &**(userdata as *const Box<dyn CascFileSystem>) }
}

unsafe extern "C" fn casc_read_file(
    userdata: *mut c_void,
    file_id: u32,
    out_data: *mut *mut u8,
    out_size: *mut usize,
) {
    let data = guard(Vec::new(), || {
        // SAFETY: contract above.
        unsafe { casc_of(userdata) }
            .read_file(file_id)
            .unwrap_or_default()
    });
    leak_buffer(data, out_data, out_size);
}

unsafe extern "C" fn casc_reserve_file_id(
    userdata: *mut c_void,
    path: *const c_char,
    path_len: usize,
    out_id: *mut u32,
) -> i32 {
    guard(0, || {
        // SAFETY: contracts above.
        let fs = unsafe { casc_of(userdata) };
        let path = unsafe { str_of(path, path_len) };
        match fs.reserve_file_id(path) {
            Some(id) => {
                // SAFETY: the C++ caller supplies a valid out-pointer.
                unsafe { *out_id = id };
                1
            }
            None => 0,
        }
    })
}

unsafe extern "C" fn casc_write_file(
    userdata: *mut c_void,
    file_id: u32,
    data: *const u8,
    size: usize,
) -> i32 {
    guard(0, || {
        // SAFETY: contracts above.
        let fs = unsafe { casc_of(userdata) };
        let bytes = unsafe { bytes_of(data, size) };
        i32::from(fs.write_file(file_id, bytes))
    })
}

unsafe extern "C" fn casc_file_exists(userdata: *mut c_void, file_id: u32) -> i32 {
    guard(0, || {
        // SAFETY: contract above.
        i32::from(unsafe { casc_of(userdata) }.file_exists(file_id))
    })
}

extern "C" {
    fn whiteout_hostimpl_CascFileSystem_create(
        userdata: *mut c_void,
        fns: *const CascFsFnTable,
    ) -> *mut c_void;
    fn whiteout_hostimpl_CascFileSystem_delete(handle: *mut c_void);

    fn whiteout_hostimpl_test_CascFileSystem_readFile(handle: *mut c_void, id: u32) -> RawBytes;
    fn whiteout_hostimpl_test_CascFileSystem_fileExists(handle: *mut c_void, id: u32) -> i32;
}

/// A [`CascFileSystem`] handed to the library.
pub struct HostCascFileSystem {
    handle: *mut c_void,
    userdata: *mut Box<dyn CascFileSystem>,
}

impl HostCascFileSystem {
    pub fn new<F: CascFileSystem + 'static>(fs: F) -> Self {
        let boxed: Box<dyn CascFileSystem> = Box::new(fs);
        let userdata = Box::into_raw(Box::new(boxed));
        let table = CascFsFnTable {
            read_file: casc_read_file,
            free_buffer,
            reserve_file_id: casc_reserve_file_id,
            write_file: casc_write_file,
            file_exists: casc_file_exists,
        };
        // SAFETY: as `HostFileSystem::new`.
        let handle =
            unsafe { whiteout_hostimpl_CascFileSystem_create(userdata as *mut c_void, &table) };
        HostCascFileSystem { handle, userdata }
    }

    pub fn as_ptr(&self) -> *mut c_void {
        self.handle
    }

    pub fn read_through_native(&self, file_id: u32) -> Option<Vec<u8>> {
        // SAFETY: `handle` is live for `&self`.
        let raw = unsafe { whiteout_hostimpl_test_CascFileSystem_readFile(self.handle, file_id) };
        // SAFETY: the invoker transfers ownership when non-empty.
        unsafe { crate::support::Bytes::from_raw(raw) }.map(|b| b.to_vec())
    }

    pub fn exists_through_native(&self, file_id: u32) -> bool {
        // SAFETY: `handle` is live for `&self`.
        unsafe { whiteout_hostimpl_test_CascFileSystem_fileExists(self.handle, file_id) != 0 }
    }
}

impl Drop for HostCascFileSystem {
    fn drop(&mut self) {
        // SAFETY: both pointers were produced in `new` and are freed once.
        unsafe {
            whiteout_hostimpl_CascFileSystem_delete(self.handle);
            drop(Box::from_raw(self.userdata));
        }
    }
}

impl core::fmt::Debug for HostCascFileSystem {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HostCascFileSystem").finish_non_exhaustive()
    }
}

// SAFETY: as `HostFileSystem`.
unsafe impl Send for HostCascFileSystem {}
unsafe impl Sync for HostCascFileSystem {}

// ── HttpHandler ───────────────────────────────────────────────────────────

/// Capability flags an [`HttpHandler`] may report.
pub mod http_capability {
    /// No optional capabilities.
    pub const NONE: u32 = 0;
    /// Connection multiplexing (HTTP/2).
    pub const HTTP2_MULTIPLEXING: u32 = 0x1;
}

/// The one-shot reply channel handed to an [`HttpHandler`].
///
/// Consuming it with [`respond`](Self::respond) or [`fail`](Self::fail)
/// fires the C++ callback exactly once. Dropping it without replying
/// cancels the request with a transport error rather than leaving library
/// code waiting forever — which is why the type owns the handle rather than
/// exposing it.
///
/// `Send`, so a handler may hand it to a worker thread or an async runtime
/// and reply later.
pub struct HttpResponder {
    handle: *mut c_void,
}

impl HttpResponder {
    /// Deliver a response.
    pub fn respond(self, status: i32, body: &[u8]) {
        let me = core::mem::ManuallyDrop::new(self);
        // SAFETY: the handle is live and fired exactly once — `self` is
        // consumed and `Drop` suppressed.
        unsafe {
            whiteout_hostimpl_HttpResponseCallback_fire(
                me.handle,
                status,
                body.as_ptr(),
                body.len(),
                core::ptr::null(),
            );
        }
    }

    /// Report a transport-level failure.
    pub fn fail(self, error: &str) {
        let me = core::mem::ManuallyDrop::new(self);
        let c = std::ffi::CString::new(error).unwrap_or_default();
        // SAFETY: as `respond`.
        unsafe {
            whiteout_hostimpl_HttpResponseCallback_fire(
                me.handle,
                0,
                core::ptr::null(),
                0,
                c.as_ptr(),
            );
        }
    }
}

impl Drop for HttpResponder {
    fn drop(&mut self) {
        // Only reached when the handler never replied. Cancelling fires a
        // transport error, so waiting library code fails cleanly instead of
        // hanging.
        // SAFETY: the handle is live and has not been fired.
        unsafe { whiteout_hostimpl_HttpResponseCallback_cancel(self.handle) };
    }
}

impl core::fmt::Debug for HttpResponder {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HttpResponder").finish_non_exhaustive()
    }
}

// SAFETY: the handle is a heap `std::function` with no thread affinity, and
// `HttpResponder` owns it exclusively.
unsafe impl Send for HttpResponder {}

/// An HTTP client the library uses to fetch CDN data.
///
/// Both methods are asynchronous: reply through the [`HttpResponder`] when
/// the request finishes, from whichever thread you like. Replying
/// synchronously inside the method is also fine.
pub trait HttpHandler: Send + Sync {
    fn capabilities(&self) -> u32 {
        http_capability::NONE
    }

    fn get(&self, url: &str, responder: HttpResponder);

    /// Inclusive byte range.
    fn get_range(&self, url: &str, start: u64, end: u64, responder: HttpResponder);
}

#[repr(C)]
struct HttpFnTable {
    capabilities: unsafe extern "C" fn(*mut c_void) -> u32,
    get_async: unsafe extern "C" fn(*mut c_void, *const c_char, usize, *mut c_void),
    get_range_async: unsafe extern "C" fn(*mut c_void, *const c_char, usize, u64, u64, *mut c_void),
}

/// # Safety
/// `userdata` must be the pointer `HostHttpHandler::new` created.
unsafe fn http_of<'a>(userdata: *mut c_void) -> &'a dyn HttpHandler {
    // SAFETY: contract above.
    unsafe { &**(userdata as *const Box<dyn HttpHandler>) }
}

unsafe extern "C" fn http_capabilities(userdata: *mut c_void) -> u32 {
    guard(http_capability::NONE, || {
        // SAFETY: contract above.
        unsafe { http_of(userdata) }.capabilities()
    })
}

unsafe extern "C" fn http_get_async(
    userdata: *mut c_void,
    url: *const c_char,
    url_len: usize,
    callback: *mut c_void,
) {
    let responder = HttpResponder { handle: callback };
    // A panic drops the responder, which cancels the request — so the
    // caller sees a transport error rather than waiting on a callback that
    // will never fire.
    guard((), move || {
        // SAFETY: contracts above.
        let h = unsafe { http_of(userdata) };
        let url = unsafe { str_of(url, url_len) };
        h.get(url, responder);
    });
}

unsafe extern "C" fn http_get_range_async(
    userdata: *mut c_void,
    url: *const c_char,
    url_len: usize,
    start: u64,
    end: u64,
    callback: *mut c_void,
) {
    let responder = HttpResponder { handle: callback };
    guard((), move || {
        // SAFETY: contracts above.
        let h = unsafe { http_of(userdata) };
        let url = unsafe { str_of(url, url_len) };
        h.get_range(url, start, end, responder);
    });
}

extern "C" {
    fn whiteout_hostimpl_HttpHandler_create(
        userdata: *mut c_void,
        fns: *const HttpFnTable,
    ) -> *mut c_void;
    fn whiteout_hostimpl_HttpHandler_delete(handle: *mut c_void);
    fn whiteout_hostimpl_HttpResponseCallback_fire(
        callback: *mut c_void,
        status: i32,
        body: *const u8,
        body_len: usize,
        error: *const c_char,
    );
    fn whiteout_hostimpl_HttpResponseCallback_cancel(callback: *mut c_void);

    fn whiteout_hostimpl_test_HttpHandler_capabilities(handle: *mut c_void) -> u32;
    fn whiteout_hostimpl_test_HttpHandler_getAsync(
        handle: *mut c_void,
        url: *const c_char,
        out_status: *mut i32,
        out_body: *mut RawBytes,
        out_error: *mut RawCString,
    );
    fn whiteout_hostimpl_test_HttpHandler_getRangeAsync(
        handle: *mut c_void,
        url: *const c_char,
        start: u64,
        end: u64,
        out_status: *mut i32,
        out_body: *mut RawBytes,
        out_error: *mut RawCString,
    );
}

/// The result of driving a handler through the C++ interface.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpOutcome {
    pub status: i32,
    pub body: Vec<u8>,
    pub error: String,
}

fn empty_bytes() -> RawBytes {
    RawBytes {
        data: core::ptr::null(),
        size: 0,
        owner: core::ptr::null_mut(),
    }
}

fn empty_cstring() -> RawCString {
    RawCString {
        chars: core::ptr::null(),
        length: 0,
        owner: core::ptr::null_mut(),
    }
}

/// # Safety
/// `body`/`error` must have been filled by one of the test invokers, which
/// transfer ownership of both buffers.
unsafe fn collect_outcome(status: i32, body: RawBytes, error: RawCString) -> HttpOutcome {
    // SAFETY: contract above.
    let body = unsafe { crate::support::Bytes::from_raw(body) }
        .map(|b| b.to_vec())
        .unwrap_or_default();
    // SAFETY: contract above.
    let error = unsafe { crate::support::take_string_opt(error) }.unwrap_or_default();
    HttpOutcome {
        status,
        body,
        error,
    }
}

/// An [`HttpHandler`] handed to the library.
pub struct HostHttpHandler {
    handle: *mut c_void,
    userdata: *mut Box<dyn HttpHandler>,
}

impl HostHttpHandler {
    pub fn new<H: HttpHandler + 'static>(handler: H) -> Self {
        let boxed: Box<dyn HttpHandler> = Box::new(handler);
        let userdata = Box::into_raw(Box::new(boxed));
        let table = HttpFnTable {
            capabilities: http_capabilities,
            get_async: http_get_async,
            get_range_async: http_get_range_async,
        };
        // SAFETY: the shim copies the table; `userdata` lives until drop.
        let handle =
            unsafe { whiteout_hostimpl_HttpHandler_create(userdata as *mut c_void, &table) };
        HostHttpHandler { handle, userdata }
    }

    /// Raw `interfaces::HttpHandler*`, for library calls that take one.
    pub fn as_ptr(&self) -> *mut c_void {
        self.handle
    }

    pub fn capabilities_through_native(&self) -> u32 {
        // SAFETY: `handle` is live for `&self`.
        unsafe { whiteout_hostimpl_test_HttpHandler_capabilities(self.handle) }
    }

    /// Drive `getAsync` and collect the reply.
    ///
    /// # Testing only
    ///
    /// The native invoker behind this captures the response into a **stack
    /// local** and returns as soon as `getAsync` does. A handler that
    /// replies after returning therefore writes into a dead frame. Real
    /// library call sites own the callback properly and may be replied to
    /// whenever; this helper exists so tests can drive a handler without
    /// standing up a CDN, and a handler used with it must reply before
    /// returning.
    pub fn get_through_native(&self, url: &str) -> HttpOutcome {
        let c = std::ffi::CString::new(url).unwrap_or_default();
        let mut status = 0i32;
        let mut body = empty_bytes();
        let mut error = empty_cstring();
        // SAFETY: all three out-pointers are live locals.
        unsafe {
            whiteout_hostimpl_test_HttpHandler_getAsync(
                self.handle,
                c.as_ptr(),
                &mut status,
                &mut body,
                &mut error,
            );
            collect_outcome(status, body, error)
        }
    }

    pub fn get_range_through_native(&self, url: &str, start: u64, end: u64) -> HttpOutcome {
        let c = std::ffi::CString::new(url).unwrap_or_default();
        let mut status = 0i32;
        let mut body = empty_bytes();
        let mut error = empty_cstring();
        // SAFETY: as above.
        unsafe {
            whiteout_hostimpl_test_HttpHandler_getRangeAsync(
                self.handle,
                c.as_ptr(),
                start,
                end,
                &mut status,
                &mut body,
                &mut error,
            );
            collect_outcome(status, body, error)
        }
    }
}

impl Drop for HostHttpHandler {
    fn drop(&mut self) {
        // SAFETY: both pointers were produced in `new` and are freed once.
        unsafe {
            whiteout_hostimpl_HttpHandler_delete(self.handle);
            drop(Box::from_raw(self.userdata));
        }
    }
}

impl core::fmt::Debug for HostHttpHandler {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HostHttpHandler").finish_non_exhaustive()
    }
}

// SAFETY: as `HostFileSystem`.
unsafe impl Send for HostHttpHandler {}
unsafe impl Sync for HostHttpHandler {}

// ── WorkerPool ────────────────────────────────────────────────────────────

/// A unit of work submitted by the library.
///
/// Run it with [`run`](Self::run), from whatever thread the pool chooses.
/// Dropping it without running cancels the work — which is safe but will
/// stall anything waiting on the task's signal semaphore, so prefer
/// running it.
///
/// `Send` so it can be moved onto a worker thread; deliberately not `Sync`,
/// since it may only be run once.
pub struct WorkerTask {
    fn_handle: *mut c_void,
    wait: Option<(*mut c_void, u64)>,
    signal: Option<(*mut c_void, u64)>,
}

impl WorkerTask {
    /// Wait on the task's semaphore if it has one, run the work, then
    /// signal. This is the whole contract a pool implementation owes.
    pub fn run(self) {
        let me = core::mem::ManuallyDrop::new(self);
        // SAFETY: each handle is live, and `fire` consumes the function
        // exactly once because `self` is consumed and `Drop` suppressed.
        unsafe {
            if let Some((sem, value)) = me.wait {
                whiteout_hostimpl_TimelineSemaphore_await(sem, value);
            }
            whiteout_hostimpl_WorkerTaskFn_fire(me.fn_handle);
            if let Some((sem, value)) = me.signal {
                whiteout_hostimpl_TimelineSemaphore_signal(sem, value);
            }
        }
    }
}

impl Drop for WorkerTask {
    fn drop(&mut self) {
        // Only reached when the pool declined to run the task.
        // SAFETY: the function has not been fired.
        unsafe { whiteout_hostimpl_WorkerTaskFn_cancel(self.fn_handle) };
    }
}

impl core::fmt::Debug for WorkerTask {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("WorkerTask").finish_non_exhaustive()
    }
}

// SAFETY: the handles are heap objects with no thread affinity; the task
// owns them exclusively and runs at most once.
unsafe impl Send for WorkerTask {}

/// A thread pool the library submits work to.
pub trait WorkerPool: Send + Sync {
    /// Run `task` — now, or on a worker thread.
    fn submit(&self, task: WorkerTask);

    /// Block until every submitted task has finished.
    fn wait_idle(&self);

    fn thread_count(&self) -> usize;
}

#[repr(C)]
struct WorkerTaskFlat {
    fn_handle: *mut c_void,
    wait_semaphore: *mut c_void,
    wait_value: u64,
    signal_semaphore: *mut c_void,
    signal_value: u64,
}

#[repr(C)]
struct WorkerPoolFnTable {
    submit: unsafe extern "C" fn(*mut c_void, *const WorkerTaskFlat),
    wait_idle: unsafe extern "C" fn(*mut c_void),
    thread_count: unsafe extern "C" fn(*mut c_void) -> usize,
}

/// # Safety
/// `userdata` must be the pointer `HostWorkerPool::new` created.
unsafe fn pool_of<'a>(userdata: *mut c_void) -> &'a dyn WorkerPool {
    // SAFETY: contract above.
    unsafe { &**(userdata as *const Box<dyn WorkerPool>) }
}

unsafe extern "C" fn pool_submit(userdata: *mut c_void, flat: *const WorkerTaskFlat) {
    if flat.is_null() {
        return;
    }
    // SAFETY: the shim passes a live stack struct.
    let flat = unsafe { &*flat };
    let task = WorkerTask {
        fn_handle: flat.fn_handle,
        wait: (!flat.wait_semaphore.is_null()).then_some((flat.wait_semaphore, flat.wait_value)),
        signal: (!flat.signal_semaphore.is_null())
            .then_some((flat.signal_semaphore, flat.signal_value)),
    };
    guard((), move || {
        // SAFETY: contract above.
        unsafe { pool_of(userdata) }.submit(task);
    });
}

unsafe extern "C" fn pool_wait_idle(userdata: *mut c_void) {
    guard((), || {
        // SAFETY: contract above.
        unsafe { pool_of(userdata) }.wait_idle();
    });
}

unsafe extern "C" fn pool_thread_count(userdata: *mut c_void) -> usize {
    guard(1, || {
        // SAFETY: contract above.
        unsafe { pool_of(userdata) }.thread_count()
    })
}

extern "C" {
    fn whiteout_hostimpl_WorkerPool_create(
        userdata: *mut c_void,
        fns: *const WorkerPoolFnTable,
    ) -> *mut c_void;
    fn whiteout_hostimpl_WorkerPool_delete(handle: *mut c_void);
    fn whiteout_hostimpl_WorkerTaskFn_fire(fn_handle: *mut c_void);
    fn whiteout_hostimpl_WorkerTaskFn_cancel(fn_handle: *mut c_void);
    fn whiteout_hostimpl_TimelineSemaphore_await(sem: *mut c_void, value: u64);
    fn whiteout_hostimpl_TimelineSemaphore_signal(sem: *mut c_void, value: u64);

    fn whiteout_hostimpl_test_WorkerPool_threadCount(handle: *mut c_void) -> usize;
    fn whiteout_hostimpl_test_WorkerPool_waitIdle(handle: *mut c_void);
    fn whiteout_hostimpl_test_WorkerPool_submitIncrementSentinel(
        handle: *mut c_void,
        out_sentinel: *mut i32,
    );
}

/// A [`WorkerPool`] handed to the library.
pub struct HostWorkerPool {
    handle: *mut c_void,
    userdata: *mut Box<dyn WorkerPool>,
}

impl HostWorkerPool {
    pub fn new<P: WorkerPool + 'static>(pool: P) -> Self {
        let boxed: Box<dyn WorkerPool> = Box::new(pool);
        let userdata = Box::into_raw(Box::new(boxed));
        let table = WorkerPoolFnTable {
            submit: pool_submit,
            wait_idle: pool_wait_idle,
            thread_count: pool_thread_count,
        };
        // SAFETY: the shim copies the table; `userdata` lives until drop.
        let handle =
            unsafe { whiteout_hostimpl_WorkerPool_create(userdata as *mut c_void, &table) };
        HostWorkerPool { handle, userdata }
    }

    /// Raw `interfaces::WorkerPool*`, for library calls that take one.
    pub fn as_ptr(&self) -> *mut c_void {
        self.handle
    }

    pub fn thread_count_through_native(&self) -> usize {
        // SAFETY: `handle` is live for `&self`.
        unsafe { whiteout_hostimpl_test_WorkerPool_threadCount(self.handle) }
    }

    pub fn wait_idle_through_native(&self) {
        // SAFETY: `handle` is live for `&self`.
        unsafe { whiteout_hostimpl_test_WorkerPool_waitIdle(self.handle) }
    }

    /// Submit a task from the C++ side that increments `sentinel`.
    ///
    /// This is how the tests prove a submitted task actually reached the
    /// Rust pool and ran: the closure lives in C++, so nothing but a real
    /// round trip can move the counter.
    pub fn submit_sentinel_through_native(&self, sentinel: &mut i32) {
        // SAFETY: `sentinel` outlives the call, and the task runs before
        // `waitIdle` returns.
        unsafe {
            whiteout_hostimpl_test_WorkerPool_submitIncrementSentinel(self.handle, sentinel);
        }
    }
}

impl Drop for HostWorkerPool {
    fn drop(&mut self) {
        // SAFETY: both pointers were produced in `new` and are freed once.
        unsafe {
            whiteout_hostimpl_WorkerPool_delete(self.handle);
            drop(Box::from_raw(self.userdata));
        }
    }
}

impl core::fmt::Debug for HostWorkerPool {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("HostWorkerPool").finish_non_exhaustive()
    }
}

// SAFETY: as `HostFileSystem`.
unsafe impl Send for HostWorkerPool {}
unsafe impl Sync for HostWorkerPool {}
