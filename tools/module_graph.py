# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# WhiteoutLib's module graph: which module each file of include/ and src/
# belongs to, and the include and link edges between modules.
#
# Usage, from the repository root:
#   python tools/module_graph.py includes            # include edges between modules
#   python tools/module_graph.py links <build-dir>   # link edges, from llvm-nm over the objects
#   python tools/module_graph.py closure <build-dir> # what a .wem reader and the converters link
#   python tools/module_graph.py check               # fails on an upward #include
#   python tools/module_graph.py tests               # the modules each test's includes need
#
# An edge is upward when its target's module is not one its source's module
# may depend on (DEPS). `links` needs llvm-nm (LLVM_NM, PATH, or the LLVM
# install); the objects can come from any compiler llvm-nm reads.

from __future__ import annotations

import argparse
import collections
import fnmatch
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

# Path stem (relative to include/whiteout/ or src/whiteout/, no extension) ->
# module. The first matching pattern wins.
W = 'models/wem/'
MODULE_MAP = [
    # WEM converting: whatever knows a file format.
    (W + 'converters/*', 'wem_convert'),
    (W + 'converters', 'wem_convert'),
    (W + 'converter_base', 'wem_convert'),
    (W + 'converter_registry', 'wem_convert'),
    (W + '*_converter', 'wem_convert'),
    (W + 'interchange', 'wem_convert'),
    (W + 'game_minimum', 'wem_convert'),
    (W + 'inline_models', 'wem_convert'),
    (W + 'optimize', 'wem_convert'),
    (W + 'wem_convert', 'wem_convert'),
    (W + 'materials/*_core', 'wem_convert'),
    (W + 'materials/m2_shader_table', 'wem_convert'),
    (W + 'materials/surface_flatten', 'wem_convert'),
    (W + 'materials/draw_order', 'wem_convert'),
    (W + 'native/*_copy', 'wem_convert'),
    ('models/cross/*', 'wem_convert'),
    # WEM editing: authoring, simulation and its bakes.
    (W + 'scene_ops', 'wem_edit'),
    (W + 'wem_edit', 'wem_edit'),
    (W + 'geometry/ops', 'wem_edit'),
    (W + 'geometry/modelling', 'wem_edit'),
    (W + 'geometry/select', 'wem_edit'),
    (W + 'geometry/bvh', 'wem_edit'),
    (W + 'geometry/cdt', 'wem_edit'),
    (W + 'geometry/fit', 'wem_edit'),
    (W + 'geometry/hull', 'wem_edit'),
    (W + 'geometry/interpolate', 'wem_edit'),
    (W + 'geometry/lattice', 'wem_edit'),
    (W + 'geometry/primitives', 'wem_edit'),
    (W + 'geometry/refine', 'wem_edit'),
    (W + 'geometry/fracture/*', 'wem_edit'),
    (W + 'geometry/retopo/*', 'wem_edit'),
    (W + 'geometry/uv/*', 'wem_edit'),
    (W + 'anim/key_reduce', 'wem_edit'),
    (W + 'anim/key_reduce_tolerance', 'wem_edit'),
    (W + 'anim/stage_bake', 'wem_edit'),
    (W + 'skinning/fit', 'wem_edit'),
    (W + 'skinning/generate*', 'wem_edit'),
    (W + 'skinning/mirror', 'wem_edit'),
    (W + 'skinning/ops', 'wem_edit'),
    (W + 'skinning/reduce', 'wem_edit'),
    (W + 'skinning/solid', 'wem_edit'),
    (W + 'skinning/transfer', 'wem_edit'),
    (W + 'rigging/detect*', 'wem_edit'),
    (W + 'rigging/limbs', 'wem_edit'),
    (W + 'rigging/names', 'wem_edit'),
    (W + 'rigging/rebind', 'wem_edit'),
    (W + 'rigging/shape', 'wem_edit'),
    (W + 'rigging/tpose', 'wem_edit'),
    (W + 'physics/bake', 'wem_edit'),
    (W + 'physics/cloth_common', 'wem'),  # header-only helpers the referencers read too
    (W + 'physics/cloth_*', 'wem_edit'),
    (W + 'physics/crossing', 'wem_edit'),
    (W + 'physics/switches', 'wem_edit'),
    (W + 'physics/soft_common', 'wem'),  # header-only helpers the referencers read too
    (W + 'physics/soft_*', 'wem_edit'),
    # WEM core: the document and what anyone holding one needs.
    (W + '*', 'wem'),
    ('sno/d3/native/*', 'models'),
    ('models/*', 'models'),
    ('sno/*', 'sno'),
    ('textures/*', 'textures'),
    ('storages/casc/*', 'casc'),
    ('utils/casc_file_system', 'casc'),
    ('storages/mpq/*', 'mpq'),
    ('utils/mpq_file_system', 'mpq'),
    # The storage sources casc and mpq each compile; the headers are core's.
    ('storages/common/jenkins', 'storage'),
    ('storages/common/mapped_file', 'storage'),
    ('storages/common/md5', 'storage'),
    ('storages/common/sha1', 'storage'),
    ('storages/common/codecs/*', 'storage'),
    ('database/*', 'database'),
    ('*', 'core'),
]

