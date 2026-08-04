# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> C ABI wrapper (.h + .cpp).

The C wrapper is what Project Panama (Java FFM), Rust FFI, Swift, and
similar tools want to consume: stable name-mangling-free symbols, plain
data types, no exceptions crossing the boundary.

Every C++ class becomes an opaque handle (`struct whiteout_Foo;`).
Methods become free functions with the handle as first arg:

    whiteout_Foo* whiteout_Foo_new(void);
    void          whiteout_Foo_delete(whiteout_Foo* self);
    int32_t       whiteout_Foo_someMethod(const whiteout_Foo* self, ...);

Conventions:
  - `std::span<const u8>`        → `const uint8_t* data, size_t size`
  - `std::span<const T>`         → `const T* data, size_t count`
  - `std::vector<u8>` return     → `whiteout_Bytes` (data + size + opaque
                                    handle to free via `whiteout_Bytes_free`)
  - `std::optional<class>` ret   → nullable handle (`nullptr` = empty)
  - `std::string` return         → `whiteout_CString` (heap-allocated)
  - enums                        → `int32_t`
  - bool                         → `int32_t` (0/1)
  - move-only classes            → opaque handle, ownership transfers
                                    on return; freed via `_delete`

**The whole C wrapper is compiled with exceptions disabled**
(`-fno-exceptions` / `/EHs-c-`). Parsers default to Lenient mode (which
returns `nullopt` instead of throwing) and the library proper avoids
exceptions in normal flow. If anything DOES throw under `-fno-exceptions`,
the runtime aborts — that's accepted as the tradeoff for a clean C ABI.

Emits three files via three entry points:
  - `emit_header(module)` → `whiteout_<module>.h` (C-compatible)
  - `emit_source(module)` → `whiteout_<module>.cpp` (compiled as C++)
  - `emit_common()`       → `whiteout_c_common.cpp` (link-once runtime
                            support shared across every module)
