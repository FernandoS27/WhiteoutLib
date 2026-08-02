// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Hand-written host-implementation trampolines: a function-pointer table
// plus a `void* userdata`, letting a host language subclass the abstract
// interfaces in <whiteout/interfaces.h> and have C++ call back into it.
//
// Originally written for C# (hence the `whiteout_csharp_*` aliases at the
// bottom of this file, kept so the existing P/Invoke signatures still
// resolve). The mechanism is language-neutral, and Rust consumes it
// directly: a `Box<dyn Trait>` becomes the userdata and `extern "C"`
// thunks fill the table. Java does not use this path — it has its own JNI
// bridges under `bindings/java/jni/`.
//
// Compiled into the `whiteout_c` target; consumers that never call the
// factories simply never trigger these symbols.

#include <whiteout/interfaces.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "whiteout_c_common.h"

namespace {

using whiteout::u8;
using whiteout::interfaces::DirectoryEntry;
using whiteout::interfaces::VirtualPathFileSystem;

// ── VirtualPathFileSystem ─────────────────────────────────────────────────

struct HostVfsFnTable {
    void    (*readFile)   (void* userdata, const char* path, std::size_t path_len,
                           std::uint8_t** out_data, std::size_t* out_size);
    void    (*freeBuffer) (std::uint8_t* data);
    std::int32_t (*writeFile)  (void* userdata, const char* path, std::size_t path_len,
                                 const std::uint8_t* data, std::size_t data_size);
    std::int32_t (*fileExists) (void* userdata, const char* path, std::size_t path_len);
};

class HostVirtualPathFileSystem : public VirtualPathFileSystem {
public:
    HostVirtualPathFileSystem(void* userdata, const HostVfsFnTable* fns)
        : userdata_(userdata), fns_(*fns) {}

    std::vector<u8> readFile(const std::string& path) const override {
        std::uint8_t* data = nullptr;
        std::size_t   size = 0;
        fns_.readFile(userdata_, path.data(), path.size(), &data, &size);
        if (data == nullptr || size == 0) {
            // Even when the caller returned an empty buffer with non-null
            // pointer we'd want to free it — but the trampoline only
            // allocates on success, so a null `data` is the clean signal.
            return {};
        }
        std::vector<u8> result(data, data + size);
        fns_.freeBuffer(data);
        return result;
    }

    bool writeFile(const std::string& path, const std::vector<u8>& data) override {
        return fns_.writeFile(userdata_, path.data(), path.size(),
                              data.data(), data.size()) != 0;
    }

    bool fileExists(const std::string& path) const override {
        return fns_.fileExists(userdata_, path.data(), path.size()) != 0;
    }

    std::vector<DirectoryEntry> listDirectory(const std::string&) const override {
        // Not exposed across the trampoline yet — library code that wants
        // directory enumeration should fall back to file-by-file probing
        // via fileExists/readFile until the FnTable grows a listDirectory
        // entry.
        return {};
    }

private:
    void* userdata_;
    HostVfsFnTable fns_;
};

}  // namespace

// ── HttpHandler ───────────────────────────────────────────────────────────
//
// The async callback (`std::function<void(HttpResponse)>`) is heap-allocated
// on the C++ side and handed to the C# trampoline as an opaque pointer.
// When the C# handler has a response, it calls back through
// `HttpResponseCallback_fire`, which reconstructs an HttpResponse, fires the
// std::function exactly once, then deletes it.

using whiteout::u32;
using whiteout::u64;
using whiteout::i32;
using whiteout::interfaces::HttpCallback;
using whiteout::interfaces::HttpHandler;
using whiteout::interfaces::HttpResponse;

namespace {

struct HostHttpFnTable {
    std::uint32_t (*capabilities)  (void* userdata);
    void          (*getAsync)      (void* userdata, const char* url, std::size_t url_len,
                                    void* callback_handle);
    void          (*getRangeAsync) (void* userdata, const char* url, std::size_t url_len,
                                    std::uint64_t start, std::uint64_t end,
                                    void* callback_handle);
};

class HostHttpHandler : public HttpHandler {
public:
    HostHttpHandler(void* userdata, const HostHttpFnTable* fns)
        : userdata_(userdata), fns_(*fns) {}

