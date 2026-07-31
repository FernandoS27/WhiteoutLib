# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> Rust value ABI (.h + .cpp).

A second, narrow C ABI that exists alongside `emit_c.py`'s. Where that one
models every C++ class as an opaque handle with per-field accessors — the
shape Java and C# need — this one moves plain data **by value**.

Rationale, in one line: 79% of the bound surface is plain data, and paying
an FFI call per field for it is the single largest cost in a Rust binding.

Scope is deliberately the *data plane* only:

  - by-value POD types (the shared math types), and
  - (later) Tier A slice accessors and Tier B bulk view transcoders.

Control-plane types — handles with real behaviour — keep using the symbols
`emit_c.py` already emits. Their C ABI is already the right shape for Rust;
its Java/C#-isms there are cosmetic and never reach the user.

Symbols are prefixed `whiteout_v_` ("value") so they can coexist with the
handle ABI in one shared library.

Layout contract: every emitted C struct is asserted at compile time to match
the size of its C++ counterpart, and `whiteout_v_layout()` reports the same
sizes at runtime so the Rust side can verify the library it actually linked
against. See `docs/plans/rust-bindings.md` §5, R3.
"""

from __future__ import annotations

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

# Bumped whenever the emitted struct layouts or symbol contract change.
# The Rust side refuses to run against a mismatched library.
ABI_VERSION = 2

_PREFIX = 'whiteout_v'


# ── Tier A: mutable borrows into C++-owned contiguous buffers ─────────────
#
# `emit_c.py` already hands out *const* spans as `whiteout_Bytes` with
# `_owner = NULL`, which Rust surfaces as `&[u8]` borrowed from the owning
# handle. The mutable half has no equivalent — C# and Java cannot express a
# borrowed mutable view, so it was never emitted. Rust can, safely, because
# the borrow checker forbids resizing the owner while the slice is alive.
#
# Each entry: (C++ include, handle name, C++ type, [(method, extra params)])
# The generated function returns a raw `uint8_t*` plus a size out-param.
_TIER_A_MUT = [
    (
        'whiteout/textures/texture.h',
        'Texture',
        'whiteout::textures::Texture',
        [
            ('data', []),
            ('mipData', [('uint32_t', 'mip'), ('uint32_t', 'layer')]),
        ],
    ),
]


def _tier_a_sym(handle: str, method: str) -> str:
    return f'{_PREFIX}_{handle}_{method}_mut'


def _emit_tier_a_header(buf: StringIO) -> None:
    if not _TIER_A_MUT:
        return
    buf.write('/* ── Tier A: mutable pixel/element access ─────────────────────────── */\n')
    buf.write('/* Returns a pointer into the C++-owned buffer and writes its length to\n')
    buf.write(' * *out_size. The pointer stays valid until the owning object is resized\n')
    buf.write(' * or destroyed — Rust ties it to a `&mut` borrow of the owner, which\n')
    buf.write(' * makes both hazards compile errors. */\n\n')
    for _inc, handle, _cpp, methods in _TIER_A_MUT:
        buf.write(f'typedef struct {_PREFIX}_{handle} {_PREFIX}_{handle};\n')
    buf.write('\n')
    for _inc, handle, _cpp, methods in _TIER_A_MUT:
        for method, extra in methods:
            params = ''.join(f', {t} {n}' for t, n in extra)
            buf.write(f'uint8_t* {_tier_a_sym(handle, method)}'
                      f'({_PREFIX}_{handle}* self{params}, size_t* out_size);\n')
    buf.write('\n')


def _emit_tier_a_source(buf: StringIO) -> None:
    if not _TIER_A_MUT:
        return
    buf.write('// ── Tier A: mutable pixel/element access ──────────────\n')
    for _inc, handle, cpp, methods in _TIER_A_MUT:
        for method, extra in methods:
            params = ''.join(f', {t} {n}' for t, n in extra)
            args = ', '.join(n for _t, n in extra)
            buf.write(f'uint8_t* {_tier_a_sym(handle, method)}'
                      f'({_PREFIX}_{handle}* self{params}, size_t* out_size) {{\n')
            buf.write(f'    auto __s = reinterpret_cast<{cpp}*>(self)->{method}({args});\n')
            buf.write('    if (out_size) *out_size = __s.size();\n')
            buf.write('    return reinterpret_cast<uint8_t*>(__s.data());\n')
            buf.write('}\n\n')


def _struct(name: str) -> str:
    return f'{_PREFIX}_{name}'


def _c_type(t: str) -> str:
    """Return type spelling. Math types come back **by value**."""
    if t == 'bool':
        return 'int32_t'
    if t in _PRIMITIVES_PASSTHRU:
        return t
    return _struct(t)


def _c_param(t: str, name: str) -> str:
    """Parameter spelling. Math types are passed **by value** — they are at
    most 64 bytes and the platform C ABI handles them without a copy the
    caller can observe."""
    if t in _PRIMITIVES_PASSTHRU:
        return f'{t} {name}'
    return f'{_struct(t)} {name}'


def _self_param(owner: str, is_const: bool) -> str:
    """`self` is by value for const methods and by pointer for mutating
    ones (`normalize`), so the mutation is visible to the caller."""
    return (f'{_struct(owner)} self' if is_const
            else f'{_struct(owner)}* self')


def _method_sym(owner: str, name: str) -> str:
    return f'{_PREFIX}_{owner}_{name}'


def _signature(owner: str, m: tuple) -> tuple[str, str]:
    """Return (return_type, parameter_list) for a method-table entry."""
    name, ret, params, is_static, is_const = m[:5]
    parts = []
    if not is_static:
        parts.append(_self_param(owner, is_const))
    for p_t, p_n in params:
        parts.append(_c_param(p_t, p_n))
    return _c_type(ret), (', '.join(parts) if parts else 'void')


# ── Header ────────────────────────────────────────────────────────────────

def emit_header() -> str:
    buf = StringIO()
    buf.write(f'''/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Fernando Sahmkow */
/* AUTOGENERATED by tools/codegen/emit_rust_abi.py — do not edit. */
/* Regenerate via:  python -m tools.codegen.codegen <any> --backend rust-abi-header */

/* Rust value ABI: plain data by value, no opaque handles, no per-field
 * accessors. Coexists with the handle ABI in whiteout_c_common.h. */

#ifndef WHITEOUT_V_H
#define WHITEOUT_V_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {{
#endif

#define WHITEOUT_V_ABI_VERSION {ABI_VERSION}

/* ── Value types ──────────────────────────────────────────────────── */
/* Layouts mirror the C++ types exactly; static_asserts in the .cpp make
 * a mismatch a compile error, and whiteout_v_layout() lets the caller
 * re-check against the library it actually linked. */

''')

    for name in _SHARED_MATH_TYPES:
        fields = _SHARED_MATH_FIELDS.get(name)
        buf.write('typedef struct {\n')
        if fields:
            buf.write(f'    float {", ".join(fields)};\n')
        else:
            dim = _MATRIX_DIMS[name]
            buf.write(f'    float data[{dim}][{dim}];   /* row-major */\n')
        buf.write(f'}} {_struct(name)};\n\n')

    # Layout report — lets Rust verify the linked library at run time.
    buf.write('/* ── Layout self-report ───────────────────────────────────────────── */\n\n')
    buf.write('typedef struct {\n')
    buf.write('    uint32_t abi_version;\n')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'    uint32_t {name.lower()};\n')
    buf.write(f'}} {_PREFIX}_Layout;\n\n')
    buf.write(f'{_PREFIX}_Layout {_PREFIX}_layout(void);\n\n')

    for name in _SHARED_MATH_TYPES:
        buf.write(f'/* ── {name} ──────────────────────────────────────────── */\n')
        for m in _SHARED_MATH_METHODS.get(name, []):
            ret_c, sig = _signature(name, m)
            buf.write(f'{ret_c} {_method_sym(name, m[0])}({sig});\n')
        if name in _MATRIX_DIMS:
            buf.write(f'float {_PREFIX}_{name}_get_at({_struct(name)} self, size_t row, size_t col);\n')
            buf.write(f'void {_PREFIX}_{name}_set_at({_struct(name)}* self, size_t row, size_t col, float value);\n')
        buf.write('\n')

    if _SHARED_MATH_FREE_FUNCTIONS:
        buf.write('/* ── Free functions ──────────────────────────────────────── */\n')
        for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
            param_strs = [_c_param(t, n) for t, n in params]
            sig = ', '.join(param_strs) if param_strs else 'void'
            buf.write(f'{_c_type(ret)} {_PREFIX}_{name}({sig});\n')
        buf.write('\n')

    _emit_tier_a_header(buf)

    buf.write('''#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WHITEOUT_V_H */
''')
    return buf.getvalue()


# ── Source ────────────────────────────────────────────────────────────────

def _emit_method_def(buf: StringIO, owner: str, m: tuple) -> None:
    name, ret, params, is_static, is_const = m[:5]
    cpp_expr = m[5] if len(m) > 5 else None
    cpp_owner = f'whiteout::{owner}'
    ret_c, sig = _signature(owner, m)
    sym = _method_sym(owner, name)

    buf.write(f'{ret_c} {sym}({sig}) {{\n')

    placeholders: dict[str, str] = {}
    cpp_args: list[str] = []

    if not is_static:
        if is_const:
            buf.write(f'    const {cpp_owner} __self = fromC(self);\n')
        else:
            # Mutating: round-trip through a local so the change lands back
            # in the caller's struct.
            buf.write(f'    {cpp_owner} __self = fromC(*self);\n')
        placeholders['self'] = '__self'

    for p_t, p_n in params:
        if p_t in _PRIMITIVES_PASSTHRU:
            cpp_args.append(p_n)
            placeholders[p_n] = p_n
        else:
            buf.write(f'    const whiteout::{p_t} __{p_n} = fromC({p_n});\n')
            cpp_args.append(f'__{p_n}')
            placeholders[p_n] = f'__{p_n}'

    cpp_args_str = ', '.join(cpp_args)
    if cpp_expr is not None:
        call = cpp_expr.format(**placeholders)
    elif is_static:
        call = f'{cpp_owner}::{name}({cpp_args_str})'
    else:
        call = f'__self.{name}({cpp_args_str})'

    if ret == 'void':
        buf.write(f'    {call};\n')
        if not is_static and not is_const:
            buf.write('    *self = toC(__self);\n')
    elif ret == 'bool':
        buf.write(f'    return ({call}) ? 1 : 0;\n')
    elif ret in _PRIMITIVES_PASSTHRU:
        buf.write(f'    return {call};\n')
    else:
        buf.write(f'    return toC({call});\n')
    buf.write('}\n\n')


def emit_source() -> str:
    buf = StringIO()
    buf.write('''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
// AUTOGENERATED by tools/codegen/emit_rust_abi.py — do not edit.
// Regenerate via:  python -m tools.codegen.codegen <any> --backend rust-abi-source

#include <whiteout/vector_types.h>
#include <whiteout/textures/texture.h>

#include <cstring>
#include <type_traits>

#include "whiteout_v.h"

namespace {

// The C structs and the C++ types are layout-identical by construction
// (see the static_asserts below). memcpy is the conversion with the
// laxest preconditions — it needs only trivial copyability — and every
// compiler folds it away entirely at -O1 and above.
template <typename Cpp, typename C>
inline Cpp fromC_impl(const C& v) {
    static_assert(sizeof(Cpp) == sizeof(C), "value ABI layout mismatch");
    static_assert(std::is_trivially_copyable<Cpp>::value, "not trivially copyable");
    Cpp out;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

template <typename C, typename Cpp>
inline C toC_impl(const Cpp& v) {
    static_assert(sizeof(Cpp) == sizeof(C), "value ABI layout mismatch");
    C out;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

''')

    # Typed conversion overloads — one pair per math type, so the generated
    # bodies can just say fromC(x) / toC(x) and let overload resolution pick.
    for name in _SHARED_MATH_TYPES:
        c = _struct(name)
        buf.write(f'inline whiteout::{name} fromC(const {c}& v) '
                  f'{{ return fromC_impl<whiteout::{name}>(v); }}\n')
        buf.write(f'inline {c} toC(const whiteout::{name}& v) '
                  f'{{ return toC_impl<{c}>(v); }}\n')
    buf.write('\n')

    buf.write('// Layout contract — a mismatch here is a compile error, not a runtime surprise.\n')
    for name in _SHARED_MATH_TYPES:
        size = _SHARED_MATH_BYTE_SIZES[name]
        buf.write(f'static_assert(sizeof(whiteout::{name}) == {size}, '
                  f'"whiteout::{name} changed size");\n')
        buf.write(f'static_assert(sizeof({_struct(name)}) == {size}, '
                  f'"{_struct(name)} changed size");\n')
    buf.write('\n}  // namespace\n\nextern "C" {\n\n')

    # Layout self-report.
    buf.write(f'{_PREFIX}_Layout {_PREFIX}_layout(void) {{\n')
    buf.write(f'    {_PREFIX}_Layout l;\n')
    buf.write(f'    l.abi_version = WHITEOUT_V_ABI_VERSION;\n')
    for name in _SHARED_MATH_TYPES:
        buf.write(f'    l.{name.lower()} = (uint32_t)sizeof({_struct(name)});\n')
    buf.write('    return l;\n}\n\n')

    for name in _SHARED_MATH_TYPES:
        buf.write(f'// ── {name} ──────────────────────────────────────────\n')
        for m in _SHARED_MATH_METHODS.get(name, []):
            _emit_method_def(buf, name, m)
        if name in _MATRIX_DIMS:
            c = _struct(name)
            buf.write(f'float {_PREFIX}_{name}_get_at({c} self, size_t row, size_t col) {{\n')
            buf.write('    return self.data[row][col];\n}\n\n')
            buf.write(f'void {_PREFIX}_{name}_set_at({c}* self, size_t row, size_t col, float value) {{\n')
            buf.write('    self->data[row][col] = value;\n}\n\n')

    if _SHARED_MATH_FREE_FUNCTIONS:
        buf.write('// ── Free functions ──────────────────────────────────\n')
        for name, ret, params in _SHARED_MATH_FREE_FUNCTIONS:
            param_strs = [_c_param(t, n) for t, n in params]
            sig = ', '.join(param_strs) if param_strs else 'void'
            buf.write(f'{_c_type(ret)} {_PREFIX}_{name}({sig}) {{\n')
            args = []
            for t, n in params:
                if t in _PRIMITIVES_PASSTHRU:
                    args.append(n)
                else:
                    buf.write(f'    const whiteout::{t} __{n} = fromC({n});\n')
                    args.append(f'__{n}')
            call = f'whiteout::{name}({", ".join(args)})'
            if ret in _PRIMITIVES_PASSTHRU:
                buf.write(f'    return {call};\n')
            else:
                buf.write(f'    return toC({call});\n')
            buf.write('}\n\n')

    _emit_tier_a_source(buf)

    buf.write('}  // extern "C"\n')
    return buf.getvalue()