"""

from __future__ import annotations

import re
from io import StringIO

from .ir import (
    BindClass, BindEnum, BindField, BindMethod, BindMethodParam, BindModule,
    TypeKind, TypeRef,
)
from .parser import _short_name


# ── Shared math types ─────────────────────────────────────────────────────
#
# Vector2f / Vector3f / Vector4f / Quaternion / Matrix33f / Matrix44f
# appear in every model module (mdx, m2, m3, …) — they're auto-bound in
# each module's IR but the C ABI emits them ONCE in
# `whiteout_c_common.{h,cpp}` so their accessor symbols live in a single
# TU. Per-module emit filters these out by name.
#
# Method tables are hand-coded: the C++ side gets these methods through
# CRTP (VectorMethods / MatrixMethods) and libclang doesn't surface them
# as cursors on the derived class, so an IR-driven approach would miss
# most of the surface. The list mirrors what the embind/pybind hand-
# written bindings already expose.

_SHARED_MATH_TYPES = ('Vector2f', 'Vector3f', 'Vector4f', 'Quaternion',
                     'Matrix33f', 'Matrix44f')

# Scalar fields per type (omitted for Matrix types — exposed via get_at).
_SHARED_MATH_FIELDS = {
    'Vector2f':   ['x', 'y'],
    'Vector3f':   ['x', 'y', 'z'],
    'Vector4f':   ['x', 'y', 'z', 'w'],
    'Quaternion': ['x', 'y', 'z', 'w'],
}

# Methods per type. Each entry is a tuple:
#   (name, return_type, params, is_static, is_const)
# or, for methods that don't map to a C++ identifier (operator overloads
# exposed as named methods like `add` for `operator+`):
#   (name, return_type, params, is_static, is_const, cpp_expr)
# where `cpp_expr` is a C++ expression with these placeholders:
#   __self    → the C++ object behind `self` (dereferenced)
#   <pname>   → the named parameter; if its declared type is a math
#               handle the placeholder resolves to the dereferenced C++
#               value, otherwise to the primitive pass-through.
#
# Return-type / param-type conventions:
#   'void' / 'float' / 'size_t' / 'bool'   – primitive, passed by value
#   '<TypeName>'                            – handle ptr to a shared math type
_SHARED_MATH_METHODS = {
    'Vector2f': [
        # Inherited geometric helpers
        ('dot',            'float',     [('Vector2f', 'other')], False, True),
        ('length',         'float',     [],                       False, True),
        ('length_squared', 'float',     [],                       False, True),
        ('normalize',      'void',      [],                       False, False),
        ('normalized',     'Vector2f',  [],                       False, True),
        # Operator-equivalents — named methods over the CRTP arithmetic.
        ('add',            'Vector2f',  [('Vector2f', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Vector2f',  [('Vector2f', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Vector2f',  [('Vector2f', 'other')], False, True, '{self} * {other}'),
        ('div',            'Vector2f',  [('Vector2f', 'other')], False, True, '{self} / {other}'),
        ('mul_scalar',     'Vector2f',  [('float', 'scalar')],   False, True, '{self} * scalar'),
        ('div_scalar',     'Vector2f',  [('float', 'scalar')],   False, True, '{self} / scalar'),
        ('negate',         'Vector2f',  [],                       False, True, '-{self}'),
        ('equals',         'bool',      [('Vector2f', 'other')], False, True, '{self} == {other}'),
        # Static interpolation
        ('lerp',           'Vector2f',  [('Vector2f', 'start'), ('Vector2f', 'end'), ('float', 't')], True, False),
        ('tcb_in_tangent', 'Vector2f',  [('Vector2f', 'prev'), ('Vector2f', 'current'), ('Vector2f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('tcb_out_tangent','Vector2f',  [('Vector2f', 'prev'), ('Vector2f', 'current'), ('Vector2f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('bezier_lerp',    'Vector2f',  [('Vector2f', 'start'), ('Vector2f', 'outtan'),
                                          ('Vector2f', 'intan'), ('Vector2f', 'end'), ('float', 't')], True, False),
        ('hermite_lerp',   'Vector2f',  [('Vector2f', 'prev'), ('Vector2f', 'start'),
                                          ('Vector2f', 'end'), ('Vector2f', 'next'), ('float', 't')], True, False),
    ],
    'Vector3f': [
        ('dot',            'float',     [('Vector3f', 'other')], False, True),
        ('length',         'float',     [],                       False, True),
        ('length_squared', 'float',     [],                       False, True),
        ('normalize',      'void',      [],                       False, False),
        ('normalized',     'Vector3f',  [],                       False, True),
        ('add',            'Vector3f',  [('Vector3f', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Vector3f',  [('Vector3f', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Vector3f',  [('Vector3f', 'other')], False, True, '{self} * {other}'),
        ('div',            'Vector3f',  [('Vector3f', 'other')], False, True, '{self} / {other}'),
        ('mul_scalar',     'Vector3f',  [('float', 'scalar')],   False, True, '{self} * scalar'),
        ('div_scalar',     'Vector3f',  [('float', 'scalar')],   False, True, '{self} / scalar'),
        ('negate',         'Vector3f',  [],                       False, True, '-{self}'),
        ('equals',         'bool',      [('Vector3f', 'other')], False, True, '{self} == {other}'),
        ('lerp',           'Vector3f',  [('Vector3f', 'start'), ('Vector3f', 'end'), ('float', 't')], True, False),
        ('tcb_in_tangent', 'Vector3f',  [('Vector3f', 'prev'), ('Vector3f', 'current'), ('Vector3f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('tcb_out_tangent','Vector3f',  [('Vector3f', 'prev'), ('Vector3f', 'current'), ('Vector3f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('bezier_lerp',    'Vector3f',  [('Vector3f', 'start'), ('Vector3f', 'outtan'),
                                          ('Vector3f', 'intan'), ('Vector3f', 'end'), ('float', 't')], True, False),
        ('hermite_lerp',   'Vector3f',  [('Vector3f', 'prev'), ('Vector3f', 'start'),
                                          ('Vector3f', 'end'), ('Vector3f', 'next'), ('float', 't')], True, False),
    ],
    'Vector4f': [
        ('dot',            'float',     [('Vector4f', 'other')], False, True),
        ('length',         'float',     [],                       False, True),
        ('length_squared', 'float',     [],                       False, True),
        ('normalize',      'void',      [],                       False, False),
        ('normalized',     'Vector4f',  [],                       False, True),
        ('add',            'Vector4f',  [('Vector4f', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Vector4f',  [('Vector4f', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Vector4f',  [('Vector4f', 'other')], False, True, '{self} * {other}'),
        ('div',            'Vector4f',  [('Vector4f', 'other')], False, True, '{self} / {other}'),
        ('mul_scalar',     'Vector4f',  [('float', 'scalar')],   False, True, '{self} * scalar'),
        ('div_scalar',     'Vector4f',  [('float', 'scalar')],   False, True, '{self} / scalar'),
        ('negate',         'Vector4f',  [],                       False, True, '-{self}'),
        ('equals',         'bool',      [('Vector4f', 'other')], False, True, '{self} == {other}'),
        ('lerp',           'Vector4f',  [('Vector4f', 'start'), ('Vector4f', 'end'), ('float', 't')], True, False),
        ('tcb_in_tangent', 'Vector4f',  [('Vector4f', 'prev'), ('Vector4f', 'current'), ('Vector4f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('tcb_out_tangent','Vector4f',  [('Vector4f', 'prev'), ('Vector4f', 'current'), ('Vector4f', 'next'),
                                          ('float', 'tension'), ('float', 'continuity'), ('float', 'bias')], True, False),
        ('bezier_lerp',    'Vector4f',  [('Vector4f', 'start'), ('Vector4f', 'outtan'),
                                          ('Vector4f', 'intan'), ('Vector4f', 'end'), ('float', 't')], True, False),
        ('hermite_lerp',   'Vector4f',  [('Vector4f', 'prev'), ('Vector4f', 'start'),
                                          ('Vector4f', 'end'), ('Vector4f', 'next'), ('float', 't')], True, False),
    ],
    'Quaternion': [
        ('dot',            'float',     [('Quaternion', 'other')], False, True),
        ('length',         'float',     [],                         False, True),
        ('length_squared', 'float',     [],                         False, True),
        ('normalize',      'void',      [],                         False, False),
        ('normalized',     'Quaternion',[],                         False, True),
        # Arithmetic — `mul` is Hamilton product (overrides VectorMethods' component-wise *)
        ('add',            'Quaternion',[('Quaternion', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Quaternion',[('Quaternion', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Quaternion',[('Quaternion', 'other')], False, True, '{self} * {other}'),
        ('mul_scalar',     'Quaternion',[('float', 'scalar')],     False, True, '{self} * scalar'),
        ('div_scalar',     'Quaternion',[('float', 'scalar')],     False, True, '{self} / scalar'),
        ('negate',         'Quaternion',[],                         False, True, '-{self}'),
        ('equals',         'bool',      [('Quaternion', 'other')], False, True, '{self} == {other}'),
        # Quaternion-specific
        ('conjugate',      'Quaternion',[],                         False, True),
        ('inverse',        'Quaternion',[],                         False, True),
        ('log',            'Quaternion',[],                         False, True),
        ('exp',            'Quaternion',[],                         False, True),
        ('to_euler_angles','Vector3f',  [],                         False, True),
        ('rotate_vector',  'Vector3f',  [('Vector3f', 'v')],        False, True),
        # Statics
        ('identity',       'Quaternion',[],                         True,  False),
        ('from_axis_angle','Quaternion',[('Vector3f', 'axis'), ('float', 'angle_rad')], True, False),
        ('from_euler_angles','Quaternion',[('Vector3f', 'euler_rad')], True, False),
        ('slerp',          'Quaternion',[('Quaternion', 'a'), ('Quaternion', 'b'), ('float', 't')], True, False),
        ('squad',          'Quaternion',[('Quaternion', 'start'), ('Quaternion', 'outtan'),
                                          ('Quaternion', 'inttan'), ('Quaternion', 'end'),
                                          ('float', 't')], True, False),
        ('ln_dif',         'Quaternion',[('Quaternion', 'a'), ('Quaternion', 'b')], True, False),
    ],
    'Matrix33f': [
        # Arithmetic from MatrixMethods CRTP
        ('add',            'Matrix33f', [('Matrix33f', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Matrix33f', [('Matrix33f', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Matrix33f', [('Matrix33f', 'other')], False, True, '{self} * {other}'),
        ('mul_scalar',     'Matrix33f', [('float', 'scalar')],     False, True, '{self} * scalar'),
        ('identity',       'Matrix33f', [], True, False),
        ('zero',           'Matrix33f', [], True, False),
    ],
    'Matrix44f': [
        ('add',            'Matrix44f', [('Matrix44f', 'other')], False, True, '{self} + {other}'),
        ('sub',            'Matrix44f', [('Matrix44f', 'other')], False, True, '{self} - {other}'),
        ('mul',            'Matrix44f', [('Matrix44f', 'other')], False, True, '{self} * {other}'),
        ('mul_scalar',     'Matrix44f', [('float', 'scalar')],     False, True, '{self} * scalar'),
        ('identity',           'Matrix44f', [],                         True, False),
        ('zero',               'Matrix44f', [],                         True, False),
        ('translation',        'Matrix44f', [('Vector3f', 't')],        True, False),
        ('rotation',           'Matrix44f', [('Quaternion', 'q')],      True, False),
        ('scaling',            'Matrix44f', [('Vector3f', 's')],        True, False),
        ('compose',            'Matrix44f', [('Vector3f', 'translation'),
                                              ('Quaternion', 'rotation'),
                                              ('Vector3f', 'scale')],   True, False),
        # `inverse(m, layout = RowMajor)` — pass the default explicitly so
        # the C wrapper doesn't need to expose MatrixLayout.
        ('inverse',            'Matrix44f', [('Matrix44f', 'm')],       True, False,
                                              'whiteout::Matrix44f::inverse({m})'),
        ('rotation_x',         'Matrix44f', [('float', 'angle')],       True, False),
        ('rotation_y',         'Matrix44f', [('float', 'angle')],       True, False),
        ('rotation_z',         'Matrix44f', [('float', 'angle')],       True, False),
        ('look_at_rh',         'Matrix44f', [('Vector3f', 'eye'), ('Vector3f', 'target'), ('Vector3f', 'up')], True, False),
        ('look_at_lh',         'Matrix44f', [('Vector3f', 'eye'), ('Vector3f', 'target'), ('Vector3f', 'up')], True, False),
        ('look_at_lh_sgcompat','Matrix44f', [('Vector3f', 'eye'), ('Vector3f', 'target'), ('Vector3f', 'up')], True, False),
        ('perspective_fov_rh',     'Matrix44f', [('float', 'fovY'), ('float', 'aspect'),
                                                  ('float', 'nearZ'), ('float', 'farZ')], True, False),
        ('perspective_fov_lh',     'Matrix44f', [('float', 'fovY'), ('float', 'aspect'),
                                                  ('float', 'nearZ'), ('float', 'farZ')], True, False),
        ('perspective_diag_sgcompat','Matrix44f', [('float', 'fovDiagonal'), ('float', 'aspect'),
                                                    ('float', 'nearZ'), ('float', 'farZ')], True, False),
        ('orthographic_rh',    'Matrix44f', [('float', 'width'), ('float', 'height'),
                                              ('float', 'nearZ'), ('float', 'farZ')], True, False),
        ('transpose',          'Matrix44f', [],                         False, True),
        ('extract_translation','Vector3f',  [],                         False, True),
        ('extract_rotation',   'Quaternion',[],                         False, True),
        ('extract_scale',      'Vector3f',  [],                         False, True),
    ],
}

# Matrix dimensions (rows == cols) for the get_at/set_at element accessors.
_MATRIX_DIMS = {'Matrix33f': 3, 'Matrix44f': 4}

# Byte sizes of every shared math type — known at codegen time because
# their layouts are pinned (packed `float` strips, no padding). Used by
# downstream backends (Java in particular) to take direct sub-slices
# when a shared math type appears as an embedded field in another
# struct, without having to recurse into the IR cache.
_SHARED_MATH_BYTE_SIZES = {
    'Vector2f': 8,
    'Vector3f': 12,
    'Vector4f': 16,
    'Quaternion': 16,
    'Matrix33f': 36,
    'Matrix44f': 64,
}

# Free functions in `namespace whiteout`.
_SHARED_MATH_FREE_FUNCTIONS = [
    ('cross',            'Vector3f', [('Vector3f', 'a'), ('Vector3f', 'b')]),
    ('transform_point',  'Vector3f', [('Vector3f', 'v'), ('Matrix44f', 'm')]),
    ('transform_normal', 'Vector3f', [('Vector3f', 'v'), ('Matrix44f', 'm')]),
]


# ── C-side primitive mapping ──────────────────────────────────────────────

_C_PRIMITIVE = {
    'u8': 'uint8_t',  'u16': 'uint16_t', 'u32': 'uint32_t', 'u64': 'uint64_t',
    'i8': 'int8_t',   'i16': 'int16_t',  'i32': 'int32_t',  'i64': 'int64_t',
    'f32': 'float',   'f64': 'double',
    'bool': 'int32_t',                                       # bools through C as int32
    'unsigned char': 'uint8_t',
    'unsigned short': 'uint16_t',
    'unsigned int': 'uint32_t',
    'unsigned long long': 'uint64_t',
    'signed char': 'int8_t',
    'short': 'int16_t',
    'int': 'int32_t',
    'long long': 'int64_t',
    'float': 'float',
    'double': 'double',
    'char': 'int8_t',
    # `size_t` is preserved as its typedef by the parser (see
    # `parser._PRESERVE_TYPEDEFS`). Map to the same fixed-width type the
    # C ABI used when size_t was canonicalised to u64 — keeps the wire
    # format stable across the parser change. The wheels target only
    # 64-bit platforms where size_t is 8 bytes, so this is exact.
    'size_t':       'uint64_t',
    'std::size_t':  'uint64_t',
}


def _c_handle_name(cls_qual: str) -> str:
    """`whiteout::textures::png::Parser` -> `whiteout_PngParser` (the C
    handle struct name)."""
    # Strip namespaces; libclang gives us the qualified name.
    return cls_qual.split('::')[-1]


def _handle_map(module: BindModule) -> dict[str, str]:
    """Memoised `cpp_text → C handle short name` map for cross-class
    refs. A field declared `Sequence sequence;` on the C++ side arrives
    here with cpp_text like `whiteout::mdx::Sequence`; without this map
    the codegen would render the type as `whiteout_Sequence`, but the
    matching class is emitted as `whiteout_MdxSequence`. The map lets
    cross-refs find the right typedef regardless of which spelling
    libclang produced for the type."""
    cached = getattr(module, '_c_handle_map', None)
    if cached is not None:
        return cached
    m: dict[str, str] = {}
    for c in module.classes:
        js = _c_handle_name(c.js_name)
        keys = {
            c.cpp_qualifier,
            _c_handle_name(c.cpp_qualifier),
            js,
        }
        if c.cpp_namespace:
            keys.add(f'{c.cpp_namespace}::{c.cpp_qualifier}')
        for k in keys:
            m[k] = js
    # Shared math types live in whiteout_c_common.{h,cpp} regardless of
    # which module's TU we're emitting. Register them so cross-class
    # refs (e.g. `Vector3f translation` field) resolve to the common
    # header's `whiteout_Vector3f` typedef.
    for name in _SHARED_MATH_TYPES:
        m[name] = name
        m[f'whiteout::{name}'] = name
    module._c_handle_map = m
    return m


def _resolve_handle_short(cpp_text: str, module: BindModule) -> str:
    """C handle short name for an arbitrary cpp_text. Falls back to the
    last `::`-component when the map doesn't know about the type — that
    covers UNKNOWN-kind types that turn out to name a class bound
    elsewhere."""
    m = _handle_map(module)
    if cpp_text in m:
        return m[cpp_text]
    short = _c_handle_name(cpp_text)
    return m.get(short, short)


def _is_span_const_u8(t: TypeRef) -> bool:
    """`std::span<const u8>` / `std::span<u8>` — a borrowed byte view.
    These don't own memory; the C wrapper hands back a whiteout_Bytes
    with `_owner = NULL` so callers know not to free it."""
    if t.kind != TypeKind.UNKNOWN:
        return False
    s = t.cpp_text
    if not s.startswith('std::span<'):
        return False
    return ('uint8_t' in s or 'u8' in s
            or 'unsigned char' in s)


def _c_type(t: TypeRef, module: BindModule) -> str:
    """Render a TypeRef as a C type. Pointers to handles encode the
    ownership semantics elsewhere; this just produces the spelling."""
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        return _C_PRIMITIVE.get(short, 'int32_t')
    if t.kind == TypeKind.STRING:
        return 'whiteout_CString'
    if t.kind == TypeKind.ENUM:
        return 'int32_t'
    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        # UNKNOWN here generally means "class bound in another module";
        # still emit a handle. The header forward-declares any such type.
        if _is_span_const_u8(t):
            return 'whiteout_Bytes'  # borrowed: _owner = NULL
        return f'struct whiteout_{_resolve_handle_short(t.cpp_text, module)}*'
    if t.kind == TypeKind.OPTIONAL:
        if t.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            # nullable handle
            return f'struct whiteout_{_resolve_handle_short(t.element.cpp_text, module)}*'
        if t.element.kind == TypeKind.VECTOR and \
                _short_name(t.element.element.cpp_text) in ('u8', 'unsigned char'):
            return 'whiteout_Bytes'        # empty = data==NULL
        return _c_type(t.element, module)
    if t.kind == TypeKind.VECTOR:
        if _short_name(t.element.cpp_text) in ('u8', 'unsigned char'):
            return 'whiteout_Bytes'
        return 'whiteout_Bytes'            # generic byte-bag fallback
    return 'void*'


def _ctor_param_c_type(p: BindMethodParam, module: BindModule) -> str:
    """Render one constructor parameter for the C header/source.
    `std::string` is taken as `const char*` (callers pass `argv[1]` etc.);
    everything else follows _c_type."""
    if p.type.kind == TypeKind.STRING:
        return 'const char*'
    return _c_type(p.type, module)


def _ctor_overloads_named(c: BindClass, module: BindModule | None = None):
    """Return (ctor, name_suffix, c_params_str) for every non-default ctor
    on c whose param types are representable in C.

    The suffix is derived from the parameter names so each overload gets
    a distinct C symbol. Collisions append a numeric index. Returning the
    ctor reference (not just metadata) lets the source emitter recover the
    BindMethodParam list when building the C++ forwarder call.

    Ctors with params not representable in C (e.g. unbound class types)
    are dropped; the parser already filters most of these out, this is a
    belt-and-suspenders check.
    """
    seen: dict[str, int] = {}
    out = []
    for ctor in c.constructors:
        bits = []
        for p in ctor.params:
            if p.type.kind == TypeKind.STRING:
                bits.append(f'const char* {p.name}')
            elif p.type.kind == TypeKind.PRIMITIVE:
                short = _short_name(p.type.cpp_text)
                bits.append(f'{_C_PRIMITIVE.get(short, "int32_t")} {p.name}')
            elif p.type.kind == TypeKind.ENUM:
                bits.append(f'int32_t {p.name}')
            elif _is_interface_pointer_param(p):
                bits.append(f'void* {p.name}')
            elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
                    and not p.type.cpp_text.startswith('std::'):
                # Class-handle ctor param (e.g. `MpqFileSystem(Storage&)`).
                # Module is required to resolve the typedef short name —
                # callers supply it.
                if module is None:
                    bits = None
                    break
                handle = _resolve_handle_short(p.type.cpp_text, module)
                bits.append(f'struct whiteout_{handle}* {p.name}')
            else:
                bits = None
                break
        if bits is None:
            continue
        sig_name = '_'.join(p.name or 'arg' for p in ctor.params)
        n = seen.get(sig_name, 0)
        seen[sig_name] = n + 1
        if n > 0:
            sig_name = f'{sig_name}_{n + 1}'
        out.append((ctor, sig_name, ', '.join(bits)))
    return out


def _c_qual_class(prefix: str, c_handle: str) -> str:
    """`whiteout_textures` + `PngParser` -> `whiteout_textures_PngParser`."""
    return f'{prefix}_{c_handle}'


def _module_prefix(module: BindModule) -> str:
    return f'whiteout_{module.name}'


# ── Header emission ───────────────────────────────────────────────────────

HEADER_PROLOGUE = '''/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Fernando Sahmkow */
/* AUTOGENERATED by tools/codegen/emit_c.py — do not edit. */
/* Regenerate via:  python -m tools.codegen.codegen {module} --backend c-header */

#ifndef {guard}
#define {guard}

#include "whiteout_c_common.h"

#ifdef __cplusplus
extern "C" {{
#endif

'''


HEADER_EPILOGUE = '''
#ifdef __cplusplus
}} /* extern "C" */
#endif

#endif /* {guard} */
'''


def emit_header(module: BindModule) -> str:
    prefix = _module_prefix(module)
    guard = (prefix + '_H').upper()

    buf = StringIO()
    buf.write(HEADER_PROLOGUE.format(module=module.name, guard=guard))

    # ── Owned string list ────────────────────────────────────────────────
    # For `std::vector<std::string>` returned by value. See
    # `_is_owned_string_list_return` for why the count/at pair is not
    # enough. `_at` hands back a borrowed CString (NULL owner) pointing
    # into the list, so reading every element allocates nothing extra.
    if _module_has_string_list(module):
        buf.write('/* \u2500\u2500 Owned string list '
                  '\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */\n\n')
        buf.write('typedef struct whiteout_StringList whiteout_StringList;\n')
        buf.write(f'size_t {prefix}_StringList_size(const whiteout_StringList* self);\n')
        buf.write('/* Borrowed; valid until the list is destroyed. */\n')
        buf.write(f'whiteout_CString {prefix}_StringList_at'
                  f'(whiteout_StringList* self, size_t index);\n')
        buf.write(f'void {prefix}_StringList_delete(whiteout_StringList* self);\n\n')

    # ── Owned handle lists ───────────────────────────────────────────────
    # `vector<Class>` returns are handed back as an opaque owned list so a
    # single call transfers the whole result. Same shape as the
    # hand-written BlizzardGameInfoList, generated per element type.
    _lists = _handle_list_types(module)
    if _lists:
        buf.write('/* \u2500\u2500 Owned handle lists '
                  '\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500 */\n\n')
        for elem in _lists:
            lh = _c_handle_name(elem)
            buf.write(f'typedef struct whiteout_{lh}List whiteout_{lh}List;\n')
            buf.write(f'size_t {prefix}_{lh}List_size(const whiteout_{lh}List* self);\n')
            buf.write('/* Borrowed element; valid until the list is destroyed. */\n')
            buf.write(f'struct whiteout_{lh}* {prefix}_{lh}List_at'
                      f'(whiteout_{lh}List* self, size_t index);\n')
            buf.write(f'void {prefix}_{lh}List_delete(whiteout_{lh}List* self);\n')
        buf.write('\n')

    # ── Enums (deduped by short name) ────────────────────────────────────
    # Multiple sub-namespaces can declare an enum with the same short
    # name (`blp::Parser::ParseMode`, `png::Parser::ParseMode`, …) — we
    # collapse them since they're typically identical underlying values.
    if module.enums:
        buf.write('/* ── Enums ─────────────────────────────────────────────────── */\n\n')
        seen_enums: set[str] = set()
        for e in module.enums:
            short = _c_handle_name(e.cpp_qualifier)
            if short in seen_enums:
                continue
            seen_enums.add(short)
            buf.write(f'typedef enum {{\n')
            seen_values: set[str] = set()
            for v in e.values:
                if v.js_name in seen_values:
                    continue
                seen_values.add(v.js_name)
                tail = v.cpp_qualifier.split('::')[-1]
                buf.write(f'    {prefix}_{short}_{tail},\n')
            buf.write(f'}} {prefix}_{short};\n\n')

    # ── Opaque handles ───────────────────────────────────────────────────
    # Every class — including cross-cutting ones like Texture — emits one
    # typedef. Handle names use the bare `whiteout_<JsName>` form so a
    # cross-class reference (e.g. BlpParser.parse returning Texture*) lines
    # up with the typedef regardless of module prefix.
    # Value-object structs (Extent, MipLevel, …) still need an opaque
    # handle + field accessors at the C ABI; the value-vs-pointer
    # distinction only matters to JS/Python's value-binding backends.
    # Shared math types (Vector*, Quaternion) live in the common TU.
    # `@bind record` types are reached through their owning method's
    # snapshot accessors, so they get no handle of their own.
    classes = [c for c in module.classes
               if c.cpp_qualifier not in _SHARED_MATH_TYPES and not c.is_record]
    if classes:
        buf.write('/* ── Opaque handles ───────────────────────────────────────── */\n\n')
        for c in classes:
            short = _c_handle_name(c.js_name)
            buf.write(f'typedef struct whiteout_{short} whiteout_{short};\n')
        buf.write('\n')

    # ── Per-class function declarations ──────────────────────────────────
    for c in classes:
        _emit_class_header(buf, c, prefix, module)

    buf.write(HEADER_EPILOGUE.format(guard=guard))
    return buf.getvalue()


def _emit_class_header(buf: StringIO, c: BindClass, prefix: str,
                       module: BindModule):
    short = _c_handle_name(c.js_name)
    cls_t = f'whiteout_{short}*'

    buf.write(f'/* ── {short} ─────────────────────────────────────────────── */\n\n')

    if c.doc:
        for line in c.doc.splitlines():
            buf.write(f'/* {line} */\n')

    # Constructor.
    if not c.no_default_ctor:
        buf.write(f'{cls_t} {prefix}_{short}_new(void);\n')
    # Non-default constructors. Named after the first parameter to keep the
    # C surface ergonomic — collisions across overloads append a numeric
    # suffix. Single-param string ctors (e.g. OsFileSystem(root)) take a
    # `const char*`; vector / span params follow the same flat pair pattern
    # used for method args.
    for _ctor, sig_name, sig_params in _ctor_overloads_named(c, module):
        buf.write(f'{cls_t} {prefix}_{short}_new_{sig_name}({sig_params});\n')
    # Destructor.
    buf.write(f'void {prefix}_{short}_delete({cls_t} self);\n\n')

    for m in c.methods:
        if not _is_method_supported(m, module):
            continue
        if _is_vector_string_return(m):
            _emit_vector_string_decls(buf, m, c, prefix)
            if _is_owned_string_list_return(m):
                short_c = _c_handle_name(c.js_name)
                buf.write('/* Materialises the whole list in one call. Prefer this '
                          'over the\n * _count/_at pair above, which re-runs the '
                          'query per index. */\n')
                buf.write(f'struct whiteout_StringList* {prefix}_{short_c}_{m.name}'
                          f'(const whiteout_{short_c}* self);\n')
            continue
        if _is_vector_record_return(m, module):
            _emit_record_vector_decls(buf, m, c, prefix, module)
            continue
        _emit_method_decl(buf, m, c, prefix, module)

    _emit_field_decls(buf, c, prefix, module, _known_class_short_names(module))
    buf.write('\n')


def _emit_method_decl(buf: StringIO, m: BindMethod, c: BindClass, prefix: str,
                      module: BindModule):
    short = _c_handle_name(c.js_name)
    ret_c = _c_return_type(m, module)
    self_arg = '' if m.is_static else (
        f'const whiteout_{short}* self' if m.is_const else
        f'whiteout_{short}* self'
    )
    param_strs = [self_arg] if self_arg else []
    for p in m.params:
        if p.span_scalar is not None:
            short_t, _ = p.span_scalar
            c_elem = _C_PRIMITIVE[short_t]
            param_strs.append(f'const {c_elem}* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _is_interface_pointer_param(p):
            # whiteout::interfaces::X* — emitted opaque so consumers
            # outside the textures/mpq TUs don't need the typedef.
            param_strs.append(f'void* {p.name}')
        elif p.type.kind == TypeKind.STRING:
            param_strs.append(f'const char* {p.name}')
        elif _is_byte_vector_param(p):
            param_strs.append(f'const uint8_t* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _scalar_vector_param_elem(p) is not None:
            c_elem = _scalar_vector_param_elem(p)
            param_strs.append(f'const {c_elem}* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _is_class_vector_param(p, module):
            elem_handle = _resolve_handle_short(p.type.element.cpp_text, module)
            param_strs.append(
                f'const struct whiteout_{elem_handle}* const* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        else:
            param_strs.append(f'{_c_type(p.type, module)} {p.name}')
    opt_elem = _optional_primitive_return(m)
    if opt_elem is not None:
        param_strs.append(f'{opt_elem}* out_value')
    else:
        opt_arr = _optional_array_return(m)
        if opt_arr is not None:
            # Caller supplies a buffer of `opt_arr[1]` elements.
            param_strs.append(f'{opt_arr[0]}* out_value')
    params = ', '.join(param_strs) if param_strs else 'void'

    if m.doc:
        for line in m.doc.splitlines():
            buf.write(f'/* {line} */\n')
    buf.write(f'{ret_c} {prefix}_{short}_{m.name}({params});\n')


def _c_return_type(m: BindMethod, module: BindModule) -> str:
    """Map a method's return into the C ABI shape."""
    ret = m.return_type
    if ret.cpp_text == 'void':
        return 'void'
    if (_optional_primitive_return(m) is not None
            or _optional_array_return(m) is not None):
        return 'int32_t'   # has-value flag; the value goes to an out-param
    _list_elem = _handle_list_return(m, module)
    if _list_elem is not None:
        return f'struct whiteout_{_c_handle_name(_list_elem)}List*'
    return _c_type(ret, module)


