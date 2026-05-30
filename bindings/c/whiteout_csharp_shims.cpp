// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Hand-written C# trampoline shims. Mirrors the Java JNI bridges under
// `bindings/java/jni/` but using a function-pointer table + `void* userdata`
// shape, so the C# side can plug in `[UnmanagedCallersOnly]` static methods
// and route through `GCHandle` instead of the JNI env/jobject pair.
//
// Compiled into the `whiteout_c` target; non-C# consumers see the extra
// symbols but never invoke them (the factories are only called from the
// managed Whiteout.Host base classes).

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

struct CsharpVfsFnTable {
    void    (*readFile)   (void* userdata, const char* path, std::size_t path_len,
                           std::uint8_t** out_data, std::size_t* out_size);
    void    (*freeBuffer) (std::uint8_t* data);
    std::int32_t (*writeFile)  (void* userdata, const char* path, std::size_t path_len,
                                 const std::uint8_t* data, std::size_t data_size);
    std::int32_t (*fileExists) (void* userdata, const char* path, std::size_t path_len);
};

class CsharpVirtualPathFileSystem : public VirtualPathFileSystem {
public:
    CsharpVirtualPathFileSystem(void* userdata, const CsharpVfsFnTable* fns)
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
    CsharpVfsFnTable fns_;
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

struct CsharpHttpFnTable {
    std::uint32_t (*capabilities)  (void* userdata);
    void          (*getAsync)      (void* userdata, const char* url, std::size_t url_len,
                                    void* callback_handle);
    void          (*getRangeAsync) (void* userdata, const char* url, std::size_t url_len,
                                    std::uint64_t start, std::uint64_t end,
                                    void* callback_handle);
};

class CsharpHttpHandler : public HttpHandler {
public:
    CsharpHttpHandler(void* userdata, const CsharpHttpFnTable* fns)
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
    CsharpHttpFnTable fns_;
};

}  // namespace

