# SPDX-License-Identifier: BSD-3-Clause
"""MPQ storage module config for the codegen.

Currently Python-only. Wired into:

  - `bindings/python/mpq_bindings.cpp`     (codegen output)
  - `bindings/python/CMakeLists.txt`       (link to `whiteout_mpq`)
  - `bindings/python/module.cpp`           (`bind_mpq()` submodule registration)
  - `packages/python/whiteout-stubs/mpq.pyi` (PEP 561 stubs)
  - `scripts/build-python.ps1`, `pyproject.toml` (WHITEOUT_ENABLE_MPQ=ON)

The MPQ binding covers:
  - `mpq::Storage`             — main archive class (move-only, factory-built)
  - `mpq::{FileInfo,ArchiveInfo,WriteOptions,CreateOptions}` — POD structs
  - `mpq::{FormatVersion,Compression,FileFlags}`             — enums
  - `utils::MpqFileSystem`     — VirtualPathFileSystem adapter (different
                                 namespace; opted in via explicit @bind)

Deferred / known gaps:
  - `Locale::English` / `Locale::Russian` / ... constants are unbound. The
    codegen doesn't yet group sub-namespace constants into a holder class.
    Callers can pass the raw `u16` value to `WriteOptions(locale=0x0409)`.
  - `Storage::save(path)` overload (`save` returns to source path; for new
    archives, the path-variant is needed). The codegen picks one overload
    only.
  - WASM/Embind backend isn't wired. Embind doesn't auto-convert
    `std::optional` returns, so `Storage::open` would need custom glue.
"""

from tools.codegen.ir import ModuleConfig

CONFIG = ModuleConfig(
    name='mpq',
    cpp_namespace='whiteout::storages::mpq',
    js_prefix='Mpq',
    embind_block='mpq',
    headers=[
        'include/whiteout/storages/mpq/types.h',
        'include/whiteout/storages/mpq/storage.h',
        'include/whiteout/utils/mpq_file_system.h',
    ],
    output_path='bindings/wasm/mpq_bindings.cpp',
    include_dirs=['include'],
    # VectorU8/VectorString are bound once at root in module.cpp/bindings.cpp;
    # don't re-register here.
    skip_vector_js_names=['VectorU8', 'VectorString'],
    auto_bind=True,
    auto_bind_skip=[
        # `MpqFileSystem` lives in `whiteout::utils`, not the MPQ namespace.
        # It's opted in via explicit @bind in its header so auto_bind_skip
        # would be redundant for it — listed here only as documentation.
    ],
)