# ── Field accessors ───────────────────────────────────────────────────────
#
# Every public field on a bound class gets a get/set pair so FFI consumers
# can read/write the same data Python's `obj.field = ...` exposes. Vector
# fields get a (count, at, resize) trio; arrays of primitives get (at,
# set_at). Field types the C ABI can't model (unbound enums/classes,
# nested-vectors, Track<T>) are skipped silently.


def _is_field_supported(f: BindField, module: BindModule,
                        known: set[str]) -> bool:
    t = f.type
    if t.kind == TypeKind.PRIMITIVE:
        return True
    if t.kind == TypeKind.ENUM:
        return True
    if t.kind == TypeKind.STRING:
        return True
    if t.kind == TypeKind.NESTED:
        return _c_handle_name(t.cpp_text) in known
    if t.kind == TypeKind.UNKNOWN:
        # E.g. Track<T> shows up here when classify_type didn't recognise it.
        return False
    if t.kind == TypeKind.ARRAY:
        inner = t.element
        if inner.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            return True
        if inner.kind == TypeKind.NESTED:
            return _c_handle_name(inner.cpp_text) in known
        return False
    if t.kind == TypeKind.VECTOR:
        inner = t.element
        if inner.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            return True
        if inner.kind == TypeKind.NESTED:
            return _c_handle_name(inner.cpp_text) in known
        return False
    if t.kind == TypeKind.NESTED_VEC:
        # vector<vector<T>> — supported when the inner element is a
        # type the bulk-flat machinery can transfer (primitives, enums,
        # math types) or a known bound class.
        inner = t.element.element
        if inner.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            return True
        if inner.kind == TypeKind.NESTED:
            return (_is_bulk_flat_inner(inner)
                    or _c_handle_name(inner.cpp_text) in known)
        return False
    return False


def _field_cpp_qual(f: BindField, cls_cpp_qual: str) -> str:
    """C++ qualifier for the field's type — used to build the cast in the
    generated body."""
    return f.type.cpp_text


def _emit_field_decls(buf: StringIO, c: BindClass, prefix: str,
                      module: BindModule, known: set[str]):
    short = _c_handle_name(c.js_name)
    cls_t = f'whiteout_{short}*'
    cls_t_const = f'const whiteout_{short}*'

    for f in c.fields:
        if not _is_field_supported(f, module, known):
            continue
        t = f.type
        if f.doc:
            for line in f.doc.splitlines():
                buf.write(f'/* {line} */\n')

        if t.kind == TypeKind.PRIMITIVE:
            ct = _C_PRIMITIVE.get(_short_name(t.cpp_text), 'int32_t')
            buf.write(f'{ct} {prefix}_{short}_get_{f.name}({cls_t_const} self);\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, {ct} value);\n')
        elif t.kind == TypeKind.ENUM:
            buf.write(f'int32_t {prefix}_{short}_get_{f.name}({cls_t_const} self);\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, int32_t value);\n')
        elif t.kind == TypeKind.STRING:
            buf.write(f'whiteout_CString {prefix}_{short}_get_{f.name}({cls_t_const} self);\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, const char* value);\n')
        elif t.kind == TypeKind.NESTED:
            inner = _resolve_handle_short(t.cpp_text, module)
            buf.write(f'whiteout_{inner}* {prefix}_{short}_get_{f.name}({cls_t} self);\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, const whiteout_{inner}* value);\n')
        elif t.kind == TypeKind.ARRAY:
            inner = t.element
            elem_c = _field_elem_c_type(inner, module)
            buf.write(f'size_t {prefix}_{short}_{f.name}_size(void);\n')
            buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t_const} self, size_t index);\n')
            if inner.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
                buf.write(f'void {prefix}_{short}_set_{f.name}_at({cls_t} self, size_t index, {elem_c} value);\n')
        elif t.kind == TypeKind.VECTOR:
            inner = t.element
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_count({cls_t_const} self);\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}({cls_t} self, size_t count);\n')
            if _is_bulk_flat_inner(inner):
                comp = _bulk_component_c_type(inner)
                buf.write(f'const {comp}* {prefix}_{short}_get_{f.name}_data({cls_t_const} self);\n')
                buf.write(f'void {prefix}_{short}_assign_{f.name}({cls_t} self, const {comp}* data, size_t count);\n')
            elif inner.kind == TypeKind.NESTED:
                elem_c = _field_elem_c_type(inner, module)
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t} self, size_t index);\n')
        elif t.kind == TypeKind.NESTED_VEC:
            # vector<vector<T>> — outer/inner counts + per-row bulk
            # transfer when T is bulk-flat-friendly, per-element handle
            # access otherwise.
            inner = t.element.element
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_count({cls_t_const} self);\n')
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_inner_count({cls_t_const} self, size_t outer_idx);\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}({cls_t} self, size_t count);\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}_inner({cls_t} self, size_t outer_idx, size_t inner_count);\n')
            if _is_bulk_flat_inner(inner):
                comp = _bulk_component_c_type(inner)
                buf.write(f'const {comp}* {prefix}_{short}_get_{f.name}_inner_data({cls_t_const} self, size_t outer_idx);\n')
                buf.write(f'void {prefix}_{short}_assign_{f.name}_inner({cls_t} self, size_t outer_idx, const {comp}* data, size_t count);\n')
            elif inner.kind == TypeKind.NESTED:
                elem_c = _field_elem_c_type(inner, module)
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t} self, size_t outer_idx, size_t inner_idx);\n')


