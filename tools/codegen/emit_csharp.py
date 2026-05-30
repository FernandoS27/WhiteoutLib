# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> idiomatic C# bindings.

Output layout (per module M):

  bindings/csharp/Whiteout/<M>/
      NativeMethods.<M>.cs        internal partial class: every LibraryImport
      <Enum>.cs                   public enum
      <Class>.cs                  public sealed class : WhiteoutHandle

Methods are mapped to PascalCase C# methods on the public class. Each
generated public method calls into the `NativeMethods` LibraryImport stub
and handles marshalling (NativeBytes copy-and-free, optional handle
wrapping, ReadOnlySpan<byte> pinning, enum casts, bool ↔ int32).

Method shapes the C emitter exposes today and how we lower them:

  C++                                C# public                  Native (LibraryImport)
  ---------------------------------- -------------------------- --------------------------
  uint32_t width()                    uint Width()               uint whiteout_M_C_width(...)
  int32_t isSrgb()                    bool IsSrgb()              [MarshalAs(I4)] bool ...
  Type type()  // enum                Type Type()                cast int → enum
  std::optional<Texture> parse(span)  Texture? Parse(ROS<byte>)  IntPtr (null = empty)
  std::vector<u8> write(Texture)      byte[] Write(Texture)      NativeBytes (free after)
  std::optional<vec<u8>> tryWrite()   byte[]? TryWrite()         NativeBytes (null = empty)
  std::string lastError()             string LastError()         NativeCString
  Texture create2D(...)               static Texture Create2D(.) IntPtr → new Texture(h)

Skipped (silently — Phase 3+):
  - methods with `interfaces::X*` callback params (WorkerPool, HttpHandler)
  - methods with `std::vector<Class>` params
  - methods with `std::span<const T>` for T != u8
  - methods with `std::vector<T>` returns where T != u8
  - move-only return types where ownership semantics aren't clear