    u32 capabilities() const noexcept override {
        return fns_.capabilities(userdata_);
    }

    void getAsync(const std::string& url, HttpCallback callback) override {
        auto* cb = new HttpCallback(std::move(callback));
        fns_.getAsync(userdata_, url.data(), url.size(), cb);
    }

    void getRangeAsync(const std::string& url, u64 start, u64 end,
                       HttpCallback callback) override {
        auto* cb = new HttpCallback(std::move(callback));
        fns_.getRangeAsync(userdata_, url.data(), url.size(), start, end, cb);
    }

private:
    void* userdata_;
    HostHttpFnTable fns_;
};

}  // namespace

extern "C" {

void* whiteout_hostimpl_VirtualPathFileSystem_create(void* userdata,
                                                   const HostVfsFnTable* fns) {
    return new HostVirtualPathFileSystem(userdata, fns);
}

void whiteout_hostimpl_VirtualPathFileSystem_delete(void* handle) {
    delete reinterpret_cast<HostVirtualPathFileSystem*>(handle);
}

void* whiteout_hostimpl_HttpHandler_create(void* userdata, const HostHttpFnTable* fns) {
    return new HostHttpHandler(userdata, fns);
}

void whiteout_hostimpl_HttpHandler_delete(void* handle) {
    delete reinterpret_cast<HostHttpHandler*>(handle);
}

/// Fire the C++ HttpCallback the managed side was handed. Single-shot —
/// the std::function is destroyed after firing, so calling fire twice on
/// the same handle is undefined behaviour and the callers must not retry.
/// Mirrors the JNI `HttpResponseConsumer._fire` semantics.
void whiteout_hostimpl_HttpResponseCallback_fire(
        void* callback_handle, std::int32_t status_code,
        const std::uint8_t* body, std::size_t body_len,
        const char* error) {
    auto* cb = reinterpret_cast<HttpCallback*>(callback_handle);
    if (cb == nullptr) return;
    HttpResponse response;
    response.statusCode = static_cast<i32>(status_code);
    if (body != nullptr && body_len > 0) {
        response.body.assign(body, body + body_len);
    }
    if (error != nullptr) {
        response.error = error;
    }
    (*cb)(std::move(response));
    delete cb;
}

/// Discard a callback without firing it. Used when the managed trampoline
/// can't deliver a response — e.g. the user's HttpHandler implementation
/// threw before invoking the supplied Action. Without this the C++ side
/// would leak the std::function and library code waiting on the callback
/// would hang forever.
void whiteout_hostimpl_HttpResponseCallback_cancel(void* callback_handle) {
    auto* cb = reinterpret_cast<HttpCallback*>(callback_handle);
    if (cb == nullptr) return;
    // Fire with a transport error so callers see a clean failure rather
    // than a hung wait.
    HttpResponse response;
    response.statusCode = 0;
    response.error = "callback cancelled (managed-side exception)";
    (*cb)(std::move(response));
    delete cb;
}

// ── Smoke-test invokers ──────────────────────────────────────────────────
// These let tests drive a managed VFS impl directly without spinning up an
// actual M2 parse. Mirrors the helpers in `bindings/java/jni/smoke_invokers.cpp`.

whiteout_Bytes whiteout_hostimpl_test_VirtualPathFileSystem_readFile(
        void* handle, const char* path) {
    auto* vfs = reinterpret_cast<VirtualPathFileSystem*>(handle);
    auto data = vfs->readFile(std::string(path));
    if (data.empty()) {
        return whiteout_Bytes{nullptr, 0, nullptr};
    }
    auto* owned = new std::vector<u8>(std::move(data));
    return whiteout_Bytes{owned->data(), owned->size(), owned};
}

std::int32_t whiteout_hostimpl_test_VirtualPathFileSystem_fileExists(
        void* handle, const char* path) {
    auto* vfs = reinterpret_cast<VirtualPathFileSystem*>(handle);
    return vfs->fileExists(std::string(path)) ? 1 : 0;
}

std::int32_t whiteout_hostimpl_test_VirtualPathFileSystem_writeFile(
        void* handle, const char* path, const std::uint8_t* data, std::size_t size) {
    auto* vfs = reinterpret_cast<VirtualPathFileSystem*>(handle);
    std::vector<u8> v(data, data + size);
    return vfs->writeFile(std::string(path), v) ? 1 : 0;
}

// ── HttpHandler smoke-test invokers ──────────────────────────────────────
//
// Fire getAsync / getRangeAsync on a managed handler, capture the response
// synchronously, and return its status / body / error. Body is wrapped in
// whiteout_Bytes so the caller frees via the existing whiteout_Bytes_free.
// Error string lives inside a heap std::string and is freed via
// whiteout_CString_free.

std::uint32_t whiteout_hostimpl_test_HttpHandler_capabilities(void* handle) {
    auto* h = reinterpret_cast<HttpHandler*>(handle);
    return static_cast<std::uint32_t>(h->capabilities());
}

namespace {

struct CapturedHttpResponse {
    std::int32_t statusCode = 0;
    std::vector<u8> body;
    std::string error;
};

void pack_response(const CapturedHttpResponse& captured,
                   std::int32_t* out_status,
                   whiteout_Bytes* out_body,
                   whiteout_CString* out_error) {
    *out_status = captured.statusCode;
    if (!captured.body.empty()) {
        auto* ownedBody = new std::vector<u8>(captured.body);
        out_body->data   = ownedBody->data();
        out_body->size   = ownedBody->size();
        out_body->_owner = ownedBody;
    } else {
        out_body->data = nullptr;
        out_body->size = 0;
        out_body->_owner = nullptr;
    }
    if (!captured.error.empty()) {
        auto* ownedErr = new std::string(captured.error);
        out_error->chars  = ownedErr->c_str();
        out_error->length = ownedErr->size();
        out_error->_owner = ownedErr;
    } else {
        out_error->chars  = nullptr;
        out_error->length = 0;
        out_error->_owner = nullptr;
    }
}

}  // namespace

void whiteout_hostimpl_test_HttpHandler_getAsync(
        void* handle, const char* url,
        std::int32_t* out_status, whiteout_Bytes* out_body, whiteout_CString* out_error) {
    auto* h = reinterpret_cast<HttpHandler*>(handle);
    CapturedHttpResponse captured;
    h->getAsync(std::string(url), [&captured](HttpResponse r) {
        captured.statusCode = r.statusCode;
        captured.body  = std::move(r.body);
        captured.error = std::move(r.error);
    });
    pack_response(captured, out_status, out_body, out_error);
}

void whiteout_hostimpl_test_HttpHandler_getRangeAsync(
        void* handle, const char* url, std::uint64_t start, std::uint64_t end,
        std::int32_t* out_status, whiteout_Bytes* out_body, whiteout_CString* out_error) {
    auto* h = reinterpret_cast<HttpHandler*>(handle);
    CapturedHttpResponse captured;
    h->getRangeAsync(std::string(url), start, end, [&captured](HttpResponse r) {
        captured.statusCode = r.statusCode;
        captured.body  = std::move(r.body);
        captured.error = std::move(r.error);
    });
    pack_response(captured, out_status, out_body, out_error);
}

}  // extern "C"

