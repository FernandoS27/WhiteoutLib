// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Test-only invocation helpers for the JNI bridge smoke tests.
//
// The bridges produced by tools/codegen/emit_jni.py expose JavaXxx C++
// wrappers that derive from the matching interface and forward into Java.
// Production code reaches them indirectly (e.g. CASC online storage calls
// HttpHandler::getAsync internally). The smoke tests need to drive the
// vtable methods directly from Java without those library-side callers,
// so we expose flat C entry points here.
//
// Symbols are exported via WINDOWS_EXPORT_ALL_SYMBOLS in CMakeLists.txt;
// Java's smoke tests resolve them via Panama's `SymbolLookup.loaderLookup`.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <whiteout/interfaces.h>
#include <whiteout/utils/timeline_semaphore.h>

// ── Generic byte-bag for returning byte[] across the boundary ────────────
//
// Returned as a *heap pointer* (not by value) because Panama's
// downcallHandle injects an extra SegmentAllocator parameter for any
// struct-by-value return — keeping a pointer makes the Java side a plain
// `(long, ...) -> ptr` shape with no allocator threading.

struct SmokeBytes {
    std::uint8_t* data;
    std::int64_t  size;
};

namespace {
SmokeBytes* vecToSmokeBytes(const std::vector<std::uint8_t>& v) {
    auto* out = new SmokeBytes{nullptr, 0};
    if (!v.empty()) {
        out->data = new std::uint8_t[v.size()];
        std::memcpy(out->data, v.data(), v.size());
        out->size = static_cast<std::int64_t>(v.size());
    }
    return out;
}
} // namespace

extern "C" {

void whiteout_jni_smoke_bytes_free(SmokeBytes* b) {
    if (b == nullptr) return;
    delete[] b->data;
    delete b;
}

// Accessor for the smoke-test side (avoid layout-encoding the struct in
// Panama). Returns NULL when b is null or size is zero.
const std::uint8_t* whiteout_jni_smoke_bytes_data(const SmokeBytes* b) {
    return (b == nullptr || b->size == 0) ? nullptr : b->data;
}
std::int64_t whiteout_jni_smoke_bytes_size(const SmokeBytes* b) {
    return b == nullptr ? 0 : b->size;
}

// ── HttpHandler ─────────────────────────────────────────────────────────

whiteout::interfaces::HttpResponse* whiteout_jni_smoke_invoke_getAsync(
        std::int64_t handle, const char* url) {
    auto* h = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handle);
    auto* out = new whiteout::interfaces::HttpResponse();
    if (h == nullptr || url == nullptr) {
        out->error = "smoke_invoke_getAsync: null handle or url";
        return out;
    }
    h->getAsync(url, [out](whiteout::interfaces::HttpResponse r) {
        *out = std::move(r);
    });
    return out;
}

// Same as invoke_getAsync but waits up to `waitMs` for the callback to
// fire — needed when the Java side defers completion onto another
// thread (java.util.concurrent.Executor / CompletableFuture / etc.).
// Returns the captured response when the callback fires, an empty one
// (with no error) when the timeout elapses.
whiteout::interfaces::HttpResponse* whiteout_jni_smoke_invoke_getAsync_wait_ms(
        std::int64_t handle, const char* url, std::int32_t waitMs) {
    auto* h = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handle);
    auto* out = new whiteout::interfaces::HttpResponse();
    if (h == nullptr || url == nullptr) {
        out->error = "smoke_invoke_getAsync_wait_ms: null handle or url";
        return out;
    }
    auto state = std::make_shared<std::tuple<std::mutex, std::condition_variable, bool>>();
    h->getAsync(url, [out, state](whiteout::interfaces::HttpResponse r) {
        std::lock_guard<std::mutex> lk(std::get<0>(*state));
        *out = std::move(r);
        std::get<2>(*state) = true;
        std::get<1>(*state).notify_one();
    });
    std::unique_lock<std::mutex> lk(std::get<0>(*state));
    std::get<1>(*state).wait_for(lk, std::chrono::milliseconds(waitMs),
        [&] { return std::get<2>(*state); });
    return out;
}