"""

from __future__ import annotations

import re
from io import StringIO

from .ir import (
    BindClass, BindEnum, BindField, BindMethod, BindMethodParam, BindModule,
    TypeKind, TypeRef,
)
from .parser import _short_name
from .emit_c import (
    _c_handle_name, _module_prefix,
    _is_method_supported, _is_byte_vector_param, _is_field_supported,
    _is_interface_pointer_param, _is_span_const_u8,
    _resolve_handle_short, _known_class_short_names,
    _is_vector_string_return,
    _ctor_overloads_named,
)


# ── Naming ─────────────────────────────────────────────────────────────────

def _pascal(name: str) -> str:
    """`mdx` → `Mdx`; `parseBufferFormat` → `ParseBufferFormat`."""
    if not name:
        return name
    parts = re.split(r'[_\s]+', name)
    out = ''.join(p[:1].upper() + p[1:] for p in parts if p)
    return out[:1].upper() + out[1:]


def _ns(module: BindModule) -> str:
    return f'Whiteout.{_pascal(module.name)}'


def _strip_prefix(js_name: str, prefix: str) -> str:
    if prefix and js_name.startswith(prefix):
        return js_name[len(prefix):] or js_name
    return js_name


def _cs_type_name(js_name: str, module: BindModule) -> str:
    return _strip_prefix(js_name, module.js_prefix)


# ── Math types ────────────────────────────────────────────────────────────
#
# Recognised math types are aliased (via global using in
# Whiteout.Common/MathTypes.cs) to System.Numerics value-type structs:
#   Vector2f → System.Numerics.Vector2  (8 bytes, 2 floats)
#   Vector3f → System.Numerics.Vector3  (12 bytes, 3 floats)
#   Vector4f → System.Numerics.Vector4  (16 bytes, 4 floats)
#   Quaternion → System.Numerics.Quaternion  (16 bytes, X/Y/Z/W floats)
#
# The C ABI exposes them as opaque pointer-to-struct handles. The C# side
# reads/writes the underlying 12/16 bytes directly via `Unsafe.Read<T>` /
# `Unsafe.Write<T>` so the value semantics work without a SafeHandle dance
# at every read.

_MATH_TYPES: dict[str, tuple[str, int]] = {
    # short_name → (C# struct type, size in bytes)
    'Vector2f':    ('Vector2f',    8),
    'Vector3f':    ('Vector3f',   12),
    'Vector4f':    ('Vector4f',   16),
    'Quaternion':  ('Quaternion', 16),
}


def _math_info(cpp_text: str) -> tuple[str, int] | None:
    """If `cpp_text` references a recognised math type, return its
    (C#-type, byte-size). Otherwise None."""
    short = cpp_text.split('::')[-1].split('<')[0]
    return _MATH_TYPES.get(short)


# ── Primitive type maps ────────────────────────────────────────────────────

_CS_PRIMITIVE = {
    'u8':   'byte',   'u16': 'ushort', 'u32': 'uint',  'u64': 'ulong',
    'i8':   'sbyte',  'i16': 'short',  'i32': 'int',   'i64': 'long',
    'f32':  'float',  'f64': 'double',
    'bool': 'bool',
    'unsigned char':      'byte',
    'unsigned short':     'ushort',
    'unsigned int':       'uint',
    'unsigned long long': 'ulong',
    'signed char':        'sbyte',
    'short':              'short',
    'int':                'int',
    'long long':          'long',
    'float':              'float',
    'double':             'double',
    'char':               'sbyte',
}

# C ABI primitives → C# native types for LibraryImport. Bool crosses the
# ABI as int32 with a MarshalAs marshaller applied externally.
_CS_NATIVE_PRIMITIVE = dict(_CS_PRIMITIVE)
_CS_NATIVE_PRIMITIVE['bool'] = 'int'   # raw native side; public side stays bool


def _is_known_handle(cpp_text: str, module: BindModule) -> bool:
    return _c_handle_name(cpp_text) in _known_class_short_names(module)


# ── Type lowering ─────────────────────────────────────────────────────────

# Method names that collide with members of the System.Runtime.InteropServices.SafeHandle
# base. Generated methods using these names need the `new` keyword to suppress CS0108.
_SAFE_HANDLE_RESERVED = frozenset({
    'Close',     # SafeHandle.Close() — aliases Dispose. Domain methods like
                 # mpq::Storage::close() shouldn't pretend to be Dispose.
    'Dispose',
})


_TRAMPOLINED_INTERFACES = {
    # cpp_text → (C# managed base class, fully qualified)
    'whiteout::interfaces::VirtualPathFileSystem': 'Whiteout.Host.VirtualPathFileSystem',
    'whiteout::interfaces::HttpHandler':           'Whiteout.Host.HttpHandler',
    'whiteout::interfaces::WorkerPool':            'Whiteout.Host.WorkerPool',
}


# Per trampolined base: the method names that exist as abstracts/virtuals
# on the managed base. When the codegen-emitted class derives from one of
# these bases, methods with matching names use `override` instead of being
# emitted fresh; methods missing entirely from the C ABI get a throwing
# stub so the derived class isn't left abstract.
#
# Entry: cpp method name → kind ('method' or 'prop')
_TRAMPOLINED_BASE_MEMBERS: dict[str, dict[str, str]] = {
    'Whiteout.Host.VirtualPathFileSystem': {
        'readFile':   'method',
        'writeFile':  'method',
        'fileExists': 'method',
    },
    'Whiteout.Host.HttpHandler': {
        'capabilities':   'prop',
        'getAsync':       'method',
        'getRangeAsync':  'method',
    },
    'Whiteout.Host.WorkerPool': {
        'submit':       'method',
        'waitIdle':     'method',
        'threadCount':  'prop',
    },
}


def _trampolined_base_for_class(c: BindClass) -> str | None:
    """If `c` derives from a trampolined interface, return the C# managed
    base class name."""
    if not c.base_class:
        return None
    return _TRAMPOLINED_INTERFACES.get(c.base_class)


# Throwing-stub signatures for abstract methods missing from a concrete
# class's bound C ABI. Indexed by (base_class_short, member_name) → full
# C# declaration line (without the leading `    public override `).
_THROWING_STUBS: dict[tuple[str, str], str] = {
    ('VirtualPathFileSystem', 'readFile'):
        'byte[] ReadFile(string path) => throw new NotSupportedException("native dispatch");',
    ('VirtualPathFileSystem', 'writeFile'):
        'bool WriteFile(string path, ReadOnlySpan<byte> data) => throw new NotSupportedException("native dispatch");',
    ('VirtualPathFileSystem', 'fileExists'):
        'bool FileExists(string path) => throw new NotSupportedException("native dispatch");',
    ('HttpHandler', 'getAsync'):
        'void GetAsync(string url, Action<Whiteout.Host.HttpResponse> callback) => throw new NotSupportedException("native dispatch");',
    ('HttpHandler', 'getRangeAsync'):
        'void GetRangeAsync(string url, ulong start, ulong end, Action<Whiteout.Host.HttpResponse> callback) => throw new NotSupportedException("native dispatch");',
    ('WorkerPool', 'submit'):
        'void Submit(Whiteout.Host.WorkerTask task) => throw new NotSupportedException("native dispatch");',
    ('WorkerPool', 'waitIdle'):
        'void WaitIdle() => throw new NotSupportedException("native dispatch");',
    ('WorkerPool', 'threadCount'):
        'ulong ThreadCount => throw new NotSupportedException("native dispatch");',
}


def _trampolined_interface(p: BindMethodParam) -> str | None:
    """If `p` is a reference/pointer to a managed-subclassable abstract
    interface (currently only VirtualPathFileSystem), return the C# base
    class name to surface in the public API. Other interface params
    (HttpHandler, WorkerPool) still skip until their trampolines land."""
    if not _is_interface_pointer_param(p):
        return None
    return _TRAMPOLINED_INTERFACES.get(p.type.cpp_text)


def _native_param(p: BindMethodParam, module: BindModule) -> str | None:
    """Render one method parameter for the LibraryImport stub. Returns the
    full declaration string (one or two comma-separated params) or None if
    we can't lower this parameter shape."""
    t = p.type
    if p.span_scalar is not None:
        # std::span<const T> for some primitive T. Skip for now — we only
        # ship byte spans in Phase 2.
        if p.span_scalar[0] not in ('u8', 'unsigned char'):
            return None
        return f'ReadOnlySpan<byte> {p.name}, nuint {p.name}_size'
    if _trampolined_interface(p) is not None:
        return f'IntPtr {p.name}'
    if _is_interface_pointer_param(p):
        return None
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        cs = _CS_NATIVE_PRIMITIVE.get(short)
        if cs is None:
            return None
        return f'{cs} {p.name}'
    if t.kind == TypeKind.STRING:
        return f'[MarshalAs(UnmanagedType.LPUTF8Str)] string {p.name}'
    if t.kind == TypeKind.ENUM:
        return f'int {p.name}'
    if _is_byte_vector_param(p):
        return f'ReadOnlySpan<byte> {p.name}, nuint {p.name}_size'
    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        if t.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(t.cpp_text, module):
            return None
        return f'IntPtr {p.name}'
    return None


def _native_return(m: BindMethod, module: BindModule) -> str | None:
    """Render the LibraryImport stub return type. Returns None when we
    can't model the return — caller should skip the method."""
    r = m.return_type
    if r.cpp_text == 'void':
        return 'void'
    if r.kind == TypeKind.PRIMITIVE:
        short = _short_name(r.cpp_text)
        cs = _CS_NATIVE_PRIMITIVE.get(short)
        return cs
    if r.kind == TypeKind.ENUM:
        return 'int'
    if r.kind == TypeKind.STRING:
        return 'Whiteout.Common.NativeCString'
    if r.kind == TypeKind.NESTED:
        if r.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(r.cpp_text, module):
            return None
        return 'IntPtr'
    if r.kind == TypeKind.UNKNOWN:
        if _is_span_const_u8(r):
            return 'Whiteout.Common.NativeBytes'
        if r.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(r.cpp_text, module):
            return None
        return 'IntPtr'
    if r.kind == TypeKind.OPTIONAL:
        e = r.element
        if e.kind == TypeKind.NESTED and _is_known_handle(e.cpp_text, module):
            return 'IntPtr'
        if e.kind == TypeKind.STRING:
            return 'Whiteout.Common.NativeCString'
        if e.kind == TypeKind.VECTOR and \
                _short_name(e.element.cpp_text) in ('u8', 'unsigned char'):
            return 'Whiteout.Common.NativeBytes'
        return None
    if r.kind == TypeKind.VECTOR:
        if _short_name(r.element.cpp_text) in ('u8', 'unsigned char'):
            return 'Whiteout.Common.NativeBytes'
        return None
    return None


def _public_return(m: BindMethod, module: BindModule) -> tuple[str, str] | None:
    """(public C# return type, internal classification tag). The tag drives
    how the wrapper marshals the native call back to managed land:

        'void'      void return
        'prim'      primitive (incl. int-as-bool)
        'bool'      bool: native int32, public bool
        'enum:X'    public enum X (cast from int)
        'bytes'     non-nullable byte[]
        'bytes?'    nullable byte[]?
        'string'    string
        'string?'   nullable string?
        'handle:X'  non-nullable handle wrapper X
        'handle?:X' nullable handle wrapper X?
    """
    r = m.return_type
    if r.cpp_text == 'void':
        return 'void', 'void'
    if r.kind == TypeKind.PRIMITIVE:
        short = _short_name(r.cpp_text)
        cs = _CS_PRIMITIVE.get(short)
        if cs is None:
            return None
        return (cs, 'bool' if short == 'bool' else 'prim')
    if r.kind == TypeKind.ENUM:
        # Resolve enum short name (strips ns + js_prefix)
        name = _resolve_enum_cs_name(r.cpp_text, module)
        if name is None:
            return None
        return name, f'enum:{name}'
    if r.kind == TypeKind.STRING:
        return 'string', 'string'
    if r.kind == TypeKind.NESTED:
        if r.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(r.cpp_text, module):
            return None
        name = _wrapper_name(r.cpp_text, module)
        return name, f'handle:{name}'
    if r.kind == TypeKind.UNKNOWN:
        if _is_span_const_u8(r):
            return 'byte[]', 'bytes'
        if r.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(r.cpp_text, module):
            return None
        name = _wrapper_name(r.cpp_text, module)
        return name, f'handle:{name}'
    if r.kind == TypeKind.OPTIONAL:
        e = r.element
        if e.kind == TypeKind.NESTED and _is_known_handle(e.cpp_text, module):
            name = _wrapper_name(e.cpp_text, module)
            return f'{name}?', f'handle?:{name}'
        if e.kind == TypeKind.STRING:
            return 'string?', 'string?'
        if e.kind == TypeKind.VECTOR and \
                _short_name(e.element.cpp_text) in ('u8', 'unsigned char'):
            return 'byte[]?', 'bytes?'
        return None
    if r.kind == TypeKind.VECTOR:
        if _short_name(r.element.cpp_text) in ('u8', 'unsigned char'):
            return 'byte[]', 'bytes'
        return None
    return None


def _resolve_enum_cs_name(cpp_text: str, module: BindModule) -> str | None:
    """Find the C# enum name corresponding to a C++ enum cpp_text reference."""
    short = cpp_text.split('::')[-1]
    for e in module.enums:
        if e.cpp_qualifier.split('::')[-1] == short or e.js_name == short:
            return _cs_type_name(e.js_name, module)
    return None


def _native_ctor_params(ctor, module: BindModule) -> str | None:
    """LibraryImport signature for a non-default constructor."""
    decls: list[str] = []
    for p in ctor.params:
        t = p.type
        if t.kind == TypeKind.STRING:
            decls.append(f'[MarshalAs(UnmanagedType.LPUTF8Str)] string {p.name}')
            continue
        if t.kind == TypeKind.PRIMITIVE:
            short = _short_name(t.cpp_text)
            cs = _CS_NATIVE_PRIMITIVE.get(short)
            if cs is None:
                return None
            decls.append(f'{cs} {p.name}')
            continue
        if t.kind == TypeKind.ENUM:
            decls.append(f'int {p.name}')
            continue
        if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
                and not t.cpp_text.startswith('std::') \
                and _is_known_handle(t.cpp_text, module):
            decls.append(f'IntPtr {p.name}')
            continue
        return None
    return ', '.join(decls)


def _cs_ctor_params(ctor, module: BindModule) -> tuple[str, str] | tuple[None, None]:
    """Render a non-default constructor's parameters for the C# public ctor.
    Returns (cs_param_list, cs_arg_list_for_native_call) or (None, None) if
    any parameter type isn't representable. Mirrors the subset of C ABI
    constructor param types the C emitter handles."""
    decls: list[str] = []
    args: list[str] = []
    for p in ctor.params:
        t = p.type
        if t.kind == TypeKind.STRING:
            decls.append(f'string {p.name}')
            args.append(p.name)
            continue
        if t.kind == TypeKind.PRIMITIVE:
            short = _short_name(t.cpp_text)
            cs = _CS_PRIMITIVE.get(short)
            if cs is None:
                return None, None
            decls.append(f'{cs} {p.name}')
            args.append(p.name)
            continue
        if t.kind == TypeKind.ENUM:
            name = _resolve_enum_cs_name(t.cpp_text, module)
            if name is None:
                return None, None
            decls.append(f'{name} {p.name}')
            args.append(f'(int){p.name}')
            continue
        # Class-handle ctor params (e.g. MpqFileSystem(Storage&)) need a
        # known handle to extract DangerousGet() from.
        if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
                and not t.cpp_text.startswith('std::') \
                and _is_known_handle(t.cpp_text, module):
            wrapper = _wrapper_name(t.cpp_text, module)
            decls.append(f'{wrapper} {p.name}')
            args.append(f'{p.name}.DangerousGet()')
            continue
        return None, None
    return ', '.join(decls), ', '.join(args)


def _wrapper_name(cpp_text: str, module: BindModule) -> str:
    """Resolve the public C# type name for a class referenced by cpp_text.

    Math types short-circuit to their `Whiteout.Common` alias name
    (`Vector3f` → `System.Numerics.Vector3` at use sites). Otherwise the
    normal cross-class lookup applies — the C emitter's
    `_resolve_handle_short` maps the short spelling (e.g. `ApngFrameInfo`)
    to the bound JS name (`PngApngFrameInfo`)."""
    info = _math_info(cpp_text)
    if info is not None:
        return info[0]
    resolved = _resolve_handle_short(cpp_text, module)
    for c in module.classes:
        if _c_handle_name(c.js_name) == resolved:
            return _cs_type_name(c.js_name, module)
    return _strip_prefix(resolved, module.js_prefix)


# ── Public-method parameters ───────────────────────────────────────────────

def _public_param(p: BindMethodParam, module: BindModule) -> tuple[str, str, str] | None:
    """(C# parameter decl, native arg expression, optional prologue stmt).

    `native arg expression` is what we pass into NativeMethods.* at the
    call site. The prologue is empty for trivial conversions and non-empty
    when we need a temporary variable (e.g. enum cast, bool→int)."""
    t = p.type
    pname = p.name

    if p.span_scalar is not None:
        if p.span_scalar[0] not in ('u8', 'unsigned char'):
            return None
        return (f'ReadOnlySpan<byte> {pname}',
                f'{pname}, (nuint){pname}.Length', '')

    managed_iface = _trampolined_interface(p)
    if managed_iface is not None:
        return (f'{managed_iface} {pname}',
                f'{pname}.DangerousGet()', '')
    if _is_interface_pointer_param(p):
        return None

    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        cs = _CS_PRIMITIVE.get(short)
        if cs is None:
            return None
        if short == 'bool':
            # public bool → native int32
            return (f'bool {pname}',
                    f'{pname} ? 1 : 0', '')
        return (f'{cs} {pname}', pname, '')

    if t.kind == TypeKind.ENUM:
        name = _resolve_enum_cs_name(t.cpp_text, module)
        if name is None:
            return None
        return (f'{name} {pname}', f'(int){pname}', '')

    if t.kind == TypeKind.STRING:
        return (f'string {pname}', pname, '')

    if _is_byte_vector_param(p):
        return (f'ReadOnlySpan<byte> {pname}',
                f'{pname}, (nuint){pname}.Length', '')

    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        if t.cpp_text.startswith('std::'):
            return None
        if not _is_known_handle(t.cpp_text, module):
            return None
        name = _wrapper_name(t.cpp_text, module)
        return (f'{name} {pname}', f'{pname}.DangerousGet()', '')

    return None


# ── Method emission ────────────────────────────────────────────────────────

def _can_emit_method(m: BindMethod, c: BindClass, module: BindModule) -> bool:
    if not _is_method_supported(m, module):
        return False
    if _is_vector_string_return(m):
        # Lowered to (count, at) pair — no other constraints to check.
        return True
    if _native_return(m, module) is None:
        return False
    if _public_return(m, module) is None:
        return False
    for p in m.params:
        if _native_param(p, module) is None:
            return False
        if _public_param(p, module) is None:
            return False
    return True


def _emit_native_method(buf: StringIO, m: BindMethod, c: BindClass,
                       module: BindModule) -> None:
    prefix = _module_prefix(module)
    short = _c_handle_name(c.js_name)
    sym = f'{prefix}_{short}_{m.name}'

    if _is_vector_string_return(m):
        # Lowered pair on the C ABI: count + at.
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial nuint {sym}_count(IntPtr self);\n\n')
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial Whiteout.Common.NativeCString {sym}_at(IntPtr self, nuint index);\n\n')
        return

    ret = _native_return(m, module)

    parts: list[str] = []
    if not m.is_static:
        parts.append('IntPtr self')
    for p in m.params:
        np = _native_param(p, module)
        assert np is not None
        parts.append(np)
    sig = ', '.join(parts)

    buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
    if ret == 'int' and _native_return_is_bool(m):
        # Special-case bool returns: MarshalAs on the return value sits on
        # the method, not the parameter list.
        buf.write(f'    [return: MarshalAs(UnmanagedType.I4)]\n')
    buf.write(f'    internal static partial {ret} {sym}({sig});\n\n')


def _native_return_is_bool(m: BindMethod) -> bool:
    return (m.return_type.kind == TypeKind.PRIMITIVE
            and _short_name(m.return_type.cpp_text) == 'bool')


# ── Getter promotion + getter/setter pairing ──────────────────────────────
#
# C# idiom is to expose accessor pairs as properties. This pass:
#   1. Detects no-arg const methods returning primitive/enum/bool as getters
#      → emit `T Foo { get; }` (read-only property).
#   2. Detects matching setters (`setFoo` taking one arg of type T) → fold
#      into the same property as a setter: `T Foo { get; set; }`.
#
# Methods returning byte[] / string / handles are NOT promoted — they do
# nontrivial work and properties are expected to be cheap.

def _is_getter_candidate(m: BindMethod) -> bool:
    if m.is_static or not m.is_const:
        return False
    if m.params:
        return False
    if m.return_type.kind == TypeKind.PRIMITIVE:
        return True
    if m.return_type.kind == TypeKind.ENUM:
        return True
    return False


def _matching_setter_name(getter_name: str) -> str:
    """`width` → `setWidth`; `isSrgb` → `setSrgb` (strip the `is` prefix
    and re-capitalise). The C++ side uses both conventions."""
    g = getter_name
    if g[:2] == 'is' and len(g) > 2 and g[2].isupper():
        # isFoo → setFoo (drop "is", keep capitalisation)
        return 'set' + g[2:]
    if g[:3] == 'has' and len(g) > 3 and g[3].isupper():
        return 'set' + g[3:]
    return 'set' + g[:1].upper() + g[1:]


def _is_matching_setter(m: BindMethod, getter: BindMethod) -> bool:
    """A setter for `getter` is non-const, takes exactly one parameter of
    the same type as the getter's return, returns void, and is named per
    `_matching_setter_name`."""
    if m.is_static or m.is_const:
        return False
    if m.return_type.cpp_text != 'void':
        return False
    if len(m.params) != 1:
        return False
    if m.name != _matching_setter_name(getter.name):
        return False
    p = m.params[0]
    # Match by kind + spelling so `bool ↔ bool`, `u32 ↔ u32`, enum ↔ same
    # enum. We don't bother with cross-enum / -primitive coercion.
    return (p.type.kind == getter.return_type.kind
            and _short_name(p.type.cpp_text)
                == _short_name(getter.return_type.cpp_text))


def _property_name(getter_name: str) -> str:
    """`width` → `Width`; `isSrgb` → `IsSrgb`; `hasIssues` → `HasIssues`;
    `frameCount` → `FrameCount`."""
    return _pascal(getter_name)


def _is_matching_overload_setter(m: BindMethod, getter: BindMethod) -> bool:
    """C++-side: same method name, non-const overload taking one parameter
    of the getter's return type, returning void. The IR renames the second
    overload (`format` → `format_overload2`) but preserves `cpp_name` so
    we can match by it."""
    if m is getter:
        return False
    if m.cpp_name != getter.cpp_name:
        return False
    if m.is_static or m.is_const:
        return False
    if m.return_type.cpp_text != 'void':
        return False
    if len(m.params) != 1:
        return False
    p = m.params[0]
    return (p.type.kind == getter.return_type.kind
            and _short_name(p.type.cpp_text)
                == _short_name(getter.return_type.cpp_text))


def _pair_properties(methods: list[BindMethod], module: BindModule
                     ) -> tuple[list[tuple[BindMethod, BindMethod | None, str]], set[int]]:
    """Walk the method list and identify getter / setter pairs.

    Returns (props, consumed_method_ids):
      - props: list of (getter, setter_or_None, property_name) tuples
      - consumed_method_ids: id() of every method already represented as a
        property, so the regular method emitter can skip them.

    Two pairing rules — same-cpp_name overloads first (highest signal),
    then the `setFoo` naming convention:

      1. `T foo() const` + `void foo(T)`         (overload pair)
      2. `T foo() const` + `void setFoo(T)`      (Java-bean convention)
      3. `T isFoo() const` + `void setFoo(T)`    (boolean convention)
    """
    props: list[tuple[BindMethod, BindMethod | None, str]] = []
    consumed: set[int] = set()
    methods_by_name = {m.name: m for m in methods}

    for m in methods:
        if id(m) in consumed:
            continue
        if not _is_getter_candidate(m):
            continue
        if _public_return(m, module) is None:
            continue

        # Rule 1: same-cpp_name overload pair.
        overload_setter = None
        for other in methods:
            if id(other) in consumed:
                continue
            if _is_matching_overload_setter(other, m):
                sp = _public_param(other.params[0], module)
                if sp is not None:
                    overload_setter = other
                    break
        if overload_setter is not None:
            prop_name = _pascal(m.cpp_name)
            props.append((m, overload_setter, prop_name))
            consumed.add(id(m))
            consumed.add(id(overload_setter))
            continue

        # Rules 2 + 3: setFoo naming convention.
        setter = methods_by_name.get(_matching_setter_name(m.name))
        if setter is not None and id(setter) not in consumed \
                and _is_matching_setter(setter, m):
            sp = _public_param(setter.params[0], module)
            if sp is not None and _public_return(setter, module) == ('void', 'void'):
                props.append((m, setter, _property_name(m.name)))
                consumed.add(id(m))
                consumed.add(id(setter))
                continue

        # Getter-only.
        props.append((m, None, _property_name(m.name)))
        consumed.add(id(m))
    return props, consumed


def _emit_property_from_methods(buf: StringIO, getter: BindMethod,
                                setter: BindMethod | None, prop_name: str,
                                c: BindClass, module: BindModule,
                                native_class: str, override: bool = False) -> None:
    prefix = _module_prefix(module)
    short = _c_handle_name(c.js_name)
    get_sym = f'{native_class}.{prefix}_{short}_{getter.name}'
    pub_ret, ret_tag = _public_return(getter, module)

    if getter.doc:
        _write_xmldoc(buf, getter.doc, indent='    ')

    override_kw = 'override ' if override else ''
    buf.write(f'    public {override_kw}{pub_ret} {prop_name}\n')
    buf.write('    {\n')

    if ret_tag == 'prim':
        buf.write(f'        get => {get_sym}(DangerousGet());\n')
    elif ret_tag == 'bool':
        buf.write(f'        get => {get_sym}(DangerousGet()) != 0;\n')
    elif ret_tag.startswith('enum:'):
        enum_name = ret_tag[len('enum:'):]
        buf.write(f'        get => ({enum_name}){get_sym}(DangerousGet());\n')
    else:
        # Belt-and-suspenders: _is_getter_candidate gates these tags out.
        buf.write(f'        get => default!;\n')

    if setter is not None:
        set_sym = f'{native_class}.{prefix}_{short}_{setter.name}'
        sp = _public_param(setter.params[0], module)
        assert sp is not None
        _, native_arg_expr, _ = sp
        # The native_arg_expr is computed off the parameter's declared name
        # (e.g. `srgb` or `value`); substitute the C# property's `value`
        # keyword in.
        value_expr = native_arg_expr.replace(setter.params[0].name, 'value')
        buf.write(f'        set => {set_sym}(DangerousGet(), {value_expr});\n')

    buf.write('    }\n\n')


def _emit_public_method(buf: StringIO, m: BindMethod, c: BindClass,
                       module: BindModule, native_class: str, override: bool = False) -> None:
    prefix = _module_prefix(module)
    short = _c_handle_name(c.js_name)
    sym = f'{prefix}_{short}_{m.name}'

    if _is_vector_string_return(m):
        cs_prop_name = _pascal(m.name)
        if cs_prop_name.startswith('Get') and len(cs_prop_name) > 3 \
                and cs_prop_name[3].isupper():
            # `getIssues` → property `Issues` (drop the `Get` prefix).
            cs_prop_name = cs_prop_name[3:]
        cnt_sym = f'{native_class}.{sym}_count'
        at_sym  = f'{native_class}.{sym}_at'
        if m.doc:
            _write_xmldoc(buf, m.doc, indent='    ')
        buf.write(f'    public IReadOnlyList<string> {cs_prop_name} =>\n')
        buf.write(f'        new NativeListView<string>(\n')
        buf.write(f'            DangerousGet(),\n')
        buf.write(f'            {cnt_sym},\n')
        buf.write(f'            (h, i) => {at_sym}(h, i).ToManagedString());\n\n')
        return

    pub_ret, ret_tag = _public_return(m, module)

    public_params: list[str] = []
    native_args: list[str] = []
    prologue: list[str] = []
    for p in m.params:
        pp = _public_param(p, module)
        assert pp is not None
        decl, arg, pre = pp
        public_params.append(decl)
        native_args.append(arg)
        if pre:
            prologue.append(pre)

    cs_name = _pascal(m.name)
    static_kw = 'static ' if m.is_static else ''
    self_arg = 'DangerousGet()' if not m.is_static else ''
    call_args = [self_arg] + native_args if self_arg else native_args
    call = f'{native_class}.{sym}({", ".join(a for a in call_args if a)})'

    if m.doc:
        _write_xmldoc(buf, m.doc, indent='    ')

    # `Close` / `Dispose` shadow members of the SafeHandle base — quiet
    # the CS0108 warning with the `new` keyword. Both are domain methods
    # on the C++ side (e.g. `mpq::Storage::close()` releases the archive
    # file); we don't want them to literally call Dispose.
    new_kw = 'new ' if not m.is_static and cs_name in _SAFE_HANDLE_RESERVED else ''
    override_kw = 'override ' if override else ''

    sig = ', '.join(public_params)
    buf.write(f'    public {override_kw}{new_kw}{static_kw}{pub_ret} {cs_name}({sig})\n')
    buf.write('    {\n')
    for line in prologue:
        buf.write(f'        {line}\n')

    if ret_tag == 'void':
        buf.write(f'        {call};\n')
    elif ret_tag == 'prim':
        buf.write(f'        return {call};\n')
    elif ret_tag == 'bool':
        # native int32 → bool
        buf.write(f'        return {call} != 0;\n')
    elif ret_tag.startswith('enum:'):
        enum_name = ret_tag[len('enum:'):]
        buf.write(f'        return ({enum_name}){call};\n')
    elif ret_tag == 'bytes':
        buf.write(f'        var __b = {call};\n')
        buf.write(f'        return __b.ToManagedArray();\n')
    elif ret_tag == 'bytes?':
        buf.write(f'        var __b = {call};\n')
        buf.write(f'        return __b.IsEmpty ? null : __b.ToManagedArray();\n')
    elif ret_tag == 'string':
        buf.write(f'        var __s = {call};\n')
        buf.write(f'        return __s.ToManagedString();\n')
    elif ret_tag == 'string?':
        buf.write(f'        var __s = {call};\n')
        buf.write(f'        return __s.IsEmpty ? null : __s.ToManagedString();\n')
    elif ret_tag.startswith('handle:'):
        name = ret_tag[len('handle:'):]
        buf.write(f'        var __h = {call};\n')
        buf.write(f'        return new {name}(__h);\n')
    elif ret_tag.startswith('handle?:'):
        name = ret_tag[len('handle?:'):]
        buf.write(f'        var __h = {call};\n')
        buf.write(f'        return __h == IntPtr.Zero ? null : new {name}(__h);\n')
    else:
        buf.write(f'        // UNHANDLED: {ret_tag}\n')

    buf.write('    }\n\n')


# ── Field emission ─────────────────────────────────────────────────────────

def _can_emit_field(f: BindField, module: BindModule) -> bool:
    """Subset of the C ABI's field shapes we know how to lower in v1.
    Handle-typed (single Class) fields and byte-vector fields still need
    non-owning wrapper plumbing / span-setter support; std::vector<Class>
    fields are now lowered to IReadOnlyList<T> via the (count, at) pair."""
    known = _known_class_short_names(module)
    if not _is_field_supported(f, module, known):
        return False
    t = f.type
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        return short in _CS_NATIVE_PRIMITIVE
    if t.kind == TypeKind.ENUM:
        return _resolve_enum_cs_name(t.cpp_text, module) is not None
    if t.kind == TypeKind.STRING:
        return True
    if _is_vector_class_field(f, module):
        return True
    if _is_vector_math_field(f):
        return True
    if _is_scalar_math_field(f):
        return True
    return False


def _is_scalar_math_field(f: BindField) -> bool:
    """Scalar math-type field (Extent.minimum: Vector3f, Layer.fresnelColor:
    Vector3f, ...) — lowered to a `System.Numerics.Vector3` property whose
    getter reads the C++ struct via `Unsafe.Read<T>` after a single native
    `_get` call, and whose setter allocates a temp via `whiteout_Vector3f_new`,
    writes via `Unsafe.Write<T>`, then calls the native `_set`."""
    t = f.type
    if t.kind not in (TypeKind.NESTED, TypeKind.UNKNOWN):
        return False
    if t.cpp_text.startswith('std::'):
        return False
    return _math_info(t.cpp_text) is not None


def _is_vector_class_field(f: BindField, module: BindModule) -> bool:
    """`std::vector<Class>` field where Class is a bound handle type and
    NOT a math type. The C ABI exposes these via `_get_<field>_count()` +
    `_get_<field>_at(i)` returning a borrowed handle into the parent's
    storage. We lower the pair to a read-only IReadOnlyList<T>."""
    t = f.type
    if t.kind != TypeKind.VECTOR:
        return False
    e = t.element
    if e.kind not in (TypeKind.NESTED, TypeKind.UNKNOWN):
        return False
    if e.cpp_text.startswith('std::'):
        return False
    if _math_info(e.cpp_text) is not None:
        return False
    return _is_known_handle(e.cpp_text, module)


def _is_vector_math_field(f: BindField) -> bool:
    """`std::vector<MathType>` field. The C ABI exposes these via
    `_get_<field>_count()` + `_get_<field>_data()` returning a raw `const
    float*` pointer into the contiguous storage. We lower the pair to a
    zero-copy `ReadOnlySpan<T>` (T = System.Numerics.Vector3 etc.)."""
    t = f.type
    if t.kind != TypeKind.VECTOR:
        return False
    return _math_info(t.element.cpp_text) is not None


def _field_native_type(f: BindField) -> tuple[str, str]:
    """(native C# type for LibraryImport, native return shape for getter)."""
    t = f.type
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        return _CS_NATIVE_PRIMITIVE[short], 'prim' if short != 'bool' else 'bool'
    if t.kind == TypeKind.ENUM:
        return 'int', 'enum'
    if t.kind == TypeKind.STRING:
        return 'Whiteout.Common.NativeCString', 'string'
    raise AssertionError(f'unreachable: {t.kind} {t.cpp_text}')


def _field_public_type(f: BindField, module: BindModule) -> str:
    t = f.type
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        return _CS_PRIMITIVE[short]
    if t.kind == TypeKind.ENUM:
        return _resolve_enum_cs_name(t.cpp_text, module)
    if t.kind == TypeKind.STRING:
        return 'string'
    raise AssertionError(f'unreachable: {t.kind} {t.cpp_text}')


def _emit_native_field(buf: StringIO, f: BindField, c: BindClass,
                       module: BindModule) -> None:
    """Emit LibraryImport stubs for the field's get/set pair (scalar
    fields) or count/at pair (vector<class> fields)."""
    prefix = _module_prefix(module)
    short = _c_handle_name(c.js_name)

    if _is_vector_class_field(f, module):
        # std::vector<Class> — emit count + at as IntPtr-returning stubs.
        cnt_sym = f'{prefix}_{short}_get_{f.name}_count'
        at_sym  = f'{prefix}_{short}_get_{f.name}_at'
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial nuint {cnt_sym}(IntPtr self);\n\n')
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial IntPtr {at_sym}(IntPtr self, nuint index);\n\n')
        return

    if _is_vector_math_field(f):
        # std::vector<MathType> — emit count + data as a raw pointer pair
        # so the wrapper can build a zero-copy ReadOnlySpan<MathType>.
        cnt_sym  = f'{prefix}_{short}_get_{f.name}_count'
        data_sym = f'{prefix}_{short}_get_{f.name}_data'
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial nuint {cnt_sym}(IntPtr self);\n\n')
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial IntPtr {data_sym}(IntPtr self);\n\n')
        return

    if _is_scalar_math_field(f):
        # Scalar math field — get returns a fresh whiteout_<Type>* (owned),
        # set takes a borrowed whiteout_<Type>*. We marshal via Unsafe in
        # the property body; here we just declare the IntPtr stubs.
        get_sym = f'{prefix}_{short}_get_{f.name}'
        set_sym = f'{prefix}_{short}_set_{f.name}'
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial IntPtr {get_sym}(IntPtr self);\n\n')
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial void {set_sym}(IntPtr self, IntPtr value);\n\n')
        return

    native_t, _ = _field_native_type(f)
    get_sym = f'{prefix}_{short}_get_{f.name}'
    set_sym = f'{prefix}_{short}_set_{f.name}'

    buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
    buf.write(f'    internal static partial {native_t} {get_sym}(IntPtr self);\n\n')

    setter_param_t = native_t
    if f.type.kind == TypeKind.STRING:
        setter_param_t = '[MarshalAs(UnmanagedType.LPUTF8Str)] string'
    buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
    buf.write(f'    internal static partial void {set_sym}(IntPtr self, {setter_param_t} value);\n\n')


def _emit_public_property(buf: StringIO, f: BindField, c: BindClass,
                          module: BindModule, native_class: str) -> None:
    prefix = _module_prefix(module)
    short = _c_handle_name(c.js_name)
    cs_name = _pascal(f.name)

    if f.doc:
        _write_xmldoc(buf, f.doc, indent='    ')

    if _is_vector_class_field(f, module):
        elem_name = _wrapper_name(f.type.element.cpp_text, module)
        cnt_sym = f'{native_class}.{prefix}_{short}_get_{f.name}_count'
        at_sym  = f'{native_class}.{prefix}_{short}_get_{f.name}_at'
        buf.write(f'    public IReadOnlyList<{elem_name}> {cs_name} =>\n')
        buf.write(f'        new NativeListView<{elem_name}>(\n')
        buf.write(f'            DangerousGet(),\n')
        buf.write(f'            {cnt_sym},\n')
        buf.write(f'            (h, i) => new {elem_name}({at_sym}(h, i), owned: false));\n\n')
        return

    if _is_vector_math_field(f):
        elem_name = _wrapper_name(f.type.element.cpp_text, module)
        cnt_sym  = f'{native_class}.{prefix}_{short}_get_{f.name}_count'
        data_sym = f'{native_class}.{prefix}_{short}_get_{f.name}_data'
        buf.write(f'    public unsafe ReadOnlySpan<{elem_name}> {cs_name}\n')
        buf.write(f'    {{\n')
        buf.write(f'        get\n')
        buf.write(f'        {{\n')
        buf.write(f'            var __count = checked((int){cnt_sym}(DangerousGet()));\n')
        buf.write(f'            var __ptr = {data_sym}(DangerousGet());\n')
        buf.write(f'            return new ReadOnlySpan<{elem_name}>((void*)__ptr, __count);\n')
        buf.write(f'        }}\n')
        buf.write(f'    }}\n\n')
        return

    if _is_scalar_math_field(f):
        # C ABI scalar math getter returns a *borrowed* pointer into the
        # parent's storage (not a fresh allocation) — read the bytes via
        # Unsafe.Read; DO NOT free. Setter takes a borrowed pointer too,
        # so we allocate a temporary, populate it, hand it to the setter
        # (which copies in), then free our temp.
        math_name, _ = _math_info(f.type.cpp_text)
        get_sym = f'{native_class}.{prefix}_{short}_get_{f.name}'
        set_sym = f'{native_class}.{prefix}_{short}_set_{f.name}'
        alloc_sym = f'Whiteout.Common.NativeMath.whiteout_{math_name}_new'
        free_sym  = f'Whiteout.Common.NativeMath.whiteout_{math_name}_delete'
        buf.write(f'    public unsafe {math_name} {cs_name}\n')
        buf.write(f'    {{\n')
        buf.write(f'        get\n')
        buf.write(f'        {{\n')
        buf.write(f'            var __ptr = {get_sym}(DangerousGet());\n')
        buf.write(f'            return System.Runtime.CompilerServices.Unsafe.Read<{math_name}>((void*)__ptr);\n')
        buf.write(f'        }}\n')
        buf.write(f'        set\n')
        buf.write(f'        {{\n')
        buf.write(f'            var __ptr = {alloc_sym}();\n')
        buf.write(f'            try\n')
        buf.write(f'            {{\n')
        buf.write(f'                System.Runtime.CompilerServices.Unsafe.Write((void*)__ptr, value);\n')
        buf.write(f'                {set_sym}(DangerousGet(), __ptr);\n')
        buf.write(f'            }}\n')
        buf.write(f'            finally\n')
        buf.write(f'            {{\n')
        buf.write(f'                {free_sym}(__ptr);\n')
        buf.write(f'            }}\n')
        buf.write(f'        }}\n')
        buf.write(f'    }}\n\n')
        return

    pub_t = _field_public_type(f, module)
    get_sym = f'{native_class}.{prefix}_{short}_get_{f.name}'
    set_sym = f'{native_class}.{prefix}_{short}_set_{f.name}'

    buf.write(f'    public {pub_t} {cs_name}\n')
    buf.write('    {\n')

    if f.type.kind == TypeKind.PRIMITIVE:
        short_t = _short_name(f.type.cpp_text)
        if short_t == 'bool':
            buf.write(f'        get => {get_sym}(DangerousGet()) != 0;\n')
            buf.write(f'        set => {set_sym}(DangerousGet(), value ? 1 : 0);\n')
        else:
            buf.write(f'        get => {get_sym}(DangerousGet());\n')
            buf.write(f'        set => {set_sym}(DangerousGet(), value);\n')
    elif f.type.kind == TypeKind.ENUM:
        buf.write(f'        get => ({pub_t}){get_sym}(DangerousGet());\n')
        buf.write(f'        set => {set_sym}(DangerousGet(), (int)value);\n')
    elif f.type.kind == TypeKind.STRING:
        buf.write(f'        get => {get_sym}(DangerousGet()).ToManagedString();\n')
        buf.write(f'        set => {set_sym}(DangerousGet(), value);\n')

    buf.write('    }\n\n')


# ── Top-level emission ─────────────────────────────────────────────────────

def emit(module: BindModule) -> dict[str, str]:
    files: dict[str, str] = {}
    out_dir = f'bindings/csharp/Whiteout/{_pascal(module.name)}'

    files[f'{out_dir}/NativeMethods.{_pascal(module.name)}.cs'] = _emit_native_methods(module)

    for e in module.enums:
        name = _cs_type_name(e.js_name, module)
        files[f'{out_dir}/{name}.cs'] = _emit_enum(e, name, module)

    for c in module.classes:
        # Math types (Vector3f, Quaternion, ...) are aliased to
        # System.Numerics value-type structs in Whiteout.Common — skip
        # generating a SafeHandle wrapper for them.
        if _math_info(c.js_name) is not None or _math_info(c.cpp_qualifier) is not None:
            continue
        # Module config can explicitly skip a class (`skip_class_js_names`)
        # when a hand-written C# type owns the binding — e.g. the host
        # module's trampoline base classes live under Whiteout/Host/.
        if c.js_name in module.skip_class_js_names:
            continue
        # value_object types map to C# structs ideally, but until the
        # layout-parity test confirms struct field offsets we emit them
        # as SafeHandle wrappers (the C ABI exposes them via handles
        # regardless). Bringing them in keeps cross-class refs resolvable.
        name = _cs_type_name(c.js_name, module)
        files[f'{out_dir}/{name}.cs'] = _emit_class(c, name, module)

    return files


def emit_common() -> dict[str, str]:
    return {}


def _emit_enum(e: BindEnum, cs_name: str, module: BindModule) -> str:
    buf = StringIO()
    _write_header(buf)
    buf.write(f'namespace {_ns(module)};\n\n')
    if e.doc:
        _write_xmldoc(buf, e.doc, indent='')
    buf.write(f'public enum {cs_name} : int\n')
    buf.write('{\n')
    for v in e.values:
        if v.doc:
            _write_xmldoc(buf, v.doc, indent='    ')
        buf.write(f'    {_pascal(v.js_name)} = {v.value},\n')
    buf.write('}\n')
    return buf.getvalue()


def _emit_class(c: BindClass, cs_name: str, module: BindModule) -> str:
    short = _c_handle_name(c.js_name)
    prefix = _module_prefix(module)
    nm = 'NativeMethods'

    # If the C++ class derives from a trampolined interface, emit a C#
    # class that inherits from the managed base. Constructors call the
    # base's native-handle ctor; abstract methods we don't have a C ABI
    # for get throwing-stub overrides; methods we DO have a C ABI for get
    # `override` keyword.
    tramp_base = _trampolined_base_for_class(c)
    base_short = tramp_base.split('.')[-1] if tramp_base else None
    base_members = _TRAMPOLINED_BASE_MEMBERS.get(tramp_base, {}) if tramp_base else {}

    buf = StringIO()
    _write_header(buf)
    buf.write('using System.Runtime.InteropServices;\n')
    buf.write('using Whiteout.Common;\n')
    buf.write(f'using Whiteout.{_pascal(module.name)}.Internal;\n\n')
    buf.write(f'namespace {_ns(module)};\n\n')
    if c.doc:
        _write_xmldoc(buf, c.doc, indent='')

    base_clause = tramp_base if tramp_base else 'WhiteoutHandle'
    buf.write(f'public sealed class {cs_name} : {base_clause}\n')
    buf.write('{\n')

    # Constructors. When inheriting from a trampolined base, every ctor
    # forwards (handle, owned) to the base's native-handle ctor instead
    # of WhiteoutHandle directly.
    if not c.no_default_ctor:
        ctor_call = f'{nm}.{prefix}_{short}_new()'
        if tramp_base:
            buf.write(f'    public {cs_name}() : base({ctor_call}, owned: true) {{ }}\n\n')
        else:
            buf.write(f'    public {cs_name}() : base({ctor_call}) {{ }}\n\n')
    for ctor, sig_name, _c_params in _ctor_overloads_named(c, module):
        cs_params, cs_args = _cs_ctor_params(ctor, module)
        if cs_params is None:
            continue
        ctor_sym = f'{nm}.{prefix}_{short}_new_{sig_name}'
        if tramp_base:
            buf.write(f'    public {cs_name}({cs_params}) : base({ctor_sym}({cs_args}), owned: true) {{ }}\n\n')
        else:
            buf.write(f'    public {cs_name}({cs_params}) : base({ctor_sym}({cs_args})) {{ }}\n\n')
    if tramp_base:
        buf.write(f'    internal {cs_name}(IntPtr handle, bool owned = true) : base(handle, owned) {{ }}\n\n')
    else:
        buf.write(f'    internal {cs_name}(IntPtr handle, bool owned = true) : base(handle, owned) {{ }}\n\n')

    buf.write('    protected override bool ReleaseHandle()\n')
    buf.write('    {\n')
    buf.write(f'        {nm}.{prefix}_{short}_delete(handle);\n')
    buf.write('        return true;\n')
    buf.write('    }\n')

    for f in c.fields:
        if not _can_emit_field(f, module):
            continue
        buf.write('\n')
        _emit_public_property(buf, f, c, module, native_class=nm)

    # Lift no-arg const getters (and their matching setters) into C#
    # properties before emitting the remaining methods as methods.
    emittable_methods = [m for m in c.methods if _can_emit_method(m, c, module)]
    props, consumed = _pair_properties(emittable_methods, module)
    matched_base_members: set[str] = set()
    for getter, setter, prop_name in props:
        buf.write('\n')
        # If a promoted property's getter cpp_name matches a base member
        # whose kind is 'prop', emit it as `override`.
        is_override = base_members.get(getter.cpp_name) == 'prop'
        if is_override:
            matched_base_members.add(getter.cpp_name)
        _emit_property_from_methods(buf, getter, setter, prop_name, c, module,
                                    native_class=nm, override=is_override)

    for m in emittable_methods:
        if id(m) in consumed:
            continue
        buf.write('\n')
        is_override = base_members.get(m.cpp_name) == 'method'
        if is_override:
            matched_base_members.add(m.cpp_name)
        _emit_public_method(buf, m, c, module, native_class=nm, override=is_override)

    # Emit throwing stubs for any base abstract member the C ABI didn't
    # provide. These are unreachable in practice — the C++ virtual goes
    # directly to the concrete impl without crossing back to managed
    # code — but C# requires concrete classes to satisfy every abstract.
    if base_short is not None:
        for member, _kind in base_members.items():
            if member in matched_base_members:
                continue
            stub = _THROWING_STUBS.get((base_short, member))
            if stub is None:
                continue
            buf.write('\n')
            buf.write(f'    public override {stub}\n')

    buf.write('}\n')
    return buf.getvalue()


def _emit_native_methods(module: BindModule) -> str:
    prefix = _module_prefix(module)
    buf = StringIO()
    _write_header(buf)
    buf.write('using System.Runtime.InteropServices;\n\n')
    buf.write(f'namespace Whiteout.{_pascal(module.name)}.Internal;\n\n')
    buf.write('internal static partial class NativeMethods\n')
    buf.write('{\n')

    first = True
    for c in module.classes:
        if _math_info(c.js_name) is not None or _math_info(c.cpp_qualifier) is not None:
            # Math types are handled via Unsafe.Read/Write — no per-class
            # new/delete stubs needed here.
            continue
        short = _c_handle_name(c.js_name)
        if not first:
            buf.write('\n')
        first = False

        if not c.no_default_ctor:
            buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
            buf.write(f'    internal static partial IntPtr {prefix}_{short}_new();\n\n')
        # Non-default constructors (one per overload).
        for ctor, sig_name, _c_params in _ctor_overloads_named(c, module):
            cs_native_params = _native_ctor_params(ctor, module)
            if cs_native_params is None:
                continue
            buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
            buf.write(f'    internal static partial IntPtr {prefix}_{short}_new_{sig_name}({cs_native_params});\n\n')
        buf.write(f'    [LibraryImport(Runtime.LibraryName)]\n')
        buf.write(f'    internal static partial void {prefix}_{short}_delete(IntPtr self);\n')

        for f in c.fields:
            if not _can_emit_field(f, module):
                continue
            buf.write('\n')
            _emit_native_field(buf, f, c, module)

        for m in c.methods:
            if not _can_emit_method(m, c, module):
                continue
            buf.write('\n')
            _emit_native_method(buf, m, c, module)

    buf.write('}\n')
    return buf.getvalue()


# ── Helpers ────────────────────────────────────────────────────────────────

def _write_header(buf: StringIO) -> None:
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// Copyright (c) 2026 Fernando Sahmkow\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_csharp.py — DO NOT EDIT.\n\n')


def _write_xmldoc(buf: StringIO, doc: str, indent: str) -> None:
    for line in doc.splitlines():
        line = line.rstrip()
        if not line:
            continue
        buf.write(f'{indent}/// <summary>{_xml_escape(line)}</summary>\n')
        return


def _xml_escape(s: str) -> str:
    return (s.replace('&', '&amp;')
             .replace('<', '&lt;')
             .replace('>', '&gt;'))
