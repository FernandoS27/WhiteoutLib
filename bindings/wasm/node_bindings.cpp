// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// Node.js-only Embind additions.
//
// Compiled into the `whiteout_wasm_node` target only. The corresponding
// `whiteout.js`/`whiteout.wasm` web build excludes this TU and the host-
// only sources it depends on (utils::OsFileSystem, utils::SimpleThreadPool).
//
// Surface added on top of the shared `bindings.cpp`:
//
//   - OsFileSystem        — VirtualPathFileSystem backed by NODERAWFS disk
//                           access. Lets M2 parsers / MPQ / CASC reach the
//                           host filesystem without the in-memory copy step.
//   - SimpleThreadPool    — WorkerPool backed by std::thread (mapped onto
//                           Emscripten pthreads, which are worker_threads
//                           under Node). Used to parallelise mipmap/BC7
//                           generation and CASC/MPQ decode.
//   - HttpHandler trampoline — abstract base class subclassable from JS so
//                           Node code can wire up undici/https/axios as the
//                           CDN backend for online CASC.
//   - MPQ Storage::open(path) — explicit overload that takes an optional
//                           pool, enabling parallel decompression.
//   - CASC Storage::open / openOnline — gated on WHITEOUT_HAS_CASC.
//   - Texture::generateMipmapsWith(pool) — overload accepting the pool so
//                           BC7 / Kaiser filters run multi-threaded.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "optional_marshal.h"

#include <whiteout/common_types.h>
#include <whiteout/interfaces.h>
#include <whiteout/textures/texture.h>
#include <whiteout/utils/os_file_system.h>
#include <whiteout/utils/simple_thread_pool.h>

#if defined(WHITEOUT_HAS_MPQ)
#include <whiteout/storages/mpq/storage.h>
#endif

#if defined(WHITEOUT_HAS_CASC)
#include <whiteout/storages/casc/storage.h>
#endif

using namespace emscripten;
using namespace whiteout;

namespace {

// ── HttpHandler trampoline ───────────────────────────────────────────────
// Embind's `class_<...>::allow_subclass` machinery requires a wrapper that
// forwards C++ virtual calls into JS overrides. Calls happen on whichever
// thread invokes the handler; the JS side is responsible for marshalling
// the work back onto Node's main loop (e.g. by enqueueing work and
// resolving via MAIN_THREAD_EM_ASM_INT or just doing sync calls — pure-JS
// HTTP libraries dispatched from the main thread will Just Work because
// the WASM pool thread blocks waiting for the callback).
struct HttpHandlerWrapper : public wrapper<interfaces::HttpHandler> {
    EMSCRIPTEN_WRAPPER(HttpHandlerWrapper);

    u32 capabilities() const noexcept override {
        // Embind's `call<>` is non-const; const_cast keeps the interface
        // signature intact. The wrapper itself is mutable from JS's POV.
        return const_cast<HttpHandlerWrapper*>(this)
            ->call<u32>("capabilities");
    }

    void getAsync(const std::string& url,
                  interfaces::HttpCallback callback) override {
        // The callback is C++-only state; surface it as an opaque integer
        // handle for JS, which calls back via Module.dispatchHttpCallback.
        auto* cb = new interfaces::HttpCallback(std::move(callback));
        call<void>("getAsync", url,
                   reinterpret_cast<std::uintptr_t>(cb));
    }

    void getRangeAsync(const std::string& url, u64 start, u64 end,
                       interfaces::HttpCallback callback) override {
        auto* cb = new interfaces::HttpCallback(std::move(callback));
        call<void>("getRangeAsync", url,
                   static_cast<double>(start), static_cast<double>(end),
                   reinterpret_cast<std::uintptr_t>(cb));
    }
};

// Invoked by JS once it has assembled an HttpResponse. Consumes (and frees)
// the callback handle previously handed out.
void dispatchHttpCallback(std::uintptr_t handle, i32 statusCode,
                          const val& bodyArray, const std::string& error) {
    std::unique_ptr<interfaces::HttpCallback> cb(
        reinterpret_cast<interfaces::HttpCallback*>(handle));
    if (!cb || !*cb) return;
    interfaces::HttpResponse resp;
    resp.statusCode = statusCode;
    resp.error = error;
    if (!bodyArray.isNull() && !bodyArray.isUndefined()) {
        resp.body = convertJSArrayToNumberVector<u8>(bodyArray);
    }
    (*cb)(std::move(resp));
}

// ── Texture: pool-aware mipmap generation ────────────────────────────────

std::string generateMipmapsWithPool(textures::Texture& t,
                                    interfaces::WorkerPool* pool) {
    auto err = t.generateMipmaps(pool);
    return err.value_or(std::string{});
}

std::string generateMipmapsWithCountAndPool(textures::Texture& t,
                                            u32 newMipCount,
                                            interfaces::WorkerPool* pool) {
    auto err = t.generateMipmaps(newMipCount, pool);
    return err.value_or(std::string{});
}

// ── MPQ / CASC openers with optional pool ────────────────────────────────

#if defined(WHITEOUT_HAS_MPQ)
storages::mpq::Storage* mpqOpenWithPool(const std::string& path,
                                        interfaces::WorkerPool* pool) {
    return wasm::to_optional_ptr<storages::mpq::Storage>(
        storages::mpq::Storage::open(path, pool));
}

// `Storage::save(path)` is the persist-to-named-file overload; codegen
// only emits the no-arg version. Free function here gives JS access
// without re-registering the class.
bool mpqSaveStorageToPath(storages::mpq::Storage& self,
                          const std::string& path) {
    return self.save(path);
}
#endif

#if defined(WHITEOUT_HAS_CASC)
storages::casc::Storage* cascOpenWithPool(const std::string& path,
                                          interfaces::WorkerPool* pool) {
    return wasm::to_optional_ptr<storages::casc::Storage>(
        storages::casc::Storage::open(path, pool));
}
#endif

} // namespace