# What each module may include and link.
DEPS = {
    'core': set(),
    'sno': {'core'},
    'textures': {'core', 'sno'},
    'models': {'core'},
    'wem': {'core', 'models'},
    'wem_convert': {'core', 'models', 'wem'},
    'wem_edit': {'core', 'models', 'textures', 'wem'},
    'storage': {'core'},
    'casc': {'core', 'sno', 'storage'},
    'mpq': {'core', 'storage'},
    'database': {'core'},
}

# Upward link edges still to be cut, as (from, to) stems -> the seam that cuts
# them (WHITEOUTLIB_MODULES_DESIGN.md section 5). Empty: every seam has landed,
# so any upward link edge fails `closure`.
SEAMS: dict[tuple[str, str], str] = {}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
SOURCE_EXT = ('.h', '.hpp', '.inl', '.cpp')


def stem(path: str) -> str:
    """'src/whiteout/models/wem/model.cpp' -> 'models/wem/model'."""
    s = re.sub(r'^(include|src)/whiteout/', '', path.replace('\\', '/'))
    return s.rsplit('.', 1)[0]


def module_of(path: str) -> str:
    s = stem(path)
    for pattern, module in MODULE_MAP:
        if fnmatch.fnmatchcase(s, pattern):
            return module
    raise AssertionError('unreachable: the map ends in "*"')


def upward(a: str, b: str) -> bool:
    return a != b and b not in DEPS[a]


def library_files() -> list[str]:
    files = []
    for base in ('include', 'src'):
        for dp, _, fn in os.walk(os.path.join(ROOT, base)):
            for f in fn:
                if f.endswith(SOURCE_EXT):
                    files.append(os.path.relpath(os.path.join(dp, f), ROOT).replace(os.sep, '/'))
    return sorted(files)


def include_graph(files: list[str], fileset: set[str]) -> dict[str, list[str]]:
    """file -> the library files it includes. Third-party and system headers drop out."""
    def resolve(src: str, path: str) -> str | None:
        if path.startswith('whiteout/'):
            cands = ['include/' + path, 'src/' + path]
        else:
            here = os.path.normpath(os.path.join(os.path.dirname(src), path)).replace(os.sep, '/')
            cands = [here, 'include/whiteout/' + path]
        return next((c for c in cands if c in fileset), None)

    edges = {}
    for f in files:
        with open(os.path.join(ROOT, f), encoding='utf-8', errors='replace') as fh:
            text = fh.read()
        edges[f] = sorted({r for p in INCLUDE.findall(text) if (r := resolve(f, p))})
    return edges


def upward_includes() -> list[tuple[str, str]]:
    files = library_files()
    graph = include_graph(files, set(files))
    return [(f, h) for f in files for h in graph[f] if upward(module_of(f), module_of(h))]


def find_nm() -> str:
    for cand in (os.environ.get('LLVM_NM'), shutil.which('llvm-nm'),
                 r'C:/Program Files/LLVM/bin/llvm-nm.exe'):
        if cand and os.path.exists(cand):
            return cand
    sys.exit('llvm-nm not found: set LLVM_NM or put it on PATH')


def objects(tree: str) -> dict[str, str]:
    """Library objects under a build tree, by the stem of their source."""
    found = {}
    marker = re.compile(r'[/\\]whiteout_[a-z_]+\.dir[/\\](src[/\\]whiteout[/\\].*)\.cpp\.(?:o|obj)$')
    for dp, _, fn in os.walk(tree):
        for f in fn:
            path = os.path.join(dp, f)
            m = marker.search(path)
            if m and '_mt.dir' not in path:
                found[stem(m.group(1))] = path
    return found