def _field_elem_c_type(t: TypeRef, module: BindModule) -> str:
    """C-ABI element type for an array/vector inner element."""
    if t.kind == TypeKind.PRIMITIVE:
        return _C_PRIMITIVE.get(_short_name(t.cpp_text), 'int32_t')
    if t.kind == TypeKind.ENUM:
        return 'int32_t'
    if t.kind == TypeKind.NESTED:
        return f'whiteout_{_resolve_handle_short(t.cpp_text, module)}*'
    return 'int32_t'


# ── Bulk-flat vector marshalling ──────────────────────────────────────────
#
# `std::vector<T>` fields whose element type has a fixed scalar layout
# are exposed via flat-array bulk accessors:
#   const T* <field>_data(self);          /* borrowed view into the buffer */
#   void     <field>_assign(self, data, count); /* replace contents */
# Inner types eligible for bulk: numeric primitives, enums (marshalled
# as int32), and the math types Vector2f/3f/4f/Quaternion (each treated
# as a tightly-packed strip of float components).

_BULK_MATH_COMPONENTS = {
    'Vector2f': 2, 'Vector3f': 3, 'Vector4f': 4, 'Quaternion': 4,
}


def _is_bulk_flat_inner(t: TypeRef) -> bool:
    if t.kind == TypeKind.PRIMITIVE:
        return _short_name(t.cpp_text) in (
            'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64',
            'f32', 'f64',
        )
    if t.kind == TypeKind.ENUM:
        return True
    if t.kind == TypeKind.NESTED:
        return _c_handle_name(t.cpp_text) in _BULK_MATH_COMPONENTS
    return False


def _bulk_component_c_type(t: TypeRef) -> str:
    """C scalar type for each component of a bulk-flat inner."""
    if t.kind == TypeKind.PRIMITIVE:
        return _C_PRIMITIVE.get(_short_name(t.cpp_text), 'int32_t')
    if t.kind == TypeKind.ENUM:
        return 'int32_t'
    return 'float'   # Vector*/Quaternion components


def _bulk_components(t: TypeRef) -> int:
    if t.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
        return 1
    return _BULK_MATH_COMPONENTS[_c_handle_name(t.cpp_text)]


def _field_cpp_target(f: BindField, cls_cpp_qual: str, is_const: bool) -> str:
    """The C++ lvalue expression for the field — e.g. `reinterpret_cast<...>(self)->name`."""
    ref = 'const ' if is_const else ''
    return (f'reinterpret_cast<{ref}{cls_cpp_qual}*>(self)->{f.cpp_name}')