whiteout::interfaces::HttpResponse* whiteout_jni_smoke_invoke_getRangeAsync(
        std::int64_t handle, const char* url,
        std::int64_t start, std::int64_t end) {
    auto* h = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handle);
    auto* out = new whiteout::interfaces::HttpResponse();
    if (h == nullptr || url == nullptr) {
        out->error = "smoke_invoke_getRangeAsync: null handle or url";
        return out;
    }
    h->getRangeAsync(url, static_cast<whiteout::u64>(start),
                     static_cast<whiteout::u64>(end),
                     [out](whiteout::interfaces::HttpResponse r) {
                         *out = std::move(r);
                     });
    return out;
}

std::int32_t whiteout_jni_smoke_invoke_capabilities(std::int64_t handle) {
    auto* h = reinterpret_cast<whiteout::interfaces::HttpHandler*>(handle);
    return h == nullptr ? 0 : static_cast<std::int32_t>(h->capabilities());
}

void whiteout_jni_smoke_response_free(whiteout::interfaces::HttpResponse* r) {
    delete r;
}

std::int32_t whiteout_jni_smoke_response_status(
        const whiteout::interfaces::HttpResponse* r) {
    return r == nullptr ? 0 : r->statusCode;
}

const std::uint8_t* whiteout_jni_smoke_response_body_ptr(
        const whiteout::interfaces::HttpResponse* r) {
    return (r == nullptr || r->body.empty()) ? nullptr : r->body.data();
}

std::int64_t whiteout_jni_smoke_response_body_len(
        const whiteout::interfaces::HttpResponse* r) {
    return r == nullptr ? 0 : static_cast<std::int64_t>(r->body.size());
}

const char* whiteout_jni_smoke_response_error(
        const whiteout::interfaces::HttpResponse* r) {
    return (r == nullptr || r->error.empty()) ? nullptr : r->error.c_str();
}

// ── VirtualPathFileSystem ──────────────────────────────────────────────

SmokeBytes* whiteout_jni_smoke_vpfs_readFile(std::int64_t handle, const char* path) {
    auto* fs = reinterpret_cast<whiteout::interfaces::VirtualPathFileSystem*>(handle);
    if (fs == nullptr || path == nullptr) return new SmokeBytes{nullptr, 0};
    return vecToSmokeBytes(fs->readFile(path));
}

std::int32_t whiteout_jni_smoke_vpfs_writeFile(std::int64_t handle,
                                               const char* path,
                                               const std::uint8_t* data,
                                               std::int64_t size) {
    auto* fs = reinterpret_cast<whiteout::interfaces::VirtualPathFileSystem*>(handle);
    if (fs == nullptr || path == nullptr) return 0;
    std::vector<std::uint8_t> v(data, data + size);
    return fs->writeFile(path, v) ? 1 : 0;
}

std::int32_t whiteout_jni_smoke_vpfs_fileExists(std::int64_t handle,
                                                const char* path) {
    auto* fs = reinterpret_cast<whiteout::interfaces::VirtualPathFileSystem*>(handle);
    if (fs == nullptr || path == nullptr) return 0;
    return fs->fileExists(path) ? 1 : 0;
}

// ── WorkerPool ─────────────────────────────────────────────────────────

void whiteout_jni_smoke_pool_waitIdle(std::int64_t handle) {
    auto* p = reinterpret_cast<whiteout::interfaces::WorkerPool*>(handle);
    if (p != nullptr) p->waitIdle();
}

std::int64_t whiteout_jni_smoke_pool_threadCount(std::int64_t handle) {
    auto* p = reinterpret_cast<whiteout::interfaces::WorkerPool*>(handle);
    return p == nullptr ? 0 : static_cast<std::int64_t>(p->threadCount());
}

// Submit a WorkerTask whose `fn` increments an atomic counter (the
// `flag` pointer below). The Java pool queues the runnable; the test
// polls the flag to confirm the task ran. Optional wait/signal
// semaphores honour the standard contract.
//
// `flagPtr` is a `std::atomic<int>*` allocated via
// whiteout_jni_smoke_atomic_new.
void whiteout_jni_smoke_pool_submit(std::int64_t poolHandle,
                                    std::int64_t flagPtr,
                                    std::int64_t waitSemHandle,
                                    std::int64_t waitValue,
                                    std::int64_t signalSemHandle,
                                    std::int64_t signalValue) {
    auto* pool = reinterpret_cast<whiteout::interfaces::WorkerPool*>(poolHandle);
    auto* flag = reinterpret_cast<std::atomic<int>*>(flagPtr);
    if (pool == nullptr || flag == nullptr) return;
    whiteout::interfaces::WorkerTask task;
    task.fn = [flag]() { flag->fetch_add(1, std::memory_order_acq_rel); };
    task.waitSemaphore = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(waitSemHandle);
    task.waitValue = static_cast<whiteout::u64>(waitValue);
    task.signalSemaphore = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(signalSemHandle);
    task.signalValue = static_cast<whiteout::u64>(signalValue);
    pool->submit(task);
}