def link_graph(tree: str) -> tuple[dict[str, dict[str, list[str]]], dict[str, int]]:
    """object -> object it takes an out-of-line symbol from -> the symbols.

    An edge is a symbol undefined in one object and defined in exactly one
    other: a symbol two objects define (inline, template, weak) needs neither.
    """
    objs = objects(tree)
    if not objs:
        sys.exit(f'no library objects under {tree}')
    by_path = {os.path.normcase(os.path.abspath(p)): s for s, p in objs.items()}
    nm = find_nm()
    defs: dict[str, set[str]] = collections.defaultdict(set)
    undefs: dict[str, set[str]] = collections.defaultdict(set)
    paths = sorted(objs.values())
    for i in range(0, len(paths), 40):
        out = subprocess.run([nm, '-A', '--no-sort', '--extern-only'] + paths[i:i + 40],
                             capture_output=True, text=True, errors='replace').stdout
        for line in out.splitlines():
            path, sep, rest = line.rpartition('.obj: ') if '.obj: ' in line else line.rpartition('.o: ')
            if not sep:
                continue
            o = by_path.get(os.path.normcase(os.path.abspath(path + sep[:-2])))
            parts = rest.split()
            if o is None or len(parts) < 2:
                continue
            kind, sym = (parts[0], parts[1]) if len(parts) == 2 else (parts[1], parts[2])
            if kind == 'U':
                undefs[o].add(sym)
            elif kind in 'TDBRV':
                defs[sym].add(o)
    edges: dict[str, dict[str, list[str]]] = collections.defaultdict(lambda: collections.defaultdict(list))
    for o, syms in undefs.items():
        for sym in syms:
            d = defs.get(sym)
            if d and len(d) == 1:
                (t,) = d
                if t != o:
                    edges[o][t].append(sym)
    sizes = {s: os.path.getsize(p) for s, p in objs.items()}
    return edges, sizes


def demangle(sym: str) -> str:
    tool = shutil.which('llvm-undname') or r'C:/Program Files/LLVM/bin/llvm-undname.exe'
    if sym.startswith('?') and os.path.exists(tool):
        out = subprocess.run([tool, sym], capture_output=True, text=True).stdout.strip().splitlines()
        if len(out) >= 2:
            return out[-1]
    return sym


def cmd_includes(args) -> int:
    pairs = collections.defaultdict(list)
    for f, h in upward_includes() if not args.all else _all_cross():
        pairs[(module_of(f), module_of(h))].append((f, h))
    for (a, b), es in sorted(pairs.items()):
        print(f'{a} -> {b}: {len(es)}')
        if args.verbose:
            for f, h in es:
                print(f'    {f} -> {h}')
    return 0


def _all_cross():
    files = library_files()
    graph = include_graph(files, set(files))
    return [(f, h) for f in files for h in graph[f] if module_of(f) != module_of(h)]


def cmd_links(args) -> int:
    edges, _ = link_graph(args.tree)
    for a in sorted(edges):
        for b, syms in sorted(edges[a].items()):
            ma, mb = module_of(a), module_of(b)
            if ma != mb or args.all:
                flag = ' UPWARD' if upward(ma, mb) else ''
                print(f'{a} [{ma}] -> {b} [{mb}]{flag}: {len(syms)}')
    return 0


def cmd_closure(args) -> int:
    edges, sizes = link_graph(args.tree)

    def reach(seeds: list[str]) -> set[str]:
        seen, todo = set(), [s for s in seeds if s in sizes]
        while todo:
            o = todo.pop()
            if o not in seen:
                seen.add(o)
                todo.extend(edges.get(o, {}))
        return seen

    total = sum(sizes.values())
    print(f'archive: {len(sizes)} objects {total / 1e6:.1f} MB')
    for name, seeds in (('.wem read + validate', ['document_io', 'validate']),
                        ('converters (mdx, m3, registry)', ['converters/mdx_converter',
                                                            'converters/m3_converter',
                                                            'converter_registry'])):
        s = reach([W + x for x in seeds])
        by_module = collections.Counter(module_of(o) for o in s)
        mods = ', '.join(f'{m} {n}' for m, n in sorted(by_module.items()))
        print(f'{name:32s}: {len(s):3d} objects {sum(sizes[o] for o in s) / 1e6:5.1f} MB  ({mods})')

    planned, unplanned = [], []
    for a in sorted(edges):
        for b, syms in sorted(edges[a].items()):
            if upward(module_of(a), module_of(b)):
                (planned if (a, b) in SEAMS else unplanned).append((a, b, syms))
    for a, b, syms in planned:
        print(f'planned {SEAMS[(a, b)]}: {a} -> {b} ({len(syms)} symbols)')
    for a, b, syms in unplanned:
        print(f'UNPLANNED: {a} [{module_of(a)}] -> {b} [{module_of(b)}]: '
              + ', '.join(demangle(s) for s in sorted(syms)[:3]) + (' ...' if len(syms) > 3 else ''))
    live = {(a, b) for a, b, _ in planned}
    for k in sorted(set(SEAMS) - live):
        print(f'cut (drop the row): {SEAMS[k]} {k[0]} -> {k[1]}')
    print(f'upward link edges: {len(planned)} planned, {len(unplanned)} unplanned')
    return 1 if unplanned else 0


