# SPDX-License-Identifier: BSD-3-Clause
"""Freshness + schema-lock gate for the generated WEM native blocks.

    python -m tools.codegen.check_wem_native [module ...]

Two checks, one per rule that keeps §15.2's generation from re-coupling the WEM
file format to the parsers' structs:

1. **Freshness.** Re-run the generator for every module that has a `wem_native`
   config and diff against the checked-in output. A non-empty diff means someone
   changed a parser header and did not regenerate — the mirror is allowed to lag
   the parser, but only deliberately, and "deliberately" means a commit.

2. **The lock is load-bearing.** Take the real signatures, corrupt one block's
   recorded hash, and assert that generation *fails*. A lock that has quietly
   stopped rejecting drift looks exactly like a lock that is working, which is
   the failure mode this check exists for.

Exits non-zero on either. Nothing is written; the freshness pass runs the
emitter against a throwaway copy of the lock so a check can never mutate it.
"""

from __future__ import annotations

import difflib
import importlib
import json
import shutil
import sys
import tempfile
from pathlib import Path

from . import emit_wem_native
from .parser import parse_module

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULES = ('mdx', 'm2', 'm3', 'd3')


def _sandbox_lock(tmp: Path) -> Path:
    """A repo-shaped directory whose only real file is the lock, so the emitter
    can read and rewrite it without touching the checked-in one."""
    dst = tmp / emit_wem_native.SCHEMA_LOCK
    dst.parent.mkdir(parents=True, exist_ok=True)
    src = REPO_ROOT / emit_wem_native.SCHEMA_LOCK
    if src.is_file():
        shutil.copyfile(src, dst)
    return dst


def _generate(name: str, repo_root: Path) -> dict:
    config = importlib.import_module(f'tools.codegen.modules.{name}').CONFIG
    module = parse_module(config, REPO_ROOT)
    files = emit_wem_native.emit(module, config, repo_root)
    files.pop('_report', None)
    return files


def check_freshness(names) -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        _sandbox_lock(tmp)
        for name in names:
            try:
                files = _generate(name, tmp)
            except emit_wem_native.GenerationError as e:
                print(f'FAIL {name}: generation failed\n{e}')
                failures += 1
                continue
            for rel, text in files.items():
                on_disk = REPO_ROOT / rel
                if not on_disk.is_file():
                    print(f'FAIL {name}: {rel} is not checked in')
                    failures += 1
                    continue
                have = on_disk.read_text(encoding='utf-8').replace('\r\n', '\n')
                if have == text:
                    continue
                failures += 1
                print(f'FAIL {name}: {rel} is stale. Re-run:')
                print(f'    python -m tools.codegen.codegen {name} --backend wem-native')
                diff = difflib.unified_diff(have.splitlines(), text.splitlines(),
                                            'checked-in', 'generated', lineterm='')
                for line in list(diff)[:40]:
                    print(f'  {line}')
        # The lock itself is output too: a new block adds a row.
        live = (REPO_ROOT / emit_wem_native.SCHEMA_LOCK)
        sandboxed = tmp / emit_wem_native.SCHEMA_LOCK
        if live.is_file() and sandboxed.is_file():
            if json.loads(live.read_text(encoding='utf-8')) != \
                    json.loads(sandboxed.read_text(encoding='utf-8')):
                # Only meaningful when every module was regenerated: a partial
                # run drops the modules it did not visit.
                if set(names) == set(MODULES):
                    print(f'FAIL {emit_wem_native.SCHEMA_LOCK} is stale')
                    failures += 1
    return failures


def check_lock_rejects_drift(name: str = 'mdx') -> int:
    """Corrupt one recorded hash and assert generation refuses."""
    config = importlib.import_module(f'tools.codegen.modules.{name}').CONFIG
    module = parse_module(config, REPO_ROOT)

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        lock_path = _sandbox_lock(tmp)
        lock = json.loads(lock_path.read_text(encoding='utf-8'))
        blocks = lock['blocks'][name]
        victim = sorted(blocks)[0]
        blocks[victim]['hash'] = 'deadbeefdeadbeef'
        lock_path.write_text(json.dumps(lock, indent=2), encoding='utf-8')

        try:
            emit_wem_native.emit(module, config, tmp)
        except emit_wem_native.GenerationError as e:
            if victim in str(e):
                print(f'ok   schema lock rejects drift ({name}.{victim})')
                return 0
            print(f'FAIL schema lock raised, but not about {victim}:\n{e}')
            return 1

        print(f'FAIL schema lock accepted a changed hash for {name}.{victim} — '
              'drift would reach the file format unreviewed')
        return 1

    return 1


def main(argv=None) -> int:
    names = list(argv or sys.argv[1:]) or list(MODULES)
    failures = check_freshness(names)
    failures += check_lock_rejects_drift()
    if failures:
        print(f'\n{failures} check(s) failed')
        return 1
    print(f'ok   {len(names)} module(s) up to date')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
