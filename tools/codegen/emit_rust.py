# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> safe, idiomatic Rust over the value ABI (`emit_rust_abi.py`).

Phase 1 scope: the shared math types.

Two design points worth stating, because they are what make the output read
as Rust rather than as a transliterated C# binding:

1. **The public type IS the FFI type.** Every math type is `#[repr(C)]` and
   `Copy`, so it crosses the boundary with no wrapper, no conversion, and no
   allocation. That is also why the `extern "C"` block lives in this crate's
   private `ffi` module rather than a separate `-sys` crate: Rust's orphan
   rule forbids inherent impls on a foreign type, and a newtype wrapper would
   buy nothing but noise. `whiteout-sys` is still the right home for the
   *handle* ABI in later phases, where the raw pointers genuinely want hiding.

2. **Hot operations are native Rust; exotic ones defer to C++.** Component-wise
   arithmetic, dot, and length are a few instructions — routing them through
   FFI would be silly. Anything with non-obvious semantics (Hamilton product,
   slerp, matrix inverse, the TCB/Bezier/Hermite interpolators) calls the C++
   implementation so the binding cannot drift from the library.
"""

from __future__ import annotations

import re
import subprocess
from io import StringIO

from .emit_c import (
    _MATRIX_DIMS,
    _PRIMITIVES_PASSTHRU,
    _SHARED_MATH_BYTE_SIZES,
    _SHARED_MATH_FIELDS,
    _SHARED_MATH_FREE_FUNCTIONS,
    _SHARED_MATH_METHODS,
    _SHARED_MATH_TYPES,
)
from .emit_rust_abi import ABI_VERSION

# Methods implemented directly in Rust instead of calling through the ABI.
# Restricted to operations whose semantics are unambiguous from the C++
# source (component-wise, per `VectorMethods`). Deliberately excludes
# Quaternion::mul (Hamilton product) and the matrix products.
_NATIVE_VECTORLIKE = {
    'add', 'sub', 'negate', 'mul_scalar', 'div_scalar',
    'dot', 'length', 'length_squared',
}
_NATIVE_COMPONENTWISE_MULDIV = {'mul', 'div'}

# `equals` maps to C++ `operator==`, which compares components exactly —
# identical to Rust's derived PartialEq. Emitting both would be redundant.
_SKIP = {'equals'}

_VECTOR_LIKE = ('Vector2f', 'Vector3f', 'Vector4f', 'Quaternion')


# Newline for emitted Rust, spelled as a name so the f-string
# templates below stay readable.
NL = chr(10)


def _rustfmt(text: str) -> str:
    """Run the emitted source through rustfmt.

    Generated code is read by humans and diffed in review, so it should be
    formatted the same way hand-written code is — and `cargo fmt --check`
    in CI should pass without special-casing generated files. Falls back to
    the unformatted text when rustfmt isn't installed, since codegen must
    keep working on a machine with no Rust toolchain.
    """
    try:
        r = subprocess.run(
            ['rustfmt', '--edition', '2021', '--emit', 'stdout', '--quiet'],
            input=text, capture_output=True, text=True, encoding='utf-8',
        )
    except (OSError, FileNotFoundError):
        return text
    if r.returncode != 0 or not r.stdout.strip():
        return text
    return r.stdout


def _snake(name: str) -> str:
    """`fovY` -> `fov_y`, `nearZ` -> `near_z`. The C++ tables use camelCase
    for parameters; leaving that in the Rust signature is both a lint and a
    readability failure."""
    s = re.sub(r'(.)([A-Z][a-z]+)', r'\1_\2', name)
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', s)
    s = s.lower()
    # `create2D` lands here as `create2_d`. Rejoin the dimension token and
    # split before the digit instead, giving `create_2d`.
    s = re.sub(r'(\d)_([a-z])(?=_|$)', r'\1\2', s)
    return re.sub(r'([a-z])(\d)', r'\1_\2', s)


def _rust_ty(t: str) -> str:
    if t == 'void':
        return '()'
    if t == 'bool':
        return 'bool'
    if t == 'float':
        return 'f32'
    if t == 'size_t':
        return 'usize'
    if t == 'int':
        return 'i32'
    return t


def _ffi_ty(t: str) -> str:
    """Return type as spelled in the extern block (bool is int32_t)."""
    if t == 'void':
        return '()'
    if t == 'bool':
        return 'i32'
    if t == 'float':
        return 'f32'
    if t == 'size_t':
        return 'usize'
    if t == 'int':
        return 'i32'
    return t


def _is_native(owner: str, name: str) -> bool:
    if owner not in _VECTOR_LIKE:
        return False
    if name in _NATIVE_VECTORLIKE:
        return True
    # Component-wise * and / exist on Vector*, but Quaternion overrides
    # operator* with the Hamilton product, so it must go through C++.
    return name in _NATIVE_COMPONENTWISE_MULDIV and owner != 'Quaternion'


def _sym(owner: str, name: str) -> str:
    return f'whiteout_v_{owner}_{name}'


# ── FFI module ────────────────────────────────────────────────────────────

def _emit_ffi(buf: StringIO) -> None:
    buf.write('''use super::*;

''')
    buf.write(f'pub const ABI_VERSION: u32 = {ABI_VERSION};\n\n')
    buf.write('#[repr(C)]\n#[derive(Clone, Copy, Debug)]\npub struct Layout {\n')
    buf.write('    pub abi_version: u32,\n')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'    pub {name.lower()}: u32,\n')
    buf.write('}\n\n')

    buf.write('extern "C" {\n')
    buf.write('    pub fn whiteout_v_layout() -> Layout;\n')
    for owner in _SHARED_MATH_TYPES:
        for m in _SHARED_MATH_METHODS.get(owner, []):
            name, ret, params, is_static, is_const = m[:5]
            # Natively-implemented methods are declared here too, even
            # though the public wrapper never calls them: the parity tests
            # cross-check every native implementation against the C++ one,
            # which is the only thing stopping the two from drifting.
            if name in _SKIP:
                continue
            parts = []
            if not is_static:
                parts.append(f'self_: {owner}' if is_const else f'self_: *mut {owner}')
            for p_t, p_n in params:
                parts.append(f'{_snake(p_n)}: {_ffi_ty(p_t)}')
            sig = ', '.join(parts)
            ret_s = '' if ret == 'void' else f' -> {_ffi_ty(ret)}'
            buf.write(f'    pub fn {_sym(owner, name)}({sig}){ret_s};\n')
    for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
        sig = ', '.join(f'{_snake(n)}: {_ffi_ty(t)}' for t, n in params)
        buf.write(f'    pub fn whiteout_v_{name}({sig}) -> {_ffi_ty(ret)};\n')
    buf.write('}\n')


# ── Type definitions + impls ──────────────────────────────────────────────

def _emit_struct(buf: StringIO, name: str) -> None:
    fields = _SHARED_MATH_FIELDS.get(name)
    size = _SHARED_MATH_BYTE_SIZES[name]
    buf.write('#[repr(C)]\n')
    buf.write('#[derive(Clone, Copy, Debug, Default, PartialEq)]\n')
    if fields:
        buf.write(f'pub struct {name} {{\n')
        for f in fields:
            buf.write(f'    pub {f}: f32,\n')
        buf.write('}\n\n')
    else:
        dim = _MATRIX_DIMS[name]
        buf.write(f'/// Row-major, matching the C++ `MatrixMethods` storage order.\n')
        buf.write(f'pub struct {name} {{\n    pub data: [[f32; {dim}]; {dim}],\n}}\n\n')

    buf.write(f'const _: () = assert!(core::mem::size_of::<{name}>() == {size});\n\n')


def _emit_native_impls(buf: StringIO, name: str) -> None:
    """Constructors, operators and the trivial geometry, in pure Rust."""
    fields = _SHARED_MATH_FIELDS.get(name)
    if not fields:
        dim = _MATRIX_DIMS[name]
        buf.write(f'''impl {name} {{
    pub const DIM: usize = {dim};

    #[inline]
    pub const fn from_rows(data: [[f32; {dim}]; {dim}]) -> Self {{
        Self {{ data }}
    }}
}}

impl core::ops::Index<(usize, usize)> for {name} {{
    type Output = f32;
    #[inline]
    fn index(&self, (row, col): (usize, usize)) -> &f32 {{
        &self.data[row][col]
    }}
}}

impl core::ops::IndexMut<(usize, usize)> for {name} {{
    #[inline]
    fn index_mut(&mut self, (row, col): (usize, usize)) -> &mut f32 {{
        &mut self.data[row][col]
    }}
}}

''')
        return

    args = ', '.join(f'{f}: f32' for f in fields)
    init = ', '.join(fields)
    buf.write(f'''impl {name} {{
    #[inline]
    pub const fn new({args}) -> Self {{
        Self {{ {init} }}
    }}

''')
    # dot / length / length_squared
    dot_expr = ' + '.join(f'self.{f} * other.{f}' for f in fields)
    buf.write(f'''    #[inline]
    pub fn dot(self, other: Self) -> f32 {{
        {dot_expr}
    }}

    #[inline]
    pub fn length_squared(self) -> f32 {{
        self.dot(self)
    }}

    #[inline]
    pub fn length(self) -> f32 {{
        self.length_squared().sqrt()
    }}

''')
    buf.write('}\n\n')

    # Operators.
    binops = [('Add', 'add', '+'), ('Sub', 'sub', '-')]
    if name != 'Quaternion':
        binops += [('Mul', 'mul', '*'), ('Div', 'div', '/')]
    for trait, fn, op in binops:
        body = ', '.join(f'{f}: self.{f} {op} other.{f}' for f in fields)
        buf.write(f'''impl core::ops::{trait} for {name} {{
    type Output = Self;
    #[inline]
    fn {fn}(self, other: Self) -> Self {{
        Self {{ {body} }}
    }}
}}

''')
    mul_body = ', '.join(f'{f}: self.{f} * scalar' for f in fields)
    buf.write(f'''impl core::ops::Mul<f32> for {name} {{
    type Output = Self;
    #[inline]
    fn mul(self, scalar: f32) -> Self {{
        Self {{ {mul_body} }}
    }}
}}

''')
    # C++ `operator/(scalar)` computes the reciprocal once and multiplies
    # (`VectorMethods::operator/=`). Dividing per component here instead
    # would differ in the last ulp — the parity test catches it.
    div_body = ', '.join(f'{f}: self.{f} * inv' for f in fields)
    buf.write(f'''impl core::ops::Div<f32> for {name} {{
    type Output = Self;
    #[inline]
    fn div(self, scalar: f32) -> Self {{
        let inv = 1.0f32 / scalar;
        Self {{ {div_body} }}
    }}
}}

''')
    neg = ', '.join(f'{f}: -self.{f}' for f in fields)
    buf.write(f'''impl core::ops::Neg for {name} {{
    type Output = Self;
    #[inline]
    fn neg(self) -> Self {{
        Self {{ {neg} }}
    }}
}}

''')


# Operator-named methods that must be spelled as trait impls rather than
# inherent methods — `m1 * m2` reads better than `m1.mul(m2)`, and clippy
# rightly flags an inherent `mul` as shadowing `std::ops::Mul::mul`.
_OPERATOR_TRAITS = {
    'add':        ('Add', 'add', None),
    'sub':        ('Sub', 'sub', None),
    'mul':        ('Mul', 'mul', None),
    'div':        ('Div', 'div', None),
    'negate':     ('Neg', 'neg', None),
    'mul_scalar': ('Mul', 'mul', 'f32'),
    'div_scalar': ('Div', 'div', 'f32'),
}


def _emit_ffi_operator(buf: StringIO, owner: str, m: tuple) -> None:
    """Emit an FFI-backed operator as a `core::ops` impl."""
    mname, ret, params, _is_static, _is_const = m[:5]
    trait, fn, rhs_prim = _OPERATOR_TRAITS[mname]
    sym = _sym(owner, mname)
    if mname == 'negate':
        buf.write(f'''impl core::ops::Neg for {owner} {{
    type Output = {owner};
    #[inline]
    fn neg(self) -> {owner} {{
        unsafe {{ ffi::{sym}(self) }}
    }}
}}

''')
        return
    rhs = rhs_prim if rhs_prim else owner
    arg = 'scalar' if rhs_prim else 'other'
    generic = f'<{rhs}>' if rhs_prim else ''
    buf.write(f'''impl core::ops::{trait}{generic} for {owner} {{
    type Output = {_rust_ty(ret)};
    #[inline]
    fn {fn}(self, {arg}: {rhs}) -> {_rust_ty(ret)} {{
        unsafe {{ ffi::{sym}(self, {arg}) }}
    }}
}}

''')


def _emit_ffi_impls(buf: StringIO, name: str) -> None:
    candidates = [m for m in _SHARED_MATH_METHODS.get(name, [])
                  if m[0] not in _SKIP and not _is_native(name, m[0])]
    operators = [m for m in candidates if m[0] in _OPERATOR_TRAITS]
    methods = [m for m in candidates if m[0] not in _OPERATOR_TRAITS]

    for m in operators:
        _emit_ffi_operator(buf, name, m)

    if not methods:
        return
    buf.write(f'impl {name} {{\n')
    for m in methods:
        mname, ret, params, is_static, is_const = m[:5]
        rust_ret = _rust_ty(ret)
        parts = []
        if not is_static:
            parts.append('self' if is_const else '&mut self')
        for p_t, p_n in params:
            parts.append(f'{_snake(p_n)}: {_rust_ty(p_t)}')
        sig = ', '.join(parts)
        ret_s = '' if ret == 'void' else f' -> {rust_ret}'

        call_args = []
        if not is_static:
            call_args.append('self' if is_const else 'self')
        call_args += [_snake(p_n) for _, p_n in params]
        call = f'ffi::{_sym(name, mname)}({", ".join(call_args)})'

        buf.write(f'    #[inline]\n    pub fn {mname}({sig}){ret_s} {{\n')
        # Every one of these is a pure computation on POD passed by value —
        # no allocation, no lifetime, nothing that can fail.
        if ret == 'bool':
            buf.write(f'        unsafe {{ {call} != 0 }}\n')
        else:
            buf.write(f'        unsafe {{ {call} }}\n')
        buf.write('    }\n\n')
    buf.write('}\n\n')


def emit_math() -> str:
    buf = StringIO()
    buf.write('''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
// AUTOGENERATED by tools/codegen/emit_rust.py — do not edit.
// Regenerate via:  python -m tools.codegen.codegen <any> --backend rust-math

//! Vector, quaternion and matrix types.
//!
//! Every type here is `#[repr(C)]` and `Copy`, laid out identically to its
//! C++ counterpart, so it crosses the FFI boundary with no conversion and
//! no allocation. Component-wise arithmetic, `dot` and `length` are plain
//! Rust; anything with subtler semantics (Hamilton product, `slerp`,
//! matrix inverse, the spline interpolators) calls into the C++
//! implementation so this binding cannot drift from the library.

''')
    for name in _SHARED_MATH_TYPES:
        _emit_struct(buf, name)
    for name in _SHARED_MATH_TYPES:
        buf.write(f'// ── {name} ─────────────────────────────────────────────\n\n')
        _emit_native_impls(buf, name)
        _emit_ffi_impls(buf, name)

    if _SHARED_MATH_FREE_FUNCTIONS:
        buf.write('// ── Free functions ─────────────────────────────────────\n\n')
        for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
            sig = ', '.join(f'{_snake(n)}: {_rust_ty(t)}' for t, n in params)
            args = ', '.join(_snake(n) for _, n in params)
            buf.write(f'''#[inline]
pub fn {name}({sig}) -> {_rust_ty(ret)} {{
    unsafe {{ ffi::whiteout_v_{name}({args}) }}
}}

''')

    buf.write('''/// Verify that the linked native library matches the layouts this crate
/// was generated against.
///
/// The `const` assertions above pin the Rust side at compile time; this
/// catches the other half — a `whiteout_native` built from different
/// headers or by a toolchain that lays the C++ types out differently.
pub fn check_abi() -> Result<(), crate::Error> {
    let l = unsafe { ffi::whiteout_v_layout() };
    if l.abi_version != ffi::ABI_VERSION {
        return Err(crate::Error::Layout {
            what: "abi_version",
            expected: ffi::ABI_VERSION as usize,
            actual: l.abi_version as usize,
        });
    }
''')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'''    if l.{name.lower()} as usize != core::mem::size_of::<{name}>() {{
        return Err(crate::Error::Layout {{
            what: "{name}",
            expected: core::mem::size_of::<{name}>(),
            actual: l.{name.lower()} as usize,
        }});
    }}
''')
    buf.write('    Ok(())\n}\n\n')

    buf.write('''/// Raw ABI surface.
///
/// Exposed so the parity tests can cross-check the natively-implemented
/// operations against the C++ ones. Not covered by semver — use the safe
/// API above.
#[doc(hidden)]
pub mod ffi {
''')
    inner = StringIO()
    _emit_ffi(inner)
    for line in inner.getvalue().splitlines():
        buf.write(f'    {line}\n' if line else '\n')
    buf.write('}\n')
    return _rustfmt(buf.getvalue())


# ══════════════════════════════════════════════════════════════════════════
# Phase 2: IR-driven emission for handle-bearing modules.
#
# Where the math module is table-driven (its CRTP surface isn't visible to
# libclang), everything else comes straight from the IR that already feeds
# emit_c.py. The C symbol names are derived with emit_c's own helpers, so
# the extern block cannot drift from the header.
# ══════════════════════════════════════════════════════════════════════════

from .emit_c import (  # noqa: E402
    _module_has_string_list,
    _is_owned_string_list_return,
    _handle_list_return,
    _handle_list_types,
    _optional_array_return,
    _scalar_vector_param_elem,
    _optional_primitive_return,
    _is_byte_vector_param,
    _is_class_vector_param,
    _is_interface_pointer_param,
    _c_handle_name,
    _ctor_overloads_named,
    _is_span_const_u8,
    _module_prefix,
)
from .ir import BindClass, BindModule, TypeKind, TypeRef  # noqa: E402

_RUST_PRIMITIVE = {
    'bool': 'bool',
    'u8': 'u8', 'i8': 'i8',
    'u16': 'u16', 'i16': 'i16',
    'u32': 'u32', 'i32': 'i32',
    'u64': 'u64', 'i64': 'i64',
    'f32': 'f32', 'f64': 'f64',
    'unsigned char': 'u8', 'signed char': 'i8', 'char': 'i8',
    'unsigned short': 'u16', 'short': 'i16',
    'unsigned int': 'u32', 'int': 'i32',
    'unsigned long long': 'u64', 'long long': 'i64',
    'float': 'f32', 'double': 'f64',
    'size_t': 'usize', 'unsigned long': 'u64', 'long': 'i64',
    'void': '()',
}

# C ABI spells these as int32_t.
_C_PRIMITIVE = dict(_RUST_PRIMITIVE)
_C_PRIMITIVE['bool'] = 'i32'


def _rust_prim(cpp: str) -> str | None:
    """Primitive lookup, tolerant of the namespace libclang attaches.
    `whiteout::u32` and `u32` are the same type."""
    t = cpp.replace('const ', '').replace('&', '').strip()
    if t.startswith('whiteout::'):
        t = t[len('whiteout::'):]
    return _RUST_PRIMITIVE.get(t)


def _pascal(name: str) -> str:
    return name[:1].upper() + name[1:]


def _variant_name(js_name: str) -> str:
    """PascalCase identifier for an enumerator.

    Trailing-segment-only naming (`Layer_ShaderType_HD` -> `HD`) reads well
    until a segment is not identifier-shaped: `Unk_0x400000` would become
    `0x400000`. Fall back to the whole name in that case, and prefix a
    letter if it still starts with a digit.
    """
    tail = js_name.split('_')[-1] if '_' in js_name else js_name
    out = _pascal(tail)
    if not out or not (out[0].isalpha() or out[0] == '_'):
        out = ''.join(_pascal(part) for part in js_name.split('_') if part)
    if not out:
        out = 'Unnamed'
    if not (out[0].isalpha() or out[0] == '_'):
        out = f'V{out}'
    return out


def _screaming(name: str) -> str:
    out = _snake(name).upper()
    return out if out[:1].isalpha() or out[:1] == '_' else f'V{out}'


def _is_flags_enum(e) -> bool:
    """True when the enumerators describe a bitmask rather than a choice.

    Two signals: repeated discriminants (aliases like `Unshaded = 0x8000`
    sharing a bit with another name), which a Rust `enum` cannot represent
    at all; or every non-zero value being a distinct power of two, which is
    what a flag set looks like.
    """
    values = [v.value for v in e.values]
    if len(values) != len(set(values)):
        return True
    nonzero = [v for v in values if v != 0]
    if len(nonzero) >= 3 and all(v > 0 and (v & (v - 1)) == 0 for v in nonzero):
        return True
    # The C++ convention here is to name bitmask enums `...Flag`/`...Flags`.
    # Trust it: those carry composite values that break the bit test but are
    # still meant to be OR'd, not matched.
    short = _c_handle_name(e.js_name)
    return len(nonzero) >= 3 and (short.endswith('Flag') or short.endswith('Flags'))


def _enum_rust_name(e, module: BindModule | None = None) -> str:
    """Strip the module prefix the C/JS naming adds — the Rust module
    already namespaces it. `MdxLayerFilterMode` in module `mdx` becomes
    `LayerFilterMode`."""
    short = _c_handle_name(e.js_name)
    if module is not None:
        prefix = _pascal(module.name)
        if short.startswith(prefix) and len(short) > len(prefix):
            return short[len(prefix):]
    return short


def _class_rust_name(c: BindClass, module: BindModule) -> str:
    """`BlpParser` stays `BlpParser`; a class whose name merely repeats the
    module (`CascStorage` in `casc`) loses the prefix — `casc::Storage`
    reads better than `casc::CascStorage`."""
    short = _c_handle_name(c.js_name)
    prefix = _pascal(module.name)
    if short.startswith(prefix) and len(short) > len(prefix):
        return short[len(prefix):]
    return short


# Method renames. The C ABI's overload disambiguation (`_overload2`,
# parameter-name suffixes) is a C-symbol concern; it must not reach the
# Rust surface. Keyed by (rust class, IR method name).
#
# The long-term home for these is a `@bind rust_name=` annotation on the
# C++ declaration (§6 of the plan); this table is the interim.
_KEYWORDS = {
    'type', 'move', 'ref', 'box', 'match', 'loop', 'impl', 'fn', 'let',
    'mut', 'use', 'mod', 'crate', 'self', 'super', 'where', 'trait',
    'const', 'static', 'struct', 'enum', 'async', 'await', 'dyn', 'yield',
}

_RENAMES = {
    ('Texture', 'format'): 'convert_to',
    ('Texture', 'type'): 'texture_type',
    ('Texture', 'format_overload2'): 'format',
    ('Texture', 'generateMipmaps_pool'): 'generate_mipmaps_default',
    # The buffer-based overloads are the primary API in Rust; the
    # path-based ones are the special case, not the other way round.
    ('Parser', 'parse'): 'parse_file',
    ('Parser', 'parse_buffer_format'): 'parse',
    ('Writer', 'write'): 'write_file',
    ('Writer', 'write_mdx_format_mdlFormat'): 'write',
    ('Parser', 'parse_buffer'): 'parse',
    ('Writer', 'write_model'): 'write',
}


# Methods on a subclassable interface that carry work across the boundary
# in a struct rather than as a bare callback, so `callback_target` does not
# see them. Small and explicit beats a predicate that guesses.
_HOST_IMPLEMENTED = {'submit'}


def _is_host_implemented(m) -> bool:
    """A method the host implements rather than calls.

    These cross through the fn-table shims in
    `bindings/c/whiteout_host_shims.cpp` and are wrapped by hand in
    `interfaces.rs`, so the generated module must neither claim them nor
    report them as gaps.
    """
    return m.name in _HOST_IMPLEMENTED or any(p.callback_target for p in m.params)


def _method_name(rust_cls: str, m) -> str:
    """Rust method name: explicit rename, else `getFoo` -> `foo`, else
    plain snake_case."""
    override = _RENAMES.get((rust_cls, m.name))
    if override:
        return override
    name = m.name
    # `getIssues` -> `issues`: a bare noun reads better than `get_issues`,
    # and matches Rust's convention of omitting `get_` on accessors.
    if name.startswith('get') and len(name) > 3 and name[3].isupper():
        name = name[3:]
    out = _snake(name)
    # A raw identifier (`r#type`) would compile but reads badly in a public
    # API; the rename table is the place to fix these properly.
    return f'{out}_' if out in _KEYWORDS else out


def _value_struct_names(module: BindModule) -> set[str]:
    """Short names of classes to emit as plain Rust structs rather than
    handles.

    A candidate has fields, no methods, and only primitive members — but
    that alone is not enough. Two further conditions, both learned the hard
    way:

      * It must be *passed* somewhere (an options struct). `mdx::Model` has
        fields and no methods too; emitting it by value would move the
        model out of C++ entirely.
      * It must never appear as another class's field. The C ABI hands out
        a borrowed interior pointer for those, which a by-value struct
        cannot represent. `m3::AnimRef<f32>` is all-primitive but is a
        field of nearly every M3 type.
    """
    cached = getattr(module, '_rust_value_structs', None)
    if cached is not None:
        return cached

    candidates = {
        _c_handle_name(c.js_name)
        for c in module.classes
        if c.fields and not c.methods
        and all(f.type.kind == TypeKind.PRIMITIVE for f in c.fields)
    }
    used_as_field: set[str] = set()
    passed: set[str] = set()
    for c in module.classes:
        for f in c.fields:
            t = f.type
            for ref in (t, t.element, t.element.element if t.element else None):
                if ref is not None and ref.kind == TypeKind.NESTED:
                    used_as_field.add(_resolve_handle_short(ref.cpp_text, module))
        for m in c.methods:
            for p in m.params:
                passed.add(_resolve_handle_short(p.type.cpp_text, module))

    out = (candidates & passed) - used_as_field
    module._rust_value_structs = out
    return out


# Abstract interfaces the host can implement, and the Rust wrapper that
# carries one. Anything not listed still binds — the parameter is dropped
# and null passed, which the library reads as "not supplied".
_HOST_WRAPPERS = {
    'WorkerPool': 'HostWorkerPool',
    'CascFileSystem': 'HostCascFileSystem',
    'VirtualPathFileSystem': 'HostFileSystem',
    'HttpHandler': 'HostHttpHandler',
}


def _is_field_only(c: BindClass, module: BindModule | None = None) -> bool:
    if module is None:
        return False
    return _c_handle_name(c.js_name) in _value_struct_names(module)


def _module_classes(module: BindModule):
    """Classes this module owns. The shared math types are auto-bound into
    every model module's IR, but they live in `crate::math` — emitting them
    again here would declare `whiteout_mdx_Vector3f_*` symbols that the C
    side never defines (emit_c.py filters them the same way)."""
    return [c for c in module.classes
            if c.cpp_qualifier not in _SHARED_MATH_TYPES
            and _c_handle_name(c.js_name) not in _SHARED_MATH_TYPES]


class _Ctx:
    """Everything the per-method emitters need to resolve names."""

    def __init__(self, module: BindModule):
        self.module = module
        self.prefix = _module_prefix(module)
        # Nested enums arrive as `whiteout::mdx::Layer::FilterMode` on a
        # field but as js_name `MdxLayerFilterMode` on the declaration, so
        # index every spelling that could reach us.
        self.enums = {}
        for e in module.enums:
            for key in (_c_handle_name(e.js_name),
                        e.cpp_qualifier,
                        _c_handle_name(e.cpp_qualifier),
                        e.js_name):
                if key:
                    self.enums.setdefault(key, e)
        self.classes = {}
        for c in _module_classes(module):
            self.classes[_c_handle_name(c.js_name)] = c
            self.classes[_c_handle_name(c.cpp_qualifier)] = c
        self.skipped: list[str] = []

    def sym(self, c: BindClass, method: str) -> str:
        return f'{self.prefix}_{_c_handle_name(c.js_name)}_{method}'

    def handle(self, c: BindClass) -> str:
        return f'whiteout_{_c_handle_name(c.js_name)}'

    def rust_class(self, c: BindClass) -> str:
        return _class_rust_name(c, self.module)

    def class_for(self, cpp_text: str) -> BindClass | None:
        key = _c_handle_name(cpp_text.replace('const ', '').strip())
        return self.classes.get(key)

    def enum_for(self, cpp_text: str):
        return self.enums.get(_c_handle_name(cpp_text))


# ── Return-shape classification ───────────────────────────────────────────
#
# Each shape names how the C return value becomes a Rust value. Keeping this
# as an explicit tag (rather than branching on TypeKind in five places) is
# what keeps the extern block and the safe wrapper in agreement.

def _return_shape(m, ctx: _Ctx):
    """(shape, rust_type, ffi_type) or None when unsupported."""
    t: TypeRef = m.return_type
    raw = t.cpp_text.replace('const ', '').strip()

    if _is_span_const_u8(t):
        return ('borrowed_slice', "BorrowedSlice<'_>", 'RawBytes')

    if m.bytes_out:
        if t.kind == TypeKind.OPTIONAL:
            return ('bytes_opt', 'Option<Bytes>', 'RawBytes')
        return ('bytes', 'Bytes', 'RawBytes')

    if t.kind == TypeKind.PRIMITIVE or raw in _RUST_PRIMITIVE:
        prim = _rust_prim(raw)
        if prim is None:
            return None
        if prim == '()':
            return ('void', '()', '()')
        if prim == 'bool':
            return ('bool', 'bool', 'i32')
        return ('prim', prim, prim)

    if t.kind == TypeKind.ENUM:
        e = ctx.enum_for(raw)
        if e is None:
            return None
        return ('enum', _enum_rust_name(e, ctx.module), 'i32')

    if t.kind == TypeKind.STRING:
        return ('string', 'String', 'RawCString')

    list_elem = _handle_list_return(m, ctx.module)
    if list_elem is not None:
        ec = ctx.class_for(list_elem)
        if ec is None or _is_field_only(ec, ctx.module):
            return None
        lname = f'{ctx.rust_class(ec)}List'
        # Always Option: the vector-returning form cannot fail, but the
        # optional one can, and one shape is easier to use than two.
        # Unqualified: this spelling is used inside the `ffi` module.
        return ('handle_list', f'Option<{lname}>',
                f'*mut whiteout_{_c_handle_name(list_elem)}List')

    opt_arr = _optional_array_return(m)
    if opt_arr is not None:
        c_elem, n = opt_arr
        rty = _C_COMP_TO_RUST.get(c_elem)
        if rty is None:
            return None
        return ('opt_array', f'Option<[{rty}; {n}]>', rty)

    opt_scalar = _optional_primitive_return(m)
    if opt_scalar is not None:
        if t.element is not None and t.element.kind == TypeKind.ENUM:
            e = ctx.enum_for(t.element.cpp_text)
            if e is None:
                return None
            return ('opt_enum', f'Option<{_enum_rust_name(e, ctx.module)}>', 'i32')
        prim = _rust_prim(t.element.cpp_text) if t.element else None
        if prim is None or prim == '()':
            return None
        return ('opt_prim', f'Option<{prim}>', prim)

    if t.kind == TypeKind.OPTIONAL:
        inner = raw
        if inner.startswith('std::optional<') and inner.endswith('>'):
            inner = inner[len('std::optional<'):-1].strip()
        if inner in ('std::string', 'std::basic_string<char>'):
            return ('string_opt', 'Option<String>', 'RawCString')
        c = ctx.class_for(inner)
        if c is not None and not _is_field_only(c, ctx.module):
            return ('handle_opt', f'Option<{ctx.rust_class(c)}>', f'*mut {ctx.handle(c)}')
        return None

    if t.kind == TypeKind.VECTOR:
        el = t.element.cpp_text if t.element else ''
        if el in ('std::string', 'std::basic_string<char>'):
            # By-value returns get an owned list: one call materialises it,
            # then reading is O(1) per element. The `_count`/`_at` pair
            # re-invokes the C++ method for every index, which is O(n^2)
            # and catastrophic for something like `listFiles()` over a
            # 135k-entry storage. Reference returns keep the cheap pair.
            if _is_owned_string_list_return(m):
                return ('string_list', 'Vec<String>', None)
            return ('string_vec', 'Vec<String>', None)
        return None

    if t.kind == TypeKind.NESTED:
        c = ctx.class_for(raw)
        if c is not None and not _is_field_only(c, ctx.module):
            if m.return_is_reference and not m.is_static:
                # A C++ reference return is an interior pointer into
                # something the callee still owns — emit_c hands back
                # `&__r`, not a heap copy. Wrapping that in an owning
                # handle would free memory we do not own, and the owner
                # would free it again. Borrow it instead, tied to `&self`.
                return ('handle_ref',
                        f"Option<crate::support::Ref<'_, {ctx.rust_class(c)}>>",
                        f'*mut {ctx.handle(c)}')
            # Otherwise the C ABI heap-allocates a copy and transfers it,
            # returning NULL when the call produced nothing.
            return ('handle_opt', f'Option<{ctx.rust_class(c)}>', f'*mut {ctx.handle(c)}')
        return None

    return None


# ── Parameter classification ──────────────────────────────────────────────

def _params(m, ctx: _Ctx):
    """[(kind, rust_decl, ffi_decls, call_expr)] or None when unsupported.

    `pool` parameters are dropped: they take an `interfaces::WorkerPool*`,
    which lands with the `host` module in a later phase. Until then the
    generated wrapper passes null, which the library treats as "run on the
    calling thread".
    """
    out = []
    params = list(m.params)

    if m.bytes_in and params:
        p0 = params.pop(0)
        out.append(('bytes_in', f'{_snake(p0.name)}: &[u8]',
                    [f'{_snake(p0.name)}: *const u8', f'{_snake(p0.name)}_size: usize'],
                    [f'{_snake(p0.name)}.as_ptr()', f'{_snake(p0.name)}.len()']))

    for p in params:
        name = _snake(p.name)
        raw = p.type.cpp_text.replace('const ', '').strip()

        # A `std::span<const u8>` anywhere but first. `m.bytes_in` only
        # covers the leading-parameter case, but e.g.
        # `m2::Parser::parse(CascFileSystem&, span<const u8>)` puts it
        # second, and the C ABI expands it to the same (ptr, len) pair.
        if _is_span_const_u8(p.type) or (p.span_scalar and p.span_scalar[0] == 'u8'):
            out.append(('bytes_in', f'{name}: &[u8]',
                        [f'{name}: *const u8', f'{name}_size: usize'],
                        [f'{name}.as_ptr()', f'{name}.len()']))
            continue

        if _is_interface_pointer_param(p):
            wrapper = _HOST_WRAPPERS.get(_c_handle_name(raw))
            if wrapper is None:
                # An interface we have no Rust trait for yet: keep the
                # method usable by passing null, which the library treats
                # as "not supplied".
                out.append(('dropped_iface', None, [f'{name}: *mut core::ffi::c_void'],
                            ['core::ptr::null_mut()']))
                continue
            out.append((
                'iface',
                f'{name}: Option<&crate::interfaces::{wrapper}>',
                [f'{name}: *mut core::ffi::c_void'],
                [f'{name}.map_or(core::ptr::null_mut(), |v| v.as_ptr())'],
            ))
            continue

        if p.type.kind == TypeKind.ENUM:
            e = ctx.enum_for(raw)
            if e is None:
                return None
            out.append(('enum', f'{name}: {_enum_rust_name(e, ctx.module)}', [f'{name}: i32'],
                        [f'{name} as i32']))
            continue

        prim = _rust_prim(raw)
        if p.type.kind == TypeKind.PRIMITIVE or prim is not None:
            if prim is None or prim == '()':
                return None
            if prim == 'bool':
                out.append(('bool', f'{name}: bool', [f'{name}: i32'],
                            [f'if {name} {{ 1 }} else {{ 0 }}']))
            else:
                out.append(('prim', f'{name}: {prim}', [f'{name}: {prim}'], [name]))
            continue

        if p.type.kind == TypeKind.STRING:
            out.append(('string', f'{name}: &str',
                        [f'{name}: *const core::ffi::c_char'],
                        [f'{name}_cstr.as_ptr()']))
            continue

        if _is_byte_vector_param(p):
            out.append(('bytes_in', f'{name}: &[u8]',
                        [f'{name}: *const u8', f'{name}_size: usize'],
                        [f'{name}.as_ptr()', f'{name}.len()']))
            continue

        scalar_vec = _scalar_vector_param_elem(p)
        if scalar_vec is not None:
            el = p.type.element
            if el.kind == TypeKind.ENUM:
                e = ctx.enum_for(el.cpp_text)
                if e is None:
                    return None
                rty = _enum_rust_name(e, ctx.module)
                # Enums are `#[repr(i32)]`, so a slice of them is already
                # laid out as the `const int32_t*` the C ABI wants.
                cargs = [f'{name}.as_ptr() as *const i32', f'{name}.len()']
            else:
                rty = _C_COMP_TO_RUST.get(scalar_vec)
                if rty is None:
                    return None
                cargs = [f'{name}.as_ptr()', f'{name}.len()']
            cty = 'i32' if el.kind == TypeKind.ENUM else rty
            out.append(('scalar_slice', f'{name}: &[{rty}]',
                        [f'{name}: *const {cty}', f'{name}_size: usize'], cargs))
            continue

        if _is_class_vector_param(p, ctx.module):
            ec = ctx.class_for(p.type.element.cpp_text)
            if ec is None or _is_field_only(ec, ctx.module):
                return None
            out.append((
                'handle_slice',
                f'{name}: &[&{ctx.rust_class(ec)}]',
                [f'{name}: *const *mut {ctx.handle(ec)}', f'{name}_size: usize'],
                [f'{name}_ptrs.as_ptr()', f'{name}.len()'],
            ))
            continue

        c = ctx.class_for(raw)
        if c is not None and _is_field_only(c, ctx.module):
            out.append((
                'value_struct',
                f'{name}: &{ctx.rust_class(c)}',
                [f'{name}: *mut {ctx.handle(c)}'],
                [f'{name}_native'],
            ))
            continue

        if c is not None:
            out.append(('handle', f'{name}: &{ctx.rust_class(c)}',
                        [f'{name}: *mut {ctx.handle(c)}'], [f'{name}.raw.as_ptr()']))
            continue

        return None

    return out


# `Ref<CHAR>` and friends: rustdoc parses the angle brackets as HTML.
# Wrapping the whole token in backticks renders it as code, which is what
# was meant anyway. Skips anything already inside backticks.
_DOC_ANGLE_RE = re.compile(r'(?<![`\w])\w+<[A-Za-z_][\w:, ]*>(?![`])')

# A bare `[0,1]` reads as an intra-doc link target.
_DOC_LINK_RE = re.compile(r'\[[0-9]+\s*,\s*[0-9]+\]')


def _doc(buf: StringIO, text: str, indent: str = '') -> None:
    """Emit C++ doc text as Rust doc comments.

    The source text is doxygen prose written for C++, so it contains things
    rustdoc reads as markup: `Ref<CHAR>` looks like an unclosed HTML tag,
    and a bare `[0,1]` looks like an intra-doc link. Both are escaped so a
    published doc build is warning-free.
    """
    if not text:
        return
    for line in text.splitlines():
        line = line.rstrip()
        line = _DOC_ANGLE_RE.sub(r'`\g<0>`', line)
        line = _DOC_LINK_RE.sub(lambda m: f'`{m.group(0)}`', line)
        if line:
            buf.write(f'{indent}/// {line}\n')
        else:
            buf.write(f'{indent}///\n')


def _emit_flags(buf: StringIO, e, ctx: _Ctx) -> None:
    name = _enum_rust_name(e, ctx.module)
    _doc(buf, e.doc)
    buf.write(f"""/// Bit flags. Combine with `|`, test with [`{name}::contains`].