extern "C" {

void* whiteout_csharp_VirtualPathFileSystem_create(void* userdata,
                                                   const CsharpVfsFnTable* fns) {
    return new CsharpVirtualPathFileSystem(userdata, fns);
}

void whiteout_csharp_VirtualPathFileSystem_delete(void* handle) {
    delete reinterpret_cast<CsharpVirtualPathFileSystem*>(handle);
}

void* whiteout_csharp_HttpHandler_create(void* userdata, const CsharpHttpFnTable* fns) {
    return new CsharpHttpHandler(userdata, fns);
}

void whiteout_csharp_HttpHandler_delete(void* handle) {
    delete reinterpret_cast<CsharpHttpHandler*>(handle);
}

/// Fire the C++ HttpCallback the managed side was handed. Single-shot —
/// the std::function is destroyed after firing, so calling fire twice on
/// the same handle is undefined behaviour and the callers must not retry.
/// Mirrors the JNI `HttpResponseConsumer._fire` semantics.
void whiteout_csharp_HttpResponseCallback_fire(
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
void whiteout_csharp_HttpResponseCallback_cancel(void* callback_handle) {
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

whiteout_Bytes whiteout_csharp_test_VirtualPathFileSystem_readFile(
        void* handle, const char* path) {
    auto* vfs = reinterpret_cast<VirtualPathFileSystem*>(handle);
    auto data = vfs->readFile(std::string(path));
    if (data.empty()) {
        return whiteout_Bytes{nullptr, 0, nullptr};
    }
    auto* owned = new std::vector<u8>(std::move(data));
    return whiteout_Bytes{owned->data(), owned->size(), owned};
}

std::int32_t whiteout_csharp_test_VirtualPathFileSystem_fileExists(
        void* handle, const char* path) {
    auto* vfs = reinterpret_cast<VirtualPathFileSystem*>(handle);
    return vfs->fileExists(std::string(path)) ? 1 : 0;
}

std::int32_t whiteout_csharp_test_VirtualPathFileSystem_writeFile(
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

std::uint32_t whiteout_csharp_test_HttpHandler_capabilities(void* handle) {
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

void whiteout_csharp_test_HttpHandler_getAsync(
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

void whiteout_csharp_test_HttpHandler_getRangeAsync(
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

struct CsharpWorkerTaskFlat {
    void*        fn_handle;        // heap std::function<void()>*
    void*        wait_semaphore;   // TimelineSemaphore* or null
    std::uint64_t wait_value;
    void*        signal_semaphore; // TimelineSemaphore* or null
    std::uint64_t signal_value;
};

struct CsharpWorkerPoolFnTable {
    void        (*submit)      (void* userdata, const CsharpWorkerTaskFlat* task);
    void        (*waitIdle)    (void* userdata);
    std::size_t (*threadCount) (void* userdata);
};

class CsharpWorkerPool : public WorkerPool {
public:
    CsharpWorkerPool(void* userdata, const CsharpWorkerPoolFnTable* fns)
        : userdata_(userdata), fns_(*fns) {}

    void submit(const WorkerTask& task) override {
        auto* fnHeap = new std::function<void()>(task.fn);
        CsharpWorkerTaskFlat flat{
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
    CsharpWorkerPoolFnTable fns_;
};

}  // namespace

extern "C" {

void* whiteout_csharp_WorkerPool_create(void* userdata, const CsharpWorkerPoolFnTable* fns) {
    return new CsharpWorkerPool(userdata, fns);
}

void whiteout_csharp_WorkerPool_delete(void* handle) {
    delete reinterpret_cast<CsharpWorkerPool*>(handle);
}

/// Fire the C++ task function exactly once, then delete the heap
/// std::function. Calling twice on the same handle is undefined.
void whiteout_csharp_WorkerTaskFn_fire(void* fn_handle) {
    auto* fn = reinterpret_cast<std::function<void()>*>(fn_handle);
    if (fn == nullptr) return;
    (*fn)();
    delete fn;
}

/// Discard the task function without firing it. Library code that
/// submitted the task will deadlock if a signal semaphore was attached and
/// never gets signalled — managed pools should prefer fire() and let any
/// signal semaphore in the WorkerTask flat struct get signalled after.
void whiteout_csharp_WorkerTaskFn_cancel(void* fn_handle) {
    auto* fn = reinterpret_cast<std::function<void()>*>(fn_handle);
    if (fn == nullptr) return;
    delete fn;
}

/// Block until the timeline semaphore reaches `value`. Named "_await" on
/// the C# side to avoid the `wait` C# keyword collision; routes to the
/// C++ `TimelineSemaphore::wait()` virtual.
void whiteout_csharp_TimelineSemaphore_await(void* sem_handle, std::uint64_t value) {
    auto* sem = reinterpret_cast<TimelineSemaphore*>(sem_handle);
    if (sem == nullptr) return;
    sem->wait(value);
}

/// Signal the timeline semaphore with `value`.
void whiteout_csharp_TimelineSemaphore_signal(void* sem_handle, std::uint64_t value) {
    auto* sem = reinterpret_cast<TimelineSemaphore*>(sem_handle);
    if (sem == nullptr) return;
    sem->signal(value);
}

// ── WorkerPool smoke-test invokers ───────────────────────────────────────

std::size_t whiteout_csharp_test_WorkerPool_threadCount(void* handle) {
    auto* pool = reinterpret_cast<WorkerPool*>(handle);
    return pool->threadCount();
}

void whiteout_csharp_test_WorkerPool_waitIdle(void* handle) {
    auto* pool = reinterpret_cast<WorkerPool*>(handle);
    pool->waitIdle();
}

/// Submit a task to the managed pool whose only job is to set a sentinel.
/// The test then verifies the sentinel was set after waitIdle() returns,
/// proving submit() reached the managed implementation and the task ran.
/// `out_sentinel` is incremented by the task.
void whiteout_csharp_test_WorkerPool_submitIncrementSentinel(
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
