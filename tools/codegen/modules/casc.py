# SPDX-License-Identifier: BSD-3-Clause
"""CASC storage module config for the codegen.

Covers the read-only Storage surface plus the writable StorageWritable
subclass, the option structs, and the enums. Bound only when
WHITEOUT_ENABLE_CASC=ON in the parent build (the storage code depends
on whiteout_casc, which is excluded otherwise).

`auto_bind` is OFF — every CASC type opts in explicitly via `@bind` so
the codegen doesn't accidentally drag in the many internal-only helpers
(BatchReadRequest, FileSpanInfo, etc.) or callback-bearing methods it
can't yet marshal.

Deferred / known gaps:
  - `Storage::enumerate()`: codegen doesn't auto-wrap std::function
    params. Use `listFiles()` / `listEntries()` for full materialisation
    instead.
  - The `ProgressCallback` field on OpenOptions / OnlineOpenOptions and
    `Storage::setProgressCallback` are std::function-shaped too, and are
    covered by hand-written shims instead — see
    bindings/c/whiteout_casc_progress.h and the C# StorageProgress class.
  - `OpenOptions::listfile` (std::span<const u8> field) is an unsupported
    field shape — callers needing a custom listfile go through the
    C++ surface directly.
  - `Storage::readBatch` uses vector<BatchReadRequest>/vector<BatchReadResult>
    which need bigger struct-marshalling work.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='casc',
    cpp_namespace='whiteout::storages::casc',
    js_prefix='Casc',
    embind_block='casc',
    headers=[
        'include/whiteout/storages/casc/types.h',
        'include/whiteout/storages/casc/storage.h',
        'include/whiteout/storages/casc/storage_writable.h',
    ],
    output_path='bindings/wasm/casc_bindings.cpp',
    include_dirs=['include'],
    skip_vector_js_names=['VectorU8', 'VectorString'],
    auto_bind=False,
)