EMSCRIPTEN_BINDINGS(whiteout_node) {
    // ── OsFileSystem ─────────────────────────────────────────────────────
    // Bound as a subclass of VirtualPathFileSystem (registered in the
    // shared bindings.cpp). Constructible from a host path; subsequent
    // readFile/listDirectory calls hit the real disk via NODERAWFS.
    class_<utils::OsFileSystem, base<interfaces::VirtualPathFileSystem>>(
        "OsFileSystem")
        .constructor<std::string>()
        .function("readFile",
                  optional_override([](const utils::OsFileSystem& self,
                                       const std::string& path) {
                      // Surface raw bytes as a Uint8Array copy. The view
                      // would not survive subsequent allocations.
                      auto bytes = self.readFile(path);
                      return val(typed_memory_view(bytes.size(), bytes.data()));
                  }))
        .function("writeFile",
                  optional_override([](utils::OsFileSystem& self,
                                       const std::string& path,
                                       const val& jsArray) {
                      auto bytes = convertJSArrayToNumberVector<u8>(jsArray);
                      return self.writeFile(path, bytes);
                  }))
        .function("fileExists", &utils::OsFileSystem::fileExists);

    // ── SimpleThreadPool ─────────────────────────────────────────────────
    // Construct with the desired worker count. The hosting Emscripten
    // build was linked with -sPTHREAD_POOL_SIZE=N pre-spawned worker_threads;
    // requesting more than N forces additional thread creation, which is
    // legal but slower than re-using the pool.
    class_<utils::SimpleThreadPool, base<interfaces::WorkerPool>>(
        "SimpleThreadPool")
        .constructor<size_t>()
        .function("waitIdle", &utils::SimpleThreadPool::waitIdle)
        .function("threadCount", &utils::SimpleThreadPool::threadCount);

    // Bare WorkerPool base — exposed so other constructors can accept
    // either concrete or abstract pool references (cast on the JS side).
    class_<interfaces::WorkerPool>("WorkerPool");

    // ── HttpHandler (subclassable from JS) ───────────────────────────────
    // The JS object passed to `Module.HttpHandler.implement({...})` must
    // expose:
    //   capabilities():               number
    //   getAsync(url, cbHandle):      void   — invoke Module.dispatchHttpCallback
    //   getRangeAsync(url, s, e, cbH): void
    //
    // Library-internal C++ code (CASC online storage) calls these via the
    // virtual interface; HttpHandlerWrapper forwards each call into the
    // matching JS function. Users never invoke them directly from JS, so
    // we only expose `capabilities` as a queryable property.
    class_<interfaces::HttpHandler>("HttpHandler")
        .allow_subclass<HttpHandlerWrapper>("HttpHandlerWrapper")
        .function("capabilities", optional_override(
            [](interfaces::HttpHandler& self) { return self.capabilities(); }));

    function("dispatchHttpCallback", &dispatchHttpCallback);

    // ── Texture mipmap generation with pool ──────────────────────────────
    function("textureGenerateMipmapsWithPool",
             &generateMipmapsWithPool, allow_raw_pointers());
    function("textureGenerateMipmapsWithCountAndPool",
             &generateMipmapsWithCountAndPool, allow_raw_pointers());

#if defined(WHITEOUT_HAS_MPQ)
    function("mpqOpenWithPool", &mpqOpenWithPool, allow_raw_pointers());
    function("mpqSaveStorageToPath", &mpqSaveStorageToPath);
#endif

#if defined(WHITEOUT_HAS_CASC)
    // ── CASC Storage (local + online) ────────────────────────────────────
    // Online CASC needs an HttpHandler — JS supplies one via the subclass
    // wrapper above, then opens with the OnlineOpenOptions struct.
    class_<storages::casc::Storage>("CascStorage")
        .class_function("open", &cascOpenWithPool, allow_raw_pointers())
        .function("close", &storages::casc::Storage::close)
        .function("readFile",
                  optional_override([](const storages::casc::Storage& self,
                                       const std::string& path) {
                      return self.readFile(path);
                  }))
        .function("readFileById",
                  optional_override([](const storages::casc::Storage& self,
                                       i32 fileId) {
                      return self.readFile(fileId);
                  }))
        .function("fileExists",
                  optional_override([](const storages::casc::Storage& self,
                                       const std::string& path) {
                      return self.fileExists(path);
                  }))
        .function("fileExistsById",
                  optional_override([](const storages::casc::Storage& self,
                                       i32 fileId) {
                      return self.fileExists(fileId);
                  }));
#endif
}