def _emit_field_defs(buf: StringIO, c: BindClass, cls_cpp_qual: str,
                     prefix: str, module: BindModule, known: set[str]):
    short = _c_handle_name(c.js_name)
    cls_t = f'whiteout_{short}*'
    cls_t_const = f'const whiteout_{short}*'

    for f in c.fields:
        if not _is_field_supported(f, module, known):
            continue
        t = f.type
        target_mut   = _field_cpp_target(f, cls_cpp_qual, is_const=False)
        target_const = _field_cpp_target(f, cls_cpp_qual, is_const=True)

        if t.kind == TypeKind.PRIMITIVE:
            ct = _C_PRIMITIVE.get(_short_name(t.cpp_text), 'int32_t')
            buf.write(f'{ct} {prefix}_{short}_get_{f.name}({cls_t_const} self) {{\n')
            buf.write(f'    return {target_const};\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, {ct} value) {{\n')
            buf.write(f'    {target_mut} = value;\n')
            buf.write('}\n\n')
        elif t.kind == TypeKind.ENUM:
            # Inherited enum fields on template instantiations sometimes
            # come back unqualified from libclang (e.g. `InterpolationType`
            # instead of `whiteout::mdx::InterpolationType`). The setter
            # lives at global scope inside `extern "C"` so we need a fully
            # qualified type for the static_cast.
            enum_cpp = t.cpp_text
            if '::' not in enum_cpp and module.cpp_namespace:
                enum_cpp = f'{module.cpp_namespace}::{enum_cpp}'
            buf.write(f'int32_t {prefix}_{short}_get_{f.name}({cls_t_const} self) {{\n')
            buf.write(f'    return static_cast<int32_t>({target_const});\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, int32_t value) {{\n')
            buf.write(f'    {target_mut} = static_cast<{enum_cpp}>(value);\n')
            buf.write('}\n\n')
        elif t.kind == TypeKind.STRING:
            buf.write(f'whiteout_CString {prefix}_{short}_get_{f.name}({cls_t_const} self) {{\n')
            buf.write(f'    auto* __owned = new std::string({target_const});\n')
            buf.write(f'    return whiteout_CString{{ __owned->c_str(), __owned->size(), __owned }};\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, const char* value) {{\n')
            buf.write(f'    {target_mut} = (value ? value : "");\n')
            buf.write('}\n\n')
        elif t.kind == TypeKind.NESTED:
            inner_short = _resolve_handle_short(t.cpp_text, module)
            buf.write(f'whiteout_{inner_short}* {prefix}_{short}_get_{f.name}({cls_t} self) {{\n')
            buf.write(f'    return reinterpret_cast<whiteout_{inner_short}*>(&{target_mut});\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_set_{f.name}({cls_t} self, const whiteout_{inner_short}* value) {{\n')
            buf.write(f'    {target_mut} = *reinterpret_cast<const {t.cpp_text}*>(value);\n')
            buf.write('}\n\n')
        elif t.kind == TypeKind.ARRAY:
            inner = t.element
            elem_c = _field_elem_c_type(inner, module)
            n = t.array_size if t.array_size is not None else 0
            buf.write(f'size_t {prefix}_{short}_{f.name}_size(void) {{\n')
            buf.write(f'    return {n};\n')
            buf.write('}\n\n')
            if inner.kind == TypeKind.PRIMITIVE:
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t_const} self, size_t index) {{\n')
                buf.write(f'    return {target_const}[index];\n')
                buf.write('}\n\n')
                buf.write(f'void {prefix}_{short}_set_{f.name}_at({cls_t} self, size_t index, {elem_c} value) {{\n')
                buf.write(f'    {target_mut}[index] = value;\n')
                buf.write('}\n\n')
            elif inner.kind == TypeKind.ENUM:
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t_const} self, size_t index) {{\n')
                buf.write(f'    return static_cast<int32_t>({target_const}[index]);\n')
                buf.write('}\n\n')
                buf.write(f'void {prefix}_{short}_set_{f.name}_at({cls_t} self, size_t index, {elem_c} value) {{\n')
                buf.write(f'    {target_mut}[index] = static_cast<{inner.cpp_text}>(value);\n')
                buf.write('}\n\n')
            elif inner.kind == TypeKind.NESTED:
                inner_short = _resolve_handle_short(inner.cpp_text, module)
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t_const} self, size_t index) {{\n')
                buf.write(f'    return const_cast<whiteout_{inner_short}*>(reinterpret_cast<const whiteout_{inner_short}*>(&{target_const}[index]));\n')
                buf.write('}\n\n')
        elif t.kind == TypeKind.VECTOR:
            inner = t.element
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_count({cls_t_const} self) {{\n')
            buf.write(f'    return {target_const}.size();\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}({cls_t} self, size_t count) {{\n')
            buf.write(f'    {target_mut}.resize(count);\n')
            buf.write('}\n\n')

            if _is_bulk_flat_inner(inner):
                comp = _bulk_component_c_type(inner)
                # Borrowed pointer to the vector's storage; caller copies
                # eagerly because the next mutation invalidates it.
                buf.write(f'const {comp}* {prefix}_{short}_get_{f.name}_data({cls_t_const} self) {{\n')
                buf.write(f'    const auto& __v = {target_const};\n')
                buf.write(f'    return __v.empty() ? nullptr : reinterpret_cast<const {comp}*>(__v.data());\n')
                buf.write('}\n\n')
                # Replace contents from a flat array of `count` elements
                # (or `count * components` scalars for Vector*/Quat).
                buf.write(f'void {prefix}_{short}_assign_{f.name}({cls_t} self, const {comp}* data, size_t count) {{\n')
                buf.write(f'    auto& __v = {target_mut};\n')
                if inner.kind == TypeKind.ENUM:
                    buf.write('    __v.resize(count);\n')
                    buf.write(f'    for (size_t __i = 0; __i < count; ++__i) __v[__i] = static_cast<{inner.cpp_text}>(data[__i]);\n')
                else:
                    buf.write('    __v.resize(count);\n')
                    buf.write(f'    if (count) std::memcpy(__v.data(), data, count * sizeof({inner.cpp_text}));\n')
                buf.write('}\n\n')
            elif inner.kind == TypeKind.NESTED:
                elem_c = _field_elem_c_type(inner, module)
                inner_short = _resolve_handle_short(inner.cpp_text, module)
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t} self, size_t index) {{\n')
                buf.write(f'    return reinterpret_cast<whiteout_{inner_short}*>(&{target_mut}[index]);\n')
                buf.write('}\n\n')
        elif t.kind == TypeKind.NESTED_VEC:
            inner = t.element.element
            inner_cpp = inner.cpp_text
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_count({cls_t_const} self) {{\n')
            buf.write(f'    return {target_const}.size();\n')
            buf.write('}\n\n')
            buf.write(f'size_t {prefix}_{short}_get_{f.name}_inner_count({cls_t_const} self, size_t outer_idx) {{\n')
            buf.write(f'    const auto& __v = {target_const};\n')
            buf.write('    return outer_idx < __v.size() ? __v[outer_idx].size() : 0;\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}({cls_t} self, size_t count) {{\n')
            buf.write(f'    {target_mut}.resize(count);\n')
            buf.write('}\n\n')
            buf.write(f'void {prefix}_{short}_resize_{f.name}_inner({cls_t} self, size_t outer_idx, size_t inner_count) {{\n')
            buf.write(f'    auto& __v = {target_mut};\n')
            buf.write('    if (outer_idx < __v.size()) __v[outer_idx].resize(inner_count);\n')
            buf.write('}\n\n')
            if _is_bulk_flat_inner(inner):
                comp = _bulk_component_c_type(inner)
                buf.write(f'const {comp}* {prefix}_{short}_get_{f.name}_inner_data({cls_t_const} self, size_t outer_idx) {{\n')
                buf.write(f'    const auto& __v = {target_const};\n')
                buf.write('    if (outer_idx >= __v.size() || __v[outer_idx].empty()) return nullptr;\n')
                buf.write(f'    return reinterpret_cast<const {comp}*>(__v[outer_idx].data());\n')
                buf.write('}\n\n')
                buf.write(f'void {prefix}_{short}_assign_{f.name}_inner({cls_t} self, size_t outer_idx, const {comp}* data, size_t count) {{\n')
                buf.write(f'    auto& __v = {target_mut};\n')
                buf.write('    if (outer_idx >= __v.size()) return;\n')
                buf.write('    auto& __row = __v[outer_idx];\n')
                if inner.kind == TypeKind.ENUM:
                    buf.write('    __row.resize(count);\n')
                    buf.write(f'    for (size_t __i = 0; __i < count; ++__i) __row[__i] = static_cast<{inner_cpp}>(data[__i]);\n')
                else:
                    buf.write('    __row.resize(count);\n')
                    buf.write(f'    if (count) std::memcpy(__row.data(), data, count * sizeof({inner_cpp}));\n')
                buf.write('}\n\n')
            elif inner.kind == TypeKind.NESTED:
                elem_c = _field_elem_c_type(inner, module)
                inner_short = _resolve_handle_short(inner.cpp_text, module)
                buf.write(f'{elem_c} {prefix}_{short}_get_{f.name}_at({cls_t} self, size_t outer_idx, size_t inner_idx) {{\n')
                buf.write(f'    return reinterpret_cast<whiteout_{inner_short}*>(&{target_mut}[outer_idx][inner_idx]);\n')
                buf.write('}\n\n')


def _known_class_short_names(module: BindModule) -> set[str]:
    """Set of `_c_handle_name(cpp_qualifier)` for every bound class in this
    module — used to decide whether an UNKNOWN-kind type spotted in a
    method signature is "a class we know about" or "something we can't
    safely render across the C boundary". Value-object classes count too
    (they're emitted with handles + field accessors, same as the rest)."""
    out = set()
    for c in module.classes:
        out.add(_c_handle_name(c.cpp_qualifier))
        out.add(_c_handle_name(c.js_name))
    return out


def _is_return_supported(ret: TypeRef, module: BindModule) -> bool:
    """Whether the C emitter knows how to wrap this return type. Methods
    with unsupported returns (vector<string>, std::string, vector<T> for
    T != u8, unbound classes/enums, reference returns to unbound types)
    get skipped — the C ABI doesn't yet model those shapes."""
    if ret.cpp_text == 'void':
        return True
    if ret.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM, TypeKind.NESTED):
        return True
    if ret.kind == TypeKind.STRING:
        return True
    if ret.kind == TypeKind.UNKNOWN:
        # `std::span<const u8>` lands here too; route it through Bytes.
        if _is_span_const_u8(ret):
            return True
        # Other UNKNOWN returns are OK only if they name a class we've
        # bound in this module (so we have a typedef + cpp qualifier).
        if ret.cpp_text.startswith('std::'):
            return False
        return _c_handle_name(ret.cpp_text) in _known_class_short_names(module)
    if ret.kind == TypeKind.VECTOR:
        if _short_name(ret.element.cpp_text) in ('u8', 'unsigned char'):
            return True
        if (ret.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN)
                and not ret.element.cpp_text.startswith('std::')
                and _c_handle_name(ret.element.cpp_text)
                in _known_class_short_names(module)):
            return True
        # std::vector<std::string> — supported via a (count, at) expansion
        # (one method becomes two C functions). See `_is_vector_string_return`.
        if ret.element.kind == TypeKind.STRING:
            return True
        # std::vector<record> — snapshot + per-field index accessors.
        if _find_record_class(module, ret.element.cpp_text) is not None:
            return True
        return False
    if ret.kind == TypeKind.OPTIONAL:
        if _ARRAY_RE.fullmatch(ret.element.cpp_text.strip()):
            return _C_PRIMITIVE.get(
                _short_name(_ARRAY_RE.fullmatch(
                    ret.element.cpp_text.strip()).group(1))) is not None
        if ret.element.kind == TypeKind.ENUM:
            return True
        if ret.element.kind == TypeKind.PRIMITIVE:
            return _C_PRIMITIVE.get(_short_name(ret.element.cpp_text)) is not None
        if ret.element.kind in (TypeKind.NESTED,):
            return True
        if ret.element.kind == TypeKind.STRING:
            return True
        if ret.element.kind == TypeKind.UNKNOWN:
            if ret.element.cpp_text.startswith('std::'):
                return False
            return _c_handle_name(ret.element.cpp_text) in _known_class_short_names(module)
        if ret.element.kind == TypeKind.VECTOR:
            inner = ret.element.element
            if _short_name(inner.cpp_text) in ('u8', 'unsigned char'):
                return True
            return (inner.kind in (TypeKind.NESTED, TypeKind.UNKNOWN)
                    and not inner.cpp_text.startswith('std::')
                    and _c_handle_name(inner.cpp_text)
                    in _known_class_short_names(module))
    return False


_ARRAY_RE = re.compile(r'std::array<\s*(.+?)\s*,\s*(\d+)\s*>')


def _handle_list_return(m: BindMethod, module: BindModule) -> str | None:
    """`std::vector<Class>` or `std::optional<std::vector<Class>>` where
    Class is a bound handle type — returns the C++ element spelling.

    Lowered to an owned opaque list: the vector is heap-moved and handed
    back as `whiteout_<Elem>List*`, with `_size` / `_at` / `_delete`. The
    same shape the hand-written `BlizzardGameInfoList` uses, generalised.
    """
    r = m.return_type
    if r.kind == TypeKind.OPTIONAL and r.element is not None:
        r = r.element
    if r.kind != TypeKind.VECTOR or r.element is None:
        return None
    elem = r.element
    if elem.kind not in (TypeKind.NESTED, TypeKind.UNKNOWN):
        return None
    if elem.cpp_text.startswith('std::'):
        return None
    if _c_handle_name(elem.cpp_text) not in _known_class_short_names(module):
        return None
    # `@bind record` elements have no handle to hand out — they go through
    # the snapshot lowering instead. Excluding them here keeps every
    # consumer of this helper (including emit_rust) off the handle path.
    if _find_record_class(module, elem.cpp_text) is not None:
        return None
    return elem.cpp_text


def _handle_list_types(module: BindModule) -> list[str]:
    """Distinct element types needing a list wrapper in this module."""
    seen = []
    for c in module.classes:
        for m in c.methods:
            if m.is_skipped:
                continue
            elem = _handle_list_return(m, module)
            if elem is not None and elem not in seen:
                seen.append(elem)
    return seen


def _optional_array_return(m: BindMethod):
    """`std::optional<std::array<T, N>>` for scalar T — returns (c_elem, N).

    Lowered like the scalar case, but the out-param is a caller-provided
    buffer of N elements rather than a single value. `findEncryptionKey`
    returning a 16-byte key is the motivating shape.
    """
    r = m.return_type
    if r.kind != TypeKind.OPTIONAL or r.element is None:
        return None
    match = _ARRAY_RE.fullmatch(r.element.cpp_text.strip())
    if not match:
        return None
    c_elem = _C_PRIMITIVE.get(_short_name(match.group(1)))
    if c_elem is None:
        return None
    return c_elem, int(match.group(2))


def _optional_primitive_return(m: BindMethod) -> str | None:
    """`std::optional<T>` for a scalar T — returns the C element type.

    Lowered to `int32_t f(..., T* out_value)`: 1 when the optional held a
    value (written to `*out_value`), 0 otherwise. Every other optional
    shape already has a natural empty sentinel (null handle, empty Bytes,
    empty CString); a scalar has none, so it needs the out-param.
    """
    r = m.return_type
    if r.kind != TypeKind.OPTIONAL or r.element is None:
        return None
    if r.element.kind == TypeKind.ENUM:
        return 'int32_t'
    if r.element.kind != TypeKind.PRIMITIVE:
        return None
    return _C_PRIMITIVE.get(_short_name(r.element.cpp_text))


def _is_owned_string_list_return(m: BindMethod) -> bool:
    """`std::vector<std::string>` returned **by value**.

    The `_count`/`_at` expansion below is only sound for a method that
    returns a *reference* to a stored vector — `_at` re-invokes the method
    for every index, so a by-value return re-materialises the whole list
    each time. `casc::Storage::listFiles()` enumerating 135k entries turns
    that into 135k full enumerations.

    These get an owned list handle instead: one call materialises the
    vector on the heap, then `_size`/`_at` are O(1) against it.
    """
    return _is_vector_string_return(m) and not m.return_is_reference


def _module_has_string_list(module: BindModule) -> bool:
    return any(_is_owned_string_list_return(m)
               for c in module.classes for m in c.methods if not m.is_skipped)


def _is_vector_string_return(m: BindMethod) -> bool:
    """`std::vector<std::string>` method return — lowered to two C
    functions: `_<method>_count(self) -> size_t` and `_<method>_at(self, i)
    -> whiteout_CString`. The C++ method is expected to be const and to
    return a reference (`const std::vector<std::string>&`) so per-element
    lookup doesn't re-execute work."""
    r = m.return_type
    return r.kind == TypeKind.VECTOR and r.element.kind == TypeKind.STRING


def _emit_vector_string_decls(buf: StringIO, m: BindMethod, c: BindClass,
                              prefix: str) -> None:
    short = _c_handle_name(c.js_name)
    self_q = f'const whiteout_{short}* self'
    if m.doc:
        for line in m.doc.splitlines():
            buf.write(f'/* {line} */\n')
    buf.write(f'size_t {prefix}_{short}_{m.name}_count({self_q});\n')
    buf.write(f'whiteout_CString {prefix}_{short}_{m.name}_at({self_q}, size_t index);\n')


def _emit_vector_string_defs(buf: StringIO, m: BindMethod, c: BindClass,
                             cpp_qual: str, prefix: str) -> None:
    short = _c_handle_name(c.js_name)
    self_q = f'const whiteout_{short}* self'
    cast = f'reinterpret_cast<const {cpp_qual}*>(self)'
    buf.write(f'size_t {prefix}_{short}_{m.name}_count({self_q}) {{\n')
    buf.write(f'    return {cast}->{m.cpp_name}().size();\n')
    buf.write('}\n\n')
    buf.write(f'whiteout_CString {prefix}_{short}_{m.name}_at({self_q}, size_t index) {{\n')
    buf.write(f'    const auto& __v = {cast}->{m.cpp_name}();\n')
    buf.write(f'    if (index >= __v.size()) return emptyCString();\n')
    buf.write(f'    return wrapCString(std::string(__v[index]));\n')
    buf.write('}\n\n')


# ── vector<record> returns ────────────────────────────────────────────────
#
# A method returning `std::vector<R>` for a `@bind record` type R is lowered
# to a snapshot handle (materialised once) + per-field index accessors +
# free, so the managed side can build a list of immutable values in O(n)
# without a native handle per element. Mirrors the value-object field
# accessors, but keyed on the snapshot rather than on `self`.


def _find_record_class(module: BindModule, cpp_text: str) -> BindClass | None:
    short = cpp_text.split('::')[-1].strip()
    for c in module.classes:
        if c.is_record and (c.cpp_qualifier.split('::')[-1] == short
                            or c.js_name == short
                            or _c_handle_name(c.js_name) == short):
            return c
    return None


def _is_vector_record_return(m: BindMethod, module: BindModule) -> bool:
    r = m.return_type
    return (r.kind == TypeKind.VECTOR and r.element is not None
            and _find_record_class(module, r.element.cpp_text) is not None)


def _record_field_c_ret(f: BindField) -> str | None:
    """C return type for a record field's snapshot accessor, or None when the
    field kind isn't representable in a record."""
    t = f.type
    if t.kind == TypeKind.PRIMITIVE:
        return _C_PRIMITIVE.get(_short_name(t.cpp_text), 'int32_t')
    if t.kind == TypeKind.ENUM:
        return 'int32_t'
    if t.kind == TypeKind.STRING:
        return 'whiteout_CString'
    if t.kind == TypeKind.ARRAY and t.element is not None \
            and t.element.kind == TypeKind.PRIMITIVE \
            and _short_name(t.element.cpp_text) in ('u8', 'unsigned char'):
        return 'whiteout_Bytes'
    return None


def _record_fields(rec: BindClass) -> list[BindField]:
    return [f for f in rec.fields if _record_field_c_ret(f) is not None]


def _emit_record_vector_decls(buf: StringIO, m: BindMethod, c: BindClass,
                              prefix: str, module: BindModule) -> None:
    rec = _find_record_class(module, m.return_type.element.cpp_text)
    short = _c_handle_name(c.js_name)
    self_q = (f'const whiteout_{short}* self' if m.is_const
              else f'whiteout_{short}* self')
    if m.doc:
        for line in m.doc.splitlines():
            buf.write(f'/* {line} */\n')
    buf.write('/* Materialises the whole list once; the snapshot is queried by\n'
              ' * index and must be released with _free. */\n')
    buf.write(f'void* {prefix}_{short}_{m.name}_snapshot({self_q});\n')
    buf.write(f'size_t {prefix}_{short}_{m.name}_count(void* snapshot);\n')
    for f in _record_fields(rec):
        buf.write(f'{_record_field_c_ret(f)} {prefix}_{short}_{m.name}_{f.name}_at'
                  f'(void* snapshot, size_t index);\n')
    buf.write(f'void {prefix}_{short}_{m.name}_free(void* snapshot);\n')


def _emit_record_vector_defs(buf: StringIO, m: BindMethod, c: BindClass,
                             cpp_qual: str, prefix: str,
                             module: BindModule) -> None:
    rec = _find_record_class(module, m.return_type.element.cpp_text)
    elem = rec.cpp_qualifier
    if '::' not in elem and module.cpp_namespace:
        elem = f'{module.cpp_namespace}::{elem}'
    short = _c_handle_name(c.js_name)
    vec_t = f'std::vector<{elem}>'
    self_q = (f'const whiteout_{short}* self' if m.is_const
              else f'whiteout_{short}* self')
    self_cast = (f'reinterpret_cast<const {cpp_qual}*>(self)' if m.is_const
                 else f'reinterpret_cast<{cpp_qual}*>(self)')

    buf.write(f'void* {prefix}_{short}_{m.name}_snapshot({self_q}) {{\n')
    buf.write(f'    return new {vec_t}({self_cast}->{m.cpp_name}());\n')
    buf.write('}\n\n')

    buf.write(f'size_t {prefix}_{short}_{m.name}_count(void* snapshot) {{\n')
    buf.write(f'    return snapshot ? reinterpret_cast<{vec_t}*>(snapshot)->size() : 0;\n')
    buf.write('}\n\n')

    for f in _record_fields(rec):
        ct = _record_field_c_ret(f)
        buf.write(f'{ct} {prefix}_{short}_{m.name}_{f.name}_at'
                  f'(void* snapshot, size_t index) {{\n')
        buf.write(f'    auto* __v = reinterpret_cast<{vec_t}*>(snapshot);\n')
        t = f.type
        if t.kind == TypeKind.STRING:
            buf.write('    if (!__v || index >= __v->size()) return emptyCString();\n')
            buf.write(f'    return wrapCString(std::string((*__v)[index].{f.cpp_name}));\n')
        elif t.kind == TypeKind.ARRAY:
            buf.write('    if (!__v || index >= __v->size()) return emptyBytes();\n')
            buf.write(f'    const auto& __a = (*__v)[index].{f.cpp_name};\n')
            buf.write('    return wrapBytes(std::vector<whiteout::u8>(__a.begin(), __a.end()));\n')
        elif t.kind == TypeKind.ENUM:
            buf.write('    if (!__v || index >= __v->size()) return 0;\n')
            buf.write(f'    return static_cast<int32_t>((*__v)[index].{f.cpp_name});\n')
        else:
            buf.write('    if (!__v || index >= __v->size()) return 0;\n')
            buf.write(f'    return (*__v)[index].{f.cpp_name};\n')
        buf.write('}\n\n')

    buf.write(f'void {prefix}_{short}_{m.name}_free(void* snapshot) {{\n')
    buf.write(f'    delete reinterpret_cast<{vec_t}*>(snapshot);\n')
    buf.write('}\n\n')


def _is_interface_pointer_param(p: BindMethodParam) -> bool:
    """`whiteout::interfaces::X*` or `whiteout::interfaces::X&` params —
    bridged through the host bindings' NativeHandled/*Bridge dispatch
    layer. Detected via cpp_raw (which preserves the `*`/`&` that
    classify_type strips off cpp_text). On the C ABI side both shapes
    collapse to the same opaque-pointer marshalling, so we treat them
    identically here."""
    return (('*' in p.cpp_raw) or ('&' in p.cpp_raw)) \
        and ('interfaces::' in p.cpp_raw)


def _is_byte_vector_param(p: BindMethodParam) -> bool:
    """`const std::vector<u8>&` style — marshalled as a (ptr, size) pair
    over the C ABI, same shape as a span<const u8>."""
    t = p.type
    return (t.kind == TypeKind.VECTOR
            and _short_name(t.element.cpp_text) in ('u8', 'unsigned char'))


def _scalar_vector_param_elem(p: BindMethodParam) -> str | None:
    """`const std::vector<T>&` for a scalar or enum T — returns the C
    element type.

    Marshalled as a (ptr, count) pair, exactly like the `vector<u8>` case
    that already existed; this just generalises it beyond bytes. Enums
    travel as `int32_t`, matching how they cross everywhere else.
    """
    t = p.type
    if t.kind != TypeKind.VECTOR or t.element is None:
        return None
    elem = t.element
    if elem.kind == TypeKind.ENUM:
        return 'int32_t'
    if elem.kind != TypeKind.PRIMITIVE:
        return None
    return _C_PRIMITIVE.get(_short_name(elem.cpp_text))


def _is_class_vector_param(p: BindMethodParam, module: BindModule) -> bool:
    """`const std::vector<Class>&` where Class is a bound class handle —
    marshalled as an array of opaque handle pointers + a size."""
    t = p.type
    if t.kind != TypeKind.VECTOR:
        return False
    elem = t.element
    if elem.kind not in (TypeKind.NESTED, TypeKind.UNKNOWN):
        return False
    if elem.cpp_text.startswith('std::'):
        return False
    return _c_handle_name(elem.cpp_text) in _known_class_short_names(module)


def _is_param_supported(p: BindMethodParam, module: BindModule) -> bool:
    """Mirror of `_is_return_supported` for parameter types. Span-of-
    primitive params are explicitly OK (the C side gets ptr+size)."""
    if p.span_scalar is not None:
        return True
    if _is_interface_pointer_param(p):
        return True
    t = p.type
    if t.kind == TypeKind.PRIMITIVE:
        return True
    if t.kind == TypeKind.ENUM:
        return True
    if t.kind == TypeKind.STRING:
        # `const std::string&` → `const char*` on the C ABI.
        return True
    if _is_byte_vector_param(p):
        return True
    if _scalar_vector_param_elem(p) is not None:
        return True
    if _is_class_vector_param(p, module):
        return True
    if t.kind == TypeKind.NESTED:
        return True
    if t.kind == TypeKind.UNKNOWN:
        if t.cpp_text.startswith('std::'):
            return False
        if '<' in t.cpp_text:           # vector<Channel>, etc.
            return False
        return _c_handle_name(t.cpp_text) in _known_class_short_names(module)
    return False


def _is_method_supported(m: BindMethod, module: BindModule) -> bool:
    if not _is_return_supported(m.return_type, module):
        return False
    return all(_is_param_supported(p, module) for p in m.params)


# ── Source emission ───────────────────────────────────────────────────────

_PRIMITIVES_PASSTHRU = {'float', 'int', 'bool', 'size_t', 'void'}


def _math_c_type(t: str) -> str:
    """Render a method-table type as its C ABI form. Primitives pass
    through unchanged; math handle types become opaque pointers."""
    if t == 'bool':
        return 'int32_t'             # match the rest of the C ABI
    if t in _PRIMITIVES_PASSTHRU:
        return t
    return f'whiteout_{t}*'


def _math_c_param(t: str, name: str) -> str:
    """Param spelling for a math-table type — handle params are
    const-pointer-to-handle (every shared math method that takes a class
    parameter takes it by const ref or by value, both of which the
    wrapper can dereference)."""
    if t in _PRIMITIVES_PASSTHRU:
        return f'{t} {name}'
    return f'const whiteout_{t}* {name}'


def _emit_math_method_decl(buf: StringIO, owner: str, m: tuple) -> None:
    name, ret, params, is_static, is_const = m[:5]
    sym = f'whiteout_{owner}_{name}'
    parts = []
    if not is_static:
        parts.append(f'{"const " if is_const else ""}whiteout_{owner}* self')
    for p_t, p_n in params:
        parts.append(_math_c_param(p_t, p_n))
    sig = ', '.join(parts) if parts else 'void'
    buf.write(f'{_math_c_type(ret)} {sym}({sig});\n')


def _emit_math_method_def(buf: StringIO, owner: str, m: tuple) -> None:
    name, ret, params, is_static, is_const = m[:5]
    cpp_expr = m[5] if len(m) > 5 else None
    sym = f'whiteout_{owner}_{name}'
    cpp_owner = f'whiteout::{owner}'

    parts = []
    cpp_args = []
    placeholders: dict[str, str] = {}
    if not is_static:
        parts.append(f'{"const " if is_const else ""}whiteout_{owner}* self')
        placeholders['self'] = (
            f'(*reinterpret_cast<{"const " if is_const else ""}{cpp_owner}*>(self))')
    for p_t, p_n in params:
        parts.append(_math_c_param(p_t, p_n))
        if p_t in _PRIMITIVES_PASSTHRU:
            cpp_args.append(p_n)
            placeholders[p_n] = p_n
        else:
            deref = f'(*reinterpret_cast<const whiteout::{p_t}*>({p_n}))'
            cpp_args.append(deref)
            placeholders[p_n] = deref

    sig = ', '.join(parts) if parts else 'void'
    cpp_args_str = ', '.join(cpp_args)

    if cpp_expr is not None:
        # Custom C++ expression with `{placeholder}` substitution — used
        # to expose operator overloads (`__self + other`, etc.) under a
        # named-method spelling the C ABI can dispatch.
        call = cpp_expr.format(**placeholders)
    elif is_static:
        call = f'{cpp_owner}::{name}({cpp_args_str})'
    else:
        self_q = f'{"const " if is_const else ""}{cpp_owner}*'
        call = f'reinterpret_cast<{self_q}>(self)->{name}({cpp_args_str})'

    c_ret = _math_c_type(ret)
    buf.write(f'{c_ret} {sym}({sig}) {{\n')
    if ret == 'void':
        buf.write(f'    {call};\n')
    elif ret == 'bool':
        buf.write(f'    return ({call}) ? 1 : 0;\n')
    elif ret in _PRIMITIVES_PASSTHRU:
        buf.write(f'    return {call};\n')
    else:
        buf.write(f'    return reinterpret_cast<whiteout_{ret}*>(new whiteout::{ret}({call}));\n')
    buf.write('}\n')


def emit_common_header() -> str:
    """`whiteout_c_common.h` — runtime types and shared-math declarations
    that every module's header `#include`s. Keeping them in one place
    avoids the multi-module redeclaration of types like Vector3f."""
    buf = StringIO()
    buf.write('''/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Fernando Sahmkow */
/* AUTOGENERATED by tools/codegen/emit_c.py — do not edit. */
/* Regenerate via:  python -m tools.codegen.codegen <any> --backend c-common-header */

#ifndef WHITEOUT_C_COMMON_H
#define WHITEOUT_C_COMMON_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Common types ─────────────────────────────────────────────────── */

/* Heap-allocated byte buffer. `data` is owned by the wrapper; free via
 * `whiteout_Bytes_free`. An empty/missing buffer has `data == NULL`. */
typedef struct {
    const uint8_t* data;
    size_t         size;
    void*          _owner;   /* opaque; passed back to free() */
} whiteout_Bytes;

void whiteout_Bytes_free(whiteout_Bytes buf);

/* Heap-allocated null-terminated string. Free via `whiteout_CString_free`. */
typedef struct {
    const char* chars;
    size_t      length;
    void*       _owner;
} whiteout_CString;

void whiteout_CString_free(whiteout_CString str);

/* ── Shared math types ────────────────────────────────────────────── */
/* These types are auto-bound in every model module's IR; we emit them
 * once here so the matching accessor symbols live in a single TU. */

''')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'typedef struct whiteout_{name} whiteout_{name};\n')
    buf.write('\n')

    for name in _SHARED_MATH_TYPES:
        buf.write(f'/* ── {name} ──────────────────────────────────────────── */\n')
        buf.write(f'whiteout_{name}* whiteout_{name}_new(void);\n')
        buf.write(f'void whiteout_{name}_delete(whiteout_{name}* self);\n')
        for f in _SHARED_MATH_FIELDS.get(name, []):
            buf.write(f'float whiteout_{name}_get_{f}(const whiteout_{name}* self);\n')
            buf.write(f'void whiteout_{name}_set_{f}(whiteout_{name}* self, float value);\n')
        if name in _MATRIX_DIMS:
            buf.write(f'size_t whiteout_{name}_dim(void);\n')
            buf.write(f'float whiteout_{name}_get_at(const whiteout_{name}* self, size_t row, size_t col);\n')
            buf.write(f'void whiteout_{name}_set_at(whiteout_{name}* self, size_t row, size_t col, float value);\n')
        for m in _SHARED_MATH_METHODS.get(name, []):
            _emit_math_method_decl(buf, name, m)
        buf.write('\n')

    if _SHARED_MATH_FREE_FUNCTIONS:
        buf.write('/* ── Free functions ──────────────────────────────────────── */\n')
        for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
            param_strs = [_math_c_param(t, n) for t, n in params]
            sig = ', '.join(param_strs) if param_strs else 'void'
            buf.write(f'{_math_c_type(ret)} whiteout_{name}({sig});\n')
        buf.write('\n')

    buf.write('''#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WHITEOUT_C_COMMON_H */
''')
    return buf.getvalue()


def emit_common() -> str:
    """`whiteout_c_common.cpp` — runtime support + shared-math accessor
    definitions, linked exactly once into whiteout_c."""
    buf = StringIO()
    buf.write('''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
// AUTOGENERATED by tools/codegen/emit_c.py — do not edit.
// Regenerate via:  python -m tools.codegen.codegen <any> --backend c-common

#include "whiteout_c_common.h"

#include <cstdint>
#include <string>
#include <vector>
#include <whiteout/vector_types.h>

extern "C" {

void whiteout_Bytes_free(whiteout_Bytes buf) {
    if (!buf._owner) return;
    delete static_cast<std::vector<uint8_t>*>(buf._owner);
}

void whiteout_CString_free(whiteout_CString str) {
    if (!str._owner) return;
    delete static_cast<std::string*>(str._owner);
}

''')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'// ── {name} ───────────────────────────────────────────────\n')
        buf.write(f'whiteout_{name}* whiteout_{name}_new(void) {{\n')
        buf.write(f'    return reinterpret_cast<whiteout_{name}*>(new whiteout::{name}());\n')
        buf.write('}\n')
        buf.write(f'void whiteout_{name}_delete(whiteout_{name}* self) {{\n')
        buf.write(f'    delete reinterpret_cast<whiteout::{name}*>(self);\n')
        buf.write('}\n')
        for f in _SHARED_MATH_FIELDS.get(name, []):
            buf.write(f'float whiteout_{name}_get_{f}(const whiteout_{name}* self) {{\n')
            buf.write(f'    return reinterpret_cast<const whiteout::{name}*>(self)->{f};\n')
            buf.write('}\n')
            buf.write(f'void whiteout_{name}_set_{f}(whiteout_{name}* self, float value) {{\n')
            buf.write(f'    reinterpret_cast<whiteout::{name}*>(self)->{f} = value;\n')
            buf.write('}\n')
        if name in _MATRIX_DIMS:
            dim = _MATRIX_DIMS[name]
            buf.write(f'size_t whiteout_{name}_dim(void) {{ return {dim}; }}\n')
            buf.write(f'float whiteout_{name}_get_at(const whiteout_{name}* self, size_t row, size_t col) {{\n')
            buf.write(f'    return reinterpret_cast<const whiteout::{name}*>(self)->data[row][col];\n')
            buf.write('}\n')
            buf.write(f'void whiteout_{name}_set_at(whiteout_{name}* self, size_t row, size_t col, float value) {{\n')
            buf.write(f'    reinterpret_cast<whiteout::{name}*>(self)->data[row][col] = value;\n')
            buf.write('}\n')
        for m in _SHARED_MATH_METHODS.get(name, []):
            _emit_math_method_def(buf, name, m)
        buf.write('\n')

    if _SHARED_MATH_FREE_FUNCTIONS:
        buf.write('// ── Free functions ───────────────────────────────────────\n')
        for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
            param_strs = [_math_c_param(t, n) for t, n in params]
            sig = ', '.join(param_strs) if param_strs else 'void'
            cpp_args = ', '.join(
                f'*reinterpret_cast<const whiteout::{t}*>({n})'
                if t not in _PRIMITIVES_PASSTHRU else n
                for t, n in params
            )
            c_ret = _math_c_type(ret)
            buf.write(f'{c_ret} whiteout_{name}({sig}) {{\n')
            if ret == 'void':
                buf.write(f'    whiteout::{name}({cpp_args});\n')
            elif ret in _PRIMITIVES_PASSTHRU:
                buf.write(f'    return whiteout::{name}({cpp_args});\n')
            else:
                buf.write(f'    return reinterpret_cast<whiteout_{ret}*>(new whiteout::{ret}(whiteout::{name}({cpp_args})));\n')
            buf.write('}\n')
        buf.write('\n')

    buf.write('} // extern "C"\n')
    return buf.getvalue()


SOURCE_PROLOGUE = '''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
// AUTOGENERATED by tools/codegen/emit_c.py — do not edit.
// Regenerate via:  python -m tools.codegen.codegen {module} --backend c-source

#include "whiteout_{module}.h"

#include <cstring>
#include <new>
#include <span>
#include <string>
#include <vector>

'''


def emit_source(module: BindModule) -> str:
    prefix = _module_prefix(module)

    buf = StringIO()
    buf.write(SOURCE_PROLOGUE.format(module=module.name))
    for h in module.headers:
        buf.write(f'#include <{h.replace("include/", "")}>\n')
    buf.write('\n')
    buf.write('namespace { using namespace whiteout; }\n\n')

    # ── Per-TU helpers (no shared definitions, no link clashes) ──────────
    # `whiteout_Bytes_free` / `whiteout_CString_free` are defined ONCE in
    # whiteout_c_common.cpp (emit_common). Module .cpp files only need the
    # internal wrapBytes/emptyBytes helpers, which live in an anonymous
    # namespace so each TU gets its own private copy.
    buf.write('''namespace {

// `whiteout::u8` is `std::uint8_t`, so a single overload suffices. We use
// the project's type so the reinterpret_cast is a no-op everywhere the
// codegen runs.
inline whiteout_Bytes wrapBytes(std::vector<whiteout::u8>&& v) {
    auto* owned = new std::vector<whiteout::u8>(std::move(v));
    return { reinterpret_cast<const uint8_t*>(owned->data()), owned->size(), owned };
}

inline whiteout_Bytes emptyBytes() {
    return { nullptr, 0, nullptr };
}

inline whiteout_CString wrapCString(std::string&& s) {
    auto* owned = new std::string(std::move(s));
    return { owned->c_str(), owned->size(), owned };
}

inline whiteout_CString emptyCString() {
    return { nullptr, 0, nullptr };
}

} // anonymous
''')

    # Owned string list. `_at` returns a borrowed CString pointing into the
    # vector — `_owner` is NULL, so whiteout_CString_free is a no-op on it
    # and walking the whole list costs no allocations.
    if _module_has_string_list(module):
        buf.write(f'''
size_t {prefix}_StringList_size(const whiteout_StringList* self) {{
    return reinterpret_cast<const std::vector<std::string>*>(self)->size();
}}

whiteout_CString {prefix}_StringList_at(whiteout_StringList* self, size_t index) {{
    auto* v = reinterpret_cast<std::vector<std::string>*>(self);
    if (index >= v->size()) return emptyCString();
    const std::string& s = (*v)[index];
    return whiteout_CString{{ s.c_str(), s.size(), nullptr }};
}}

void {prefix}_StringList_delete(whiteout_StringList* self) {{
    delete reinterpret_cast<std::vector<std::string>*>(self);
}}
''')

    # Owned handle lists: one small accessor set per element type. The
    # opaque handle is a heap `std::vector<Elem>`; `_at` hands back an
    # interior pointer, so it is borrowed and must not be freed.
    for elem in _handle_list_types(module):
        lh = _c_handle_name(elem)
        buf.write(f'''
size_t {prefix}_{lh}List_size(const whiteout_{lh}List* self) {{
    return reinterpret_cast<const std::vector<{elem}>*>(self)->size();
}}

struct whiteout_{lh}* {prefix}_{lh}List_at(whiteout_{lh}List* self, size_t index) {{
    auto* v = reinterpret_cast<std::vector<{elem}>*>(self);
    return reinterpret_cast<struct whiteout_{lh}*>(&(*v)[index]);
}}

void {prefix}_{lh}List_delete(whiteout_{lh}List* self) {{
    delete reinterpret_cast<std::vector<{elem}>*>(self);
}}
''')


    # Value-object structs (Extent, MipLevel, …) still need an opaque
    # handle + field accessors at the C ABI; the value-vs-pointer
    # distinction only matters to JS/Python's value-binding backends.
    # Shared math types (Vector*, Quaternion) live in the common TU.
    # `@bind record` types are reached through their owning method's
    # snapshot accessors, so they get no handle of their own.
    classes = [c for c in module.classes
               if c.cpp_qualifier not in _SHARED_MATH_TYPES and not c.is_record]
    for c in classes:
        _emit_class_source(buf, c, prefix, module)

    return buf.getvalue()


def _emit_class_source(buf: StringIO, c: BindClass, prefix: str,
                       module: BindModule):
    short = _c_handle_name(c.js_name)
    cls_t = f'whiteout_{short}*'

    cpp_qual = c.cpp_qualifier
    if c.cpp_namespace and not cpp_qual.startswith('whiteout::'):
        cpp_qual = f'{c.cpp_namespace}::{cpp_qual}'
    elif not cpp_qual.startswith('whiteout::'):
        cpp_qual = f'whiteout::{cpp_qual}'

    buf.write(f'// ── {short} ─────────────────────────────────────────────────\n\n')
    buf.write('extern "C" {\n\n')

    # Constructor (default). Parsers' default ctor uses Lenient mode
    # already (default arg), so allocation is the only thing that can
    # fail — and that aborts under -fno-exceptions anyway.
    if not c.no_default_ctor:
        buf.write(f'{cls_t} {prefix}_{short}_new(void) {{\n')
        buf.write(f'    return reinterpret_cast<{cls_t}>(new {cpp_qual}());\n')
        buf.write('}\n\n')

    # Non-default constructors. Header emits the matching declarations via
    # the same _ctor_overloads_named helper to keep names in sync.
    for ctor, sig_name, sig_params in _ctor_overloads_named(c, module):
        # Build the C++ argument list: forward the C params into the C++
        # ctor, converting `const char*` to `std::string` where needed.
        cpp_args = []
        for p in ctor.params:
            if p.type.kind == TypeKind.STRING:
                cpp_args.append(f'std::string({p.name})')
            elif p.type.kind == TypeKind.ENUM:
                cpp_args.append(f'static_cast<{p.type.cpp_text}>({p.name})')
            elif _is_interface_pointer_param(p):
                if '&' in p.cpp_raw:
                    cpp_args.append(
                        f'*reinterpret_cast<{p.type.cpp_text}*>({p.name})')
                else:
                    cpp_args.append(
                        f'reinterpret_cast<{p.type.cpp_text}*>({p.name})')
            elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
                    and not p.type.cpp_text.startswith('std::'):
                # Class-handle param: dereference the opaque pointer.
                cpp_args.append(
                    f'*reinterpret_cast<{p.type.cpp_text}*>({p.name})')
            else:
                cpp_args.append(p.name)
        cpp_call = ', '.join(cpp_args)
        buf.write(f'{cls_t} {prefix}_{short}_new_{sig_name}({sig_params}) {{\n')
        buf.write(f'    return reinterpret_cast<{cls_t}>(new {cpp_qual}({cpp_call}));\n')
        buf.write('}\n\n')

    # Destructor.
    buf.write(f'void {prefix}_{short}_delete({cls_t} self) {{\n')
    buf.write(f'    delete reinterpret_cast<{cpp_qual}*>(self);\n')
    buf.write('}\n\n')

    for m in c.methods:
        if not _is_method_supported(m, module):
            continue
        if _is_vector_string_return(m):
            _emit_vector_string_defs(buf, m, c, cpp_qual, prefix)
            if _is_owned_string_list_return(m):
                short_c = _c_handle_name(c.js_name)
                buf.write(f'''
struct whiteout_StringList* {prefix}_{short_c}_{m.name}(const whiteout_{short_c}* self) {{
    return reinterpret_cast<struct whiteout_StringList*>(
        new std::vector<std::string>(
            reinterpret_cast<const {cpp_qual}*>(self)->{m.cpp_name}()));
}}
''')
            continue
        if _is_vector_record_return(m, module):
            _emit_record_vector_defs(buf, m, c, cpp_qual, prefix, module)
            continue
        _emit_method_source(buf, m, c, cpp_qual, prefix, module)

    _emit_field_defs(buf, c, cpp_qual, prefix, module,
                     _known_class_short_names(module))
    buf.write('} // extern "C"\n\n')


def _emit_method_source(buf: StringIO, m: BindMethod, c: BindClass,
                        cpp_qual: str, prefix: str, module: BindModule):
    short = _c_handle_name(c.js_name)
    ret = m.return_type
    ret_c = _c_return_type(m, module)

    # Build the C parameter list.
    self_param = '' if m.is_static else (
        f'const whiteout_{short}* self' if m.is_const else
        f'whiteout_{short}* self'
    )
    param_strs = [self_param] if self_param else []
    for p in m.params:
        if p.span_scalar is not None:
            short_t, _ = p.span_scalar
            c_elem = _C_PRIMITIVE[short_t]
            param_strs.append(f'const {c_elem}* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _is_interface_pointer_param(p):
            param_strs.append(f'void* {p.name}')
        elif p.type.kind == TypeKind.STRING:
            param_strs.append(f'const char* {p.name}')
        elif _is_byte_vector_param(p):
            param_strs.append(f'const uint8_t* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _scalar_vector_param_elem(p) is not None:
            c_elem = _scalar_vector_param_elem(p)
            param_strs.append(f'const {c_elem}* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        elif _is_class_vector_param(p, module):
            elem_handle = _resolve_handle_short(p.type.element.cpp_text, module)
            param_strs.append(
                f'const struct whiteout_{elem_handle}* const* {p.name}')
            param_strs.append(f'size_t {p.name}_size')
        else:
            param_strs.append(f'{_c_type(p.type, module)} {p.name}')
    opt_elem = _optional_primitive_return(m)
    if opt_elem is not None:
        param_strs.append(f'{opt_elem}* out_value')
    else:
        opt_arr = _optional_array_return(m)
        if opt_arr is not None:
            # Caller supplies a buffer of `opt_arr[1]` elements.
            param_strs.append(f'{opt_arr[0]}* out_value')
    params = ', '.join(param_strs) if param_strs else 'void'

    buf.write(f'{ret_c} {prefix}_{short}_{m.name}({params}) {{\n')

    # Build the C++ call expression. Each C param is mapped back to a C++
    # value the underlying method accepts.
    cpp_args = []
    for p in m.params:
        if p.span_scalar is not None:
            short_t, _canon = p.span_scalar
            cpp_args.append(
                f'std::span<const whiteout::{short_t}>({p.name}, {p.name}_size)')
        elif _is_interface_pointer_param(p):
            # Interface pointer or reference: void* → cast back to X*,
            # then dereference for reference-shaped params so the C++
            # call binds to `X&` overloads correctly.
            if '&' in p.cpp_raw:
                cpp_args.append(
                    f'*reinterpret_cast<{p.type.cpp_text}*>({p.name})')
            else:
                cpp_args.append(
                    f'reinterpret_cast<{p.type.cpp_text}*>({p.name})')
        elif p.type.kind == TypeKind.STRING:
            # const char* → std::string. Null pointer treated as empty.
            cpp_args.append(f'std::string({p.name} ? {p.name} : "")')
        elif _is_byte_vector_param(p):
            # (ptr, size) → std::vector<u8>. Copy-construct from the range.
            cpp_args.append(
                f'std::vector<whiteout::u8>({p.name}, {p.name} + {p.name}_size)')
        elif _scalar_vector_param_elem(p) is not None:
            # (ptr, count) → std::vector<T>. Enums arrive as int32_t and
            # are cast element-wise; scalars copy straight from the range.
            elem_cpp = p.type.element.cpp_text
            if p.type.element.kind == TypeKind.ENUM:
                cpp_args.append(
                    f'([&]{{ std::vector<{elem_cpp}> __v; __v.reserve({p.name}_size); '
                    f'for (size_t __i = 0; __i < {p.name}_size; ++__i) '
                    f'__v.push_back(static_cast<{elem_cpp}>({p.name}[__i])); '
                    f'return __v; }})()')
            else:
                cpp_args.append(
                    f'std::vector<{elem_cpp}>({p.name}, {p.name} + {p.name}_size)')
        elif _is_class_vector_param(p, module):
            # (handle*[], size) → std::vector<Class>. Each handle is
            # dereferenced and copied into the local vector before the
            # call. Emitted inline; the std::vector is materialised in
            # a comma-expression so it lives for the duration of the
            # invocation.
            elem_cpp = p.type.element.cpp_text
            cpp_args.append(
                f'([&]{{ std::vector<{elem_cpp}> __v; __v.reserve({p.name}_size); '
                f'for (size_t __i = 0; __i < {p.name}_size; ++__i) '
                f'__v.emplace_back(*reinterpret_cast<const {elem_cpp}*>({p.name}[__i])); '
                f'return __v; }})()')
        elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
                and not p.type.cpp_text.startswith('std::'):
            # Handle params: opaque pointer → dereferenced C++ value.
            # `Texture` shows up as UNKNOWN from another module's parser,
            # but it's still a class — same dereference + cast path.
            cpp_args.append(f'*reinterpret_cast<const {p.type.cpp_text}*>({p.name})')
        elif p.type.kind == TypeKind.ENUM:
            cpp_args.append(f'static_cast<{p.type.cpp_text}>({p.name})')
        else:
            cpp_args.append(p.name)

    cpp_args_str = ', '.join(cpp_args)
    if m.is_static:
        call = f'{cpp_qual}::{m.cpp_name}({cpp_args_str})'
    else:
        call = f'reinterpret_cast<{cpp_qual}*>(self)->{m.cpp_name}({cpp_args_str})'
        if m.is_const:
            call = call.replace(
                f'reinterpret_cast<{cpp_qual}*>(self)',
                f'reinterpret_cast<const {cpp_qual}*>(self)')

    # Body — no try/catch. The whole TU is built `-fno-exceptions` and
    # parsers default to Lenient (failure → empty optional, never throw).
    buf.write(_emit_call_body(call, ret, m, module, indent='    '))
    buf.write('}\n\n')


def _emit_call_body(call: str, ret: TypeRef, m: BindMethod,
                    module: BindModule, indent: str = '    ') -> str:
    """Emit the body of an extern "C" function — converts the C++ call
    expression's return into the C-ABI shape and writes the `return`."""
    out = StringIO()
    _opt_scalar_elem = _optional_primitive_return(m)
    if ret.cpp_text == 'void':
        out.write(f'{indent}{call};\n')
        return out.getvalue()

    # Reference return to a class (`Foo& chainedMethod(...)`) — common
    # for builder-pattern methods that return `*this`. Take the address
    # of the referent rather than copy-constructing a new handle, which
    # wouldn't compile for move-only types like VertexBufferBuilder.
    if m.return_is_reference and ret.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
            and not ret.cpp_text.startswith('std::'):
        elem = ret.cpp_text
        c_handle = _resolve_handle_short(elem, module)
        # const_cast strips const off the referent — the C ABI handle
        # type is non-const (`whiteout_X*`) regardless of the C++ source
        # const-ness, and field accessors take a `const whiteout_X*`
        # so const_cast'ing here doesn't actually weaken the type
        # contract at the C ABI boundary. Without this, `const T&`
        # returns fail to reinterpret_cast.
        out.write(f'{indent}auto& __r = {call};\n')
        out.write(f'{indent}return const_cast<struct whiteout_{c_handle}*>(\n')
        out.write(f'{indent}    reinterpret_cast<const struct whiteout_{c_handle}*>(&__r));\n')
        return out.getvalue()

    # vector<Class> / optional<vector<Class>> → owned opaque list.
    _list_elem = _handle_list_return(m, module)
    if _list_elem is not None:
        _lh = _c_handle_name(_list_elem)
        if ret.kind == TypeKind.OPTIONAL:
            out.write(f'{indent}auto __r = {call};\n')
            out.write(f'{indent}if (!__r) return nullptr;\n')
            out.write(f'{indent}return reinterpret_cast<struct whiteout_{_lh}List*>(\n')
            out.write(f'{indent}    new std::vector<{_list_elem}>(std::move(*__r)));\n')
        else:
            out.write(f'{indent}return reinterpret_cast<struct whiteout_{_lh}List*>(\n')
            out.write(f'{indent}    new std::vector<{_list_elem}>({call}));\n')
        return out.getvalue()

    # std::optional<std::array<T, N>> → has-value flag + out-buffer.
    _opt_arr = _optional_array_return(m)
    if _opt_arr is not None:
        _elem_c, _n = _opt_arr
        out.write(f'{indent}auto __r = {call};\n')
        out.write(f'{indent}if (!__r) return 0;\n')
        out.write(f'{indent}if (out_value) {{\n')
        out.write(f'{indent}    for (size_t __i = 0; __i < {_n}; ++__i)\n')
        out.write(f'{indent}        out_value[__i] = '
                  f'static_cast<{_elem_c}>((*__r)[__i]);\n')
        out.write(f'{indent}}}\n')
        out.write(f'{indent}return 1;\n')
        return out.getvalue()

    # std::optional<scalar> → has-value flag + out-param. A scalar has no
    # natural empty sentinel the way a handle or a buffer does.
    if _opt_scalar_elem is not None:
        out.write(f'{indent}auto __r = {call};\n')
        out.write(f'{indent}if (!__r) return 0;\n')
        out.write(f'{indent}if (out_value) *out_value = '
                  f'static_cast<{_opt_scalar_elem}>(*__r);\n')
        out.write(f'{indent}return 1;\n')
        return out.getvalue()

    # std::optional<class> → nullable handle to a heap-moved copy.
    # UNKNOWN here captures cross-module classes like `Texture` (defined
    # in the textures namespace but not @bind'd inside the per-format
    # parsers' module scope).
    if ret.kind == TypeKind.OPTIONAL \
            and ret.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
            and not ret.element.cpp_text.startswith('std::'):
        elem = ret.element.cpp_text
        c_handle = _resolve_handle_short(elem, module)
        out.write(f'{indent}auto __r = {call};\n')
        out.write(f'{indent}if (!__r) return nullptr;\n')
        out.write(f'{indent}return reinterpret_cast<struct whiteout_{c_handle}*>(\n')
        out.write(f'{indent}    new {elem}(std::move(*__r)));\n')
        return out.getvalue()

    # std::optional<std::vector<u8>> → whiteout_Bytes or empty
    if ret.kind == TypeKind.OPTIONAL and ret.element.kind == TypeKind.VECTOR \
            and _short_name(ret.element.element.cpp_text) in ('u8', 'unsigned char'):
        out.write(f'{indent}auto __r = {call};\n')
        out.write(f'{indent}if (!__r) return emptyBytes();\n')
        out.write(f'{indent}return wrapBytes(std::move(*__r));\n')
        return out.getvalue()

    # std::optional<std::string> → whiteout_CString or empty
    if ret.kind == TypeKind.OPTIONAL and ret.element.kind == TypeKind.STRING:
        out.write(f'{indent}auto __r = {call};\n')
        out.write(f'{indent}if (!__r) return emptyCString();\n')
        out.write(f'{indent}return wrapCString(std::move(*__r));\n')
        return out.getvalue()

    # std::string → whiteout_CString
    if ret.kind == TypeKind.STRING:
        out.write(f'{indent}return wrapCString({call});\n')
        return out.getvalue()

    # std::vector<u8> → whiteout_Bytes
    if ret.kind == TypeKind.VECTOR and \
            _short_name(ret.element.cpp_text) in ('u8', 'unsigned char'):
        out.write(f'{indent}return wrapBytes({call});\n')
        return out.getvalue()

    # std::span<const u8> → whiteout_Bytes (borrowed: _owner = NULL).
    # Caller gets a view into memory owned by the C++ object; calling
    # whiteout_Bytes_free is a no-op when _owner is NULL.
    if _is_span_const_u8(ret):
        out.write(f'{indent}auto __s = {call};\n')
        out.write(f'{indent}return whiteout_Bytes{{\n')
        out.write(f'{indent}    reinterpret_cast<const uint8_t*>(__s.data()),\n')
        out.write(f'{indent}    __s.size(), nullptr }};\n')
        return out.getvalue()

    # Plain class return → heap-moved handle.
    if ret.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
            and not ret.cpp_text.startswith('std::'):
        elem = ret.cpp_text
        c_handle = _resolve_handle_short(elem, module)
        out.write(f'{indent}return reinterpret_cast<struct whiteout_{c_handle}*>(\n')
        out.write(f'{indent}    new {elem}({call}));\n')
        return out.getvalue()

    # Enum → cast to int32.
    if ret.kind == TypeKind.ENUM:
        out.write(f'{indent}return static_cast<int32_t>({call});\n')
        return out.getvalue()

    # Primitives, bools, default.
    out.write(f'{indent}return {call};\n')
    return out.getvalue()


