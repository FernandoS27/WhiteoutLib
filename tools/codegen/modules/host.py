# SPDX-License-Identifier: BSD-3-Clause
"""Host extras (threading, disk I/O, HTTP) module config for the codegen.
Native targets only — the WASM web build excludes these sources entirely
and the Node WASM target hand-binds them in its own translation unit.

Covers:
  - `interfaces::HttpResponse`            — value_object
  - `interfaces::HttpHandler`             — opaque abstract base
  - `interfaces::VirtualPathFileSystem`   — opaque abstract base
  - `interfaces::WorkerPool`              — opaque abstract base
  - `utils::OsFileSystem`                 — VirtualPathFileSystem concrete
  - `utils::SimpleThreadPool`             — WorkerPool concrete
  - `utils::SimpleHttpHandler`            — HttpHandler concrete

Subclassing the abstract bases (e.g. plugging a Python `urllib` or a
Java `HttpClient` in as the HTTP backend) is NOT codegen-driven — that
needs trampoline classes / function-pointer vtables that the generator
doesn't synthesise. Bindings users wanting custom HTTP either:

  * Use the concrete `SimpleHttpHandler` (WinHTTP / libcurl), OR
  * Add their own hand-written shim in the relevant binding layer
    (the Python build does this in `bindings/python/host_bindings.cpp`).
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='host',
    cpp_namespace='whiteout::utils',
    js_prefix='',
    embind_block='host',
    headers=[
        'include/whiteout/interfaces.h',
        'include/whiteout/utils/os_file_system.h',
        'include/whiteout/utils/simple_thread_pool.h',
        'include/whiteout/utils/simple_http_handler.h',
    ],
    output_path='bindings/wasm/host_bindings.cpp',
    pybind_output_path='bindings/python/host_bindings.cpp',
    include_dirs=['include'],
    # Vectors / strings registered at module root by the per-backend
    # umbrella file — don't double-register here.
    skip_vector_js_names=['VectorU8', 'VectorString'],
    # HttpHandler + VirtualPathFileSystem are hand-bound in
    # bindings/python/module.cpp's bind_supplementary so we can attach
    # the Python trampoline (HttpHandler) and the InMemoryFs subclass
    # (VirtualPathFileSystem). Codegen still emits their derived classes
    # — SimpleHttpHandler / OsFileSystem — which inherit from those
    # hand-registered bases via pybind11's template `<Derived, Base>`.
    # The C and Java backends don't consult skip_class_js_names, so they
    # still get the base types in their output (they don't need a
    # trampoline because subclassing happens at the C ABI / Panama layer
    # rather than at the binding layer).
    skip_class_js_names=[
        'HttpHandler',           # Python trampoline lives in module.cpp
        'VirtualPathFileSystem', # InMemoryFs subclass lives in module.cpp
        'HttpResponse',          # bytes-friendly kwargs ctor lives in module.cpp
    ],
    # auto_bind is OFF — every host type opts in explicitly via `@bind`
    # so we don't accidentally drag in CascFileSystem, TimelineSemaphore,
    # WorkerTask, DirectoryEntry, etc. from interfaces.h.
    auto_bind=False,
)