#[derive(Clone, Copy, PartialEq, Eq, Hash, Default)]
pub struct {name}(pub i32);

impl {name} {{
""")
    seen = set()
    for v in e.values:
        vn = _screaming(v.js_name)
        if vn in seen:
            continue
        seen.add(vn)
        _doc(buf, v.doc, '    ')
        buf.write(f'    pub const {vn}: Self = Self({v.value});\n')
    buf.write(f"""
    #[inline]
    pub const fn contains(self, other: Self) -> bool {{
        (self.0 & other.0) == other.0
    }}

    #[inline]
    pub const fn is_empty(self) -> bool {{
        self.0 == 0
    }}
}}

impl core::ops::BitOr for {name} {{
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {{
        Self(self.0 | rhs.0)
    }}
}}

impl core::ops::BitAnd for {name} {{
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {{
        Self(self.0 & rhs.0)
    }}
}}

impl core::ops::Not for {name} {{
    type Output = Self;
    #[inline]
    fn not(self) -> Self {{
        Self(!self.0)
    }}
}}

impl core::fmt::Debug for {name} {{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {{
        write!(f, "{name}({{:#x}})", self.0)
    }}
}}

""")


def _emit_enum(buf: StringIO, e, ctx: _Ctx) -> None:
    if _is_flags_enum(e):
        _emit_flags(buf, e, ctx)
        return
    name = _enum_rust_name(e, ctx.module)
    _doc(buf, e.doc)
    buf.write('#[repr(i32)]\n')
    buf.write('#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]\n')
    buf.write(f'pub enum {name} {{\n')
    seen = set()
    for v in e.values:
        vn = _variant_name(v.js_name)
        if vn in seen:
            continue
        seen.add(vn)
        _doc(buf, v.doc, '    ')
        buf.write(f'    {vn} = {v.value},\n')
    buf.write('}\n\n')

    # Never transmute: an out-of-range discriminant from a newer native
    # library would be instant UB.
    buf.write(f'''impl TryFrom<i32> for {name} {{
    type Error = crate::Error;
    fn try_from(v: i32) -> Result<Self, crate::Error> {{
        match v {{
''')
    seen = set()
    for v in e.values:
        vn = _variant_name(v.js_name)
        if vn in seen:
            continue
        seen.add(vn)
        buf.write(f'            {v.value} => Ok({name}::{vn}),\n')
    buf.write(f'''            other => Err(crate::Error::UnknownEnum {{
                name: "{name}",
                value: other,
            }}),
        }}
    }}
}}

''')


def _emit_handle_list(buf: StringIO, elem_cpp: str, ctx: _Ctx) -> None:
    """An owned `std::vector<Class>` handed back from a single call.

    Elements are borrowed out of the list rather than copied, so the
    wrapper hands back `Ref` exactly like an in-place vector field does.
    """
    ec = ctx.class_for(elem_cpp)
    if ec is None:
        return
    rust = ctx.rust_class(ec)
    name = f'{rust}List'
    lh = _c_handle_name(elem_cpp)
    prefix = ctx.prefix
    buf.write(f"""/// An owned list of [`{rust}`] produced by the library.
