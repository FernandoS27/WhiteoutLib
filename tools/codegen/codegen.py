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


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description='Generate Embind/pybind11 bindings from C++ headers.')
    p.add_argument('module', help='Module name under tools/codegen/modules/ (e.g. "mdx")')
    p.add_argument('--backend', choices=['embind', 'pybind11', 'dts'], default='embind')
    p.add_argument('--repo-root', default=Path(__file__).resolve().parents[2],
                   type=lambda s: Path(s).resolve(),
                   help='Repository root (defaults to autodetect)')
    p.add_argument('--stdout', action='store_true',
                   help='Print to stdout instead of overwriting the module output path')
    args = p.parse_args(argv)

    config = _load_module_config(args.module)

    from tools.codegen.parser import parse_module
    module = parse_module(config, args.repo_root)

    if args.backend == 'embind':
        from tools.codegen import emit_embind as emitter
        out_rel = config.output_path
    elif args.backend == 'pybind11':
        from tools.codegen import emit_pybind as emitter
        out_rel = config.pybind_output_path or f'bindings/python/{config.name}_bindings.cpp'
    else:  # dts
        from tools.codegen import emit_dts as emitter
        out_rel = config.dts_output_path or f'package/types/{config.name}.d.ts'
    text = emitter.emit(module)

    if args.stdout:
        sys.stdout.write(text)
        return 0

    out_path = (args.repo_root / out_rel).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding='utf-8', newline='\n')
    print(f'Wrote {out_path.relative_to(args.repo_root)} '
          f'({len(text)} bytes, {len(text.splitlines())} lines)')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