// ── WorkerPool ─────────────────────────────────────────────────────────────
//
// The C++ submit() receives a WorkerTask containing a std::function<void()>
// plus two optional TimelineSemaphore* (wait/signal) with values. We
// flatten this into a POD struct the C# trampoline can read directly, and
// surface fn-firing + semaphore wait/signal via separate C ABI helpers
// so the C# WorkerTask wrapper can drive them when the managed pool
// runs the task.

using whiteout::interfaces::TimelineSemaphore;
using whiteout::interfaces::WorkerPool;
using whiteout::interfaces::WorkerTask;

namespace {

struct HostWorkerTaskFlat {
    void*        fn_handle;        // heap std::function<void()>*
    void*        wait_semaphore;   // TimelineSemaphore* or null
    std::uint64_t wait_value;
    void*        signal_semaphore; // TimelineSemaphore* or null
    std::uint64_t signal_value;
};

struct HostWorkerPoolFnTable {
    void        (*submit)      (void* userdata, const HostWorkerTaskFlat* task);
    void        (*waitIdle)    (void* userdata);
    std::size_t (*threadCount) (void* userdata);
};

class HostWorkerPool : public WorkerPool {
public:
    HostWorkerPool(void* userdata, const HostWorkerPoolFnTable* fns)
        : userdata_(userdata), fns_(*fns) {}

