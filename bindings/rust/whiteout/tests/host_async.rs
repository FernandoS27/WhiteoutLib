// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// The two callback-bearing interfaces: HttpHandler and WorkerPool.
//
// Both hand a one-shot resource across the boundary — an HTTP callback and
// a task closure — and both must survive a handler that never uses it.
// That is the interesting part, and what these tests pin down.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use whiteout::interfaces::{
    http_capability, HostHttpHandler, HostWorkerPool, HttpHandler, HttpResponder, WorkerPool,
    WorkerTask,
};

// ── HttpHandler ───────────────────────────────────────────────────────────

/// Replies immediately from inside the call.
struct EchoHandler {
    body: Vec<u8>,
    status: i32,
}

impl HttpHandler for EchoHandler {
    fn capabilities(&self) -> u32 {
        http_capability::HTTP2_MULTIPLEXING
    }

    fn get(&self, _url: &str, responder: HttpResponder) {
        responder.respond(self.status, &self.body);
    }

    fn get_range(&self, _url: &str, start: u64, end: u64, responder: HttpResponder) {
        let len = (end - start + 1) as usize;
        let slice: Vec<u8> = self
            .body
            .iter()
            .copied()
            .skip(start as usize)
            .take(len)
            .collect();
        responder.respond(206, &slice);
    }
}

#[test]
fn capabilities_reach_cpp() {
    let h = HostHttpHandler::new(EchoHandler {
        body: Vec::new(),
        status: 200,
    });
    assert_eq!(
        h.capabilities_through_native(),
        http_capability::HTTP2_MULTIPLEXING
    );
}

#[test]
fn a_synchronous_reply_reaches_the_caller() {
    let h = HostHttpHandler::new(EchoHandler {
        body: b"hello cdn".to_vec(),
        status: 200,
    });
    let out = h.get_through_native("http://example/x");
    assert_eq!(out.status, 200);
    assert_eq!(out.body, b"hello cdn");
    assert!(out.error.is_empty());
}

#[test]
fn range_requests_carry_their_bounds() {
    let h = HostHttpHandler::new(EchoHandler {
        body: (0u8..32).collect(),
        status: 200,
    });
    let out = h.get_range_through_native("http://example/x", 4, 7);
    assert_eq!(out.status, 206);
    assert_eq!(out.body, vec![4, 5, 6, 7]);
}

/// Reports a transport failure instead of a response.
struct FailingHandler;

impl HttpHandler for FailingHandler {
    fn get(&self, _url: &str, responder: HttpResponder) {
        responder.fail("connection refused");
    }
    fn get_range(&self, _url: &str, _s: u64, _e: u64, responder: HttpResponder) {
        responder.fail("connection refused");
    }
}

#[test]
fn a_failure_reply_carries_its_message() {
    let h = HostHttpHandler::new(FailingHandler);
    let out = h.get_through_native("http://example/x");
    assert_eq!(out.error, "connection refused");
    assert!(out.body.is_empty());
}

/// Drops the responder without ever replying.
struct SilentHandler;

impl HttpHandler for SilentHandler {
    fn get(&self, _url: &str, _responder: HttpResponder) {
        // Deliberately drops it.
    }
    fn get_range(&self, _url: &str, _s: u64, _e: u64, _responder: HttpResponder) {}
}

#[test]
fn dropping_a_responder_cancels_rather_than_hanging() {
    // The C++ side waits for its callback to fire exactly once. A handler
    // that forgets would otherwise deadlock the library; `HttpResponder`'s
    // Drop turns that into a clean transport error.
    let h = HostHttpHandler::new(SilentHandler);
    let out = h.get_through_native("http://example/x");
    assert!(
        !out.error.is_empty(),
        "a dropped responder must surface as an error"
    );
}

/// Panics before replying — the responder is dropped by unwinding.
struct PanickingHandler;

impl HttpHandler for PanickingHandler {
    fn get(&self, _url: &str, _responder: HttpResponder) {
        panic!("deliberate panic inside get");
    }
    fn get_range(&self, _url: &str, _s: u64, _e: u64, _responder: HttpResponder) {
        panic!("deliberate panic inside get_range");
    }
}

#[test]
fn a_panicking_handler_is_contained_and_still_replies() {
    let h = HostHttpHandler::new(PanickingHandler);
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let out = h.get_through_native("http://example/x");
    std::panic::set_hook(prev);

    // The panic is contained, and unwinding drops the responder, which
    // cancels — so the C++ caller gets an error instead of waiting forever.
    assert!(!out.error.is_empty());
}

/// Replies from a worker thread, joined before returning.
struct DeferredHandler;

impl HttpHandler for DeferredHandler {
    fn get(&self, _url: &str, responder: HttpResponder) {
        // `HttpResponder: Send`, so a handler may move it to another
        // thread. It is joined before returning because the *test invoker*
        // captures its response into a stack local — real library call
        // sites own the callback properly and may be replied to later.
        // See `HostHttpHandler::get_through_native`.
        std::thread::spawn(move || {
            responder.respond(200, b"from another thread");
        })
        .join()
        .unwrap();
    }
    fn get_range(&self, _url: &str, _s: u64, _e: u64, responder: HttpResponder) {
        responder.respond(206, b"");
    }
}

#[test]
fn a_responder_can_cross_threads() {
    let h = HostHttpHandler::new(DeferredHandler);
    let out = h.get_through_native("http://example/x");
    assert_eq!(out.status, 200);
    assert_eq!(out.body, b"from another thread");
}