pub struct {name} {{
    raw: core::ptr::NonNull<ffi::whiteout_{lh}List>,
}}

impl {name} {{
    /// # Safety
    /// `raw` must be a live list this value takes ownership of.
    #[allow(dead_code)]
    pub(crate) unsafe fn from_raw(raw: *mut ffi::whiteout_{lh}List) -> Option<Self> {{
        core::ptr::NonNull::new(raw).map(|raw| {name} {{ raw }})
    }}

    pub fn len(&self) -> usize {{
        // SAFETY: the list is live for `&self`.
        unsafe {{ ffi::{prefix}_{lh}List_size(self.raw.as_ptr()) }}
    }}

    pub fn is_empty(&self) -> bool {{
        self.len() == 0
    }}

    /// Borrow element `index`. `None` when out of range.
    pub fn get(&self, index: usize) -> Option<crate::support::Ref<'_, {rust}>> {{
        if index >= self.len() {{
            return None;
        }}
        // SAFETY: index checked; the pointer is interior to the list and
        // is never freed by the `Ref`.
        unsafe {{
            Some(crate::support::Ref::new({rust} {{
                raw: core::ptr::NonNull::new_unchecked(
                    ffi::{prefix}_{lh}List_at(self.raw.as_ptr(), index),
                ),
            }}))
        }}
    }}

    pub fn iter(&self) -> impl ExactSizeIterator<Item = crate::support::Ref<'_, {rust}>> {{
        (0..self.len()).map(move |i| self.get(i).expect("index below len"))
    }}
}}

