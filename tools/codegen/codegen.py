# SPDX-License-Identifier: BSD-3-Clause
"""Codegen CLI: python -m tools.codegen.codegen <module> [--backend embind]."""

from __future__ import annotations

import argparse
import importlib
import sys
from pathlib import Path


def _load_module_config(name: str):
    mod = importlib.import_module(f'tools.codegen.modules.{name}')
    return mod.CONFIG


def _write(args, out_rel: str, text: str) -> int:
    """Shared write path for emitters that compute output internally."""
    if args.stdout:
        sys.stdout.write(text)
        return 0
    out_path = (args.repo_root / out_rel).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Path.write_text(newline=...) is Python 3.10+; the explicit open()
    # form keeps codegen working on the Python 3.9 that ships with Xcode.
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print(f'Wrote {out_path.relative_to(args.repo_root)} '
          f'({len(text)} bytes, {len(text.splitlines())} lines)')
    return 0


def _write_files(args, files: dict) -> int:
    """Shared write path for emitters that produce multiple files."""
    if args.stdout:
        for rel, text in files.items():
            sys.stdout.write(f'// ── {rel} ──\n')
            sys.stdout.write(text)
            sys.stdout.write('\n')
        return 0
    total_bytes = 0
    for rel, text in files.items():
        out_path = (args.repo_root / rel).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        # Path.write_text(newline=...) is Python 3.10+; the explicit open()
        # form keeps codegen working on the Python 3.9 that ships with Xcode.
        with open(out_path, 'w', encoding='utf-8', newline='\n') as fp:
            fp.write(text)
        total_bytes += len(text)
        print(f'Wrote {out_path.relative_to(args.repo_root)} '
              f'({len(text)} bytes)')
    print(f'Total: {len(files)} files, {total_bytes} bytes')
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description='Generate Embind/pybind11 bindings from C++ headers.')
    p.add_argument('module', help='Module name under tools/codegen/modules/ (e.g. "mdx")')
    p.add_argument('--backend',
                   choices=['embind', 'pybind11', 'dts', 'pyi',
                            'c-header', 'c-source',
                            'c-common', 'c-common-header',
                            'java', 'java-common', 'jni',
                            'csharp', 'csharp-common',
                            'rust-abi-header', 'rust-abi-source', 'rust-math', 'rust',
                            'wem-native'],
                   default='embind')
    p.add_argument('--wem-bump', default='',
                   help='wem-native only: comma-separated native block names (or "all") '
                        'whose schema change is accepted, raising their chunk version. '
                        'Without this a changed block fails generation.')
    p.add_argument('--repo-root', default=Path(__file__).resolve().parents[2],
                   type=lambda s: Path(s).resolve(),
                   help='Repository root (defaults to autodetect)')
    p.add_argument('--stdout', action='store_true',
                   help='Print to stdout instead of overwriting the module output path')
    args = p.parse_args(argv)

    # The Rust value ABI is module-independent (it covers the shared math
    # types), so short-circuit before paying for a libclang parse.
    if args.backend in ('rust-abi-header', 'rust-abi-source'):
        from tools.codegen import emit_rust_abi
        if args.backend == 'rust-abi-header':
            return _write(args, 'bindings/c/whiteout_v.h',
                          emit_rust_abi.emit_header())
        return _write(args, 'bindings/c/whiteout_v.cpp',
                      emit_rust_abi.emit_source())

    if args.backend == 'rust-math':
        from tools.codegen import emit_rust
        return _write(args, 'bindings/rust/whiteout/src/math.rs',
                      emit_rust.emit_math())

    config = _load_module_config(args.module)

    from tools.codegen.parser import parse_module
    module = parse_module(config, args.repo_root)

    if args.backend == 'embind':
        from tools.codegen import emit_embind as emitter
        out_rel = config.output_path
    elif args.backend == 'pybind11':
        from tools.codegen import emit_pybind as emitter
        out_rel = config.pybind_output_path or f'bindings/python/{config.name}_bindings.cpp'
    elif args.backend == 'pyi':
        from tools.codegen import emit_pyi as emitter
        # PEP 561 stub package alongside the .pyd extension. Lives inside
        # the publishable Python distribution at packages/python/.
        out_rel = (config.pyi_output_path
                   or f'packages/python/whiteout-stubs/{config.name}.pyi')
    elif args.backend == 'c-header':
        from tools.codegen import emit_c
        text = emit_c.emit_header(module)
        out_rel = (config.c_header_output_path
                   or f'bindings/c/whiteout_{config.name}.h')
        return _write(args, out_rel, text)
    elif args.backend == 'c-source':
        from tools.codegen import emit_c
        text = emit_c.emit_source(module)
        out_rel = (config.c_source_output_path
                   or f'bindings/c/whiteout_{config.name}.cpp')
        return _write(args, out_rel, text)
    elif args.backend == 'c-common':
        from tools.codegen import emit_c
        text = emit_c.emit_common()
        return _write(args, 'bindings/c/whiteout_c_common.cpp', text)
    elif args.backend == 'c-common-header':
        from tools.codegen import emit_c
        text = emit_c.emit_common_header()
        return _write(args, 'bindings/c/whiteout_c_common.h', text)
    elif args.backend == 'java':
        from tools.codegen import emit_java
        files = emit_java.emit(module)
        return _write_files(args, files)
    elif args.backend == 'java-common':
        from tools.codegen import emit_java
        files = emit_java.emit_common_java()
        return _write_files(args, files)
    elif args.backend == 'jni':
        # JNI bridge: lets Java implement the abstract interfaces in
        # include/whiteout/interfaces.h via a C++ wrapper that forwards
        # virtual overrides into the Java side over JNI.
        from tools.codegen import emit_jni
        files = emit_jni.emit(module)
        return _write_files(args, files)
    elif args.backend == 'csharp':
        from tools.codegen import emit_csharp
        files = emit_csharp.emit(module)
        return _write_files(args, files)
    elif args.backend == 'csharp-common':
        from tools.codegen import emit_csharp
        files = emit_csharp.emit_common()
        return _write_files(args, files)
    elif args.backend == 'wem-native':
        from tools.codegen import emit_wem_native
        files = emit_wem_native.emit(
            module, config, args.repo_root,
            {b.strip() for b in args.wem_bump.split(',') if b.strip()})
        report = files.pop('_report', [])
        rc = _write_files(args, files)
        for line in report:
            print(line)
        return rc
    elif args.backend == 'rust':
        from tools.codegen import emit_rust
        return _write(args, f'bindings/rust/whiteout/src/{config.name}.rs',
                      emit_rust.emit_module(module))
    else:  # dts
        from tools.codegen import emit_dts as emitter
        out_rel = (config.dts_output_path
                   or f'packages/js-ts/types/{config.name}.d.ts')
    text = emitter.emit(module)

    if args.stdout:
        sys.stdout.write(text)
        return 0

    out_path = (args.repo_root / out_rel).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Path.write_text(newline=...) is Python 3.10+; the explicit open()
    # form keeps codegen working on the Python 3.9 that ships with Xcode.
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print(f'Wrote {out_path.relative_to(args.repo_root)} '
          f'({len(text)} bytes, {len(text.splitlines())} lines)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
