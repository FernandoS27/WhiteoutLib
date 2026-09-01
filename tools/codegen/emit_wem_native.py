# SPDX-License-Identifier: BSD-3-Clause
"""`wem-native` backend: WEM-owned mirrors of the format parsers' records.

WEM stores each material's source record verbatim so a round trip is lossless
(design §7.3). Those records are *mirrors*, not the parser structs themselves —
a chunk version can describe a mirror and cannot describe someone else's header,
and `structures.h` stays free of every format's includes. Mirroring by hand
would be several thousand lines that must agree with four parsers forever, so
the mirrors are generated from the parser headers by this backend (§15.2).

Generating the mirror *from* the thing it decouples WEM from looks circular. Two
rules make it not:

- **The output is checked in.** A parser refactor changes nothing until someone
  runs the generator and commits the diff. The mirror may lag the parser
  indefinitely; that is the point.
- **`wem_schema.lock` turns drift into a hard failure.** Every block's field
  list is hashed. A hash that moves while its version stands still fails
  generation outright — the human must bump the block's version in the lock,
  which is the reviewed, versioned edit the file format deserves.

What is emitted, per module:

- `<header_path>` — the public mirror header: mirrored enums, mirrored structs
  in dependency order, each struct's `reflect(V&)` (§11.2), the
  `ChunkTagTraits` specialisations, and `k<Prefix>ManualFields` naming every
  field the copy halves deliberately do not touch.
- `src/whiteout/models/wem/native/<module>_copy.h` — the mechanical
  `CopyToNative` / `CopyFromNative` halves, inline, private to `src/`. This is
  the only generated file that includes a parser header.

The explicit non-goal: nothing WEM-*semantic* is ever generated. `CommonMaterial`,
the node model and the channel table are designed, not derived. This backend's
jurisdiction ends at the `native` boundary.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path
from typing import Optional

from .ir import (BindClass, BindEnum, BindField, BindModule, ModuleConfig,
                 TypeKind, TypeRef)

SCHEMA_LOCK = 'tools/codegen/wem_schema.lock'
NL = chr(10)
PARA = NL + NL

# Math types WEM already shares with the parsers. These are mirrored by
# identity: `Vector3f` means the same thing on both sides of the boundary, and
# a `WemVector3f` would be a copy of a copy.
SHARED_VALUE_TYPES = {
    'whiteout::Vector2f': 'Vector2f',
    'whiteout::Vector3f': 'Vector3f',
    'whiteout::Vector4f': 'Vector4f',
    'whiteout::Quaternion': 'Quaternion',
}

SCALAR_DEFAULTS = {'bool': 'false'}


class GenerationError(RuntimeError):
    """A schema-lock violation or an un-mirrorable type. Never a warning: both
    mean the emitted header would be wrong, and a wrong mirror is a wrong file
    format."""


# ── name resolution ─────────────────────────────────────────────────────────

def _short(qualifier: str) -> str:
    """'Layer::SubTexture' -> 'LayerSubTexture'; 'Material' -> 'Material'."""
    return qualifier.replace('::', '')


def _wem_of(obj, overrides: dict, qualifier: str) -> dict:
    """The `@wem` directives for a type: header annotation, then the module's
    sidecar table merged over it. The sidecar wins because it exists for
    headers that are themselves machine-written (`sno/d3/native`), where the
    annotation cannot survive the next regeneration."""
    merged = dict(getattr(obj, 'wem', None) or {})
    merged.update((overrides.get(qualifier) or {}).get('_type', {}))
    return merged


def _field_wem(field, overrides: dict, qualifier: str) -> dict:
    merged = dict(field.wem or {})
    merged.update((overrides.get(qualifier) or {}).get(field.cpp_name, {}))
    return merged


# ── the closure ─────────────────────────────────────────────────────────────

class Mirror:
    """One module's mirror set: the types reachable from the roots, their
    mirror names, and the order they must be declared in."""

    def __init__(self, module: BindModule, config: ModuleConfig):
        self.module = module
        self.config = config
        self.spec = config.wem_native
        self.overrides = self.spec.overrides

        self.ns_prefix = config.cpp_namespace + '::'
        self.classes: dict[str, BindClass] = {c.cpp_qualifier: c for c in module.classes}
        self.enums: dict[str, BindEnum] = {e.cpp_qualifier: e for e in module.enums}
        self.templates = {t.cpp_short: t for t in module.templates}

        self.used_classes: list[str] = []      # qualifiers, dependency order
        self.used_enums: list[str] = []
        self.used_templates: list[str] = []    # template short names
        self.dropped: list[str] = []           # "Type.field: reason"
        self.manual: list[str] = []            # "Type.field"

        self._visiting: set[str] = set()
        for name in self.spec.extra_enums:
            if name not in self.enums:
                raise GenerationError(
                    f'wem_native extra_enum {name!r} is not a bound enum in module '
                    f'{module.name!r}')
            self.used_enums.append(name)
        for root in self.spec.roots:
            if root not in self.classes:
                raise GenerationError(
                    f'wem_native root {root!r} is not a bound class in module '
                    f'{module.name!r} — check the name, or that its header is listed')
            self._visit_class(root)

    # -- names --

    def qual(self, cpp_text: str) -> str:
        """`whiteout::mdx::Layer::SubTexture` -> `Layer::SubTexture`. The IR keys
        types by the qualifier inside the module namespace but spells field types
        fully; everything here works in the former."""
        return (cpp_text[len(self.ns_prefix):] if cpp_text.startswith(self.ns_prefix)
                else cpp_text)

    def mirror_name(self, qualifier: str) -> str:
        obj = self.classes.get(qualifier) or self.enums.get(qualifier)
        if obj is not None:
            wem = _wem_of(obj, self.overrides, qualifier)
            if wem.get('rename'):
                return wem['rename']
        return self.spec.prefix + _short(qualifier)

    def template_name(self, short: str) -> str:
        return self.spec.prefix + short

    # -- closure --

    def _visit_class(self, qualifier: str) -> None:
        if qualifier in self.used_classes or qualifier in self._visiting:
            return
        if qualifier in self.spec.exclude:
            return
        cls = self.classes[qualifier]
        self._visiting.add(qualifier)
        for f in self.fields_of(cls):
            self._visit_type(f.type, f'{_short(qualifier)}.{f.cpp_name}')
        self._visiting.discard(qualifier)
        self.used_classes.append(qualifier)

    def _visit_type(self, t: TypeRef, where: str) -> None:
        base = _strip_ns(t.cpp_text)
        if t.kind == TypeKind.ENUM:
            q = self.qual(t.cpp_text)
            if q in self.enums and q not in self.used_enums:
                self.used_enums.append(q)
            return
        if t.kind in (TypeKind.VECTOR, TypeKind.OPTIONAL, TypeKind.ARRAY,
                      TypeKind.NESTED_VEC):
            if t.element is not None:
                self._visit_type(t.element, where)
            return
        if t.kind != TypeKind.NESTED:
            return
        if t.cpp_text in SHARED_VALUE_TYPES:
            return
        tpl = _template_short(t.cpp_text)
        if tpl and tpl in self.templates:
            if tpl not in self.used_templates:
                self.used_templates.append(tpl)
            if t.element is not None:
                self._visit_type(t.element, where)
            return
        if self.qual(t.cpp_text) in self.classes:
            self._visit_class(self.qual(t.cpp_text))
            return
        self.dropped.append(f'{where}: no mirror for {base}')

    # -- per-type field lists, `@wem skip` already applied --

    def fields_of(self, cls: BindClass) -> list:
        out = []
        for f in cls.fields:
            wem = _field_wem(f, self.overrides, cls.cpp_qualifier)
            if wem.get('skip'):
                continue
            out.append(f)
        # Aliased fields go back where they were declared: the field order is
        # the byte order, so "wherever it landed" is not good enough.
        for position, f in self._aliased(cls):
            out.insert(min(position, len(out)), f)
        return out

    def _aliased(self, cls: BindClass) -> list:
        """Fields the IR could not classify but `@wem as=` gives a mirror for.

        The case this exists for is a one-field wrapper struct — `m3::Flag` is
        a `u32` in a box — where the mirror wants the scalar and the copy needs
        one member hop to reach it (`@wem member=value`).

        Everything else that the walk dropped is a hole in the mirror, and a
        hole in a mirror is a lossy round trip that nothing downstream would
        report. So it raises."""
        out, unresolved = [], []
        for name, cpp, wem, doc, position in getattr(cls, 'unbindable_fields', []):
            merged = dict(wem or {})
            merged.update((self.overrides.get(cls.cpp_qualifier) or {}).get(name, {}))
            if merged.get('skip'):
                continue
            if not merged.get('as'):
                unresolved.append(f'    {_short(cls.cpp_qualifier)}.{name}  ({cpp})')
                continue
            out.append((position, BindField(
                name=name, cpp_name=name,
                type=TypeRef(cpp_text=merged['as'], kind=TypeKind.PRIMITIVE),
                doc=doc, wem=merged)))
        if unresolved:
            raise GenerationError(
                'these fields exist in the parser header but the IR could not '
                'classify them, so the mirror would lose them silently:' + NL +
                NL.join(unresolved) + NL + NL +
                '  Give each one `@wem as=<type>` (with `@wem member=<m>` when the '
                'value' + NL +
                '  needs a member hop to reach) or `@wem skip` if it genuinely does not '
                'belong' + NL + '  in the native block.')
        return out

    def wem_for(self, cls: BindClass, f) -> dict:
        return _field_wem(f, self.overrides, cls.cpp_qualifier)

    def field_name(self, cls: BindClass, f) -> str:
        return self.wem_for(cls, f).get('rename') or f.cpp_name

    # -- the mirror spelling of a type --

    def cpp_of(self, t: TypeRef) -> str:
        if t.kind == TypeKind.PRIMITIVE:
            return _strip_ns(t.cpp_text)
        if t.kind == TypeKind.STRING:
            return 'std::string'
        if t.kind == TypeKind.ENUM:
            return self.mirror_name(self.qual(t.cpp_text))
        if t.kind == TypeKind.VECTOR:
            return f'std::vector<{self.cpp_of(t.element)}>'
        if t.kind == TypeKind.NESTED_VEC:
            return f'std::vector<std::vector<{self.cpp_of(t.element)}>>'
        if t.kind == TypeKind.OPTIONAL:
            return f'std::optional<{self.cpp_of(t.element)}>'
        if t.kind == TypeKind.ARRAY:
            return f'std::array<{self.cpp_of(t.element)}, {t.array_size}>'
        if t.kind == TypeKind.NESTED:
            if t.cpp_text in SHARED_VALUE_TYPES:
                return SHARED_VALUE_TYPES[t.cpp_text]
            tpl = _template_short(t.cpp_text)
            if tpl and tpl in self.templates:
                return f'{self.template_name(tpl)}<{self.cpp_of(t.element)}>'
            return self.mirror_name(self.qual(t.cpp_text))
        raise GenerationError(f'un-mirrorable type {t.cpp_text!r}')


def _strip_ns(text: str) -> str:
    return re.sub(r'\bwhiteout::(?:\w+::)*', '', text)


def _template_short(cpp_text: str) -> Optional[str]:
    m = re.match(r'^(?:\w+::)*(\w+)<', cpp_text)
    return m.group(1) if m else None


# ── flag detection ──────────────────────────────────────────────────────────

_WIDTHS = {'u8': 8, 'i8': 8, 'u16': 16, 'i16': 16, 'u32': 32, 'i32': 32,
           'int': 32, 'unsigned int': 32, 'u64': 64, 'i64': 64}


def _values_of(enum: BindEnum) -> list:
    """Enumerator values as the *underlying type* sees them.

    libclang hands back a signed int, so a `0x80000000` flag arrives as
    -2147483648. Emitting that into an `enum class : u32` is a narrowing error,
    and it also defeats flag detection — the highest bit of a flag word is
    exactly the one most likely to be there."""
    width = _WIDTHS.get(enum.underlying, 32)
    signed = enum.underlying.startswith('i') or enum.underlying in ('int', 'long')
    mask = (1 << width) - 1
    return [(v, v.value if signed else v.value & mask) for v in enum.values]


def _is_flag_enum(enum: BindEnum) -> bool:
    """True when the enumerators are a bit vocabulary rather than a list.

    Requires at least three distinct non-zero values, every one a power of two,
    and a maximum of 4 or more. `{0, 1, 2}` is excluded by the maximum: a
    three-valued list is far more common than a two-bit flag word, and emitting
    `operator|` for it would be noise."""
    values = {n for _, n in _values_of(enum) if n != 0}
    if len(values) < 3 or max(values) < 4:
        return False
    return all(v > 0 and (v & (v - 1)) == 0 for v in values)


# ── schema lock ─────────────────────────────────────────────────────────────

def _block_signature(kind: str, name: str, rows: list[str]) -> str:
    return hashlib.sha256(('|'.join([kind, name] + rows)).encode('utf-8')).hexdigest()[:16]


def _reconcile_lock(repo_root: Path, module_name: str, signatures: dict,
                    bump: set) -> tuple[dict, list[str]]:
    """Merge this run's block signatures into the lock.

    Returns (per-block version map, report lines). Raises when a block's shape
    moved and the caller did not name it in `--wem-bump` — the whole reason the
    lock exists. Accepting a change is therefore a deliberate act that shows up
    in the command line and, through the version, in the file format itself.
    """
    path = repo_root / SCHEMA_LOCK
    lock = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    blocks = lock.get('blocks', {}).get(module_name, {})

    versions: dict[str, int] = {}
    report: list[str] = []
    violations: list[str] = []

    for name, sig in signatures.items():
        entry = blocks.get(name)
        if entry is None:
            versions[name] = 1
            report.append(f'  + {name} (new block, version 1)')
            continue
        if entry['hash'] == sig:
            versions[name] = entry['version']
            continue
        if name in bump or 'all' in bump:
            versions[name] = entry['version'] + 1
            report.append(f'  ^ {name} (shape changed, version '
                          f'{entry["version"]} -> {versions[name]})')
            continue
        violations.append(name)

    if violations:
        lines = NL.join(
            f'    {module_name}.{n}: locked {blocks[n]["hash"]} at version '
            f'{blocks[n]["version"]}, generated {signatures[n]}'
            for n in violations)
        raise GenerationError(
            'schema lock: these native blocks changed shape.' + NL + lines + NL + NL +
            '  A native block is part of the WEM file format, so its shape cannot move' + NL +
            '  quietly. Re-run with --wem-bump ' + ','.join(violations) + ' to accept the' + NL +
            "  change and raise each block's chunk version, which is what a reader uses" + NL +
            '  to take the compatibility path. If you did not intend this, a parser' + NL +
            '  struct moved under you.')

    for name in blocks:
        if name not in signatures:
            report.append(f'  - {name} (no longer reachable from the roots)')

    return versions, report


def _write_lock(repo_root: Path, module_name: str, signatures: dict,
                versions: dict) -> None:
    path = repo_root / SCHEMA_LOCK
    lock = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    lock.setdefault(
        '_comment',
        'Per-block schema lock for the wem-native mirrors (WEM design §15.2). '
        'A block whose hash changes must have its version bumped by hand in this '
        'file, or generation fails. The version becomes ChunkTagTraits<T>::max_version.')
    blocks = lock.setdefault('blocks', {}).setdefault(module_name, {})
    for name, sig in signatures.items():
        blocks[name] = {'version': versions.get(name, 1), 'hash': sig}
    for name in list(blocks):
        if name not in signatures:
            del blocks[name]
    lock['blocks'][module_name] = dict(sorted(blocks.items()))
    lock['blocks'] = dict(sorted(lock['blocks'].items()))
    with open(path, 'w', encoding='utf-8', newline='\n') as fp:
        json.dump(lock, fp, indent=2)
        fp.write('\n')


# ── emission ────────────────────────────────────────────────────────────────

HEADER_BANNER = """// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// GENERATED by tools/codegen (--backend wem-native) from the {module} parser
// headers. Do not edit: re-run the generator and commit the diff.
//
// A block whose shape changes here must have its version bumped in
// {lock} — see WEM design §15.2.
"""


def _doc_block(doc: str, indent: str) -> list[str]:
    """The source doc comment, rewrapped, paragraphs preserved. A `@brief`
    line and the body that follows it are two paragraphs; running them
    together produces a sentence that reads like a parser error."""
    if not doc:
        return []
    out: list[str] = []
    width = 84 - len(indent)
    for n, para in enumerate(p for p in doc.split(PARA) if p.strip()):
        if n:
            out.append(f'{indent}///')
        line = ''
        for word in ' '.join(para.split()).split():
            if line and len(line) + 1 + len(word) > width:
                out.append(f'{indent}/// {line}')
                line = word
            else:
                line = f'{line} {word}'.strip()
        if line:
            out.append(f'{indent}/// {line}')
    return out


def _default_for(mirror: Mirror, t: TypeRef) -> str:
    """The in-class initialiser. Everything scalar gets one; containers do not
    need one and reading `std::vector<X> layers{};` is worse than reading
    `std::vector<X> layers;`."""
    if t.kind in (TypeKind.STRING, TypeKind.VECTOR, TypeKind.NESTED_VEC,
                  TypeKind.OPTIONAL):
        return ''
    if t.kind == TypeKind.PRIMITIVE:
        name = _strip_ns(t.cpp_text)
        return f' = {SCALAR_DEFAULTS.get(name, "{}")}' if name in SCALAR_DEFAULTS else '{}'
    return '{}'


def _emit_enum(mirror: Mirror, qualifier: str) -> tuple[list[str], list[str]]:
    enum = mirror.enums[qualifier]
    name = mirror.mirror_name(qualifier)
    lines = _doc_block(enum.doc or f'Mirror of `{enum.cpp_namespace}::{qualifier}`.', '')
    lines.append(f'enum class {name} : {enum.underlying} {{')
    seen: set[int] = set()
    rows: list[str] = []
    flags = _is_flag_enum(enum)
    for v, value in _values_of(enum):
        # A C++ alias enumerator (two names, one value) mirrors as one name:
        # the mirror is data, and two spellings of one integer say nothing the
        # first spelling does not.
        if value in seen:
            continue
        seen.add(value)
        literal = (f'0x{value:X}' if (flags or value > 0xFFFF) and value
                   else str(value))
        doc = f' ///< {" ".join(v.doc.split())}' if v.doc else ''
        lines.append(f'    {v.js_name} = {literal},{doc}')
        rows.append(f'{v.js_name}={value}')
    lines.append('};')
    if _is_flag_enum(enum):
        lines.append(f'WHITEOUT_WEM_DEFINE_FLAG_OPERATORS({name})')
    # A name for the value, because every diagnostic this enum reaches wants one
    # and an integer in a message is a lookup the reader has to do by hand.
    lines.append('')
    lines.append(f'inline const char* ToString({name} value) {{')
    lines.append('    switch (value) {')
    for spelling in [row.split('=')[0] for row in rows]:
        lines.append(f'    case {name}::{spelling}:')
        lines.append(f'        return "{spelling}";')
    lines.append('    default:')
    lines.append('        return "?";')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    return lines, rows


def _emit_template(mirror: Mirror, short: str) -> tuple[list[str], list[str]]:
    tpl = mirror.templates[short]
    name = mirror.template_name(short)
    lines = _doc_block(tpl.doc or f'Mirror of `{tpl.cpp_qualifier}<T>`.', '')
    lines.append('template <class T>')
    lines.append(f'struct {name} {{')
    rows: list[str] = []
    body: list[str] = []
    for f in tpl.fields:
        if (f.wem or {}).get('skip'):
            continue
        raw = f.cpp_text_template
        spelled = 'T' if raw.strip() in (tpl.type_param, 'T') else _strip_ns(raw)
        fname = (f.wem or {}).get('rename') or f.cpp_name
        lines.extend(_doc_block(f.doc, '    '))
        init = '' if spelled.startswith(('std::vector', 'std::string')) else '{}'
        lines.append(f'    {spelled} {fname}{init};')
        verb = 'inlineList' if spelled.startswith('std::vector') else 'field'
        body.append(f'        v.{verb}("{fname}", {fname});')
        rows.append(f'{fname}:{spelled}')
    lines.append('')
    lines.append('    template <class V>')
    lines.append('    void reflect(V& v) {')
    lines.extend(body)
    lines.append('    }')
    lines.append('};')
    lines.append('')
    return lines, rows


def _emit_struct(mirror: Mirror, qualifier: str,
                 versioned: bool) -> tuple[list[str], list[str]]:
    cls = mirror.classes[qualifier]
    name = mirror.mirror_name(qualifier)
    lines = _doc_block(cls.doc or f'Mirror of `{cls.cpp_namespace}::{qualifier}`.', '')
    lines.append(f'struct {name} {{')
    rows: list[str] = []
    body: list[str] = []

    if versioned:
        lines.append('    /// The format version this record was read from. No parser struct')
        lines.append('    /// carries it, and a converter cannot read the record without it.')
        lines.append('    u32 sourceVersion{};')
        lines.append('')
        body.append('        v.field("sourceVersion", sourceVersion);')
        rows.append('sourceVersion:u32')

    for f in mirror.fields_of(cls):
        wem = mirror.wem_for(cls, f)
        fname = mirror.field_name(cls, f)
        spelled = mirror.cpp_of(f.type)
        doc = f.doc
        if wem.get('manual'):
            mirror.manual.append(f'{name}.{fname}')
            doc = (doc + ' ' if doc else '') + '(converter-owned: not copied mechanically)'
        lines.extend(_doc_block(doc, '    '))
        lines.append(f'    {spelled} {fname}{_default_for(mirror, f.type)};')
        # A mirror's arrays are written inline, never as separately tagged
        # chunks: §11.3 reserves one tag per native *block*, not one per struct
        # inside it, and nothing addresses a `MdxLayer` run on its own.
        if f.type.kind == TypeKind.OPTIONAL:
            verb = 'optional'
        elif spelled.startswith('std::vector'):
            verb = 'inlineList'
        else:
            verb = 'field'
        since = wem.get('since')
        prefix = f'v.since({since}).' if since else 'v.'
        body.append(f'        {prefix}{verb}("{fname}", {fname});')
        rows.append(f'{fname}:{spelled}' + (f'@{since}' if since else ''))

    lines.append('')
    lines.append('    template <class V>')
    lines.append('    void reflect(V& v) {')
    lines.extend(body)
    lines.append('    }')
    lines.append('};')
    lines.append('')
    return lines, rows


def emit_header(module: BindModule, config: ModuleConfig, repo_root: Path,
                bump: set) -> tuple[str, Mirror, dict]:
    spec = config.wem_native
    mirror = Mirror(module, config)

    signatures: dict[str, str] = {}
    enum_lines: list[str] = []
    for qualifier in mirror.used_enums:
        lines, rows = _emit_enum(mirror, qualifier)
        enum_lines.extend(lines)
        signatures[mirror.mirror_name(qualifier)] = _block_signature(
            'enum', mirror.mirror_name(qualifier), rows)

    tpl_lines: list[str] = []
    for short in mirror.used_templates:
        lines, rows = _emit_template(mirror, short)
        tpl_lines.extend(lines)
        signatures[mirror.template_name(short)] = _block_signature(
            'template', mirror.template_name(short), rows)

    struct_lines: list[str] = []
    for qualifier in mirror.used_classes:
        versioned = qualifier in spec.versioned_roots
        lines, rows = _emit_struct(mirror, qualifier, versioned)
        struct_lines.extend(lines)
        signatures[mirror.mirror_name(qualifier)] = _block_signature(
            'struct', mirror.mirror_name(qualifier), rows)

    versions, report = _reconcile_lock(repo_root, module.name, signatures, bump)

    out: list[str] = [HEADER_BANNER.format(module=module.name, lock=SCHEMA_LOCK)]
    out.append('#pragma once')
    out.append('')
    out.append('#include <optional>')
    out.append('#include <string>')
    out.append('#include <vector>')
    out.append('')
    out.append('#include <whiteout/common_types.h>')
    out.append('#include <whiteout/models/wem/chunk_traits.h>')
    out.append('#include <whiteout/models/wem/native/flag_ops.h>')
    out.append('#include <whiteout/vector_types.h>')
    out.append('')
    out.append('namespace whiteout {')
    out.append('namespace models {')
    out.append('namespace wem {')
    out.append('namespace native {')
    out.append('')
    out.extend(enum_lines)
    out.extend(tpl_lines)
    out.extend(struct_lines)

    out.append('/// Fields carrying `@wem manual`: mirrored, but the copy halves leave them')
    out.append('/// to the converter. Named here so a field newly added to a parser header')
    out.append('/// cannot end up silently half-copied — it either copies, or a test names it.')
    if mirror.manual:
        out.append(f'inline constexpr const char* k{spec.prefix}ManualFields[] = {{')
        for entry in mirror.manual:
            out.append(f'    "{entry}",')
        out.append('};')
    else:
        out.append(f'inline constexpr const char* const* k{spec.prefix}ManualFields = nullptr;')
    out.append(f'inline constexpr std::size_t k{spec.prefix}ManualFieldCount = '
               f'{len(mirror.manual)};')
    out.append('')
    out.append('} // namespace native')
    out.append('')

    tagged = [(mirror.mirror_name(q), spec.tags[_short(q)])
              for q in mirror.used_classes if _short(q) in spec.tags]
    if tagged:
        out.append('// Chunk identity for the mirrors that are chunks of their own. A mirror')
        out.append('// with no tag here is inline data inside its parent\'s chunk.')
        for name, tag in tagged:
            out.append(f'template <>')
            out.append(f'struct ChunkTagTraits<native::{name}> {{')
            out.append(f'    static constexpr u32 value = kTag("{tag}");')
            out.append(f'    static constexpr u32 max_version = {versions.get(name, 1)};')
            out.append(f'    static constexpr bool is_trivial = false;')
            out.append('};')
            out.append('')

    out.append('} // namespace wem')
    out.append('} // namespace models')
    out.append('} // namespace whiteout')
    return '\n'.join(out) + '\n', mirror, {'signatures': signatures,
                                           'versions': versions,
                                           'report': report}


# ── the copy halves ─────────────────────────────────────────────────────────

def _copy_stmt(mirror: Mirror, cls: BindClass, f, to_native: bool) -> list[str]:
    """One field's copy, in whichever direction. `src`/`dst` are the two
    records; the mirror side is `dst` going out and `src` coming back."""
    name = f.cpp_name
    mname = mirror.field_name(cls, f)
    lhs = f'dst.{mname}' if to_native else f'dst.{name}'
    rhs = f'src.{name}' if to_native else f'src.{mname}'
    t = f.type

    member = mirror.wem_for(cls, f).get('member')
    if member:
        # A one-field wrapper: the mirror holds the scalar, the parser holds
        # the box.
        return [f'    dst.{mname} = src.{name}.{member};' if to_native
                else f'    dst.{name}.{member} = src.{mname};']
    if t.kind in (TypeKind.PRIMITIVE, TypeKind.STRING):
        return [f'    {lhs} = {rhs};']
    if t.kind == TypeKind.ENUM:
        target = ('native::' + mirror.mirror_name(mirror.qual(t.cpp_text)) if to_native
                  else t.cpp_text)
        # Two unrelated scoped enums have no direct conversion; the integer in
        # the middle is required, and is also what keeps an undocumented flag
        # bit alive across the mirror.
        return [f'    {lhs} = static_cast<{target}>(static_cast<u32>({rhs}));']
    if t.kind == TypeKind.NESTED and t.cpp_text in SHARED_VALUE_TYPES:
        return [f'    {lhs} = {rhs};']
    if t.kind == TypeKind.NESTED:
        return [f'    Copy{"To" if to_native else "From"}Native({rhs}, {lhs});']
    if t.kind == TypeKind.OPTIONAL:
        inner = _element_copy(mirror, t.element, to_native, '*src_opt', '*dst_opt')
        out = [f'    if ({rhs}.has_value()) {{',
               f'        {lhs}.emplace();',
               f'        const auto& src_opt = {rhs};',
               f'        auto& dst_opt = {lhs};']
        out += [f'    {line}' for line in inner]
        out += ['    } else {', f'        {lhs}.reset();', '    }']
        return out
    if t.kind == TypeKind.VECTOR:
        inner = _element_copy(mirror, t.element, to_native, 'src_v[i]', 'dst_v[i]')
        out = [f'    {{',
               f'        const auto& src_v = {rhs};',
               f'        auto& dst_v = {lhs};',
               f'        dst_v.resize(src_v.size());',
               f'        for (std::size_t i = 0; i < src_v.size(); ++i) {{']
        out += [f'        {line}' for line in inner]
        out += ['        }', '    }']
        return out
    if t.kind == TypeKind.ARRAY:
        inner = _element_copy(mirror, t.element, to_native, 'src_a[i]', 'dst_a[i]')
        out = [f'    {{',
               f'        const auto& src_a = {rhs};',
               f'        auto& dst_a = {lhs};',
               f'        for (std::size_t i = 0; i < src_a.size(); ++i) {{']
        out += [f'        {line}' for line in inner]
        out += ['        }', '    }']
        return out
    raise GenerationError(f'no copy rule for {cls.cpp_qualifier}.{name} ({t.cpp_text})')


def _element_copy(mirror: Mirror, t: TypeRef, to_native: bool,
                  src: str, dst: str) -> list[str]:
    if t.kind in (TypeKind.PRIMITIVE, TypeKind.STRING):
        return [f'    {dst} = {src};']
    if t.kind == TypeKind.ENUM:
        target = ('native::' + mirror.mirror_name(mirror.qual(t.cpp_text)) if to_native
                  else t.cpp_text)
        return [f'    {dst} = static_cast<{target}>(static_cast<u32>({src}));']
    if t.kind == TypeKind.NESTED and t.cpp_text in SHARED_VALUE_TYPES:
        return [f'    {dst} = {src};']
    if t.kind == TypeKind.NESTED:
        return [f'    Copy{"To" if to_native else "From"}Native({src}, {dst});']
    raise GenerationError(f'no element copy rule for {t.cpp_text}')


def _copy_decls(mirror: Mirror) -> list[str]:
    """Forward declarations for every non-template pair.

    Required, not tidiness: a mirrored class template's body calls
    `CopyToNative` on its value type, and two-phase lookup resolves that at
    definition time. ADL cannot help — the overloads live in `wem`, while the
    arguments live in `wem::native` and the parser's own namespace — so a
    declaration must precede the template or `AnimRef<ColorBGRA>` fails to
    instantiate."""
    out: list[str] = []
    for qualifier in mirror.used_classes:
        cls = mirror.classes[qualifier]
        name = mirror.mirror_name(qualifier)
        src = f'{cls.cpp_namespace}::{qualifier}'
        out.append(f'inline void CopyToNative(const {src}& src, native::{name}& dst);')
        out.append(f'inline void CopyFromNative(const native::{name}& src, {src}& dst);')
    if out:
        out.append('')
    return out


def _copy_pair(mirror: Mirror, qualifier: str, versioned: bool) -> list[str]:
    cls = mirror.classes[qualifier]
    name = mirror.mirror_name(qualifier)
    src_type = f'{cls.cpp_namespace}::{qualifier}'
    fields = [f for f in mirror.fields_of(cls)
              if not mirror.wem_for(cls, f).get('manual')]

    out: list[str] = []
    out.append(f'inline void CopyToNative(const {src_type}& src, native::{name}& dst) {{')
    if versioned:
        out.append('    // sourceVersion is the converter\'s to write: the record does not')
        out.append('    // know which file it came out of.')
    for f in fields:
        out.extend(_copy_stmt(mirror, cls, f, True))
    if not fields:
        out.append('    (void)src;')
        out.append('    (void)dst;')
    out.append('}')
    out.append('')
    out.append(f'inline void CopyFromNative(const native::{name}& src, {src_type}& dst) {{')
    for f in fields:
        out.extend(_copy_stmt(mirror, cls, f, False))
    if not fields:
        out.append('    (void)src;')
        out.append('    (void)dst;')
    out.append('}')
    out.append('')
    return out


def _template_copy(mirror: Mirror, short: str) -> list[str]:
    """The copy halves for a mirrored class template.

    Two type parameters, not one: `AnimRef<ColorBGRA>` mirrors as
    `M3AnimRef<M3ColorBGRA>`, so the value type differs across the boundary even
    though the container does not. The per-member copies then go through
    `CopyToNative` so a scalar `T` takes the identity overload and a mirrored `T`
    takes its own generated one."""
    tpl = mirror.templates[short]
    name = mirror.template_name(short)
    source = tpl.cpp_qualifier
    if not source.startswith(tpl.cpp_namespace) and tpl.cpp_namespace:
        source = f'{tpl.cpp_namespace}::{source}'
    fields = [f for f in tpl.fields if not (f.wem or {}).get('skip')]

    out: list[str] = []
    for to_native in (True, False):
        fn = 'CopyToNative' if to_native else 'CopyFromNative'
        lhs_t = f'native::{name}<D>' if to_native else f'{source}<D>'
        rhs_t = f'{source}<S>' if to_native else f'native::{name}<S>'
        out.append('template <class S, class D>')
        out.append(f'inline void {fn}(const {rhs_t}& src, {lhs_t}& dst) {{')
        for f in fields:
            fname = (f.wem or {}).get('rename') or f.cpp_name
            a = f.cpp_name if to_native else fname
            b = fname if to_native else f.cpp_name
            is_value = f.cpp_text_template.strip() in (tpl.type_param, 'T')
            if is_value:
                out.append(f'    {fn}(src.{a}, dst.{b});')
            else:
                out.append(f'    dst.{b} = src.{a};')
        out.append('}')
        out.append('')
    return out


COPY_BANNER = """// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// GENERATED by tools/codegen (--backend wem-native). Do not edit.
//
// The mechanical half of the {module} material converter: every field that maps
// 1:1 between the parser record and its WEM mirror. Fields that do not map —
// resolved references, version-gated meanings — carry `@wem manual`, are absent
// here, and are named in `native::k{prefix}ManualFields` so the converter's
// tests can assert nobody forgot one.
//
// Private to src/ on purpose: this is the one generated file that includes a
// parser header, and §7.3's whole argument is that the public WEM headers do not.
"""


def emit_copy(module: BindModule, config: ModuleConfig, mirror: Mirror) -> str:
    spec = config.wem_native
    out = [COPY_BANNER.format(module=module.name, prefix=spec.prefix)]
    out.append('#pragma once')
    out.append('')
    out.append('#include <cstddef>')
    out.append('')
    for header in _parser_includes(config):
        out.append(f'#include <{header}>')
    out.append(f'#include <{spec.header_path.split("include/", 1)[-1]}>')
    out.append('')
    out.append('namespace whiteout {')
    out.append('namespace models {')
    out.append('namespace wem {')
    out.append('')
    out.extend(_copy_decls(mirror))
    if mirror.used_templates:
        out.append("// Identity copy, so a mirrored class template's body can say")
        out.append('// `CopyToNative(x, y)` without knowing whether its value type is a scalar')
        out.append('// (same type both sides) or a mirrored struct (its own overload, below).')
        out.append('template <class T>')
        out.append('inline void CopyToNative(const T& src, T& dst) {')
        out.append('    dst = src;')
        out.append('}')
        out.append('')
        out.append('template <class T>')
        out.append('inline void CopyFromNative(const T& src, T& dst) {')
        out.append('    dst = src;')
        out.append('}')
        out.append('')
    for short in mirror.used_templates:
        out.extend(_template_copy(mirror, short))
    for qualifier in mirror.used_classes:
        out.extend(_copy_pair(mirror, qualifier,
                              qualifier in spec.versioned_roots))
    out.append('} // namespace wem')
    out.append('} // namespace models')
    out.append('} // namespace whiteout')
    return '\n'.join(out) + '\n'


def _parser_includes(config: ModuleConfig) -> list[str]:
    """The module's own structure headers, as include paths. The parser config
    lists them repo-relative; everything under `include/` is on the include
    path at build time."""
    out = []
    for h in config.headers:
        if not h.startswith('include/'):
            continue
        rel = h[len('include/'):]
        if 'vector_types' in rel or rel.endswith(('parser.h', 'writer.h')):
            continue
        out.append(rel)
    return out


# ── entry point ─────────────────────────────────────────────────────────────

def _clang_format(text: str, repo_root: Path) -> str:  # noqa: ARG001
    """Run the repo's clang-format over generated text.

    Formatting here rather than leaving it to `format.bat` is what keeps the
    freshness check honest: `format.bat` sweeps all of `src/` and `include/`,
    so unformatted output would be reformatted in place and then read as stale
    on the next check, for no change anyone made."""
    exe = shutil.which('clang-format')
    # The style file comes from the repo this generator lives in, not from
    # `repo_root` — the freshness check runs the emitter against a throwaway
    # tree that holds nothing but the lock, and formatting must not vary with
    # where the output happens to be written.
    style = Path(__file__).resolve().parents[2] / '.clang-format'
    if exe is None or not style.is_file():
        return text
    try:
        done = subprocess.run([exe, f'-style=file:{style}'], input=text,
                              capture_output=True, text=True, encoding='utf-8')
    except OSError:
        return text
    return done.stdout if done.returncode == 0 and done.stdout else text


def emit(module: BindModule, config: ModuleConfig, repo_root: Path,
         bump: Optional[set] = None) -> dict:
    """Returns {repo-relative path: text} plus a '_report' key of notes."""
    if config.wem_native is None:
        raise GenerationError(
            f'module {config.name!r} has no wem_native config — add one to '
            f'tools/codegen/modules/{config.name}.py')
    spec = config.wem_native
    header, mirror, meta = emit_header(module, config, repo_root, bump or set())
    copy = emit_copy(module, config, mirror)
    _write_lock(repo_root, module.name, meta['signatures'], meta['versions'])

    notes = list(meta['report'])
    for d in mirror.dropped:
        notes.append(f'  ! dropped {d}')
    for m in mirror.manual:
        notes.append(f'  ~ manual {m}')
    return {
        spec.header_path: _clang_format(header, repo_root),
        f'src/whiteout/models/wem/native/{config.name}_copy.h':
            _clang_format(copy, repo_root),
        '_report': notes,
    }