impl Drop for {name} {{
    fn drop(&mut self) {{
        // SAFETY: the list was transferred to us and is freed once.
        unsafe {{ ffi::{prefix}_{lh}List_delete(self.raw.as_ptr()) }}
    }}
}}

impl core::fmt::Debug for {name} {{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {{
        f.debug_struct("{name}").field("len", &self.len()).finish()
    }}
}}

// SAFETY: a plain heap vector with no thread affinity, owned exclusively.
unsafe impl Send for {name} {{}}

""")


def _emit_handle_struct(buf: StringIO, c: BindClass, ctx: _Ctx) -> None:
    rust = ctx.rust_class(c)
    handle = ctx.handle(c)
    _doc(buf, c.doc)
    buf.write(f'''pub struct {rust} {{
    pub(crate) raw: core::ptr::NonNull<ffi::{handle}>,
}}

impl Drop for {rust} {{
    fn drop(&mut self) {{
        // SAFETY: `raw` came from a native constructor and Drop runs once.
        unsafe {{ ffi::{ctx.sym(c, 'delete')}(self.raw.as_ptr()) }}
    }}
}}

impl {rust} {{
    /// # Safety
    /// `raw` must be a live handle this value takes ownership of.
    #[allow(dead_code)] // used by whichever methods return this type
    pub(crate) unsafe fn from_raw(raw: *mut ffi::{handle}) -> Option<Self> {{
        core::ptr::NonNull::new(raw).map(|raw| {rust} {{ raw }})
    }}
}}

// SAFETY: handles are plain heap pointers with no thread affinity. `Sync`
// is deliberately NOT implemented — the C++ types make no documented
// guarantee about concurrent use, and claiming one we haven't verified
// would be unsound. See `@bind thread_safe` in the plan.
unsafe impl Send for {rust} {{}}

impl core::fmt::Debug for {rust} {{
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {{
        f.debug_struct("{rust}").finish_non_exhaustive()
    }}
}}

''')


def _emit_value_struct(buf: StringIO, c: BindClass, ctx: _Ctx) -> None:
    """Field-only classes become plain Rust structs used with struct-update
    syntax — the idiomatic options pattern — with a private round-trip
    through the C handle at call time."""
    rust = ctx.rust_class(c)
    handle = ctx.handle(c)
    fields = []
    for f in c.fields:
        prim = _rust_prim(f.type.cpp_text)
        if f.type.kind == TypeKind.PRIMITIVE and prim and prim != '()':
            fields.append((f, _snake(f.name), prim))
    if not fields:
        ctx.skipped.append(f'{rust} (no supported fields)')
        return

    _doc(buf, c.doc)
    buf.write('#[derive(Clone, Debug, PartialEq)]\n')
    buf.write(f'pub struct {rust} {{\n')
    for f, name, prim in fields:
        _doc(buf, f.doc, '    ')
        buf.write(f'    pub {name}: {prim},\n')
    buf.write('}\n\n')

    # Default comes from the C++ member initialisers, read once out of a
    # freshly constructed native instance — so the Rust defaults cannot
    # drift from the C++ ones.
    buf.write(f'''impl Default for {rust} {{
    fn default() -> Self {{
        // SAFETY: `_new` always returns a live handle; freed before return.
        unsafe {{
            let h = ffi::{ctx.sym(c, 'new')}();
            let out = {rust} {{
''')
    for f, name, prim in fields:
        getter = ctx.sym(c, f'get_{f.name}')
        conv = ' != 0' if prim == 'bool' else ''
        buf.write(f'                {name}: ffi::{getter}(h){conv},\n')
    buf.write(f'''            }};
            ffi::{ctx.sym(c, 'delete')}(h);
            out
        }}
    }}
}}

impl {rust} {{
    /// Build a native handle carrying these values. Caller frees it.
    #[allow(dead_code)] // consumed once the methods taking these options bind
    pub(crate) unsafe fn to_native(&self) -> *mut ffi::{handle} {{
        unsafe {{
            let h = ffi::{ctx.sym(c, 'new')}();
''')
    for f, name, prim in fields:
        setter = ctx.sym(c, f'set_{f.name}')
        val = f'if self.{name} {{ 1 }} else {{ 0 }}' if prim == 'bool' else f'self.{name}'
        buf.write(f'            ffi::{setter}(h, {val});\n')
    buf.write(f'''            h
        }}
    }}

    /// Free a handle produced by [`Self::to_native`].
    ///
    /// # Safety
    /// `h` must have come from `to_native` and not been freed already.
    #[allow(dead_code)]
    pub(crate) unsafe fn free_native(h: *mut ffi::{handle}) {{
        unsafe {{ ffi::{ctx.sym(c, 'delete')}(h) }}
    }}
}}

''')


def _wrap_return(kind: str, call: str, rust_ret: str) -> str:
    """The expression that turns a raw C return into its Rust shape."""
    if kind == 'bool':
        return f'{call} != 0'
    if kind == 'enum':
        return (f'{rust_ret}::try_from({call})'
                '.expect("unknown enum discriminant from the native library")')
    if kind == 'string':
        return f'crate::support::take_string({call})'
    if kind == 'string_opt':
        return f'crate::support::take_string_opt({call})'
    if kind == 'bytes':
        return f'Bytes::from_raw({call}).unwrap_or_else(Bytes::empty)'
    if kind == 'bytes_opt':
        return f'Bytes::from_raw({call})'
    if kind == 'handle_opt':
        inner = rust_ret[len('Option<'):-1]
        return f'{inner}::from_raw({call})'
    return call


def _emit_method(buf: StringIO, c: BindClass, m, ctx: _Ctx) -> bool:
    """Emit one safe method. Returns False when the shape isn't supported
    yet (recorded in ctx.skipped so the gap is visible, not silent)."""
    rust_cls = ctx.rust_class(c)
    shape = _return_shape(m, ctx)
    if shape is None:
        ctx.skipped.append(f'{rust_cls}::{m.name} (return {m.return_type.cpp_text})')
        return False
    kind, rust_ret, _ffi_ret = shape

    ps = _params(m, ctx)
    if ps is None:
        ctx.skipped.append(f'{rust_cls}::{m.name} (parameter shape)')
        return False

    name = _method_name(rust_cls, m)
    decls = [d for _k, d, _f, _c in ps if d is not None]
    recv = '' if m.is_static else ('&self' if m.is_const else '&mut self')
    sig_parts = ([recv] if recv else []) + decls
    sig = ', '.join(sig_parts)

    # `&self`-returning borrowed slices need an explicit lifetime tie.
    ret_sig = rust_ret
    if kind == 'borrowed_slice':
        ret_sig = "BorrowedSlice<'_>"

    _doc(buf, m.doc, '    ')
    ret_clause = '' if kind == 'void' else f' -> {ret_sig}'
    buf.write(f'    pub fn {name}({sig}){ret_clause} {{\n')

    # Per-parameter staging: temporaries the call needs but the signature
    # does not expose.
    for k, d, _f, _cargs in ps:
        if k == 'value_struct' and d:
            pname = d.split(':')[0].strip()
            # Options structs live by value in Rust; the C ABI wants a
            # handle, so build one for the call and free it afterwards.
            buf.write(f'        let {pname}_native = unsafe {{ {pname}.to_native() }};\n')
        if k == 'handle_slice' and d:
            pname = d.split(':')[0].strip()
            buf.write(f'        let {pname}_ptrs: Vec<_> = '
                      f'{pname}.iter().map(|v| v.raw.as_ptr()).collect();\n')
        if k == 'string' and d:
            pname = d.split(':')[0].strip()
            buf.write(f'        let {pname}_cstr = std::ffi::CString::new({pname})\n')
            buf.write('            .unwrap_or_default();\n')

    call_args = []
    if not m.is_static:
        call_args.append('self.raw.as_ptr()')
    for _k, _d, _f, cargs in ps:
        call_args.extend(cargs)

    if kind == 'string_list':
        call = f'ffi::{ctx.sym(c, m.name)}({", ".join(call_args)})'
        buf.write('        // SAFETY: one call materialises the list; the' + NL)
        buf.write('        // elements are borrowed out of it and it is freed' + NL)
        buf.write('        // before returning. Reading is O(1) per element.' + NL)
        buf.write('        unsafe {' + NL)
        buf.write(f'            let list = {call};' + NL)
        buf.write('            if list.is_null() {' + NL)
        buf.write('                return Vec::new();' + NL)
        buf.write('            }' + NL)
        buf.write(f'            let n = ffi::{ctx.prefix}_StringList_size(list);' + NL)
        buf.write('            let out = (0..n)' + NL)
        buf.write(f'                .map(|i| crate::support::take_string('
                  f'ffi::{ctx.prefix}_StringList_at(list, i)))' + NL)
        buf.write('                .collect();' + NL)
        buf.write(f'            ffi::{ctx.prefix}_StringList_delete(list);' + NL)
        buf.write('            out' + NL)
        buf.write('        }' + NL)
        buf.write('    }' + NL + NL)
        return True

    if kind == 'string_vec':
        cnt = f'ffi::{ctx.sym(c, m.name + "_count")}'
        at = f'ffi::{ctx.sym(c, m.name + "_at")}'
        recv_arg = 'self.raw.as_ptr()'
        buf.write(f'''        // SAFETY: index stays below the reported count.
        unsafe {{
            let n = {cnt}({recv_arg});
            (0..n)
                .map(|i| crate::support::take_string({at}({recv_arg}, i)))
                .collect()
        }}
    }}

''')
        return True

    if kind == 'borrowed_slice':
        buf.write('        let mut __size: usize = 0;\n')
        buf.write('        // SAFETY: the returned pointer borrows `self`; the\n')
        buf.write('        // lifetime on BorrowedSlice keeps it from outliving us.\n')
        buf.write('        unsafe {\n')
        buf.write(f'            let __b = ffi::{ctx.sym(c, m.name)}({", ".join(call_args)});\n')
        buf.write('            __size = __b.size;\n')
        buf.write('            BorrowedSlice::new(__b.data, __size)\n')
        buf.write('        }\n    }\n\n')
        return True

    if kind == 'handle_ref':
        call = f'ffi::{ctx.sym(c, m.name)}({", ".join(call_args)})'
        inner = rust_ret[rust_ret.index("Ref<'_, ") + len("Ref<'_, "):-2]
        buf.write('        // SAFETY: the native side returns an interior' + NL)
        buf.write('        // pointer borrowed from `self`; `Ref` derefs to it' + NL)
        buf.write('        // and never frees it.' + NL)
        buf.write('        unsafe {' + NL)
        buf.write(f'            core::ptr::NonNull::new({call})' + NL)
        buf.write(f'                .map(|raw| crate::support::Ref::new({inner} {{ raw }}))' + NL)
        buf.write('        }' + NL)
        buf.write('    }' + NL + NL)
        return True

    if kind == 'handle_list':
        call = f'ffi::{ctx.sym(c, m.name)}({", ".join(call_args)})'
        inner = rust_ret[len('Option<'):-1]
        buf.write('        // SAFETY: the native side transfers ownership of' + NL)
        buf.write('        // the list; null means the operation produced none.' + NL)
        buf.write(f'        unsafe {{ {inner}::from_raw({call}) }}' + NL)
        buf.write('    }' + NL + NL)
        return True

    if kind == 'opt_array':
        inner = rust_ret[len('Option<'):-1]        # e.g. `[u8; 16]`
        args = ', '.join(call_args + ['__v.as_mut_ptr()'])
        buf.write(f'        let mut __v: {inner} = Default::default();' + NL)
        buf.write('        // SAFETY: `__v` is a live local of exactly the' + NL)
        buf.write('        // length the native side writes.' + NL)
        buf.write(f'        let __has = unsafe {{ ffi::{ctx.sym(c, m.name)}({args}) }};' + NL)
        buf.write('        (__has != 0).then_some(__v)' + NL)
        buf.write('    }' + NL + NL)
        return True

    if kind in ('opt_prim', 'opt_enum'):
        inner = rust_ret[len('Option<'):-1]
        tmp_ty = 'i32' if kind == 'opt_enum' else inner
        args = ', '.join(call_args + ['&mut __v'])
        buf.write(f'        let mut __v: {tmp_ty} = 0;\n')
        buf.write('        // SAFETY: `__v` is a live local, written by the\n')
        buf.write('        // native side only when it returns 1.\n')
        buf.write(f'        let __has = unsafe {{ ffi::{ctx.sym(c, m.name)}({args}) }};\n')
        if kind == 'opt_enum':
            buf.write('        if __has == 0 {\n')
            buf.write('            None\n')
            buf.write('        } else {\n')
            buf.write(f'            Some({inner}::try_from(__v).expect(\n')
            buf.write('                "unknown enum discriminant from the native library",\n')
            buf.write('            ))\n')
            buf.write('        }\n')
        else:
            buf.write('        (__has != 0).then_some(__v)\n')
        buf.write('    }\n\n')
        return True

    call = f'ffi::{ctx.sym(c, m.name)}({", ".join(call_args)})'

    # Temporary option handles built during staging have to be released
    # once the call returns, whatever the return shape.
    owned_opts = [
        (d.split(':')[0].strip(), d.split('&')[-1].strip())
        for k, d, _f, _c in ps
        if k == 'value_struct' and d
    ]
    if owned_opts:
        buf.write('        // SAFETY: handle is live for the call; the staged\n')
        buf.write('        // option handles are freed immediately after.\n')
        buf.write('        unsafe {\n')
        if kind == 'void':
            buf.write(f'            {call};\n')
        else:
            buf.write(f'            let __r = {_wrap_return(kind, call, rust_ret)};\n')
        for pname, ptype in owned_opts:
            buf.write(f'            {ptype}::free_native({pname}_native);\n')
        if kind != 'void':
            buf.write('            __r\n')
        buf.write('        }\n    }\n\n')
        return True

    buf.write('        // SAFETY: handle is live for the duration of the call.\n')
    buf.write('        unsafe {\n')
    if kind == 'void':
        buf.write(f'            {call};\n')
    elif kind == 'bool':
        buf.write(f'            {call} != 0\n')
    elif kind == 'prim':
        buf.write(f'            {call}\n')
    elif kind == 'enum':
        # Never fall back to an arbitrary variant: an unrecognised
        # discriminant means the linked library is newer than this crate,
        # and silently reporting the wrong format would be worse than
        # failing. `try_from` stays available for callers who want to
        # handle skew themselves.
        buf.write(f'            {rust_ret}::try_from({call})\n')
        buf.write('                .expect("unknown enum discriminant from the '
                  'native library (ABI version skew)")\n')
    elif kind == 'string':
        buf.write(f'            crate::support::take_string({call})\n')
    elif kind == 'string_opt':
        buf.write(f'            crate::support::take_string_opt({call})\n')
    elif kind == 'bytes':
        buf.write(f'            Bytes::from_raw({call}).unwrap_or_else(Bytes::empty)\n')
    elif kind == 'bytes_opt':
        buf.write(f'            Bytes::from_raw({call})\n')
    elif kind == 'handle_opt':
        inner = rust_ret[len('Option<'):-1]
        buf.write(f'            {inner}::from_raw({call})\n')
    buf.write('        }\n    }\n\n')
    return True


def _ctor_names(c: BindClass, module: BindModule):
    """(c_symbol_suffix, ctor) pairs, mirroring `_emit_class_header`:
    a `_new(void)` unless the class is marked `no_default_ctor`, plus one
    `_new_<params>` per representable overload."""
    out = []
    if not c.no_default_ctor:
        out.append(('new', None))
    for ctor, sig_name, _cparams in _ctor_overloads_named(c, module):
        out.append((f'new_{sig_name}', ctor))
    return out


def _emit_ctors(buf: StringIO, c: BindClass, ctx: _Ctx) -> None:
    """Only the argument-less constructor for now. The overloads all take a
    WorkerPool, which lands with the `host` module."""
    if c.no_default_ctor:
        return
    rust = ctx.rust_class(c)
    buf.write(f"""    /// # Panics
    /// Panics if the native allocation fails.
    pub fn new() -> Self {{
        // SAFETY: the native constructor returns a live handle; a null here
        // means the library is unusable.
        unsafe {{
            let raw = ffi::{ctx.sym(c, 'new')}();
            Self::from_raw(raw).expect("native {rust} allocation failed")
        }}
    }}