// ── WorkerPool ────────────────────────────────────────────────────────────

/// Runs everything inline on the submitting thread.
#[derive(Default)]
struct InlinePool {
    ran: AtomicUsize,
}

impl WorkerPool for InlinePool {
    fn submit(&self, task: WorkerTask) {
        self.ran.fetch_add(1, Ordering::SeqCst);
        task.run();
    }
    fn wait_idle(&self) {}
    fn thread_count(&self) -> usize {
        1
    }
}

#[test]
fn thread_count_reaches_cpp() {
    let p = HostWorkerPool::new(InlinePool::default());
    assert_eq!(p.thread_count_through_native(), 1);
}

#[test]
fn a_submitted_task_actually_runs() {
    // The closure lives in C++ — only a real round trip through the Rust
    // pool and back can move this counter.
    let pool = HostWorkerPool::new(InlinePool::default());
    let mut sentinel = 0i32;
    pool.submit_sentinel_through_native(&mut sentinel);
    pool.wait_idle_through_native();
    assert_eq!(sentinel, 1, "the C++ task never ran");
}

/// Defers work to a real thread, joining on `wait_idle`.
#[derive(Default)]
struct ThreadedPool {
    pending: Mutex<Vec<std::thread::JoinHandle<()>>>,
}

impl WorkerPool for ThreadedPool {
    fn submit(&self, task: WorkerTask) {
        // `WorkerTask: Send`, which is what lets a pool defer it.
        let h = std::thread::spawn(move || task.run());
        self.pending.lock().unwrap().push(h);
    }
    fn wait_idle(&self) {
        for h in self.pending.lock().unwrap().drain(..) {
            let _ = h.join();
        }
    }
    fn thread_count(&self) -> usize {
        4
    }
}

#[test]
fn a_task_can_run_on_another_thread() {
    let pool = HostWorkerPool::new(ThreadedPool::default());
    assert_eq!(pool.thread_count_through_native(), 4);

    let mut sentinel = 0i32;
    pool.submit_sentinel_through_native(&mut sentinel);
    pool.wait_idle_through_native();
    assert_eq!(sentinel, 1, "the deferred task never ran");
}

/// Drops every task without running it.
struct DroppingPool;

impl WorkerPool for DroppingPool {
    fn submit(&self, _task: WorkerTask) {
        // Deliberately drops it — cancelling the C++ closure.
    }
    fn wait_idle(&self) {}
    fn thread_count(&self) -> usize {
        1
    }
}

#[test]
fn dropping_a_task_cancels_it_cleanly() {
    // Cancelling leaks nothing and does not crash; the work simply never
    // happens, which the sentinel records.
    let pool = HostWorkerPool::new(DroppingPool);
    let mut sentinel = 0i32;
    pool.submit_sentinel_through_native(&mut sentinel);
    pool.wait_idle_through_native();
    assert_eq!(sentinel, 0, "a dropped task must not run");
}

#[test]
fn a_panicking_pool_is_contained() {
    struct PanickingPool;
    impl WorkerPool for PanickingPool {
        fn submit(&self, _task: WorkerTask) {
            panic!("deliberate panic inside submit");
        }
        fn wait_idle(&self) {}
        fn thread_count(&self) -> usize {
            1
        }
    }

    let pool = HostWorkerPool::new(PanickingPool);
    let mut sentinel = 0i32;
    let prev = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    pool.submit_sentinel_through_native(&mut sentinel);
    std::panic::set_hook(prev);

    assert_eq!(sentinel, 0);
    // Still usable — the boundary is not poisoned.
    assert_eq!(pool.thread_count_through_native(), 1);
}

#[test]
fn many_pools_and_handlers_drop_cleanly() {
    for _ in 0..128 {
        let p = Arc::new(HostWorkerPool::new(InlinePool::default()));
        let h = HostHttpHandler::new(EchoHandler {
            body: b"x".to_vec(),
            status: 200,
        });
        assert_eq!(p.thread_count_through_native(), 1);
        assert_eq!(h.get_through_native("u").status, 200);
    }
}

// ── End-to-end: a Rust pool driving real library work ─────────────────────

#[test]
fn the_library_submits_real_work_to_a_rust_pool() {
    use whiteout::textures::{PixelFormat, Texture};

    /// Counts what the library hands it, then runs each task inline.
    struct CountingPool(Arc<AtomicUsize>);

    impl WorkerPool for CountingPool {
        fn submit(&self, task: WorkerTask) {
            self.0.fetch_add(1, Ordering::SeqCst);
            task.run();
        }
        fn wait_idle(&self) {}
        fn thread_count(&self) -> usize {
            4
        }
    }

    let counter = Arc::new(AtomicUsize::new(0));
    let pool = HostWorkerPool::new(CountingPool(Arc::clone(&counter)));

    // BCn encoding parallelises across the pool when one is supplied.
    let mut tex = Texture::create_2d(PixelFormat::RGBA8, 256, 256, 1).expect("create failed");
    tex.data_mut().fill(0x7F);

    let converted = tex
        .copy_as_format(PixelFormat::BC1, Some(&pool))
        .expect("conversion failed");
    assert_eq!(converted.format(), PixelFormat::BC1);

    // This is the end-to-end check that a pool parameter reaches the
    // library at all: before it was wired through, every call site passed
    // null and this count could only ever be zero.
    assert!(
        counter.load(Ordering::SeqCst) > 0,
        "the library never submitted work to the Rust pool"
    );
}