    void submit(const WorkerTask& task) override {
        auto* fnHeap = new std::function<void()>(task.fn);
        HostWorkerTaskFlat flat{
            fnHeap,
            task.waitSemaphore,   task.waitValue,
            task.signalSemaphore, task.signalValue,
        };
        fns_.submit(userdata_, &flat);
    }

    void waitIdle() override {
        fns_.waitIdle(userdata_);
    }

    std::size_t threadCount() const noexcept override {
        return fns_.threadCount(userdata_);
    }

private:
    void* userdata_;
    HostWorkerPoolFnTable fns_;
};

}  // namespace

extern "C" {

void* whiteout_hostimpl_WorkerPool_create(void* userdata, const HostWorkerPoolFnTable* fns) {
    return new HostWorkerPool(userdata, fns);
}

void whiteout_hostimpl_WorkerPool_delete(void* handle) {
    delete reinterpret_cast<HostWorkerPool*>(handle);
}

/// Fire the C++ task function exactly once, then delete the heap
/// std::function. Calling twice on the same handle is undefined.
void whiteout_hostimpl_WorkerTaskFn_fire(void* fn_handle) {
    auto* fn = reinterpret_cast<std::function<void()>*>(fn_handle);
    if (fn == nullptr) return;
    (*fn)();
    delete fn;
}

/// Discard the task function without firing it. Library code that
/// submitted the task will deadlock if a signal semaphore was attached and
/// never gets signalled — managed pools should prefer fire() and let any
/// signal semaphore in the WorkerTask flat struct get signalled after.
void whiteout_hostimpl_WorkerTaskFn_cancel(void* fn_handle) {
    auto* fn = reinterpret_cast<std::function<void()>*>(fn_handle);
    if (fn == nullptr) return;
    delete fn;
}

/// Block until the timeline semaphore reaches `value`. Named "_await" on
/// the C# side to avoid the `wait` C# keyword collision; routes to the
/// C++ `TimelineSemaphore::wait()` virtual.
void whiteout_hostimpl_TimelineSemaphore_await(void* sem_handle, std::uint64_t value) {
    auto* sem = reinterpret_cast<TimelineSemaphore*>(sem_handle);
    if (sem == nullptr) return;
    sem->wait(value);
}

/// Signal the timeline semaphore with `value`.
void whiteout_hostimpl_TimelineSemaphore_signal(void* sem_handle, std::uint64_t value) {
    auto* sem = reinterpret_cast<TimelineSemaphore*>(sem_handle);
    if (sem == nullptr) return;
    sem->signal(value);
}

// ── WorkerPool smoke-test invokers ───────────────────────────────────────

std::size_t whiteout_hostimpl_test_WorkerPool_threadCount(void* handle) {
    auto* pool = reinterpret_cast<WorkerPool*>(handle);
    return pool->threadCount();
}

void whiteout_hostimpl_test_WorkerPool_waitIdle(void* handle) {
    auto* pool = reinterpret_cast<WorkerPool*>(handle);
    pool->waitIdle();
}

/// Submit a task to the managed pool whose only job is to set a sentinel.
/// The test then verifies the sentinel was set after waitIdle() returns,
/// proving submit() reached the managed implementation and the task ran.
/// `out_sentinel` is incremented by the task.
void whiteout_hostimpl_test_WorkerPool_submitIncrementSentinel(
        void* handle, std::int32_t* out_sentinel) {
    auto* pool = reinterpret_cast<WorkerPool*>(handle);
    WorkerTask task;
    task.fn = [out_sentinel]() {
        if (out_sentinel != nullptr) {
            (*out_sentinel)++;
        }
    };
    pool->submit(task);
}

}  // extern "C"