""")


def _emit_module_ffi(buf: StringIO, ctx: _Ctx) -> None:
    """The extern block. Symbol names come from emit_c's own helpers, so
    this cannot drift from the generated header."""
    module = ctx.module
    # Opaque FFI structs are internal plumbing behind `#[doc(hidden)]`;
    # the crate-level Debug lint isn't meaningful for them.
    buf.write('#![allow(missing_debug_implementations)]\n\n')
    # Which of these a module needs depends on its shapes.
    buf.write('#[allow(unused_imports)]\n')
    buf.write('use crate::support::{RawBytes, RawCString};\n\n')
    for elem in _handle_list_types(module):
        lh = _c_handle_name(elem)
        buf.write(f'#[repr(C)]\npub struct whiteout_{lh}List {{\n')
        buf.write('    _private: [u8; 0],\n}\n')
    for c in _module_classes(module):
        buf.write(f'#[repr(C)]\npub struct {ctx.handle(c)} {{\n')
        buf.write('    _private: [u8; 0],\n}\n')
    if _module_has_string_list(ctx.module):
        buf.write('#[repr(C)]\npub struct whiteout_StringList {\n')
        buf.write('    _private: [u8; 0],\n}\n')

    buf.write('\nextern "C" {\n')
    if _module_has_string_list(ctx.module):
        buf.write(f'    pub fn {ctx.prefix}_StringList_size'
                  f'(self_: *mut whiteout_StringList) -> usize;' + NL)
        buf.write(f'    pub fn {ctx.prefix}_StringList_at'
                  f'(self_: *mut whiteout_StringList, index: usize) -> RawCString;' + NL)
        buf.write(f'    pub fn {ctx.prefix}_StringList_delete'
                  f'(self_: *mut whiteout_StringList);' + NL)
    for elem in _handle_list_types(module):
        lh = _c_handle_name(elem)
        ec = ctx.class_for(elem)
        if ec is None:
            continue
        buf.write(f'    pub fn {ctx.prefix}_{lh}List_size'
                  f'(self_: *mut whiteout_{lh}List) -> usize;' + NL)
        buf.write(f'    pub fn {ctx.prefix}_{lh}List_at'
                  f'(self_: *mut whiteout_{lh}List, index: usize)'
                  f' -> *mut {ctx.handle(ec)};' + NL)
        buf.write(f'    pub fn {ctx.prefix}_{lh}List_delete'
                  f'(self_: *mut whiteout_{lh}List);' + NL)

    for c in _module_classes(module):
        handle = ctx.handle(c)
        buf.write(f'    // {ctx.rust_class(c)}\n')
        for cname, ctor in _ctor_names(c, module):
            n = len(ctor.params) if ctor is not None else 0
            params = ', '.join(f'_{i}: *mut core::ffi::c_void' for i in range(n))
            buf.write(f'    pub fn {ctx.sym(c, cname)}({params}) -> *mut {handle};\n')
        buf.write(f'    pub fn {ctx.sym(c, "delete")}(self_: *mut {handle});\n')

        known = _known_class_short_names(module)
        if _is_field_only(c, ctx.module):
            # Value structs round-trip through the handle, so they need the
            # plain scalar accessors only.
            for f in c.fields:
                prim = _rust_prim(f.type.cpp_text)
                if f.type.kind == TypeKind.PRIMITIVE and prim and prim != '()':
                    cprim = 'i32' if prim == 'bool' else prim
                    buf.write(f'    pub fn {ctx.sym(c, "get_" + f.name)}'
                              f'(self_: *mut {handle}) -> {cprim};\n')
                    buf.write(f'    pub fn {ctx.sym(c, "set_" + f.name)}'
                              f'(self_: *mut {handle}, value: {cprim});\n')
        else:
            for f in c.fields:
                if _is_field_supported(f, module, known):
                    _emit_field_ffi(buf, c, f, ctx)

        for m in c.methods:
            if m.is_skipped:
                continue
            if c.is_subclassable and _is_host_implemented(m):
                continue
            shape = _return_shape(m, ctx)
            ps = _params(m, ctx)
            if shape is None or ps is None:
                continue
            kind, _rust_ret, ffi_ret = shape
            if kind == 'string_list':
                buf.write(f'    pub fn {ctx.sym(c, m.name)}'
                          f'(self_: *mut {handle}) -> *mut whiteout_StringList;' + NL)
                continue
            if kind == 'string_vec':
                buf.write(f'    pub fn {ctx.sym(c, m.name + "_count")}'
                          f'(self_: *mut {handle}) -> usize;\n')
                buf.write(f'    pub fn {ctx.sym(c, m.name + "_at")}'
                          f'(self_: *mut {handle}, index: usize) -> RawCString;\n')
                continue
            decls = []
            if not m.is_static:
                decls.append(f'self_: *mut {handle}')
            for _k, _d, fdecls, _cargs in ps:
                decls.extend(fdecls)
            if kind in ('handle_list', 'handle_ref'):
                buf.write(f'    pub fn {ctx.sym(c, m.name)}'
                          f'({", ".join(decls)}) -> {ffi_ret};' + NL)
                continue
            if kind == 'opt_array':
                decls.append(f'out_value: *mut {ffi_ret}')
                buf.write(f'    pub fn {ctx.sym(c, m.name)}({", ".join(decls)}) -> i32;' + NL)
                continue
            if kind in ('opt_prim', 'opt_enum'):
                # Lowered by emit_c to a has-value flag plus an out-param.
                decls.append(f'out_value: *mut {ffi_ret}')
                buf.write(f'    pub fn {ctx.sym(c, m.name)}({", ".join(decls)}) -> i32;\n')
                continue
            ret = '' if ffi_ret == '()' else f' -> {ffi_ret}'
            buf.write(f'    pub fn {ctx.sym(c, m.name)}({", ".join(decls)}){ret};\n')
    buf.write('}\n')


def emit_module(module: BindModule) -> str:
    ctx = _Ctx(module)
    buf = StringIO()
    buf.write(f'''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
// AUTOGENERATED by tools/codegen/emit_rust.py — do not edit.
// Regenerate via:  python -m tools.codegen.codegen {module.name} --backend rust

#![allow(clippy::too_many_arguments)]

// Which of these a module needs depends on its shapes; the modules that
// have no span accessors would otherwise trip the unused-import lint.
#[allow(unused_imports)]
use crate::support::{{BorrowedSlice, Bytes}};

''')

    for e in module.enums:
        _emit_enum(buf, e, ctx)

    for elem in _handle_list_types(module):
        _emit_handle_list(buf, elem, ctx)

    for c in _module_classes(module):
        if _is_field_only(c, ctx.module):
            _emit_value_struct(buf, c, ctx)
            continue
        _emit_handle_struct(buf, c, ctx)
        body = StringIO()
        _emit_ctors(body, c, ctx)
        known = _known_class_short_names(module)
        for f in c.fields:
            if _is_field_supported(f, module, known):
                _emit_field(body, c, f, ctx)
        for m in c.methods:
            if m.is_skipped:
                continue   # `@bind skip` — deliberate, not an emitter gap
            if c.is_subclassable and _is_host_implemented(m):
                # Implemented by hand in `interfaces.rs`: these take a
                # std::function and cross via the fn-table shims, not the
                # handle ABI.
                continue
            _emit_method(body, c, m, ctx)
        text = body.getvalue()
        if text.strip():
            buf.write(f'impl {ctx.rust_class(c)} {{\n{text}}}\n\n')
        _emit_tier_a(buf, c, ctx)
        if any(n == 'new' for n, _ in _ctor_names(c, module)):
            buf.write(f'''impl Default for {ctx.rust_class(c)} {{
    fn default() -> Self {{
        Self::new()
    }}
}}

''')

    if ctx.skipped:
        buf.write('// Not yet bound (shape unsupported by the emitter):\n')
        for s in sorted(set(ctx.skipped)):
            buf.write(f'//   - {s}\n')
        buf.write('\n')

    _emit_tier_a_ffi(buf, ctx)

    buf.write('#[doc(hidden)]\npub mod ffi {\n')
    inner = StringIO()
    _emit_module_ffi(inner, ctx)
    for line in inner.getvalue().splitlines():
        buf.write(f'    {line}\n' if line else '\n')
    buf.write('}\n')
    return _rustfmt(buf.getvalue())


# ── Tier A: zero-copy mutable element access ──────────────────────────────

def _emit_tier_a(buf: StringIO, c: BindClass, ctx: _Ctx) -> None:
    """Bind the mutable span accessors from the value ABI.

    These are the write half of Tier A. The const half arrives through the
    normal IR path as `BorrowedSlice`; the mutable half has no IR entry
    because `emit_c.py` never emitted it (C# and Java cannot express a
    borrowed mutable view).

    Safety rests entirely on the borrow: `&mut self` means the compiler
    rejects a resize, a second accessor, or a drop of the owner while the
    slice is alive.
    """
    from .emit_rust_abi import _TIER_A_MUT, _tier_a_sym

    rust = ctx.rust_class(c)
    for _inc, handle, _cpp, methods in _TIER_A_MUT:
        if handle != _c_handle_name(c.js_name):
            continue
        buf.write(f'impl {rust} {{\n')
        for method, extra in methods:
            name = f'{_snake(method)}_mut'
            args = ''.join(f', {_snake(n)}: u32' for _t, n in extra)
            pass_args = ''.join(f'{_snake(n)}, ' for _t, n in extra)
            buf.write(f'''    /// Mutable, zero-copy view of the underlying buffer.
    ///
    /// Writes land directly in the C++ allocation — nothing is marshalled.
    /// The borrow of `self` is what makes that safe: the buffer cannot be
    /// resized or freed while this slice exists.
    pub fn {name}(&mut self{args}) -> &mut [u8] {{
        let mut size: usize = 0;
        // SAFETY: the pointer borrows `self` mutably for the returned
        // lifetime, so no aliasing or reallocation can occur meanwhile.
        unsafe {{
            let p = tier_a::{_tier_a_sym(handle, method)}(
                self.raw.as_ptr().cast(),
                {pass_args}&mut size,
            );
            if p.is_null() || size == 0 {{
                &mut []
            }} else {{
                core::slice::from_raw_parts_mut(p, size)
            }}
        }}
    }}

''')
        buf.write('}\n\n')


def _emit_tier_a_ffi(buf: StringIO, ctx: _Ctx) -> None:
    from .emit_rust_abi import _TIER_A_MUT, _tier_a_sym

    names = {_c_handle_name(c.js_name) for c in ctx.module.classes}
    entries = [e for e in _TIER_A_MUT if e[1] in names]
    if not entries:
        return
    buf.write('''
/// Value-ABI mutable span accessors (`bindings/c/whiteout_v.h`).
#[doc(hidden)]
pub mod tier_a {
    #![allow(missing_debug_implementations)]

    #[repr(C)]
    pub struct Opaque {
        _private: [u8; 0],
    }

    extern "C" {
''')
    for _inc, handle, _cpp, methods in entries:
        for method, extra in methods:
            args = ''.join(f'{_snake(n)}: u32, ' for _t, n in extra)
            buf.write(f'        pub fn {_tier_a_sym(handle, method)}('
                      f'self_: *mut Opaque, {args}out_size: *mut usize) -> *mut u8;\n')
    buf.write('    }\n}\n\n')


# ══════════════════════════════════════════════════════════════════════════
# Phase 3: fields — the data plane.
#
# 92% of the model surface is fields, not methods. The C ABI's field
# accessors hand out *interior pointers* (`&self->field`, `&self->vec[i]`),
# which is why they map to `Ref`/`RefMut` rather than to owning handles.
# POD vectors already come with `_data`/`_assign`, which is Tier A.
# ══════════════════════════════════════════════════════════════════════════

from .emit_c import (  # noqa: E402
    _bulk_component_c_type,
    _bulk_components,
    _is_bulk_flat_inner,
    _is_field_supported,
    _known_class_short_names,
    _resolve_handle_short,
)

# Math types cross by value: they are `#[repr(C)]` + `Copy` on both sides,
# so a field getter reads through the interior pointer with no wrapper and
# no allocation.
_MATH_BY_VALUE = set(_SHARED_MATH_TYPES)

_C_COMP_TO_RUST = {
    'float': 'f32', 'double': 'f64',
    'uint8_t': 'u8', 'int8_t': 'i8',
    'uint16_t': 'u16', 'int16_t': 'i16',
    'uint32_t': 'u32', 'int32_t': 'i32',
    'uint64_t': 'u64', 'int64_t': 'i64',
}


def _field_name(f) -> str:
    out = _snake(f.name)
    return f'{out}_' if out in _KEYWORDS else out


def _elem_rust(t: TypeRef, ctx: _Ctx) -> str | None:
    """Rust type for a vector/array element."""
    prim = _rust_prim(t.cpp_text)
    if t.kind == TypeKind.PRIMITIVE and prim and prim != '()':
        return prim
    if t.kind == TypeKind.ENUM:
        e = ctx.enum_for(t.cpp_text)
        return _enum_rust_name(e, ctx.module) if e else None
    if t.kind == TypeKind.NESTED:
        short = _resolve_handle_short(t.cpp_text, ctx.module)
        if short in _MATH_BY_VALUE:
            return f'crate::math::{short}'
        c = ctx.classes.get(short)
        return ctx.rust_class(c) if c is not None else None
    return None


def _bulk_slice_rust(inner: TypeRef, ctx: _Ctx) -> str | None:
    """Rust element type for a Tier A (`_data`/`_assign`) vector."""
    short = _resolve_handle_short(inner.cpp_text, ctx.module)
    if short in _MATH_BY_VALUE:
        return f'crate::math::{short}'
    return _C_COMP_TO_RUST.get(_bulk_component_c_type(inner))


def _emit_field(buf: StringIO, c: BindClass, f, ctx: _Ctx) -> bool:
    """Accessors for one field. False when the shape isn't handled yet."""
    rust_cls = ctx.rust_class(c)
    name = _field_name(f)
    t = f.type

    def sym(suffix: str) -> str:
        return f'ffi::{ctx.sym(c, suffix)}'

    # Buffer everything: an unsupported shape must not leave a doc comment
    # stranded above the next item, which is a hard compile error.
    real, buf = buf, StringIO()
    _doc(buf, f.doc, '    ')

    def flush(ok: bool) -> bool:
        if ok:
            real.write(buf.getvalue())
        return ok

    if t.kind == TypeKind.PRIMITIVE:
        prim = _rust_prim(t.cpp_text)
        if not prim or prim == '()':
            ctx.skipped.append(f'{rust_cls}.{name} (primitive {t.cpp_text})')
            return flush(False)
        if prim == 'bool':
            conv_get, conv_set = ' != 0', 'if value { 1 } else { 0 }'
        else:
            conv_get, conv_set = '', 'value'
        buf.write(
            f'    pub fn {name}(&self) -> {prim} {{\n'
            f'        // SAFETY: plain scalar read through a live handle.\n'
            f'        unsafe {{ {sym("get_" + f.name)}(self.raw.as_ptr()){conv_get} }}\n'
            f'    }}\n\n'
            f'    pub fn set_{name}(&mut self, value: {prim}) {{\n'
            f'        // SAFETY: plain scalar write through a live handle.\n'
            f'        unsafe {{ {sym("set_" + f.name)}(self.raw.as_ptr(), {conv_set}) }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    if t.kind == TypeKind.ENUM:
        e = ctx.enum_for(t.cpp_text)
        if e is None:
            ctx.skipped.append(f'{rust_cls}.{name} (unbound enum {t.cpp_text})')
            return flush(False)
        en = _enum_rust_name(e, ctx.module)
        if _is_flags_enum(e):
            buf.write(
                f'    pub fn {name}(&self) -> {en} {{\n'
                f'        // SAFETY: scalar read; a flag set accepts any bits.\n'
                f'        {en}(unsafe {{ {sym("get_" + f.name)}(self.raw.as_ptr()) }})\n'
                f'    }}\n\n'
                f'    pub fn set_{name}(&mut self, value: {en}) {{\n'
                f'        // SAFETY: scalar write through a live handle.\n'
                f'        unsafe {{ {sym("set_" + f.name)}(self.raw.as_ptr(), value.0) }}\n'
                f'    }}\n\n'
            )
            return flush(True)
        buf.write(
            f'    pub fn {name}(&self) -> {en} {{\n'
            f'        // SAFETY: scalar read; the discriminant is validated below.\n'
            f'        unsafe {{ {sym("get_" + f.name)}(self.raw.as_ptr()) }}\n'
            f'            .try_into()\n'
            f'            .expect("unknown enum discriminant from the native library")\n'
            f'    }}\n\n'
            f'    pub fn set_{name}(&mut self, value: {en}) {{\n'
            f'        // SAFETY: scalar write through a live handle.\n'
            f'        unsafe {{ {sym("set_" + f.name)}(self.raw.as_ptr(), value as i32) }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    if t.kind == TypeKind.STRING:
        buf.write(
            f'    pub fn {name}(&self) -> String {{\n'
            f'        // SAFETY: the native side hands over an owned CString.\n'
            f'        unsafe {{ crate::support::take_string({sym("get_" + f.name)}(self.raw.as_ptr())) }}\n'
            f'    }}\n\n'
            f'    pub fn set_{name}(&mut self, value: &str) {{\n'
            f'        let value = std::ffi::CString::new(value).unwrap_or_default();\n'
            f'        // SAFETY: the pointer outlives the call.\n'
            f'        unsafe {{ {sym("set_" + f.name)}(self.raw.as_ptr(), value.as_ptr()) }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    if t.kind == TypeKind.NESTED:
        short = _resolve_handle_short(t.cpp_text, ctx.module)
        if short in _MATH_BY_VALUE:
            # Both sides are `#[repr(C)]` + `Copy`, so this is a load/store
            # through the interior pointer, not a marshalling step.
            mt = f'crate::math::{short}'
            buf.write(
                f'    pub fn {name}(&self) -> {mt} {{\n'
                f'        // SAFETY: the getter returns an interior pointer to a\n'
                f'        // layout-identical POD; we copy it out immediately.\n'
                f'        unsafe {{ *({sym("get_" + f.name)}(self.raw.as_ptr()) as *const {mt}) }}\n'
                f'    }}\n\n'
                f'    pub fn set_{name}(&mut self, value: {mt}) {{\n'
                f'        // SAFETY: as above, in the other direction.\n'
                f'        unsafe {{\n'
                f'            {sym("set_" + f.name)}(self.raw.as_ptr(), &value as *const {mt} as *const _)\n'
                f'        }}\n'
                f'    }}\n\n'
            )
            return flush(True)
        inner = ctx.classes.get(short)
        if inner is None or _is_field_only(inner, ctx.module):
            ctx.skipped.append(f'{rust_cls}.{name} (nested {short})')
            return flush(False)
        ir = ctx.rust_class(inner)
        buf.write(
            f'    /// Borrows the field in place — no copy, no allocation.\n'
            f'    pub fn {name}(&self) -> crate::support::Ref<\'_, {ir}> {{\n'
            f'        // SAFETY: an interior pointer into `self`, valid for this\n'
            f'        // borrow and never freed by the `Ref`.\n'
            f'        unsafe {{\n'
            f'            crate::support::Ref::new({ir} {{\n'
            f'                raw: core::ptr::NonNull::new_unchecked({sym("get_" + f.name)}(self.raw.as_ptr())),\n'
            f'            }})\n'
            f'        }}\n'
            f'    }}\n\n'
            f'    pub fn {name}_mut(&mut self) -> crate::support::RefMut<\'_, {ir}> {{\n'
            f'        // SAFETY: as above; `&mut self` guarantees exclusivity.\n'
            f'        unsafe {{\n'
            f'            crate::support::RefMut::new({ir} {{\n'
            f'                raw: core::ptr::NonNull::new_unchecked({sym("get_" + f.name)}(self.raw.as_ptr())),\n'
            f'            }})\n'
            f'        }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    if t.kind == TypeKind.VECTOR:
        inner = t.element
        if _is_bulk_flat_inner(inner):
            el = _bulk_slice_rust(inner, ctx)
            comp = _C_COMP_TO_RUST.get(_bulk_component_c_type(inner))
            # `vector<Vector3f>` arrives as `*const f32` and needs a cast;
            # `vector<u32>` already has the right type, where casting would
            # be redundant (clippy::unnecessary_cast).
            cast_c = '' if comp == el else f' as *const {el}'
            cast_m = f' as *mut {el}' if comp == el else f' as *const {el} as *mut {el}'
            if el is None:
                ctx.skipped.append(f'{rust_cls}.{name} (bulk element {inner.cpp_text})')
                return flush(False)
            buf.write(
                f'    /// Zero-copy view of the underlying `std::vector`.\n'
                f'    pub fn {name}(&self) -> &[{el}] {{\n'
                f'        // SAFETY: `_data`/`_count` describe one contiguous C++\n'
                f'        // allocation, borrowed for as long as `self` is.\n'
                f'        unsafe {{\n'
                f'            let n = {sym("get_" + f.name + "_count")}(self.raw.as_ptr());\n'
                f'            let p = {sym("get_" + f.name + "_data")}(self.raw.as_ptr()){cast_c};\n'
                f'            if p.is_null() || n == 0 {{\n'
                f'                &[]\n'
                f'            }} else {{\n'
                f'                core::slice::from_raw_parts(p, n)\n'
                f'            }}\n'
                f'        }}\n'
                f'    }}\n\n'
                f'    /// Zero-copy mutable view. Resize first — the borrow forbids it after.\n'
                f'    pub fn {name}_mut(&mut self) -> &mut [{el}] {{\n'
                f'        // SAFETY: as above; `&mut self` rules out aliasing and resizing.\n'
                f'        unsafe {{\n'
                f'            let n = {sym("get_" + f.name + "_count")}(self.raw.as_ptr());\n'
                f'            let p = {sym("get_" + f.name + "_data")}(self.raw.as_ptr()){cast_m};\n'
                f'            if p.is_null() || n == 0 {{\n'
                f'                &mut []\n'
                f'            }} else {{\n'
                f'                core::slice::from_raw_parts_mut(p, n)\n'
                f'            }}\n'
                f'        }}\n'
                f'    }}\n\n'
                f'    pub fn set_{name}(&mut self, values: &[{el}]) {{\n'
                f'        // SAFETY: the native side copies `values` before returning.\n'
                f'        unsafe {{\n'
                f'            {sym("assign_" + f.name)}(self.raw.as_ptr(), values.as_ptr() as *const _, values.len())\n'
                f'        }}\n'
                f'    }}\n\n'
                f'    pub fn resize_{name}(&mut self, count: usize) {{\n'
                f'        // SAFETY: reallocation is safe here precisely because\n'
                f'        // `&mut self` means no slice borrow is outstanding.\n'
                f'        unsafe {{ {sym("resize_" + f.name)}(self.raw.as_ptr(), count) }}\n'
                f'    }}\n\n'
            )
            return flush(True)

        if inner.kind == TypeKind.NESTED:
            short = _resolve_handle_short(inner.cpp_text, ctx.module)
            ic = ctx.classes.get(short)
            if ic is None or _is_field_only(ic, ctx.module):
                ctx.skipped.append(f'{rust_cls}.{name} (vector of {short})')
                return flush(False)
            ir = ctx.rust_class(ic)
            buf.write(
                f'    pub fn {name}_len(&self) -> usize {{\n'
                f'        // SAFETY: scalar read through a live handle.\n'
                f'        unsafe {{ {sym("get_" + f.name + "_count")}(self.raw.as_ptr()) }}\n'
                f'    }}\n\n'
                f'    /// Borrows element `index` in place. `None` when out of range.\n'
                f'    pub fn {name}(&self, index: usize) -> Option<crate::support::Ref<\'_, {ir}>> {{\n'
                f'        if index >= self.{name}_len() {{\n'
                f'            return None;\n'
                f'        }}\n'
                f'        // SAFETY: index checked above; the pointer is interior to `self`.\n'
                f'        unsafe {{\n'
                f'            Some(crate::support::Ref::new({ir} {{\n'
                f'                raw: core::ptr::NonNull::new_unchecked({sym("get_" + f.name + "_at")}(self.raw.as_ptr(), index)),\n'
                f'            }}))\n'
                f'        }}\n'
                f'    }}\n\n'
                f'    pub fn {name}_mut(&mut self, index: usize) -> Option<crate::support::RefMut<\'_, {ir}>> {{\n'
                f'        if index >= self.{name}_len() {{\n'
                f'            return None;\n'
                f'        }}\n'
                f'        // SAFETY: as above; `&mut self` guarantees exclusivity.\n'
                f'        unsafe {{\n'
                f'            Some(crate::support::RefMut::new({ir} {{\n'
                f'                raw: core::ptr::NonNull::new_unchecked({sym("get_" + f.name + "_at")}(self.raw.as_ptr(), index)),\n'
                f'            }}))\n'
                f'        }}\n'
                f'    }}\n\n'
                f'    /// Iterate the elements, borrowing each in turn.\n'
                f'    pub fn {name}_iter(&self) -> impl ExactSizeIterator<Item = crate::support::Ref<\'_, {ir}>> {{\n'
                f'        (0..self.{name}_len()).map(move |i| self.{name}(i).expect("index below len"))\n'
                f'    }}\n\n'
                f'    pub fn resize_{name}(&mut self, count: usize) {{\n'
                f'        // SAFETY: exclusive access, so no borrow is outstanding.\n'
                f'        unsafe {{ {sym("resize_" + f.name)}(self.raw.as_ptr(), count) }}\n'
                f'    }}\n\n'
            )
            return flush(True)

        ctx.skipped.append(f'{rust_cls}.{name} (vector of {inner.cpp_text})')
        return flush(False)

    if t.kind == TypeKind.NESTED_VEC:
        inner = t.element.element
        if not _is_bulk_flat_inner(inner) and inner.kind == TypeKind.NESTED:
            # vector<vector<Class>>: the elements own storage, so they are
            # borrowed one at a time rather than handed back as a slice.
            short = _resolve_handle_short(inner.cpp_text, ctx.module)
            ic = ctx.classes.get(short)
            if ic is None or _is_field_only(ic, ctx.module):
                ctx.skipped.append(f'{rust_cls}.{name} (nested_vec of {short})')
                return flush(False)
            ir = ctx.rust_class(ic)
            buf.write(
                f"    /// Number of inner sequences.{NL}"
                f"    pub fn {name}_len(&self) -> usize {{{NL}"
                f"        // SAFETY: scalar read through a live handle.{NL}"
                f'        unsafe {{ {sym("get_" + f.name + "_count")}(self.raw.as_ptr()) }}{NL}'
                f"    }}{NL}{NL}"
                f"    /// Length of inner sequence `outer`.{NL}"
                f"    pub fn {name}_inner_len(&self, outer: usize) -> usize {{{NL}"
                f"        if outer >= self.{name}_len() {{{NL}"
                f"            return 0;{NL}"
                f"        }}{NL}"
                f"        // SAFETY: index checked above.{NL}"
                f'        unsafe {{ {sym("get_" + f.name + "_inner_count")}(self.raw.as_ptr(), outer) }}{NL}'
                f"    }}{NL}{NL}"
                f"    /// Borrow element `inner` of sequence `outer` in place.{NL}"
                f"    pub fn {name}(&self, outer: usize, inner: usize)"
                f" -> Option<crate::support::Ref<'_, {ir}>> {{{NL}"
                f"        if inner >= self.{name}_inner_len(outer) {{{NL}"
                f"            return None;{NL}"
                f"        }}{NL}"
                f"        // SAFETY: both indices checked; the pointer is{NL}"
                f"        // interior to `self`.{NL}"
                f"        unsafe {{{NL}"
                f"            Some(crate::support::Ref::new({ir} {{{NL}"
                f"                raw: core::ptr::NonNull::new_unchecked({NL}"
                f'                    {sym("get_" + f.name + "_at")}(self.raw.as_ptr(), outer, inner),{NL}'
                f"                ),{NL}"
                f"            }})){NL}"
                f"        }}{NL}"
                f"    }}{NL}{NL}"
                f"    pub fn {name}_mut(&mut self, outer: usize, inner: usize)"
                f" -> Option<crate::support::RefMut<'_, {ir}>> {{{NL}"
                f"        if inner >= self.{name}_inner_len(outer) {{{NL}"
                f"            return None;{NL}"
                f"        }}{NL}"
                f"        // SAFETY: as above; `&mut self` guarantees exclusivity.{NL}"
                f"        unsafe {{{NL}"
                f"            Some(crate::support::RefMut::new({ir} {{{NL}"
                f"                raw: core::ptr::NonNull::new_unchecked({NL}"
                f'                    {sym("get_" + f.name + "_at")}(self.raw.as_ptr(), outer, inner),{NL}'
                f"                ),{NL}"
                f"            }})){NL}"
                f"        }}{NL}"
                f"    }}{NL}{NL}"
                f"    /// Resize the outer sequence. Do this before taking a borrow.{NL}"
                f"    pub fn resize_{name}(&mut self, count: usize) {{{NL}"
                f"        // SAFETY: exclusive access, so no borrow is outstanding.{NL}"
                f'        unsafe {{ {sym("resize_" + f.name)}(self.raw.as_ptr(), count) }}{NL}'
                f"    }}{NL}{NL}"
                f"    pub fn resize_{name}_inner(&mut self, outer: usize, count: usize) {{{NL}"
                f"        // SAFETY: as above.{NL}"
                f'        unsafe {{ {sym("resize_" + f.name + "_inner")}(self.raw.as_ptr(), outer, count) }}{NL}'
                f"    }}{NL}{NL}"
            )
            return flush(True)
        if not _is_bulk_flat_inner(inner):
            ctx.skipped.append(f'{rust_cls}.{name} (nested_vec of {inner.cpp_text})')
            return flush(False)
        el = _bulk_slice_rust(inner, ctx)
        if el is None:
            ctx.skipped.append(f'{rust_cls}.{name} (nested_vec element {inner.cpp_text})')
            return flush(False)
        comp = _C_COMP_TO_RUST.get(_bulk_component_c_type(inner))
        cast_c = '' if comp == el else f' as *const {el}'
        cast_m = f' as *mut {el}' if comp == el else f' as *const {el} as *mut {el}'
        buf.write(
            f'    /// Number of inner sequences.\n'
            f'    pub fn {name}_len(&self) -> usize {{\n'
            f'        // SAFETY: scalar read through a live handle.\n'
            f'        unsafe {{ {sym("get_" + f.name + "_count")}(self.raw.as_ptr()) }}\n'
            f'    }}\n\n'
            f'    /// Zero-copy view of inner sequence `outer`. Empty when out of range.\n'
            f'    ///\n'
            f'    /// Each inner vector is contiguous on its own, but the outer one\n'
            f'    /// is a vector of vectors — so this borrows per sequence rather\n'
            f'    /// than handing back one flat slice.\n'
            f'    pub fn {name}(&self, outer: usize) -> &[{el}] {{\n'
            f'        if outer >= self.{name}_len() {{\n'
            f'            return &[];\n'
            f'        }}\n'
            f'        // SAFETY: index checked; the pointer borrows `self`.\n'
            f'        unsafe {{\n'
            f'            let n = {sym("get_" + f.name + "_inner_count")}(self.raw.as_ptr(), outer);\n'
            f'            let p = {sym("get_" + f.name + "_inner_data")}(self.raw.as_ptr(), outer){cast_c};\n'
            f'            if p.is_null() || n == 0 {{\n'
            f'                &[]\n'
            f'            }} else {{\n'
            f'                core::slice::from_raw_parts(p, n)\n'
            f'            }}\n'
            f'        }}\n'
            f'    }}\n\n'
            f'    pub fn {name}_mut(&mut self, outer: usize) -> &mut [{el}] {{\n'
            f'        if outer >= self.{name}_len() {{\n'
            f'            return &mut [];\n'
            f'        }}\n'
            f'        // SAFETY: as above; `&mut self` rules out aliasing.\n'
            f'        unsafe {{\n'
            f'            let n = {sym("get_" + f.name + "_inner_count")}(self.raw.as_ptr(), outer);\n'
            f'            let p = {sym("get_" + f.name + "_inner_data")}(self.raw.as_ptr(), outer){cast_m};\n'
            f'            if p.is_null() || n == 0 {{\n'
            f'                &mut []\n'
            f'            }} else {{\n'
            f'                core::slice::from_raw_parts_mut(p, n)\n'
            f'            }}\n'
            f'        }}\n'
            f'    }}\n\n'
            f'    pub fn set_{name}(&mut self, outer: usize, values: &[{el}]) {{\n'
            f'        // SAFETY: the native side copies `values` before returning.\n'
            f'        unsafe {{\n'
            f'            {sym("assign_" + f.name + "_inner")}(self.raw.as_ptr(), outer, values.as_ptr() as *const _, values.len())\n'
            f'        }}\n'
            f'    }}\n\n'
            f'    /// Resize the outer sequence. Do this before taking any borrow.\n'
            f'    pub fn resize_{name}(&mut self, count: usize) {{\n'
            f'        // SAFETY: exclusive access, so no borrow is outstanding.\n'
            f'        unsafe {{ {sym("resize_" + f.name)}(self.raw.as_ptr(), count) }}\n'
            f'    }}\n\n'
            f'    pub fn resize_{name}_inner(&mut self, outer: usize, count: usize) {{\n'
            f'        // SAFETY: as above.\n'
            f'        unsafe {{ {sym("resize_" + f.name + "_inner")}(self.raw.as_ptr(), outer, count) }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    if t.kind == TypeKind.ARRAY:
        inner = t.element
        el = _elem_rust(inner, ctx)
        if (inner.kind == TypeKind.NESTED
                and _resolve_handle_short(inner.cpp_text, ctx.module) in _MATH_BY_VALUE):
            mt = f'crate::math::{_resolve_handle_short(inner.cpp_text, ctx.module)}'
            buf.write(
                f'    pub fn {name}_len() -> usize {{\n'
                f'        // SAFETY: a compile-time constant on the native side.\n'
                f'        unsafe {{ {sym(f.name + "_size")}() }}\n'
                f'    }}\n\n'
                f'    pub fn {name}(&self, index: usize) -> {mt} {{\n'
                f'        // SAFETY: the getter returns an interior pointer to a\n'
                f'        // layout-identical POD, copied out immediately. The\n'
                f'        // native side does not bounds-check, so callers stay\n'
                f'        // within `{name}_len()`.\n'
                f'        unsafe {{ *({sym("get_" + f.name + "_at")}(self.raw.as_ptr(), index) as *const {mt}) }}\n'
                f'    }}\n\n'
            )
            return flush(True)
        if el is None or inner.kind not in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            ctx.skipped.append(f'{rust_cls}.{name} (array of {inner.cpp_text})')
            return flush(False)
        if inner.kind == TypeKind.ENUM:
            conv = '\n            .try_into()\n            .expect("unknown enum discriminant")'
            set_val = 'value as i32'
        else:
            conv, set_val = '', 'value'
        buf.write(
            f'    pub fn {name}_len() -> usize {{\n'
            f'        // SAFETY: a compile-time constant on the native side.\n'
            f'        unsafe {{ {sym(f.name + "_size")}() }}\n'
            f'    }}\n\n'
            f'    pub fn {name}(&self, index: usize) -> {el} {{\n'
            f'        // SAFETY: scalar read. The native side does not bounds-\n'
            f'        // check, so callers stay within `{name}_len()`.\n'
            f'        unsafe {{ {sym("get_" + f.name + "_at")}(self.raw.as_ptr(), index) }}{conv}\n'
            f'    }}\n\n'
            f'    pub fn set_{name}(&mut self, index: usize, value: {el}) {{\n'
            f'        // SAFETY: as above.\n'
            f'        unsafe {{ {sym("set_" + f.name + "_at")}(self.raw.as_ptr(), index, {set_val}) }}\n'
            f'    }}\n\n'
        )
        return flush(True)

    ctx.skipped.append(f'{rust_cls}.{name} ({t.kind.value})')
    return flush(False)


def _emit_field_ffi(buf: StringIO, c: BindClass, f, ctx: _Ctx) -> None:
    """Extern declarations matching `_emit_field_decls` in emit_c.py."""
    handle = ctx.handle(c)
    t = f.type
    s = lambda suffix: ctx.sym(c, suffix)  # noqa: E731

    if t.kind == TypeKind.PRIMITIVE:
        prim = _rust_prim(t.cpp_text)
        if not prim or prim == '()':
            return
        cp = 'i32' if prim == 'bool' else prim
        buf.write(f'    pub fn {s("get_" + f.name)}(self_: *mut {handle}) -> {cp};\n')
        buf.write(f'    pub fn {s("set_" + f.name)}(self_: *mut {handle}, value: {cp});\n')
    elif t.kind == TypeKind.ENUM:
        if ctx.enum_for(t.cpp_text) is None:
            return
        buf.write(f'    pub fn {s("get_" + f.name)}(self_: *mut {handle}) -> i32;\n')
        buf.write(f'    pub fn {s("set_" + f.name)}(self_: *mut {handle}, value: i32);\n')
    elif t.kind == TypeKind.STRING:
        buf.write(f'    pub fn {s("get_" + f.name)}(self_: *mut {handle}) -> RawCString;\n')
        buf.write(f'    pub fn {s("set_" + f.name)}'
                  f'(self_: *mut {handle}, value: *const core::ffi::c_char);\n')
    elif t.kind == TypeKind.NESTED:
        short = _resolve_handle_short(t.cpp_text, ctx.module)
        if short in _MATH_BY_VALUE:
            ptr = 'core::ffi::c_void'
        else:
            inner = ctx.classes.get(short)
            if inner is None or _is_field_only(inner, ctx.module):
                return
            ptr = ctx.handle(inner)
        buf.write(f'    pub fn {s("get_" + f.name)}(self_: *mut {handle}) -> *mut {ptr};\n')
        buf.write(f'    pub fn {s("set_" + f.name)}'
                  f'(self_: *mut {handle}, value: *const {ptr});\n')
    elif t.kind == TypeKind.VECTOR:
        inner = t.element
        if _is_bulk_flat_inner(inner):
            if _bulk_slice_rust(inner, ctx) is None:
                return
            comp = _C_COMP_TO_RUST.get(_bulk_component_c_type(inner), 'u8')
            buf.write(f'    pub fn {s("get_" + f.name + "_count")}(self_: *mut {handle}) -> usize;\n')
            buf.write(f'    pub fn {s("resize_" + f.name)}(self_: *mut {handle}, count: usize);\n')
            buf.write(f'    pub fn {s("get_" + f.name + "_data")}(self_: *mut {handle}) -> *const {comp};\n')
            buf.write(f'    pub fn {s("assign_" + f.name)}'
                      f'(self_: *mut {handle}, data: *const {comp}, count: usize);\n')
        elif inner.kind == TypeKind.NESTED:
            short = _resolve_handle_short(inner.cpp_text, ctx.module)
            ic = ctx.classes.get(short)
            if ic is None or _is_field_only(ic, ctx.module):
                return
            buf.write(f'    pub fn {s("get_" + f.name + "_count")}(self_: *mut {handle}) -> usize;\n')
            buf.write(f'    pub fn {s("resize_" + f.name)}(self_: *mut {handle}, count: usize);\n')
            buf.write(f'    pub fn {s("get_" + f.name + "_at")}'
                      f'(self_: *mut {handle}, index: usize) -> *mut {ctx.handle(ic)};\n')
    elif t.kind == TypeKind.NESTED_VEC:
        inner = t.element.element
        if not _is_bulk_flat_inner(inner) and inner.kind == TypeKind.NESTED:
            short = _resolve_handle_short(inner.cpp_text, ctx.module)
            ic = ctx.classes.get(short)
            if ic is None or _is_field_only(ic, ctx.module):
                return
            buf.write(f'    pub fn {s("get_" + f.name + "_count")}(self_: *mut {handle}) -> usize;\n')
            buf.write(f'    pub fn {s("get_" + f.name + "_inner_count")}'
                      f'(self_: *mut {handle}, outer: usize) -> usize;\n')
            buf.write(f'    pub fn {s("resize_" + f.name)}(self_: *mut {handle}, count: usize);\n')
            buf.write(f'    pub fn {s("resize_" + f.name + "_inner")}'
                      f'(self_: *mut {handle}, outer: usize, count: usize);\n')
            buf.write(f'    pub fn {s("get_" + f.name + "_at")}'
                      f'(self_: *mut {handle}, outer: usize, inner: usize)'
                      f' -> *mut {ctx.handle(ic)};\n')
            return
        if not _is_bulk_flat_inner(inner) or _bulk_slice_rust(inner, ctx) is None:
            return
        comp = _C_COMP_TO_RUST.get(_bulk_component_c_type(inner), 'u8')
        buf.write(f'    pub fn {s("get_" + f.name + "_count")}(self_: *mut {handle}) -> usize;\n')
        buf.write(f'    pub fn {s("get_" + f.name + "_inner_count")}'
                  f'(self_: *mut {handle}, outer: usize) -> usize;\n')
        buf.write(f'    pub fn {s("resize_" + f.name)}(self_: *mut {handle}, count: usize);\n')
        buf.write(f'    pub fn {s("resize_" + f.name + "_inner")}'
                  f'(self_: *mut {handle}, outer: usize, count: usize);\n')
        buf.write(f'    pub fn {s("get_" + f.name + "_inner_data")}'
                  f'(self_: *mut {handle}, outer: usize) -> *const {comp};\n')
        buf.write(f'    pub fn {s("assign_" + f.name + "_inner")}'
                  f'(self_: *mut {handle}, outer: usize, data: *const {comp}, count: usize);\n')
    elif t.kind == TypeKind.ARRAY:
        inner = t.element
        el = _elem_rust(inner, ctx)
        if (inner.kind == TypeKind.NESTED
                and _resolve_handle_short(inner.cpp_text, ctx.module) in _MATH_BY_VALUE):
            buf.write(f'    pub fn {s(f.name + "_size")}() -> usize;\n')
            buf.write(f'    pub fn {s("get_" + f.name + "_at")}'
                      f'(self_: *mut {handle}, index: usize) -> *mut core::ffi::c_void;\n')
            return
        if el is None or inner.kind not in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            return
        ce = 'i32' if inner.kind == TypeKind.ENUM else el
        buf.write(f'    pub fn {s(f.name + "_size")}() -> usize;\n')
        buf.write(f'    pub fn {s("get_" + f.name + "_at")}'
                  f'(self_: *mut {handle}, index: usize) -> {ce};\n')
        buf.write(f'    pub fn {s("set_" + f.name + "_at")}'
                  f'(self_: *mut {handle}, index: usize, value: {ce});\n')