SOURCE = re.compile(r'\bsrc/whiteout/[^\s)"]+\.cpp\b')


def source_lists() -> dict[str, list[str]]:
    """Each target's sources: cmake/modules/<target>.cmake, and the storage
    targets' lists in the root CMakeLists.txt."""
    lists = {}
    folder = os.path.join(ROOT, 'cmake', 'modules')
    for f in sorted(os.listdir(folder)):
        if f.endswith('.cmake'):
            with open(os.path.join(folder, f), encoding='utf-8') as fh:
                lists[f[:-len('.cmake')]] = SOURCE.findall(fh.read())
    with open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8') as fh:
        root = fh.read()
    for target, opening in (('whiteout_casc', 'set(CASC_SOURCES'), ('whiteout_mpq', 'add_library(whiteout_mpq'),
                            ('whiteout_database', 'add_library(whiteout_database')):
        at = root.find(opening)
        block = root[at:at + re.search(r'\n\s*\)', root[at:]).start()] if at >= 0 else ''
        lists[target] = SOURCE.findall(block)
    return lists


def source_faults() -> list[str]:
    """A source listed by a target its module is not, or listed by no target."""
    faults, seen = [], collections.Counter()
    for target, sources in source_lists().items():
        allowed = {target[len('whiteout_'):]} | ({'storage'} if target in ('whiteout_casc', 'whiteout_mpq') else set())
        for s in sources:
            seen[s] += 1
            if module_of(s) not in allowed:
                faults.append(f'{s} [{module_of(s)}] is listed by {target}')
    for s in (f for f in library_files() if f.endswith('.cpp')):
        # A storage source compiles into each storage target that uses it.
        if seen[s] == 0 or (seen[s] > 1 and module_of(s) != 'storage'):
            faults.append(f'{s} [{module_of(s)}] is listed {seen[s]} times')
    return faults


def cmd_check(args) -> int:
    bad = upward_includes()
    for f, h in bad:
        print(f'{f} [{module_of(f)}] includes {h} [{module_of(h)}]')
    faults = source_faults()
    for fault in faults:
        print(fault)
    if bad:
        print(f'{len(bad)} upward #include(s): move what both sides need down a module (see DEPS)')
    if faults:
        print(f'{len(faults)} source list fault(s): a module file lists the sources its module holds')
    if bad or faults:
        return 1
    print('module_graph: no upward #include; every source is in its module')
    return 0


def closure_of(modules: set[str]) -> set[str]:
    out, todo = set(), list(modules)
    while todo:
        m = todo.pop()
        if m not in out:
            out.add(m)
            todo.extend(DEPS[m])
    return out


def cmd_tests(args) -> int:
    files = library_files()
    fileset = set(files)
    tests_dir = os.path.join(ROOT, 'tests')
    for f in sorted(os.listdir(tests_dir)):
        if not f.endswith('.cpp'):
            continue
        with open(os.path.join(tests_dir, f), encoding='utf-8', errors='replace') as fh:
            incs = INCLUDE.findall(fh.read())
        # A test reaches private headers through the src/ include path too, or
        # by a path relative to tests/.
        mods = {module_of(c) for p in incs
                for c in ('include/' + p, 'src/' + p, 'src/whiteout/' + p,
                          os.path.normpath('tests/' + p).replace(os.sep, '/')) if c in fileset}
        # Drop what another listed module already brings.
        narrow = {m for m in mods if not any(m in closure_of(DEPS[o]) for o in mods if o != m)}
        print(f'{f[:-4]}: {" ".join(sorted(narrow)) or "core"}')
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('includes', help='include edges between modules (upward only, unless --all)')
    s.add_argument('--all', action='store_true')
    s.add_argument('-v', '--verbose', action='store_true')
    s = sub.add_parser('links', help='link edges between modules')
    s.add_argument('tree')
    s.add_argument('--all', action='store_true', help='also edges inside a module')
    s = sub.add_parser('closure', help='objects the .wem read path and the converters link')
    s.add_argument('tree')
    sub.add_parser('check', help='fail on an upward #include')
    sub.add_parser('tests', help='the narrowest modules each test needs')
    args = p.parse_args()
    return {'includes': cmd_includes, 'links': cmd_links, 'closure': cmd_closure,
            'check': cmd_check, 'tests': cmd_tests}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
