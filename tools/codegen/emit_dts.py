# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> TypeScript ambient declarations.

Produces one `.d.ts` per module describing the JS-facing surface for
TypeScript callers. The aggregator (`packages/js-ts/index.d.ts`, copied
in from `bindings/templates/js-ts/`) imports these and bolts on the
WhiteoutAPI interface.

Naming mirrors the JS facade:
    whiteout.mdx.Bone, whiteout.mdx.NoParent, whiteout.mdx.NodeFlag.Bone
    whiteout.Vector3f, whiteout.Texture, whiteout.PixelFormat
    whiteout.blp.parse(bytes)

Conventions for the emitted TS:
    * value_object types  -> `export interface { ... }` (plain JS objects)
    * class types         -> `export class extends EmbindObject { ... }`
    * enums               -> `export const Name: { readonly Member: EnumMember; ... }`
    * constants           -> `export const NAME: number;`
    * vector container    -> `EmbindVector<T>` (generic; users never see the
                              underlying VectorXxx class name)
    * track instantiations -> dedicated `class TrackF32 { ... }`, etc.

Beautiful TS is the goal: tight indentation, namespace grouping, no
redundant `whiteout.` qualifiers (each module file declares its own
namespace), TSDoc preserved where source comments exist.
"""

from __future__ import annotations

from io import StringIO
from .ir import (
    BindClass, BindEnum, BindField, BindMethod, BindModule,
    TypeKind, TypeRef,
)


# Math types are bound at root (whiteout.Vector3f) but referenced from
# many modules — declare as imports from the shared file.
_SHARED_MATH = {'Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'}


def _ts_primitive(short: str) -> str:
    if short in ('u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64',
                 'f32', 'f64'):
        return 'number'
    if short == 'bool':
        return 'boolean'
    return 'number'  # default for any unknown primitive — Embind sends number


# ── Buffer-friendly element -> JS TypedArray ──────────────────────────────
# Mirrors emit_embind._buffer_descriptor; vectors of these elements expose
# `.view()` returning the matching TypedArray.
_TYPED_ARRAY_FOR_PRIMITIVE = {
    'u8':  'Uint8Array',
    'u16': 'Uint16Array',
    'u32': 'Uint32Array',
    'u64': 'BigUint64Array',
    'i8':  'Int8Array',
    'i16': 'Int16Array',
    'i32': 'Int32Array',
    'i64': 'BigInt64Array',
    'f32': 'Float32Array',
    'f64': 'Float64Array',
}


def _typed_array_for_element(t: TypeRef) -> str | None:
    """Return the JS TypedArray name for vector<T>.view() iff T is buffer-
    friendly (primitive or one of the standard math structs)."""
    if t.kind == TypeKind.PRIMITIVE:
        short = _strip_namespace(t.cpp_text)
        return _TYPED_ARRAY_FOR_PRIMITIVE.get(short)
    if t.kind == TypeKind.NESTED:
        short = _strip_namespace(t.cpp_text).replace('::', '')
        if short in ('Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'):
            return 'Float32Array'
        if short == 'ColorBGRA':
            return 'Uint8Array'
    return None


def _strip_namespace(s: str) -> str:
    """Drop leading lowercase namespace components (whiteout::mdx:: etc.)."""
    parts = s.split('::')
    while len(parts) > 1 and parts[0] and parts[0][0].islower():
        parts = parts[1:]
    return '::'.join(parts)


def _flatten_qualified(qual: str) -> str:
    """Layer::FilterMode -> LayerFilterMode;
    Node::NodeType -> NodeType (drop redundant outer-name overlap),
    matching the JS naming logic in parser.js_name_for_enum.
    """
    parts = qual.split('::')
    if len(parts) > 1 and parts[-1].startswith(parts[-2]):
        parts = parts[:-2] + [parts[-1]]
    return ''.join(parts)


class _NameContext:
    """Lookup table from full C++ qualifier to the generated JS name. Lets
    `_ts_type` resolve enums/classes that have `js_name=` overrides
    (e.g. `whiteout::textures::blp::Parser::ParseMode` -> `BlpParseMode`).
    Also tracks which shared symbols the generated body references so we
    emit only the imports that are actually used.
    """
    def __init__(self, module: BindModule):
        self.prefix = module.js_prefix
        self.cpp_to_js: dict[str, str] = {}
        for e in module.enums:
            full = f'{e.cpp_namespace}::{e.cpp_qualifier}' if e.cpp_namespace else e.cpp_qualifier
            self.cpp_to_js[full] = e.js_name
        for c in module.classes:
            full = f'{c.cpp_namespace}::{c.cpp_qualifier}' if c.cpp_namespace else c.cpp_qualifier
            self.cpp_to_js[full] = c.js_name
        # Short names of types declared locally in THIS module — used to
        # suppress shared imports that would conflict (e.g. when the module
        # defines its own `Texture`, don't `import { Texture } from "_shared"`).
        # Shared math types (Vector3f, Quaternion) are deliberately excluded
        # — they're skipped during emit and live solely in _shared.d.ts, so
        # they should always come from the shared import.
        self.local_class_short_names = {
            _strip_prefix(c.js_name, self.prefix) for c in module.classes
            if _strip_prefix(c.js_name, self.prefix) not in _SHARED_MATH
        }
        self.uses: set[str] = set()      # shared symbols referenced

    def js_name_for(self, cpp_text: str) -> str | None:
        """Look up the generated JS name for a C++ type, if known to this module."""
        return self.cpp_to_js.get(cpp_text)


def _ts_type(t: TypeRef, ctx: _NameContext) -> str:
    """Render a TypeRef as a TypeScript type expression.

    Resolution rules:
      1. Primitives/strings -> JS-native types.
      2. Containers (vector, optional, track) -> wrap recursively.
      3. Enums/classes -> look up the actual generated JS name in the IR
         when available (handles js_name= overrides), else derive from
         the C++ qualifier.
    """
    if t.kind == TypeKind.PRIMITIVE:
        return _ts_primitive(_strip_namespace(t.cpp_text))
    if t.kind == TypeKind.STRING:
        return 'string'
    if t.kind == TypeKind.ENUM:
        ctx.uses.add('EnumMember')
        js = ctx.js_name_for(t.cpp_text)
        if js is None:
            js = _flatten_qualified(_strip_namespace(t.cpp_text))
        short = _strip_prefix(js, ctx.prefix)
        if short in _SHARED_MATH:
            ctx.uses.add(short)
            return short
        return f'EnumMember<typeof {short}>'
    if t.kind == TypeKind.NESTED:
        js = ctx.js_name_for(t.cpp_text)
        if js is None:
            js = _strip_namespace(t.cpp_text).replace('::', '')
        short = _strip_prefix(js, ctx.prefix)
        if short in _SHARED_MATH:
            ctx.uses.add(short)
            return short
        if short == 'Texture' and 'Texture' not in ctx.local_class_short_names:
            ctx.uses.add('Texture')
        return short
    if t.kind == TypeKind.VECTOR:
        elem_ts = _ts_type(t.element, ctx)
        view = _typed_array_for_element(t.element)
        if view is not None:
            ctx.uses.add('EmbindBufferVector')
            return f'EmbindBufferVector<{elem_ts}, {view}>'
        ctx.uses.add('EmbindVector')
        return f'EmbindVector<{elem_ts}>'
    if t.kind == TypeKind.NESTED_VEC:
        ctx.uses.add('EmbindVector')
        return f'EmbindVector<EmbindVector<{_ts_type(t.element.element, ctx)}>>'
    if t.kind == TypeKind.OPTIONAL:
        return f'{_ts_type(t.element, ctx)} | null'
    if t.kind == TypeKind.ARRAY:
        return 'never'
    return 'unknown'


def _strip_prefix(name: str, prefix: str) -> str:
    if prefix and name.startswith(prefix) and name != prefix:
        return name[len(prefix):]
    return name


HEADER_PREFIX = '''// SPDX-License-Identifier: BSD-3-Clause
// AUTOGENERATED by tools/codegen/emit_dts.py — do not edit by hand.
// Regenerate via:  python -m tools.codegen.codegen {module} --backend dts

'''


def _emit_jsdoc(buf: StringIO, doc: str, indent: str = ''):
    """Emit a JSDoc block for `doc`. Single-line docs collapse to `/** ... */`;
    multi-line docs become a `/**\\n * line\\n */` block. Empty `doc` is a no-op."""
    if not doc:
        return
    # `*/` would close the JSDoc block early — defang it.
    doc = doc.replace('*/', '*\\/')
    lines: list[str] = []
    for para in doc.split('\n\n'):
        if lines:
            lines.append('')   # paragraph break -> blank `*` line
        for ln in para.splitlines():
            lines.append(ln)
    if len(lines) == 1:
        buf.write(f'{indent}/** {lines[0]} */\n')
        return
    buf.write(f'{indent}/**\n')
    for ln in lines:
        if ln:
            buf.write(f'{indent} * {ln}\n')
        else:
            buf.write(f'{indent} *\n')
    buf.write(f'{indent} */\n')

# Order shared imports for stable, readable output.
_IMPORT_ORDER = [
    'EmbindObject', 'EmbindVector', 'EmbindBufferVector',
    'EnumValue', 'EnumMember', 'Texture',
    'Vector2f', 'Vector3f', 'Vector4f', 'Quaternion',
]


def _emit_constants(buf: StringIO, module: BindModule):
    if not module.constants:
        return
    buf.write('// ── Sentinel constants ─────────────────────────────────────────────\n')
    for c in module.constants:
        name = _strip_prefix(c.js_name, module.js_prefix)
        _emit_jsdoc(buf, c.doc)
        buf.write(f'export const {name}: number;\n')
    buf.write('\n')


def _emit_enum(buf: StringIO, e: BindEnum, ctx: _NameContext):
    js = _strip_prefix(e.js_name, ctx.prefix)
    ctx.uses.add('EnumValue')
    _emit_jsdoc(buf, e.doc)
    buf.write(f'export const {js}: {{\n')
    seen = set()
    for v in e.values:
        if v.js_name in seen:
            continue
        seen.add(v.js_name)
        _emit_jsdoc(buf, v.doc, indent='    ')
        buf.write(f'    readonly {v.js_name}: EnumValue;\n')
    buf.write('};\n\n')


def _emit_value_object(buf: StringIO, c: BindClass, ctx: _NameContext):
    name = _strip_prefix(c.js_name, ctx.prefix)
    if name in _SHARED_MATH:
        return  # declared in _shared.d.ts
    _emit_jsdoc(buf, c.doc)
    buf.write(f'export interface {name} {{\n')
    for f in c.fields:
        _emit_jsdoc(buf, f.doc, indent='    ')
        buf.write(f'    {f.name}: {_ts_type(f.type, ctx)};\n')
    buf.write('}\n\n')


def _emit_class(buf: StringIO, c: BindClass, ctx: _NameContext):
    name = _strip_prefix(c.js_name, ctx.prefix)
    ctx.uses.add('EmbindObject')

    _emit_jsdoc(buf, c.doc)
    buf.write(f'export class {name} extends EmbindObject {{\n')
    buf.write('    constructor();\n')
    for ctor in c.constructors:
        params = ', '.join(
            f'{p.name}: {_ts_type(p.type, ctx)}' for p in ctor.params)
        buf.write(f'    constructor({params});\n')

    array_fields = []
    for f in c.fields:
        if f.type.kind == TypeKind.ARRAY:
            array_fields.append(f)
            continue
        ts_type = _ts_type(f.type, ctx)
        _emit_jsdoc(buf, f.doc, indent='    ')
        buf.write(f'    {f.name}: {ts_type};\n')
        if f.array_with_view:
            buf.write(f'    {f.name}View(): Uint8Array;\n')

    for f in array_fields:
        elem = _ts_type(f.type.element, ctx)
        title = f.name[0].upper() + f.name[1:]
        ctx.uses.add('EmbindVector')
        _emit_jsdoc(buf, f.doc, indent='    ')
        buf.write(f'    get{title}(): EmbindVector<{elem}>;\n')
        buf.write(f'    set{title}(values: EmbindVector<{elem}>): void;\n')

    for m in c.methods:
        if m.bytes_in:
            params_list = []
            for i, p in enumerate(m.params):
                if i == 0:
                    params_list.append(f'{p.name}: Uint8Array')
                else:
                    params_list.append(f'{p.name}: {_ts_type(p.type, ctx)}')
            params = ', '.join(params_list)
        else:
            params = ', '.join(
                f'{p.name}: {_ts_type(p.type, ctx)}' for p in m.params)
        if m.bytes_out:
            ret = 'Uint8Array'
        elif m.return_type.kind.value == 'optional':
            ret = _ts_type(m.return_type.element, ctx)
        elif m.return_type.cpp_text == 'void':
            ret = 'void'
        else:
            ret = _ts_type(m.return_type, ctx)
        _emit_jsdoc(buf, m.doc, indent='    ')
        buf.write(f'    {m.name}({params}): {ret};\n')

    buf.write('}\n\n')


def emit(module: BindModule) -> str:
    body = StringIO()
    ctx = _NameContext(module)

    skip = set(module.skip_class_js_names)
    enums = [e for e in module.enums if e.js_name not in skip]
    value_classes = [c for c in module.classes
                     if c.is_value_object and c.js_name not in skip]
    other_classes = [c for c in module.classes
                     if not c.is_value_object and c.js_name not in skip]

    _emit_constants(body, module)

    if enums:
        body.write('// ── Enums ──────────────────────────────────────────────────────────\n')
        for e in enums:
            _emit_enum(body, e, ctx)

    if value_classes:
        body.write('// ── Value-object types (plain JS objects) ──────────────────────────\n')
        for c in value_classes:
            _emit_value_object(body, c, ctx)

    if other_classes:
        body.write('// ── Classes ────────────────────────────────────────────────────────\n')
        for c in other_classes:
            _emit_class(body, c, ctx)

    # Compose final output: header + minimal `import type` + body.
    out = StringIO()
    out.write(HEADER_PREFIX.format(module=module.name))

    used = [name for name in _IMPORT_ORDER
            if name in ctx.uses and name not in ctx.local_class_short_names]
    if used:
        out.write('import type {\n')
        out.write('    ' + ', '.join(used) + ',\n')
        out.write('} from "./_shared";\n\n')

    out.write(body.getvalue())
    return out.getvalue()