// ── Atomic int helpers (test-side completion flags) ────────────────────

std::int64_t whiteout_jni_smoke_atomic_new() {
    return reinterpret_cast<std::int64_t>(new std::atomic<int>(0));
}

std::int32_t whiteout_jni_smoke_atomic_get(std::int64_t handle) {
    auto* a = reinterpret_cast<std::atomic<int>*>(handle);
    return a == nullptr ? 0 : a->load(std::memory_order_acquire);
}

void whiteout_jni_smoke_atomic_free(std::int64_t handle) {
    delete reinterpret_cast<std::atomic<int>*>(handle);
}

// ── TimelineSemaphore helpers ──────────────────────────────────────────

// Construct a fresh utils::TimelineSemaphore (concrete C++ impl). Tests
// hand this pointer to the JavaWorkerPool bridge so its submit override
// hands Java code a real, signal-able TimelineSemaphore handle.
std::int64_t whiteout_jni_smoke_sem_new() {
    return reinterpret_cast<std::int64_t>(new whiteout::utils::TimelineSemaphore());
}

void whiteout_jni_smoke_sem_free(std::int64_t handle) {
    delete reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(handle);
}

// Signal from C++ side — the test drives this to unblock a queued
// task that's waiting on the semaphore.
void whiteout_jni_smoke_sem_signal(std::int64_t handle, std::int64_t value) {
    auto* sem = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(handle);
    if (sem != nullptr) sem->signal(static_cast<whiteout::u64>(value));
}

std::int64_t whiteout_jni_smoke_sem_value(std::int64_t handle) {
    auto* sem = reinterpret_cast<whiteout::interfaces::TimelineSemaphore*>(handle);
    return sem == nullptr ? 0 : static_cast<std::int64_t>(sem->value());
}

// ── CascFileSystem ─────────────────────────────────────────────────────

SmokeBytes* whiteout_jni_smoke_casc_readFile(std::int64_t handle, std::int32_t fileId) {
    auto* fs = reinterpret_cast<whiteout::interfaces::CascFileSystem*>(handle);
    if (fs == nullptr) return new SmokeBytes{nullptr, 0};
    return vecToSmokeBytes(fs->readFile(static_cast<whiteout::u32>(fileId)));
}

// Returns -1 when reserveFileId returns nullopt, otherwise the resolved id.
std::int64_t whiteout_jni_smoke_casc_reserveFileId(std::int64_t handle,
                                                   const char* path) {
    auto* fs = reinterpret_cast<whiteout::interfaces::CascFileSystem*>(handle);
    if (fs == nullptr || path == nullptr) return -1;
    auto r = fs->reserveFileId(path);
    return r.has_value() ? static_cast<std::int64_t>(*r) : -1;
}

std::int32_t whiteout_jni_smoke_casc_writeFile(std::int64_t handle,
                                               std::int32_t fileId,
                                               const std::uint8_t* data,
                                               std::int64_t size) {
    auto* fs = reinterpret_cast<whiteout::interfaces::CascFileSystem*>(handle);
    if (fs == nullptr) return 0;
    std::vector<std::uint8_t> v(data, data + size);
    return fs->writeFile(static_cast<whiteout::u32>(fileId), v) ? 1 : 0;
}

std::int32_t whiteout_jni_smoke_casc_fileExists(std::int64_t handle,
                                                std::int32_t fileId) {
    auto* fs = reinterpret_cast<whiteout::interfaces::CascFileSystem*>(handle);
    if (fs == nullptr) return 0;
    return fs->fileExists(static_cast<whiteout::u32>(fileId)) ? 1 : 0;
}

} // extern "C"