// ── CascFileSystem ─────────────────────────────────────────────────────────
//
// Resolves files by numeric data ID rather than by path. Needed by the M2
// parser and by CASC-backed asset loading.

using whiteout::interfaces::CascFileSystem;

namespace {

struct HostCascFsFnTable {
    void (*readFile)(void* userdata, std::uint32_t fileId,
                     std::uint8_t** out_data, std::size_t* out_size);
    void (*freeBuffer)(std::uint8_t* data);
    // Returns 1 and writes *out_id when the path resolves; 0 otherwise.
    std::int32_t (*reserveFileId)(void* userdata, const char* path,
                                  std::size_t path_len, std::uint32_t* out_id);
    std::int32_t (*writeFile)(void* userdata, std::uint32_t fileId,
                              const std::uint8_t* data, std::size_t size);
    std::int32_t (*fileExists)(void* userdata, std::uint32_t fileId);
};

class HostCascFileSystem : public CascFileSystem {
public:
    HostCascFileSystem(void* userdata, const HostCascFsFnTable* fns)
        : userdata_(userdata), fns_(*fns) {}

    std::vector<u8> readFile(u32 fileId) const override {
        std::uint8_t* data = nullptr;
        std::size_t   size = 0;
        fns_.readFile(userdata_, fileId, &data, &size);
        if (data == nullptr || size == 0) return {};
        std::vector<u8> result(data, data + size);
        fns_.freeBuffer(data);
        return result;
    }

    std::optional<u32> reserveFileId(const std::string& path) override {
        std::uint32_t id = 0;
        if (fns_.reserveFileId(userdata_, path.data(), path.size(), &id) == 0) {
            return std::nullopt;
        }
        return id;
    }

    bool writeFile(u32 fileId, const std::vector<u8>& data) override {
        return fns_.writeFile(userdata_, fileId, data.data(), data.size()) != 0;
    }

    bool fileExists(u32 fileId) const override {
        return fns_.fileExists(userdata_, fileId) != 0;
    }

private:
    void* userdata_;
    HostCascFsFnTable fns_;
};

}  // namespace

extern "C" {

void* whiteout_hostimpl_CascFileSystem_create(void* userdata,
                                              const HostCascFsFnTable* fns) {
    return new HostCascFileSystem(userdata, fns);
}

void whiteout_hostimpl_CascFileSystem_delete(void* handle) {
    delete reinterpret_cast<HostCascFileSystem*>(handle);
}

// Smoke-test invokers, mirroring the VFS ones above.
whiteout_Bytes whiteout_hostimpl_test_CascFileSystem_readFile(void* handle,
                                                              std::uint32_t fileId) {
    auto* fs = reinterpret_cast<CascFileSystem*>(handle);
    auto data = fs->readFile(fileId);
    if (data.empty()) return whiteout_Bytes{nullptr, 0, nullptr};
    auto* owned = new std::vector<u8>(std::move(data));
    return whiteout_Bytes{owned->data(), owned->size(), owned};
}

std::int32_t whiteout_hostimpl_test_CascFileSystem_fileExists(void* handle,
                                                              std::uint32_t fileId) {
    return reinterpret_cast<CascFileSystem*>(handle)->fileExists(fileId) ? 1 : 0;
}

}  // extern "C"

// ── C# compatibility aliases ───────────────────────────────────────────────
//
// The trampolines above were originally named `whiteout_csharp_*`. The C#
// binding's P/Invoke declarations still use those names, so they stay as
// thin forwarders rather than forcing a lockstep change to a binding this
// refactor has no other reason to touch.

extern "C" {

void* whiteout_csharp_VirtualPathFileSystem_create(void* userdata, const void* fns) {
    return whiteout_hostimpl_VirtualPathFileSystem_create(
        userdata, reinterpret_cast<const HostVfsFnTable*>(fns));
}

void whiteout_csharp_VirtualPathFileSystem_delete(void* handle) {
    whiteout_hostimpl_VirtualPathFileSystem_delete(handle);
}

void* whiteout_csharp_HttpHandler_create(void* userdata, const void* fns) {
    return whiteout_hostimpl_HttpHandler_create(
        userdata, reinterpret_cast<const HostHttpFnTable*>(fns));
}

void whiteout_csharp_HttpHandler_delete(void* handle) {
    whiteout_hostimpl_HttpHandler_delete(handle);
}

void whiteout_csharp_HttpResponseCallback_fire(void* cb, std::int32_t status,
                                               const std::uint8_t* body,
                                               std::size_t body_len,
                                               const char* error) {
    whiteout_hostimpl_HttpResponseCallback_fire(cb, status, body, body_len, error);
}

void whiteout_csharp_HttpResponseCallback_cancel(void* cb) {
    whiteout_hostimpl_HttpResponseCallback_cancel(cb);
}

void* whiteout_csharp_WorkerPool_create(void* userdata, const void* fns) {
    return whiteout_hostimpl_WorkerPool_create(
        userdata, reinterpret_cast<const HostWorkerPoolFnTable*>(fns));
}

void whiteout_csharp_WorkerPool_delete(void* handle) {
    whiteout_hostimpl_WorkerPool_delete(handle);
}

void whiteout_csharp_WorkerTaskFn_fire(void* fn)   { whiteout_hostimpl_WorkerTaskFn_fire(fn); }
void whiteout_csharp_WorkerTaskFn_cancel(void* fn) { whiteout_hostimpl_WorkerTaskFn_cancel(fn); }

void whiteout_csharp_TimelineSemaphore_await(void* sem, std::uint64_t v) {
    whiteout_hostimpl_TimelineSemaphore_await(sem, v);
}

void whiteout_csharp_TimelineSemaphore_signal(void* sem, std::uint64_t v) {
    whiteout_hostimpl_TimelineSemaphore_signal(sem, v);
}

whiteout_Bytes whiteout_csharp_test_VirtualPathFileSystem_readFile(void* h, const char* p) {
    return whiteout_hostimpl_test_VirtualPathFileSystem_readFile(h, p);
}

std::int32_t whiteout_csharp_test_VirtualPathFileSystem_fileExists(void* h, const char* p) {
    return whiteout_hostimpl_test_VirtualPathFileSystem_fileExists(h, p);
}

std::int32_t whiteout_csharp_test_VirtualPathFileSystem_writeFile(
        void* h, const char* p, const std::uint8_t* d, std::size_t n) {
    return whiteout_hostimpl_test_VirtualPathFileSystem_writeFile(h, p, d, n);
}

std::uint32_t whiteout_csharp_test_HttpHandler_capabilities(void* h) {
    return whiteout_hostimpl_test_HttpHandler_capabilities(h);
}

void whiteout_csharp_test_HttpHandler_getAsync(
        void* h, const char* url, std::int32_t* s, whiteout_Bytes* b, whiteout_CString* e) {
    whiteout_hostimpl_test_HttpHandler_getAsync(h, url, s, b, e);
}

void whiteout_csharp_test_HttpHandler_getRangeAsync(
        void* h, const char* url, std::uint64_t start, std::uint64_t end,
        std::int32_t* s, whiteout_Bytes* b, whiteout_CString* e) {
    whiteout_hostimpl_test_HttpHandler_getRangeAsync(h, url, start, end, s, b, e);
}

std::size_t whiteout_csharp_test_WorkerPool_threadCount(void* h) {
    return whiteout_hostimpl_test_WorkerPool_threadCount(h);
}

void whiteout_csharp_test_WorkerPool_waitIdle(void* h) {
    whiteout_hostimpl_test_WorkerPool_waitIdle(h);
}

void whiteout_csharp_test_WorkerPool_submitIncrementSentinel(void* h, std::int32_t* out) {
    whiteout_hostimpl_test_WorkerPool_submitIncrementSentinel(h, out);
}

}  // extern "C"
