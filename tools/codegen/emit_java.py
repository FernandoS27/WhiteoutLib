# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> idiomatic Java FFM bindings.

The output is a single Java package â€” `whiteout.<module>` â€” with one
.java file per generated type:

  whiteout/textures/Native.java           // private: SymbolLookup + handles
  whiteout/textures/PixelFormat.java      // enum
  whiteout/textures/AttributeClass.java   // enum
  whiteout/textures/Texture.java          // AutoCloseable class
  whiteout/textures/BlpParser.java        // AutoCloseable class
  whiteout/textures/PngParser.java
  ...

Each class:
  - Implements `java.lang.AutoCloseable`.
  - Holds an opaque `MemorySegment` handle.
  - Routes calls through the `Native` class which owns the
    `Linker`/`MethodHandle`s for the whole module.

This codegen targets JDK 22+ (stable FFM). No external tools (jextract)
needed â€” we already understand the C++ surface via libclang.
"""

from __future__ import annotations

from io import StringIO
from .ir import (
    BindClass, BindEnum, BindField, BindMethod, BindMethodParam, BindModule,
    TypeKind, TypeRef,
)
from .parser import _short_name
from .emit_c import (
    _c_handle_name as _c_handle_name_ext,
    _is_method_supported as _is_method_supported_c,
    _is_field_supported as _is_field_supported_c,
    _known_class_short_names,
    _resolve_handle_short,
    _is_span_const_u8,
    _is_bulk_flat_inner,
    _bulk_components,
    _SHARED_MATH_TYPES,
    _SHARED_MATH_BYTE_SIZES,
)


# ── Direct-memory layout helpers ──────────────────────────────────────────
#
# When libclang gave us a field's byte_offset (and the class's byte_size),
# we can read/write that field straight out of the MemorySegment via
# `handle.get(LAYOUT, offset)` / `handle.set(LAYOUT, offset, value)`,
# bypassing the FFM downcall through the C wrapper. The JIT lowers these
# to a single load/store instruction.
#
# Eligibility is per-FIELD, not per-class: a Sequence whose `name`
# std::string we can't touch still gets direct access for its `u32` fields.

_PRIMITIVE_DIRECT = {
    'u8':  ('byte',   'ValueLayout.JAVA_BYTE'),
    'i8':  ('byte',   'ValueLayout.JAVA_BYTE'),
    'u16': ('short',  'ValueLayout.JAVA_SHORT'),
    'i16': ('short',  'ValueLayout.JAVA_SHORT'),
    'u32': ('int',    'ValueLayout.JAVA_INT'),
    'i32': ('int',    'ValueLayout.JAVA_INT'),
    'u64': ('long',   'ValueLayout.JAVA_LONG'),
    'i64': ('long',   'ValueLayout.JAVA_LONG'),
    'f32': ('float',  'ValueLayout.JAVA_FLOAT'),
    'f64': ('double', 'ValueLayout.JAVA_DOUBLE'),
    'bool':('byte',   'ValueLayout.JAVA_BYTE'),
}


def _primitive_direct_info(t: TypeRef) -> tuple[str, str] | None:
    """For a PRIMITIVE TypeRef, return `(java_type, ValueLayout)` for direct
    segment access. `None` for types we can't safely lay out (e.g. size_t
    whose width varies)."""
    short = _short_name(t.cpp_text)
    return _PRIMITIVE_DIRECT.get(short)


def _nested_byte_size(t: TypeRef, module: BindModule) -> int | None:
    """Byte size of a NESTED type, for direct sub-slicing. Looks first
    in the shared math table (those sizes are fixed) and then in the
    current module's IR (libclang-computed)."""
    short = _resolve_handle_short(t.cpp_text, module)
    if short in _SHARED_MATH_BYTE_SIZES:
        return _SHARED_MATH_BYTE_SIZES[short]
    for c in module.classes:
        if _c_handle_name_ext(c.js_name) == short and c.byte_size:
            return c.byte_size
    return None


def _construct_handle_wrap(java_cls: str, seg_expr: str, owned: str) -> str:
    """Construct a handle-backed wrapper from a MemorySegment.
    Per-module classes only — `whiteout.common.internal.Handles` is
    the entry point for shared math types because their package-
    private constructors are reached via reflection, not direct
    name access."""
    return f'new {java_cls}({seg_expr}, {owned})'


def _java_ctor_param_info(p) -> tuple[str, str, str, str] | None:
    """For a constructor parameter, return
    `(java_type, layout, marshal_setup, native_arg)`.

    - `java_type`     — what users see at the Java call site (`String`, `long`, …)
    - `layout`        — the FunctionDescriptor value layout
                        (`ValueLayout.ADDRESS`, `ValueLayout.JAVA_LONG`, …)
    - `marshal_setup` — Java statement(s) that turn the user-facing param
                        into the call-site argument; empty when no setup
                        is needed
    - `native_arg`    — the expression passed to `MethodHandle.invoke`

    Returns `None` for types the Java side can't surface as a constructor
    parameter (e.g. raw pointers, classes / vectors without a known
    wrapper). The caller skips such ctors when emitting factories.
    """
    if p.type.kind == TypeKind.STRING:
        # const char* on the C side; Java users pass a String. Allocate
        # off a confined arena so the C++ ctor (called inside `invoke`)
        # sees a NUL-terminated UTF-8 buffer that doesn't survive the
        # call. UTF-8 matches whiteout's path-handling convention.
        return ('String',
                'ValueLayout.ADDRESS',
                f'MemorySegment __seg_{p.name} = __arena.allocateFrom('
                f'{p.name}, StandardCharsets.UTF_8);',
                f'__seg_{p.name}')
    if p.type.kind == TypeKind.PRIMITIVE:
        from .parser import _short_name as _sn
        short = _sn(p.type.cpp_text)
        if short in _PRIMITIVE_DIRECT:
            java_t, layout = _PRIMITIVE_DIRECT[short]
            return (java_t, layout, '', p.name)
    return None


def _nested_is_pod(t: TypeRef, module: BindModule) -> bool:
    """True when a NESTED field's target type is bitwise-copyable. POD
    types can use direct memcpy in setters; non-POD must route through
    the C wrapper so the C++ copy-assignment runs (calling destructors,
    deep-copying owned `std::string`/`std::vector` buffers, etc.).
    Shared math types are POD by construction."""
    short = _resolve_handle_short(t.cpp_text, module)
    if short in _SHARED_MATH_BYTE_SIZES:
        return True
    for c in module.classes:
        if _c_handle_name_ext(c.js_name) == short:
            return c.is_pod
    return False


def _bulk_component_short(t: TypeRef) -> str:
    """Short scalar name (`u8`/`f32`/`i32`/…) for each component of a
    bulk-flat vector element. Vectors of Vector*/Quaternion split into
    a flat `float` strip; vectors of enums marshal as int32."""
    if t.kind == TypeKind.PRIMITIVE:
        return _short_name(t.cpp_text)
    if t.kind == TypeKind.ENUM:
        return 'i32'
    return 'f32'


# Shared math types live in `whiteout.common` so cross-module Java
# files import them under one canonical name.
_COMMON_PKG = 'whiteout.common'
# Marshalling / FFM-handle plumbing for shared math lives one level
# deeper, in a JPMS-internal package that's NOT exported by
# module-info.java. User code on the classpath or modulepath can't
# reach types in this package, so MemorySegment never leaks out of
# the public surface.
_COMMON_INTERNAL_PKG = 'whiteout.common.internal'


def _module_internal_pkg(module: BindModule) -> str:
    """`whiteout.<module>.internal` — also not exported. Holds the
    per-module FFM-handle registry."""
    return f'whiteout.{module.name}.internal'


def _c_handle_short(cpp_qualifier: str) -> str:
    """`whiteout::textures::png::Parser` -> `Parser`."""
    return cpp_qualifier.split('::')[-1]


# â”€â”€ Type mapping â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

_JAVA_PRIMITIVE = {
    'u8': 'byte',  'u16': 'short', 'u32': 'int',  'u64': 'long',
    'i8': 'byte',  'i16': 'short', 'i32': 'int',  'i64': 'long',
    'f32': 'float', 'f64': 'double', 'bool': 'boolean',
    'unsigned char': 'byte', 'unsigned short': 'short',
    'unsigned int': 'int', 'unsigned long long': 'long',
    'signed char': 'byte', 'short': 'short',
    'int': 'int', 'long long': 'long',
    'float': 'float', 'double': 'double', 'char': 'byte',
}


def _java_primitive(t: TypeRef) -> str:
    short = _short_name(t.cpp_text)
    return _JAVA_PRIMITIVE.get(short, 'int')


# Per-element marshalling info for `std::span<const X>` parameters.
# Maps the scalar's short name (as `_SPAN_SCALAR_TABLE` in parser.py
# produces) to (Java element type, FFM layout, byte size). Used to turn
# `byte[] for any span` into properly-typed `float[]`/`int[]`/`long[]`
# bindings.
_SPAN_ELEM_INFO = {
    'u8':  ('byte',   'ValueLayout.JAVA_BYTE',   1),
    'i8':  ('byte',   'ValueLayout.JAVA_BYTE',   1),
    'u16': ('short',  'ValueLayout.JAVA_SHORT',  2),
    'i16': ('short',  'ValueLayout.JAVA_SHORT',  2),
    'u32': ('int',    'ValueLayout.JAVA_INT',    4),
    'i32': ('int',    'ValueLayout.JAVA_INT',    4),
    'u64': ('long',   'ValueLayout.JAVA_LONG',   8),
    'i64': ('long',   'ValueLayout.JAVA_LONG',   8),
    'f32': ('float',  'ValueLayout.JAVA_FLOAT',  4),
    'f64': ('double', 'ValueLayout.JAVA_DOUBLE', 8),
}


def _span_elem(span_scalar: tuple[str, str]) -> tuple[str, str, int]:
    return _SPAN_ELEM_INFO[span_scalar[0]]


def _java_layout(t: TypeRef) -> str:
    """ValueLayout for a primitive used inside FunctionDescriptor."""
    name = _java_primitive(t)
    return {
        'byte': 'ValueLayout.JAVA_BYTE',
        'short': 'ValueLayout.JAVA_SHORT',
        'int': 'ValueLayout.JAVA_INT',
        'long': 'ValueLayout.JAVA_LONG',
        'float': 'ValueLayout.JAVA_FLOAT',
        'double': 'ValueLayout.JAVA_DOUBLE',
        'boolean': 'ValueLayout.JAVA_INT',   # 0/1 over the wire
    }[name]


def _java_strip_prefix(name: str, module: BindModule) -> str:
    """`MdxSequence` in module `mdx` → `Sequence`; the package itself
    disambiguates so the JS-style prefix is just noise in Java. Refuses
    to strip when the result would start with a lowercase letter (which
    would imply we'd swallowed the first capital of a real type name)."""
    p = module.js_prefix
    if p and name.startswith(p):
        rest = name[len(p):]
        if rest and rest[0].isupper():
            return rest
    return name


def _java_handle_map(module: BindModule) -> dict[str, str]:
    """Memoised map of `cpp_text` variants → Java class/enum short name
    (with the module's js_prefix stripped). Covers both classes and
    enums so a TypeRef referencing `Sequence::Flag` resolves to
    `SequenceFlag` rather than the ambiguous `Flag`."""
    cached = getattr(module, '_java_name_map', None)
    if cached is not None:
        return cached
    m: dict[str, str] = {}

    def _add(keys: set[str], java_short: str) -> None:
        for k in keys:
            m[k] = java_short

    # `cpp_qualifier` and its short form aren't included as map keys —
    # they collide across sub-namespaces (every per-format parser has
    # cpp_qualifier "Parser"), and libclang gives us fully-qualified
    # canonical spellings in TypeRef cpp_text anyway. The js_name (which
    # is unique by construction) plus the fully-qualified path are
    # enough to resolve every cross-class reference.
    for c in module.classes:
        java_short = _java_strip_prefix(_c_handle_name_ext(c.js_name), module)
        keys = {_c_handle_name_ext(c.js_name)}
        if c.cpp_namespace:
            keys.add(f'{c.cpp_namespace}::{c.cpp_qualifier}')
        _add(keys, java_short)

    for e in module.enums:
        java_short = _java_strip_prefix(e.js_name, module)
        keys = {e.js_name}
        if e.cpp_namespace:
            keys.add(f'{e.cpp_namespace}::{e.cpp_qualifier}')
        _add(keys, java_short)

    # Shared math types keep their short name (no module prefix in
    # `whiteout.common` to strip in the first place).
    for name in _SHARED_MATH_TYPES:
        m[name] = name
        m[f'whiteout::{name}'] = name

    module._java_name_map = m
    return m


def _resolve_java_name(cpp_text: str, module: BindModule) -> str:
    """Look up the Java class/enum name for a cpp_text. Falls back to
    the C handle short name when the type isn't in this module's IR —
    cross-module references almost always go through shared math, which
    is registered explicitly above."""
    m = _java_handle_map(module)
    if cpp_text in m:
        return m[cpp_text]
    short = _c_handle_name_ext(cpp_text)
    return m.get(short, short)


def _resolve_java_class(cpp_text: str, module: BindModule,
                        classes_in_module: dict[str, str]) -> str:
    """Back-compat shim — keeps the existing call sites compiling
    while the per-module dict argument is being phased out."""
    return _resolve_java_name(cpp_text, module)


# Public Java types for return/parameter signatures.
def _java_type(t: TypeRef, module: BindModule,
               classes_in_module: dict[str, str]) -> str:
    if t.cpp_text == 'void':
        return 'void'
    if t.kind == TypeKind.PRIMITIVE:
        # bool surfaces as boolean on the Java side, even though we ship it
        # as int32 over the wire.
        return _java_primitive(t)
    if t.kind == TypeKind.ENUM:
        return _resolve_java_class(t.cpp_text, module, classes_in_module)
    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        # `std::span<const u8>` surfaces on the Java side as byte[] â€”
        # the C wrapper has already packed it into a whiteout_Bytes view.
        if _is_span_const_u8(t):
            return 'byte[]'
        return _resolve_java_class(t.cpp_text, module, classes_in_module)
    if t.kind == TypeKind.OPTIONAL:
        # std::optional<class> -> java.util.Optional<T> so callers can
        # use the idiomatic ifPresent/orElse/map chain instead of
        # null-checking. Java's primitive Optional* types are skipped
        # because no `std::optional<f32>` etc. reaches our C ABI today.
        if t.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            inner = _java_type(t.element, module, classes_in_module)
            return f'java.util.Optional<{inner}>'
        return _java_type(t.element, module, classes_in_module)
    if t.kind == TypeKind.VECTOR:
        if _short_name(t.element.cpp_text) in ('u8', 'unsigned char'):
            return 'byte[]'
        return 'byte[]'
    return 'Object'


def _param_javadoc_type(p, module: BindModule,
                        classes_in_module: dict[str, str]) -> str:
    """Compute the human-readable Java type spelling for a parameter,
    matching what the signature actually generates. Used in @param tags
    so the doc matches the type the caller sees."""
    if p.span_scalar is not None:
        jt, _layout, _sz = _span_elem(p.span_scalar)
        return f'{jt}[]'
    return _java_type(p.type, module, classes_in_module)


# â”€â”€ File emission â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

# â”€â”€ whiteout.common emit (shared math types) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€


def emit_common_java() -> dict[str, str]:
    """Files for the `whiteout.common` Java package â€” one class per
    shared math type plus the `NativeCommon` FFM glue. Mirrors the C
    side's `whiteout_c_common.{h,cpp}`."""
    from .emit_c import (
        _SHARED_MATH_FIELDS,
        _SHARED_MATH_METHODS,
        _MATRIX_DIMS,
        _SHARED_MATH_FREE_FUNCTIONS,
        _PRIMITIVES_PASSTHRU,
    )

    base_public   = 'bindings/java/src/main/java/whiteout/common'
    base_internal = 'bindings/java/src/main/java/whiteout/common/internal'

    files: dict[str, str] = {
        # Public surface — Vector*, Quaternion, Matrix* — emitted below.

        # Internal — JPMS-hidden plumbing.
        f'{base_internal}/NativeCommon.java': _emit_native_common(),
        f'{base_internal}/Native.java': _emit_common_native_handles(
            _SHARED_MATH_FIELDS, _SHARED_MATH_METHODS, _MATRIX_DIMS,
            _SHARED_MATH_FREE_FUNCTIONS, _PRIMITIVES_PASSTHRU),
        f'{base_internal}/Handles.java': _emit_common_handles(
            list(_SHARED_MATH_FIELDS.keys() | set(_MATRIX_DIMS.keys()))),
    }

    # Free functions get inlined as static methods on their primary
    # type's class (e.g. `whiteout::cross` becomes `Vector3f.cross`).
    # The host is the first argument's type — that's the conceptual
    # "self" of an operation like `cross(a, b)` or `transform_point(v, m)`.
    free_by_owner: dict[str, list] = {}
    for ff in _SHARED_MATH_FREE_FUNCTIONS:
        fname, ret, params = ff
        owner = params[0][0] if params else ret
        free_by_owner.setdefault(owner, []).append(ff)

    for name in _SHARED_MATH_FIELDS.keys() | set(_MATRIX_DIMS.keys()):
        files[f'{base_public}/{name}.java'] = _emit_common_class(
            name, _SHARED_MATH_FIELDS.get(name, []),
            _SHARED_MATH_METHODS.get(name, []),
            _MATRIX_DIMS.get(name),
            _PRIMITIVES_PASSTHRU,
            free_by_owner.get(name, []))

    files['bindings/java/src/main/java/module-info.java'] = _emit_module_info()

    return files


def _emit_common_handles(type_names: list[str]) -> str:
    """`whiteout.common.internal.Handles` — the only place in the
    library that can hand out a Vector3f's underlying MemorySegment or
    construct a Vector3f from one. Uses MethodHandles.privateLookupIn
    at static init to reach the package-private `handle` field and
    `(MemorySegment, boolean)` constructor of each shared math class.

    Because the package is not exported by module-info.java, external
    code can't reference this class — Java's strong encapsulation
    keeps the reflective backdoor module-private."""
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {_COMMON_INTERNAL_PKG};\n\n')
    buf.write('import java.lang.foreign.MemorySegment;\n')
    buf.write('import java.lang.invoke.MethodHandle;\n')
    buf.write('import java.lang.invoke.MethodHandles;\n')
    buf.write('import java.lang.invoke.MethodType;\n')
    buf.write('import java.lang.invoke.VarHandle;\n')
    buf.write(f'import {_COMMON_PKG}.*;\n\n')
    buf.write('public final class Handles {\n')
    buf.write('    private Handles() {}\n\n')

    # Reflect into each shared-math class once at static init. The
    # privateLookupIn lookup works without `opens` because the target
    # class is in the same JPMS module as this Handles class.
    for name in sorted(type_names):
        buf.write(f'    private static final VarHandle VH_{name.upper()};\n')
        buf.write(f'    private static final MethodHandle CTOR_{name.upper()};\n')
    buf.write('    static {\n')
    buf.write('        try {\n')
    buf.write('            var __lookup = MethodHandles.lookup();\n')
    for name in sorted(type_names):
        buf.write(f'            var __p_{name} = MethodHandles.privateLookupIn({name}.class, __lookup);\n')
        buf.write(f'            VH_{name.upper()} = __p_{name}.findVarHandle({name}.class, "handle", MemorySegment.class);\n')
        buf.write(f'            CTOR_{name.upper()} = __p_{name}.findConstructor({name}.class,\n')
        buf.write('                MethodType.methodType(void.class, MemorySegment.class, boolean.class));\n')
    buf.write('        } catch (ReflectiveOperationException __ex) {\n')
    buf.write('            throw new ExceptionInInitializerError(__ex);\n')
    buf.write('        }\n')
    buf.write('    }\n\n')

    for name in sorted(type_names):
        buf.write(f'    /** Borrowed handle to {name}\'s underlying native struct. */\n')
        buf.write(f'    public static MemorySegment segmentOf({name} value) {{\n')
        buf.write(f'        return value == null ? MemorySegment.NULL : (MemorySegment) VH_{name.upper()}.get(value);\n')
        buf.write('    }\n')
        buf.write(f'    /** Wrap a foreign MemorySegment as a {name}. Caller decides ownership. */\n')
        buf.write(f'    public static {name} wrap{name}(MemorySegment seg, boolean owned) {{\n')
        buf.write('        try {\n')
        buf.write(f'            return ({name}) CTOR_{name.upper()}.invoke(seg, owned);\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n\n')
    buf.write('}\n')
    return buf.getvalue()


def _emit_module_info() -> str:
    """`module-info.java` — exports only user-facing packages. The
    `whiteout.<mod>.internal` subpackages stay module-private, hiding
    every FFM detail (MemorySegment, MethodHandle, VarHandle, the
    Handles reflection bridge, ...) from anything outside this module.
    """
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n\n')
    buf.write('module whiteout {\n')
    buf.write('    requires java.base;\n\n')
    buf.write('    // Public Java API — every module-info update should\n')
    buf.write('    // only add `exports` lines, never `opens`.\n')
    buf.write('    exports whiteout.common;\n')
    buf.write('    exports whiteout.textures;\n')
    buf.write('    exports whiteout.mdx;\n')
    buf.write('    exports whiteout.m2;\n')
    buf.write('    exports whiteout.m3;\n')
    buf.write('    exports whiteout.utils;\n')
    buf.write('}\n')
    return buf.getvalue()


def _math_layout(t: str, primitives_passthru: set[str]) -> str:
    if t == 'float':  return 'ValueLayout.JAVA_FLOAT'
    if t == 'int':    return 'ValueLayout.JAVA_INT'
    if t == 'bool':   return 'ValueLayout.JAVA_INT'
    if t == 'size_t': return 'ValueLayout.JAVA_LONG'
    if t in primitives_passthru:
        return 'ValueLayout.JAVA_INT'
    return 'ValueLayout.ADDRESS'


def _math_java_type(t: str, primitives_passthru: set[str]) -> str:
    if t == 'bool': return 'boolean'
    if t == 'size_t': return 'long'
    if t in primitives_passthru: return t
    return t   # math handle types resolve to their class name


def _emit_common_native_handles(fields_map, methods_map, matrix_dims,
                                free_funcs, primitives_passthru) -> str:
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {_COMMON_INTERNAL_PKG};\n\n')
    buf.write('import java.lang.foreign.*;\n')
    buf.write('import java.lang.invoke.MethodHandle;\n\n')
    # Public — so same-module callers in other packages can reach the
    # MethodHandles. Package is not exported, so external code can't
    # see this class at all.
    buf.write('public final class Native {\n')
    buf.write('    private Native() {}\n\n')
    buf.write('    static MethodHandle find(String name, FunctionDescriptor fd) {\n')
    buf.write('        return NativeCommon.find(name, fd);\n')
    buf.write('    }\n\n')

    all_types = sorted(set(fields_map.keys()) | set(matrix_dims.keys()))
    for name in all_types:
        addr = 'ValueLayout.ADDRESS'
        buf.write(f'    // â”€â”€ {name} â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€\n')
        buf.write(f'    public static final MethodHandle whiteout_{name}_new = find("whiteout_{name}_new",\n')
        buf.write(f'        FunctionDescriptor.of({addr}));\n')
        buf.write(f'    public static final MethodHandle whiteout_{name}_delete = find("whiteout_{name}_delete",\n')
        buf.write(f'        FunctionDescriptor.ofVoid({addr}));\n')

        for fld in fields_map.get(name, []):
            buf.write(f'    public static final MethodHandle whiteout_{name}_get_{fld} = find("whiteout_{name}_get_{fld}",\n')
            buf.write(f'        FunctionDescriptor.of(ValueLayout.JAVA_FLOAT, {addr}));\n')
            buf.write(f'    public static final MethodHandle whiteout_{name}_set_{fld} = find("whiteout_{name}_set_{fld}",\n')
            buf.write(f'        FunctionDescriptor.ofVoid({addr}, ValueLayout.JAVA_FLOAT));\n')

        if name in matrix_dims:
            buf.write(f'    public static final MethodHandle whiteout_{name}_dim = find("whiteout_{name}_dim",\n')
            buf.write('        FunctionDescriptor.of(ValueLayout.JAVA_LONG));\n')
            buf.write(f'    public static final MethodHandle whiteout_{name}_get_at = find("whiteout_{name}_get_at",\n')
            buf.write(f'        FunctionDescriptor.of(ValueLayout.JAVA_FLOAT, {addr}, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG));\n')
            buf.write(f'    public static final MethodHandle whiteout_{name}_set_at = find("whiteout_{name}_set_at",\n')
            buf.write(f'        FunctionDescriptor.ofVoid({addr}, ValueLayout.JAVA_LONG, ValueLayout.JAVA_LONG, ValueLayout.JAVA_FLOAT));\n')

        for m in methods_map.get(name, []):
            mname, ret, params, is_static, _is_const = m[:5]
            layouts = []
            if not is_static:
                layouts.append(addr)
            for p_t, _ in params:
                layouts.append(_math_layout(p_t, primitives_passthru))
            ret_layout = (
                'void' if ret == 'void'
                else _math_layout(ret, primitives_passthru)
            )
            if ret_layout == 'void':
                desc = f'FunctionDescriptor.ofVoid({", ".join(layouts)})'
            else:
                desc = f'FunctionDescriptor.of({", ".join([ret_layout] + layouts)})'
            buf.write(f'    public static final MethodHandle whiteout_{name}_{mname} = find("whiteout_{name}_{mname}", {desc});\n')

    if free_funcs:
        buf.write('    // â”€â”€ Free functions â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€\n')
        addr = 'ValueLayout.ADDRESS'
        for fname, ret, params in free_funcs:
            layouts = [_math_layout(t, primitives_passthru) for t, _ in params]
            ret_layout = ('void' if ret == 'void'
                          else _math_layout(ret, primitives_passthru))
            if ret_layout == 'void':
                desc = f'FunctionDescriptor.ofVoid({", ".join(layouts)})'
            else:
                desc = f'FunctionDescriptor.of({", ".join([ret_layout] + layouts)})'
            buf.write(f'    public static final MethodHandle whiteout_{fname} = find("whiteout_{fname}", {desc});\n')

    buf.write('}\n')
    return buf.getvalue()


def _snake_to_camel(name: str) -> str:
    """`transform_point` → `transformPoint`. Used to rename the inlined
    free functions so they read as idiomatic Java statics on the host
    class."""
    parts = name.split('_')
    return parts[0] + ''.join(p[:1].upper() + p[1:] for p in parts[1:])


def _emit_common_class(name, fields, methods, matrix_dim,
                       primitives_passthru, inline_free_funcs=()) -> str:
    """Emit a shared-math handle-backed class.

    Public API: math methods, equals/hashCode/toString, user-facing
    constructors and factories. No MemorySegment, no Arena, no
    `handle()` accessor, no `wrap()` factory.

    Cross-package access (other modules in the same JPMS module) goes
    through `whiteout.common.internal.Handles`, which uses
    MethodHandles.privateLookupIn to reach the package-private handle
    field and constructor. External code can't reference Handles
    because `whiteout.common.internal` is NOT exported by
    module-info.java.
    """
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {_COMMON_PKG};\n\n')
    buf.write('import java.lang.foreign.*;\n')
    buf.write('import java.lang.invoke.VarHandle;\n')
    buf.write('import java.util.Objects;\n')
    buf.write(f'import {_COMMON_INTERNAL_PKG}.Native;\n\n')

    is_vector = bool(fields)
    n_components = len(fields) if is_vector else None

    # Class-level Javadoc — describes the type and the lifecycle
    # contract every shared-math class shares.
    if is_vector:
        comps_phrase = ', '.join(f'`{f}`' for f in fields)
        type_purpose = (
            f'A {len(fields)}-component single-precision vector backed by a native'
            f' C struct. Components ({comps_phrase}) are stored as a packed'
            f' `float[{len(fields)}]` and accessed directly through VarHandles —'
            f' no JNI round-trip per `getX()`/`setX()`.'
        )
    elif matrix_dim is not None:
        type_purpose = (
            f'A {matrix_dim}×{matrix_dim} row-major single-precision matrix backed'
            f' by a native C struct ({matrix_dim * matrix_dim * 4} bytes of packed'
            f' `float`). Use {{@link #getAt(int,int)}} and {{@link #setAt(int,int,float)}}'
            f' for element access; static factories ({{@link #identity()}},'
            f' {{@link #zero()}}, …) wrap the native constructors.'
        )
    else:
        type_purpose = 'Native math type.'

    buf.write('/**\n')
    buf.write(f' * {type_purpose}\n')
    buf.write(' *\n')
    buf.write(' * <p><b>Lifecycle.</b> Instances own a native allocation and must be\n')
    buf.write(' * released with {@link #close()} (try-with-resources). Math methods\n')
    buf.write(f' * that return a fresh {name} allocate new native memory which the\n')
    buf.write(' * caller likewise owns.\n')
    buf.write(' *\n')
    buf.write(' * <p><b>Thread-safety.</b> Instances are not thread-safe. Don\'t share\n')
    buf.write(' * the same instance across threads without external synchronization.\n')
    buf.write(' */\n')
    buf.write(f'public final class {name} implements AutoCloseable {{\n')

    # ── Direct-memory field access via Project Panama ──────────────────
    # Private — internal to the class. VarHandle reads/writes bypass
    # MethodHandle.invoke for per-component access. Math operations
    # still cross the FFM boundary because the C++ side has the
    # authoritative implementations.
    if fields:
        buf.write('    private static final MemoryLayout LAYOUT = MemoryLayout.structLayout(\n')
        for i, f in enumerate(fields):
            sep = ',' if i < len(fields) - 1 else ''
            buf.write(f'        ValueLayout.JAVA_FLOAT.withName("{f}"){sep}\n')
        buf.write('    );\n')
        for f in fields:
            buf.write(f'    private static final VarHandle VH_{f.upper()} = '
                      f'LAYOUT.varHandle(MemoryLayout.PathElement.groupElement("{f}"));\n')
        buf.write('\n')
    elif matrix_dim is not None:
        n = matrix_dim
        buf.write(f'    private static final long BYTES = {n * n * 4}L;\n\n')

    # ── State (package-private so Handles can reflect into it) ────────
    buf.write('    final MemorySegment handle;\n')
    buf.write('    final boolean owned;\n\n')

    # Package-private wrap constructor — only whiteout.common can call
    # it directly. Cross-package codegen reaches it via Handles.
    buf.write(f'    {name}(MemorySegment seg, boolean owned) {{\n')
    if fields or matrix_dim is not None:
        size_expr = 'LAYOUT.byteSize()' if fields else 'BYTES'
        buf.write('        this.handle = (seg == null || seg.equals(MemorySegment.NULL))\n')
        buf.write(f'            ? seg : seg.reinterpret({size_expr});\n')
    else:
        buf.write('        this.handle = seg;\n')
    buf.write('        this.owned = owned;\n')
    buf.write('    }\n\n')

    # Public no-arg ctor — allocates a fresh native instance.
    buf.write(f'    public {name}() {{\n')
    buf.write('        try {\n')
    buf.write(f'            MemorySegment __raw = (MemorySegment) Native.whiteout_{name}_new.invoke();\n')
    if fields or matrix_dim is not None:
        size_expr = 'LAYOUT.byteSize()' if fields else 'BYTES'
        buf.write(f'            this.handle = __raw.reinterpret({size_expr});\n')
    else:
        buf.write('            this.handle = __raw;\n')
    buf.write('            this.owned = true;\n')
    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n\n')

    # Convenience constructor + static factory for vectors.
    if fields:
        ctor_params = ', '.join(f'float {f}' for f in fields)
        buf.write(f'    public {name}({ctor_params}) {{\n')
        buf.write('        this();\n')
        for f in fields:
            buf.write(f'        set{_cap(f)}({f});\n')
        buf.write('    }\n\n')
        buf.write(f'    public static {name} of({ctor_params}) {{\n')
        buf.write(f'        return new {name}({", ".join(fields)});\n')
        buf.write('    }\n\n')

    buf.write('    @Override\n')
    buf.write('    public void close() {\n')
    buf.write('        if (!owned) return;\n')
    buf.write('        if (handle != null && !handle.equals(MemorySegment.NULL)) {\n')
    buf.write(f'            try {{ Native.whiteout_{name}_delete.invoke(handle); }}\n')
    buf.write('            catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('        }\n')
    buf.write('    }\n\n')

    # ── Field accessors — direct VarHandle / segment indexing ─────────
    for fld in fields:
        cap = _cap(fld)
        buf.write(f'    public float get{cap}() {{ return (float) VH_{fld.upper()}.get(handle, 0L); }}\n')
        buf.write(f'    public void set{cap}(float value) {{ VH_{fld.upper()}.set(handle, 0L, value); }}\n')

    if matrix_dim is not None:
        n = matrix_dim
        buf.write(f'    public static int dim() {{ return {n}; }}\n')
        buf.write('    public float getAt(int row, int col) {\n')
        buf.write(f'        return handle.get(ValueLayout.JAVA_FLOAT, ((long) row * {n} + col) * 4L);\n')
        buf.write('    }\n')
        buf.write('    public void setAt(int row, int col, float value) {\n')
        buf.write(f'        handle.set(ValueLayout.JAVA_FLOAT, ((long) row * {n} + col) * 4L, value);\n')
        buf.write('    }\n')

    # ── Methods (native via the internal Native handles) ──────────────
    for m in methods:
        if m[0] == 'equals':
            continue   # routed through Object.equals override below
        _emit_class_math_method(buf, name, m, primitives_passthru)

    for fname, ret, params in inline_free_funcs:
        _emit_class_math_free(buf, fname, ret, params, primitives_passthru)

    # ── Object overrides ─────────────────────────────────────────────
    has_native_equals = any(m[0] == 'equals' for m in methods)
    if fields:
        _emit_math_equals_hashcode_tostring(buf, name, fields, has_native_equals)
    elif matrix_dim is not None:
        _emit_math_matrix_tostring(buf, name, matrix_dim)

    buf.write('}\n')
    return buf.getvalue()


# Hand-curated brief descriptions for shared-math methods. Keys are the
# raw snake_case method names (matching the C++/_SHARED_MATH_METHODS
# entries). Missing entries fall back to a generated brief based on the
# name alone.
_MATH_METHOD_BRIEFS: dict[str, str] = {
    'dot':           'Dot product with {other}.',
    'length':        'Euclidean length (magnitude).',
    'length_squared':'Squared Euclidean length — avoids the {@code sqrt} of {@link #length()}.',
    'normalize':     'In-place normalize to unit length (no-op when length is zero).',
    'normalized':    'A new vector pointing in the same direction with unit length.',
    'add':           'Component-wise sum.',
    'sub':           'Component-wise difference.',
    'mul':           'Component-wise product (Hamilton product for Quaternion).',
    'div':           'Component-wise quotient.',
    'mul_scalar':    'Scale every component by {scalar}.',
    'div_scalar':    'Divide every component by {scalar}.',
    'negate':        'Component-wise negation.',
    'equals':        'Component-wise bitwise equality (native comparator).',
    'lerp':          'Linear interpolation from {start} to {end} by {t} ∈ [0, 1].',
    'tcb_in_tangent':'Kochanek–Bartels in-tangent for the {current} keyframe.',
    'tcb_out_tangent':'Kochanek–Bartels out-tangent for the {current} keyframe.',
    'bezier_lerp':   'Cubic Bézier interpolation across four control points.',
    'hermite_lerp':  'Catmull–Rom / cubic Hermite interpolation.',
    'cross':         'Right-handed 3D cross product of {a} and {b}.',
    'transform_point':  'Transform point {v} (w=1) by row-major matrix {m}.',
    'transform_normal': 'Transform direction {v} (w=0) by row-major matrix {m}.',
    'conjugate':     'Quaternion conjugate (negate the imaginary part).',
    'inverse':       'Inverse of this rotation (conjugate / length²).',
    'log':           'Quaternion natural logarithm.',
    'exp':           'Quaternion exponential.',
    'to_euler_angles':'Convert this rotation to Euler angles (radians).',
    'rotate_vector': 'Rotate {v} by this quaternion (q · v · q⁻¹).',
    'from_axis_angle':'Build a quaternion rotating {angle_rad} radians around {axis}.',
    'from_euler_angles':'Build a quaternion from Euler-angle triple {euler_rad}.',
    'slerp':         'Spherical linear interpolation from {a} to {b}.',
    'squad':         'Spherical cubic interpolation across four control quaternions.',
    'ln_dif':        'Logarithmic difference: log(b · a⁻¹).',
    'identity':      'Identity element (no rotation / identity matrix).',
    'zero':          'Zero-filled matrix.',
    'translation':   'Translation matrix moving the origin by {t}.',
    'rotation':      'Rotation matrix equivalent to quaternion {q}.',
    'scaling':       'Scale matrix multiplying components by {s}.',
    'compose':       'Compose translation, rotation, and scale into a transform matrix.',
    'rotation_x':    'Rotation by {angle} radians around the X axis.',
    'rotation_y':    'Rotation by {angle} radians around the Y axis.',
    'rotation_z':    'Rotation by {angle} radians around the Z axis.',
    'transpose':     'Transpose this matrix (swap rows and columns).',
    'extract_translation':'Translation column of this transform matrix.',
    'extract_rotation':   'Rotation component as a quaternion.',
    'extract_scale':      'Scale components of this transform matrix.',
    'look_at_rh':    'Right-handed view matrix looking from {eye} at {target} with {up}.',
    'look_at_lh':    'Left-handed view matrix looking from {eye} at {target} with {up}.',
    'look_at_lh_sgcompat':'Left-handed view matrix with the SgCompat axis convention.',
    'perspective_fov_rh': 'Right-handed perspective projection with vertical FOV.',
    'perspective_fov_lh': 'Left-handed perspective projection with vertical FOV.',
    'perspective_diag_sgcompat':'SgCompat-style perspective projection with a diagonal FOV.',
    'orthographic_rh':'Right-handed orthographic projection of the given size.',
}


def _math_method_brief(mname: str, params) -> str:
    """Pick a brief description for a math method and interpolate the
    table-string's `{name}` placeholders into Javadoc `{@code name}`
    references — so the generated brief reads as natural prose."""
    brief = _MATH_METHOD_BRIEFS.get(mname)
    if brief is None:
        return f'Native {mname} operation.'
    # Replace {param} with {@code param}.
    for _p_t, p_n in params:
        brief = brief.replace('{' + p_n + '}', '{@code ' + p_n + '}')
    return brief


def _emit_math_method_javadoc(buf: StringIO, owner, mname: str, ret: str,
                              params, is_static: bool) -> None:
    """Emit a `/** ... */` block before a shared-math method. Brief
    comes from the curated table; @param / @return tags from the
    signature."""
    brief = _math_method_brief(mname, params)
    buf.write('    /**\n')
    buf.write(f'     * {brief}\n')
    for p_t, p_n in params:
        buf.write(f'     * @param {p_n} {p_t} input.\n')
    if ret != 'void':
        ret_desc = ('boolean result' if ret == 'bool'
                    else f'a fresh {ret} owning a native allocation' if ret not in {'float', 'int', 'size_t', 'long'}
                    else f'{ret} result')
        buf.write(f'     * @return {ret_desc}.\n')
    buf.write('     */\n')


def _emit_class_math_method(buf: StringIO, owner: str, m: tuple,
                            primitives_passthru: set[str]) -> None:
    mname, ret, params, is_static, _is_const = m[:5]
    static_kw = 'static ' if is_static else ''
    ret_java = _math_java_type(ret, primitives_passthru) if ret != 'void' else 'void'
    java_name = _java_method_name(mname)

    java_params = []
    invoke_args: list[str] = []
    if not is_static:
        invoke_args.append('handle')
    for p_t, p_n in params:
        if p_t in primitives_passthru:
            jt = _math_java_type(p_t, primitives_passthru)
            java_params.append(f'{jt} {p_n}')
            invoke_args.append(f'({p_n} ? 1 : 0)' if jt == 'boolean' else p_n)
        else:
            java_params.append(f'{p_t} {p_n}')
            # Same package as the math type → package-private handle
            # field access is in scope.
            invoke_args.append(f'{p_n}.handle')

    sig = ', '.join(java_params)
    invoke = f'Native.whiteout_{owner}_{mname}.invoke({", ".join(invoke_args)})'

    _emit_math_method_javadoc(buf, owner, mname, ret, params, is_static)
    buf.write(f'    public {static_kw}{ret_java} {java_name}({sig}) {{\n')
    buf.write('        try {\n')
    if ret == 'void':
        buf.write(f'            {invoke};\n')
    elif ret == 'bool':
        buf.write(f'            return ((int) {invoke}) != 0;\n')
    elif ret in primitives_passthru:
        buf.write(f'            return ({_math_java_type(ret, primitives_passthru)}) {invoke};\n')
    else:
        buf.write(f'            MemorySegment __h = (MemorySegment) {invoke};\n')
        buf.write(f'            return new {ret}(__h, true);\n')
    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')


def _emit_class_math_free(buf: StringIO, fname: str, ret: str, params,
                          primitives_passthru: set[str]) -> None:
    ret_java = _math_java_type(ret, primitives_passthru) if ret != 'void' else 'void'
    java_params = []
    invoke_args: list[str] = []
    for p_t, p_n in params:
        if p_t in primitives_passthru:
            jt = _math_java_type(p_t, primitives_passthru)
            java_params.append(f'{jt} {p_n}')
            invoke_args.append(f'({p_n} ? 1 : 0)' if jt == 'boolean' else p_n)
        else:
            java_params.append(f'{p_t} {p_n}')
            invoke_args.append(f'{p_n}.handle')
    sig = ', '.join(java_params)
    invoke = f'Native.whiteout_{fname}.invoke({", ".join(invoke_args)})'
    java_name = _snake_to_camel(fname)
    _emit_math_method_javadoc(buf, owner=None, mname=fname, ret=ret,
                              params=params, is_static=True)
    buf.write(f'    public static {ret_java} {java_name}({sig}) {{\n')
    buf.write('        try {\n')
    if ret == 'void':
        buf.write(f'            {invoke};\n')
    elif ret in primitives_passthru:
        buf.write(f'            return ({_math_java_type(ret, primitives_passthru)}) {invoke};\n')
    else:
        buf.write(f'            MemorySegment __h = (MemorySegment) {invoke};\n')
        buf.write(f'            return new {ret}(__h, true);\n')
    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')


def _emit_math_equals_hashcode_tostring(buf, name, fields, has_native_equals):
    buf.write('    @Override\n')
    buf.write('    public boolean equals(Object o) {\n')
    buf.write('        if (o == this) return true;\n')
    buf.write(f'        if (!(o instanceof {name})) return false;\n')
    buf.write(f'        {name} __other = ({name}) o;\n')
    if has_native_equals:
        buf.write(f'        try {{ return ((int) Native.whiteout_{name}_equals.invoke(handle, __other.handle)) != 0; }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    else:
        checks = ' && '.join(
            f'Float.floatToIntBits(get{_cap(f)}()) == Float.floatToIntBits(__other.get{_cap(f)}())'
            for f in fields)
        buf.write(f'        return {checks};\n')
    buf.write('    }\n\n')
    buf.write('    @Override\n')
    buf.write('    public int hashCode() {\n')
    if len(fields) == 1:
        buf.write(f'        return Float.hashCode(get{_cap(fields[0])}());\n')
    else:
        args = ', '.join(f'get{_cap(f)}()' for f in fields)
        buf.write(f'        return Objects.hash({args});\n')
    buf.write('    }\n\n')
    buf.write('    @Override\n')
    buf.write('    public String toString() {\n')
    parts = ' + ", " + '.join(f'"{f}=" + get{_cap(f)}()' for f in fields)
    buf.write(f'        return "{name}(" + {parts} + ")";\n')
    buf.write('    }\n\n')


def _emit_math_matrix_tostring(buf, name, dim):
    buf.write('    @Override\n')
    buf.write('    public String toString() {\n')
    buf.write(f'        StringBuilder __sb = new StringBuilder("{name}[\\n");\n')
    buf.write(f'        for (int __r = 0; __r < {dim}; __r++) {{\n')
    buf.write('            __sb.append("  ");\n')
    buf.write(f'            for (int __c = 0; __c < {dim}; __c++) {{\n')
    buf.write('                __sb.append(getAt(__r, __c));\n')
    buf.write(f'                if (__c < {dim} - 1) __sb.append(", ");\n')
    buf.write('            }\n')
    buf.write('            __sb.append("\\n");\n')
    buf.write('        }\n')
    buf.write('        return __sb.append("]").toString();\n')
    buf.write('    }\n\n')


def _module_classes(module: BindModule) -> list[BindClass]:
    """Classes the per-module Java emit should own. Shared math types are
    emitted exactly once in `whiteout.common`; per-module emit skips them
    to match the C side."""
    return [c for c in module.classes
            if c.cpp_qualifier not in _SHARED_MATH_TYPES]


def emit(module: BindModule) -> dict[str, str]:
    """Return a dict { relative_path: content } of Java files to write."""
    pkg = f'whiteout.{module.name}'
    pkg_path = pkg.replace('.', '/')
    base = f'bindings/java/src/main/java/{pkg_path}'

    # Internal subpackage holds the FFM-handle registry. NOT exported
    # by module-info.java, so external code can't touch MethodHandles.
    internal_pkg = _module_internal_pkg(module)
    internal_base = f'bindings/java/src/main/java/{internal_pkg.replace(".", "/")}'

    files: dict[str, str] = {}
    classes = _module_classes(module)

    # Eagerly populate the Java-name map; every emit helper hits it.
    _java_handle_map(module)
    classes_in_module: dict[str, str] = {}  # retained for back-compat shims

    # Native.java lives in the internal subpackage. Public-class fields
    # so same-module callers in `whiteout.<mod>` can reach the handles.
    files[f'{internal_base}/Native.java'] = _emit_native(module, classes, classes_in_module)

    # Enums — file & class spelling use the prefix-stripped js_name so
    # nested-enum collisions (Sequence::Flag vs Texture::Flag) get
    # distinct names (SequenceFlag, TextureFlag). Derive from js_name
    # directly because cpp_qualifier short can collide across
    # sub-namespaces (`blp::ParseMode` and `png::ParseMode` both produce
    # `ParseMode`).
    seen_enums: set[str] = set()
    for e in module.enums:
        java_short = _java_strip_prefix(e.js_name, module)
        if java_short in seen_enums:
            continue
        seen_enums.add(java_short)
        files[f'{base}/{java_short}.java'] = _emit_enum(pkg, e, java_short)

    # Classes — same reasoning: per-format `Parser` classes share a
    # cpp_qualifier, so we go through js_name to disambiguate.
    for c in classes:
        c_short = _c_handle_short(c.js_name)
        java_short = _java_strip_prefix(c_short, module)
        files[f'{base}/{java_short}.java'] = _emit_class(
            pkg, c, c_short, java_short, classes_in_module, module
        )

    return files


# â”€â”€ Native (FFM glue) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

_NATIVE_LAYOUT_PROLOGUE = '''/**
 * Common FFM layouts and runtime helpers shared by every per-module
 * Native class. Keeps the struct-layout definitions in one place so
 * descriptors line up across modules.
 */
public final class NativeCommon {
    private NativeCommon() {}

    public static final Linker LINKER = Linker.nativeLinker();
    public static final SymbolLookup LOOKUP;
    static {
        String envPath = System.getProperty("whiteout.native.path");
        if (envPath != null) {
            System.load(envPath);
        } else {
            System.loadLibrary("whiteout_c");
        }
        LOOKUP = SymbolLookup.loaderLookup();
    }

    public static MethodHandle find(String name, FunctionDescriptor fd) {
        return LINKER.downcallHandle(
            LOOKUP.find(name).orElseThrow(() ->
                new RuntimeException("symbol not found: " + name)),
            fd);
    }

    public static final MemoryLayout BYTES_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.ADDRESS.withName("data"),
        ValueLayout.JAVA_LONG.withName("size"),
        ValueLayout.ADDRESS.withName("owner")
    ).withName("whiteout_Bytes");

    public static final MemoryLayout CSTRING_LAYOUT = MemoryLayout.structLayout(
        ValueLayout.ADDRESS.withName("chars"),
        ValueLayout.JAVA_LONG.withName("length"),
        ValueLayout.ADDRESS.withName("owner")
    ).withName("whiteout_CString");

    public static final MethodHandle whiteout_Bytes_free =
        find("whiteout_Bytes_free", FunctionDescriptor.ofVoid(BYTES_LAYOUT));

    public static final MethodHandle whiteout_CString_free =
        find("whiteout_CString_free", FunctionDescriptor.ofVoid(CSTRING_LAYOUT));
}
'''


def _emit_native_common() -> str:
    """`whiteout/common/NativeCommon.java` â€” the lookup + struct layouts
    every module's Native class delegates to."""
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {_COMMON_INTERNAL_PKG};\n\n')
    buf.write('import java.lang.foreign.*;\n')
    buf.write('import java.lang.invoke.MethodHandle;\n\n')
    buf.write(_NATIVE_LAYOUT_PROLOGUE)
    return buf.getvalue()


def _emit_native(module: BindModule, classes: list[BindClass],
                 classes_in_module: dict[str, str]) -> str:
    prefix = f'whiteout_{module.name}'
    internal_pkg = _module_internal_pkg(module)
    known = _known_class_short_names(module)

    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {internal_pkg};\n\n')
    buf.write('import java.lang.foreign.*;\n')
    buf.write('import java.lang.invoke.MethodHandle;\n')
    buf.write(f'import {_COMMON_INTERNAL_PKG}.NativeCommon;\n\n')

    # `public` because same-module callers in `whiteout.{mod}` (different
    # package) need to reach the handles. The package is not exported by
    # module-info.java, so external code can't see this class at all.
    buf.write('/** FFM MethodHandle registry for this module. Internal — for codegen use. */\n')
    buf.write('public final class Native {\n')
    buf.write('    private Native() {}\n\n')
    buf.write('    public static final MemoryLayout BYTES_LAYOUT = NativeCommon.BYTES_LAYOUT;\n')
    buf.write('    public static final MemoryLayout CSTRING_LAYOUT = NativeCommon.CSTRING_LAYOUT;\n')
    buf.write('    public static final MethodHandle whiteout_Bytes_free = NativeCommon.whiteout_Bytes_free;\n')
    buf.write('    public static final MethodHandle whiteout_CString_free = NativeCommon.whiteout_CString_free;\n\n')
    buf.write('    static MethodHandle find(String name, FunctionDescriptor fd) {\n')
    buf.write('        return NativeCommon.find(name, fd);\n')
    buf.write('    }\n\n')

    for c in classes:
        short = _c_handle_short(c.js_name)
        buf.write(f'    // -- {short} --\n')
        if not c.no_default_ctor:
            buf.write(f'    public static final MethodHandle {prefix}_{short}_new = find(\n')
            buf.write(f'        "{prefix}_{short}_new", FunctionDescriptor.of(ValueLayout.ADDRESS));\n')
        # Non-default ctors: emit a MethodHandle per ctor that has a
        # Java-surface-able param list (string / primitive). The user-facing
        # class's factory methods bind to these handles. Naming mirrors
        # emit_c::_ctor_overloads_named — same `_new_<sigName>` symbols.
        for ctor in c.constructors:
            param_infos = [_java_ctor_param_info(p) for p in ctor.params]
            if any(pi is None for pi in param_infos):
                continue
            sig_name = '_'.join(p.name or 'arg' for p in ctor.params)
            layouts = ', '.join(pi[1] for pi in param_infos)
            buf.write(f'    public static final MethodHandle {prefix}_{short}_new_{sig_name} = find(\n')
            buf.write(f'        "{prefix}_{short}_new_{sig_name}", FunctionDescriptor.of(ValueLayout.ADDRESS, {layouts}));\n')
        buf.write(f'    public static final MethodHandle {prefix}_{short}_delete = find(\n')
        buf.write(f'        "{prefix}_{short}_delete", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));\n')

        for m in c.methods:
            if not _is_supported(m, module):
                continue
            sym = f'{prefix}_{short}_{m.name}'
            buf.write(f'    public static final MethodHandle {sym} = find(\n')
            buf.write(f'        "{sym}", {_function_descriptor(m, c)});\n')

        for f in c.fields:
            if not _is_field_supported_c(f, module, known):
                continue
            _emit_field_native(buf, c, f, prefix, short, module, known)
        buf.write('\n')

    buf.write('}\n')
    return buf.getvalue()


def _emit_field_native(buf: StringIO, c: BindClass, f: BindField,
                       prefix: str, short: str, module: BindModule,
                       known: set[str]) -> None:
    """Emit Native.java MethodHandle declarations for every C-side
    accessor the field accessor emitter (emit_c.py) produces. The names
    line up symbol-for-symbol."""
    t = f.type
    base = f'{prefix}_{short}_'
    addr = 'ValueLayout.ADDRESS'

    def fd(ret: str, *params: str) -> str:
        if ret == 'void':
            return f'FunctionDescriptor.ofVoid({", ".join(params)})'
        return f'FunctionDescriptor.of({", ".join((ret,) + params)})'

    def emit(sym: str, descriptor: str) -> None:
        buf.write(f'    public static final MethodHandle {sym} = find("{sym}", {descriptor});\n')

    if t.kind == TypeKind.PRIMITIVE:
        layout = _java_layout(t)
        emit(f'{base}get_{f.name}', fd(layout, addr))
        emit(f'{base}set_{f.name}', fd('void', addr, layout))
    elif t.kind == TypeKind.ENUM:
        emit(f'{base}get_{f.name}', fd('ValueLayout.JAVA_INT', addr))
        emit(f'{base}set_{f.name}', fd('void', addr, 'ValueLayout.JAVA_INT'))
    elif t.kind == TypeKind.STRING:
        emit(f'{base}get_{f.name}', fd('CSTRING_LAYOUT', addr))
        emit(f'{base}set_{f.name}', fd('void', addr, addr))
    elif t.kind == TypeKind.NESTED:
        emit(f'{base}get_{f.name}', fd(addr, addr))
        emit(f'{base}set_{f.name}', fd('void', addr, addr))
    elif t.kind == TypeKind.ARRAY:
        elem_layout = _array_elem_layout(t.element)
        emit(f'{base}{f.name}_size', fd('ValueLayout.JAVA_LONG'))
        emit(f'{base}get_{f.name}_at', fd(elem_layout, addr, 'ValueLayout.JAVA_LONG'))
        if t.element.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM):
            emit(f'{base}set_{f.name}_at',
                 fd('void', addr, 'ValueLayout.JAVA_LONG', elem_layout))
    elif t.kind == TypeKind.VECTOR:
        emit(f'{base}get_{f.name}_count', fd('ValueLayout.JAVA_LONG', addr))
        emit(f'{base}resize_{f.name}', fd('void', addr, 'ValueLayout.JAVA_LONG'))
        if _is_bulk_flat_inner(t.element):
            # Bulk: data ptr getter + assign-from-flat-array setter.
            emit(f'{base}get_{f.name}_data', fd(addr, addr))
            emit(f'{base}assign_{f.name}',
                 fd('void', addr, addr, 'ValueLayout.JAVA_LONG'))
        elif t.element.kind == TypeKind.NESTED:
            emit(f'{base}get_{f.name}_at',
                 fd('ValueLayout.ADDRESS', addr, 'ValueLayout.JAVA_LONG'))
    elif t.kind == TypeKind.NESTED_VEC:
        inner = t.element.element
        emit(f'{base}get_{f.name}_count', fd('ValueLayout.JAVA_LONG', addr))
        emit(f'{base}get_{f.name}_inner_count',
             fd('ValueLayout.JAVA_LONG', addr, 'ValueLayout.JAVA_LONG'))
        emit(f'{base}resize_{f.name}',
             fd('void', addr, 'ValueLayout.JAVA_LONG'))
        emit(f'{base}resize_{f.name}_inner',
             fd('void', addr, 'ValueLayout.JAVA_LONG', 'ValueLayout.JAVA_LONG'))
        if _is_bulk_flat_inner(inner):
            emit(f'{base}get_{f.name}_inner_data',
                 fd(addr, addr, 'ValueLayout.JAVA_LONG'))
            emit(f'{base}assign_{f.name}_inner',
                 fd('void', addr, 'ValueLayout.JAVA_LONG', addr, 'ValueLayout.JAVA_LONG'))
        elif inner.kind == TypeKind.NESTED:
            emit(f'{base}get_{f.name}_at',
                 fd('ValueLayout.ADDRESS', addr, 'ValueLayout.JAVA_LONG', 'ValueLayout.JAVA_LONG'))


def _array_elem_layout(t: TypeRef) -> str:
    """FFM layout for the inner element of an ARRAY/VECTOR field."""
    if t.kind == TypeKind.PRIMITIVE:
        return _java_layout(t)
    if t.kind == TypeKind.ENUM:
        return 'ValueLayout.JAVA_INT'
    if t.kind == TypeKind.NESTED:
        return 'ValueLayout.ADDRESS'
    return 'ValueLayout.ADDRESS'


def _is_supported(m: BindMethod, module: BindModule) -> bool:
    """A method is Java-bindable iff the C emitter binds it â€” we route
    every call through the C wrapper, so anything the C side filters out
    has no symbol on the other end."""
    return _is_method_supported_c(m, module)


def _function_descriptor(m: BindMethod, c: BindClass) -> str:
    """Build the `FunctionDescriptor.of(...)` call for an FFM downcall."""
    ret_layout = _return_layout(m.return_type)
    param_layouts = []
    if not m.is_static:
        param_layouts.append('ValueLayout.ADDRESS')   # self handle
    for p in m.params:
        if p.span_scalar is not None:
            param_layouts.append('ValueLayout.ADDRESS')   # data ptr
            param_layouts.append('ValueLayout.JAVA_LONG') # size
        elif p.type.kind == TypeKind.PRIMITIVE:
            param_layouts.append(_java_layout(p.type))
        elif p.type.kind == TypeKind.ENUM:
            param_layouts.append('ValueLayout.JAVA_INT')
        elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            param_layouts.append('ValueLayout.ADDRESS')
        else:
            param_layouts.append('ValueLayout.ADDRESS')

    params_str = ', '.join(param_layouts) if param_layouts else ''
    if ret_layout == 'void':
        return f'FunctionDescriptor.ofVoid({params_str})'
    parts = [ret_layout] + param_layouts
    return f'FunctionDescriptor.of({", ".join(parts)})'


def _return_layout(ret: TypeRef) -> str:
    if ret.cpp_text == 'void':
        return 'void'
    if ret.kind == TypeKind.PRIMITIVE:
        return _java_layout(ret)
    if ret.kind == TypeKind.ENUM:
        return 'ValueLayout.JAVA_INT'
    if ret.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        # span<const u8> is packed into whiteout_Bytes by the C wrapper.
        if _is_span_const_u8(ret):
            return 'BYTES_LAYOUT'
        return 'ValueLayout.ADDRESS'
    if ret.kind == TypeKind.OPTIONAL:
        if ret.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            return 'ValueLayout.ADDRESS'        # nullable handle
        if ret.element.kind == TypeKind.VECTOR:
            return 'BYTES_LAYOUT'               # struct
    if ret.kind == TypeKind.VECTOR:
        return 'BYTES_LAYOUT'
    return 'ValueLayout.ADDRESS'


# â”€â”€ Enum file â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def _emit_enum(pkg: str, e: BindEnum, short: str) -> str:
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py â€” do not edit.\n')
    buf.write(f'package {pkg};\n\n')
    if e.doc:
        buf.write('/**\n')
        for line in e.doc.splitlines():
            buf.write(f' * {line}\n')
        buf.write(' */\n')
    buf.write(f'public enum {short} {{\n')
    seen: set[str] = set()
    for i, v in enumerate(e.values):
        if v.js_name in seen:
            continue
        seen.add(v.js_name)
        sep = ';' if i == len(e.values) - 1 else ','
        if v.doc:
            buf.write(f'    /** {v.doc} */\n')
        buf.write(f'    {v.js_name}({i}){sep}\n')
    buf.write('\n')
    buf.write('    public final int value;\n')
    buf.write(f'    {short}(int v) {{ this.value = v; }}\n')
    buf.write(f'    public static {short} fromInt(int v) {{\n')
    buf.write('        for (var e : values()) if (e.value == v) return e;\n')
    buf.write(f'        throw new IllegalArgumentException("unknown {short}: " + v);\n')
    buf.write('    }\n')
    buf.write('}\n')
    return buf.getvalue()


# â”€â”€ Class file (AutoCloseable wrapper) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def _emit_class(pkg: str, c: BindClass, c_short: str, java_short: str,
                classes: dict[str, str], module: BindModule) -> str:
    """Emit a Java class file. `c_short` is the C-side handle name used
    for symbol lookup (e.g. `MdxSequence`); `java_short` is the Java
    class spelling (e.g. `Sequence`)."""
    prefix = f'whiteout_{module.name}'
    known = _known_class_short_names(module)
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_java.py - do not edit.\n')
    buf.write(f'package {pkg};\n\n')
    buf.write('import java.lang.foreign.*;\n')
    buf.write('import java.lang.invoke.MethodHandle;\n')
    buf.write('import java.nio.charset.StandardCharsets;\n')
    buf.write(f'import {_COMMON_PKG}.*;\n')
    buf.write(f'import {_COMMON_INTERNAL_PKG}.Handles;\n')
    buf.write(f'import {_module_internal_pkg(module)}.Native;\n\n')

    # Class-level Javadoc — always emit a block so users see the
    # lifecycle contract even on undocumented C++ types. The C++ ///
    # comment (if any) goes first; the lifecycle/thread-safety notes
    # follow.
    buf.write('/**\n')
    if c.doc:
        for line in c.doc.splitlines():
            buf.write(f' * {line}\n')
        buf.write(' *\n')
    else:
        buf.write(f' * Java binding for the native {c.cpp_qualifier} type.\n')
        buf.write(' *\n')
    buf.write(f' * <p><b>Lifecycle.</b> Instances hold a handle to a native\n')
    buf.write(f' * {c.cpp_qualifier} allocation. Always release them with\n')
    buf.write(' * {@link #close()} — try-with-resources is the natural shape:\n')
    buf.write(' * <pre>{@code\n')
    buf.write(f' * try ({java_short} obj = ...) {{\n')
    buf.write(' *     // ... use obj ...\n')
    buf.write(' * }\n')
    buf.write(' * }</pre>\n')
    buf.write(' * Borrowed views returned by field getters (e.g. nested struct slices)\n')
    buf.write(' * share the parent\'s memory and must not outlive it.\n')
    buf.write(' *\n')
    buf.write(' * <p><b>Thread-safety.</b> Instances are not thread-safe. The underlying\n')
    buf.write(' * C++ types make no synchronization guarantees, so callers must coordinate\n')
    buf.write(' * external access if a handle is shared across threads.\n')
    buf.write(' */\n')

    buf.write(f'public final class {java_short} implements AutoCloseable {{\n')

    # Struct size (from libclang). When known, the ctors reinterpret
    # the raw FFM pointer to this size so direct-memory field accessors
    # have valid segment bounds. Private — callers don't need it.
    if c.byte_size:
        buf.write(f'    private static final long BYTES = {c.byte_size}L;\n\n')

    # Native handle storage. Package-private — `handle` and `owned` are
    # not exposed to callers outside `whiteout.{module}`. No
    # cross-package access happens (only `whiteout.common` records cross
    # package boundaries, and they're marshalled by value, not handles).
    buf.write('    final MemorySegment handle;\n')
    buf.write('    final boolean owned;\n\n')

    # Package-private constructor wrapping a foreign segment. Used by
    # codegen in this package; never exposed to user code.
    buf.write(f'    {java_short}(MemorySegment seg, boolean owned) {{\n')
    if c.byte_size:
        buf.write('        this.handle = (seg == null || seg.equals(MemorySegment.NULL))\n')
        buf.write('            ? seg : seg.reinterpret(BYTES);\n')
    else:
        buf.write('        this.handle = seg;\n')
    buf.write('        this.owned = owned;\n')
    buf.write('    }\n\n')

    if not c.no_default_ctor:
        buf.write(f'    public {java_short}() {{\n')
        buf.write('        try {\n')
        buf.write(f'            MemorySegment __raw = (MemorySegment) Native.{prefix}_{c_short}_new.invoke();\n')
        buf.write('            if (__raw == null || __raw.equals(MemorySegment.NULL))\n')
        buf.write(f'                throw new RuntimeException("{java_short} allocation failed");\n')
        if c.byte_size:
            buf.write('            this.handle = __raw.reinterpret(BYTES);\n')
        else:
            buf.write('            this.handle = __raw;\n')
        buf.write('            this.owned = true;\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n\n')

    # Non-default constructors → static factory methods, one per ctor that
    # has a Java-surface-able signature. Mirror image of the Native.java
    # MethodHandles emitted at the same time. Naming: `create<UpperName>`
    # so a `SimpleThreadPool(nThreads)` ctor becomes
    # `SimpleThreadPool.createNThreads(long)`.
    for ctor in c.constructors:
        param_infos = [_java_ctor_param_info(p) for p in ctor.params]
        if any(pi is None for pi in param_infos):
            continue
        sig_name = '_'.join(p.name or 'arg' for p in ctor.params)
        # camelCase suffix for the factory method (e.g. `nThreads` → `NThreads`).
        suffix = ''.join(part[:1].upper() + part[1:]
                         for part in sig_name.split('_') if part)
        java_args = ', '.join(f'{pi[0]} {p.name}' for p, pi in zip(ctor.params, param_infos))
        invoke_args = ', '.join(pi[3] for pi in param_infos)
        needs_arena = any('allocateFrom' in pi[2] for pi in param_infos)
        buf.write(f'    public static {java_short} create{suffix}({java_args}) {{\n')
        if needs_arena:
            buf.write('        try (Arena __arena = Arena.ofConfined()) {\n')
            indent = '            '
        else:
            buf.write('        try {\n')
            indent = '            '
        for pi in param_infos:
            if pi[2]:
                buf.write(f'{indent}{pi[2]}\n')
        buf.write(f'{indent}MemorySegment __raw = (MemorySegment) '
                  f'Native.{prefix}_{c_short}_new_{sig_name}.invoke({invoke_args});\n')
        buf.write(f'{indent}if (__raw == null || __raw.equals(MemorySegment.NULL))\n')
        buf.write(f'{indent}    throw new RuntimeException("{java_short} allocation failed");\n')
        buf.write(f'{indent}return new {java_short}(__raw, true);\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n\n')

    buf.write('    @Override\n')
    buf.write('    public void close() {\n')
    buf.write('        if (!owned) return;\n')
    buf.write('        if (handle != null && !handle.equals(MemorySegment.NULL)) {\n')
    buf.write('            try {\n')
    buf.write(f'                Native.{prefix}_{c_short}_delete.invoke(handle);\n')
    buf.write('            } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('        }\n')
    buf.write('    }\n\n')

    for m in c.methods:
        if not _is_supported(m, module):
            continue
        _emit_class_method(buf, m, c, c_short, classes, prefix, module)

    for f in c.fields:
        if not _is_field_supported_c(f, module, known):
            continue
        _emit_class_field(buf, f, c_short, classes, prefix, module,
                          parent_byte_size=c.byte_size)

    _emit_class_to_string(buf, c, java_short, module, known)

    buf.write('}\n')
    return buf.getvalue()


def _emit_class_to_string(buf: StringIO, c: BindClass, java_short: str,
                          module: BindModule, known: set[str]) -> None:
    """Override Object.toString to dump primitive/enum/string fields.
    Skips vector/nested/array fields so we don't recurse into huge
    object graphs or stringify thousands of vertices."""
    summarisable = [f for f in c.fields
                    if _is_field_supported_c(f, module, known)
                    and f.type.kind in (TypeKind.PRIMITIVE, TypeKind.ENUM, TypeKind.STRING)]
    buf.write('    @Override public String toString() {\n')
    if not summarisable:
        buf.write(f'        return "{java_short}@" + Long.toHexString(handle == null ? 0 : handle.address());\n')
    else:
        parts = []
        for f in summarisable:
            cap = _cap(f.name)
            parts.append(f'"{f.name}=" + get{cap}()')
        joined = ' + ", " + '.join(parts)
        buf.write(f'        return "{java_short}(" + {joined} + ")";\n')
    buf.write('    }\n\n')


# â”€â”€ Field accessor emission â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def _cap(name: str) -> str:
    return name[:1].upper() + name[1:] if name else name


def _emit_class_field(buf: StringIO, f: BindField, short: str,
                      classes: dict[str, str], prefix: str,
                      module: BindModule,
                      parent_byte_size: int | None = None) -> None:
    t = f.type
    sym_base = f'whiteout_{module.name}_{short}_'
    cap = _cap(f.name)

    # Forward the C++ /// comment as Javadoc on the get/set pair. The
    # comment lands above the getter; readers find the matching setter
    # one declaration below.
    if f.doc:
        buf.write('    /**\n')
        for line in f.doc.splitlines():
            buf.write(f'     * {line}\n')
        buf.write(f'     * @return the {f.name} field of this {short}.\n')
        buf.write('     */\n')
    else:
        buf.write(f'    /** @return the {f.name} field of this {short}. */\n')

    direct = parent_byte_size is not None and _can_emit_direct(f, module)

    if t.kind == TypeKind.PRIMITIVE:
        jt = _java_primitive(t)
        if direct:
            info = _primitive_direct_info(t)
            if info is not None:
                _, layout = info
                _emit_direct_primitive_get_set(buf, jt, cap, layout, f.byte_offset, t)
                return
        _emit_simple_get_set(buf, jt, cap, sym_base, f.name,
                             native_to_java=_native_to_java_primitive(jt),
                             java_to_native=_java_to_native_primitive(jt))
    elif t.kind == TypeKind.ENUM:
        java_cls = _resolve_java_class(t.cpp_text, module, classes)
        if direct:
            # The C++ enum is u32 in our codebase (every `@bind` enum
            # uses `enum class X : u32`); read/write as int32 at offset.
            buf.write(f'    public {java_cls} get{cap}() {{\n')
            buf.write(f'        return {java_cls}.fromInt(handle.get(ValueLayout.JAVA_INT, {f.byte_offset}L));\n')
            buf.write('    }\n')
            buf.write(f'    public void set{cap}({java_cls} value) {{\n')
            buf.write(f'        handle.set(ValueLayout.JAVA_INT, {f.byte_offset}L, value.value);\n')
            buf.write('    }\n')
            return
        buf.write(f'    public {java_cls} get{cap}() {{\n')
        buf.write(f'        try {{ return {java_cls}.fromInt((int) Native.{sym_base}get_{f.name}.invoke(handle)); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        buf.write(f'    public void set{cap}({java_cls} value) {{\n')
        buf.write(f'        try {{ Native.{sym_base}set_{f.name}.invoke(handle, value.value); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
    elif t.kind == TypeKind.STRING:
        buf.write(f'    public String get{cap}() {{\n')
        buf.write('        try (Arena arena = Arena.ofConfined()) {\n')
        buf.write(f'            MemorySegment __s = (MemorySegment) Native.{sym_base}get_{f.name}.invoke(arena, handle);\n')
        buf.write('            MemorySegment __chars = __s.get(ValueLayout.ADDRESS, 0);\n')
        buf.write('            long __len = __s.get(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS.byteSize());\n')
        buf.write('            if (__chars == null || __chars.equals(MemorySegment.NULL)) return "";\n')
        buf.write('            byte[] __bytes = __chars.reinterpret(__len).toArray(ValueLayout.JAVA_BYTE);\n')
        buf.write('            Native.whiteout_CString_free.invoke(__s);\n')
        buf.write('            return new String(__bytes, StandardCharsets.UTF_8);\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        buf.write(f'    public void set{cap}(String value) {{\n')
        buf.write('        try (Arena arena = Arena.ofConfined()) {\n')
        buf.write('            byte[] __utf = value == null ? new byte[0] : value.getBytes(StandardCharsets.UTF_8);\n')
        buf.write('            MemorySegment __seg = arena.allocate(__utf.length + 1);\n')
        buf.write('            MemorySegment.copy(__utf, 0, __seg, ValueLayout.JAVA_BYTE, 0, __utf.length);\n')
        buf.write('            __seg.set(ValueLayout.JAVA_BYTE, __utf.length, (byte) 0);\n')
        buf.write(f'            Native.{sym_base}set_{f.name}.invoke(handle, __seg);\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
    elif t.kind == TypeKind.NESTED:
        java_cls = _resolve_java_class(t.cpp_text, module, classes)
        nested_size = _nested_byte_size(t, module)
        nested_is_pod = _nested_is_pod(t, module)
        is_shared_math = java_cls in _SHARED_MATH_TYPES

        # Getter — borrowed sub-slice for both shared-math and same-module
        # nested classes. Construction goes through Handles for shared math
        # (cross-package private ctor); direct `new T(...)` for same-module
        # classes whose package-private ctor is in scope.
        if direct and nested_size is not None:
            slice_expr = f'handle.asSlice({f.byte_offset}L, {nested_size}L)'
            if is_shared_math:
                wrap = f'Handles.wrap{java_cls}({slice_expr}, false)'
            else:
                wrap = _construct_handle_wrap(java_cls, slice_expr, 'false')
            buf.write(f'    public {java_cls} get{cap}() {{\n')
            buf.write(f'        return {wrap};\n')
            buf.write('    }\n')
        else:
            if is_shared_math:
                wrap = f'Handles.wrap{java_cls}(__h, false)'
            else:
                wrap = _construct_handle_wrap(java_cls, '__h', 'false')
            buf.write(f'    public {java_cls} get{cap}() {{\n')
            buf.write(f'        try {{ MemorySegment __h = (MemorySegment) Native.{sym_base}get_{f.name}.invoke(handle);\n')
            buf.write(f'            return {wrap}; }}\n')
            buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
            buf.write('    }\n')

        # Setter — for POD-by-libclang types the byte layout is the
        # C++ copy-assignment, so a memcpy is correct. Non-POD types
        # MUST route through the C wrapper so the C++ side runs the
        # proper assignment (destructors + deep-copy).
        if direct and nested_size is not None and nested_is_pod:
            seg_of = f'Handles.segmentOf(value)' if is_shared_math else 'value.handle'
            buf.write(f'    public void set{cap}({java_cls} value) {{\n')
            buf.write('        if (value == null) {\n')
            buf.write(f'            try {{ Native.{sym_base}set_{f.name}.invoke(handle, MemorySegment.NULL); }}\n')
            buf.write('            catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
            buf.write('            return;\n')
            buf.write('        }\n')
            buf.write(f'        MemorySegment.copy({seg_of}, 0L, handle, {f.byte_offset}L, {nested_size}L);\n')
            buf.write('    }\n')
        else:
            seg_of = ('value == null ? MemorySegment.NULL : '
                      + ('Handles.segmentOf(value)' if is_shared_math else 'value.handle'))
            buf.write(f'    public void set{cap}({java_cls} value) {{\n')
            buf.write(f'        try {{ Native.{sym_base}set_{f.name}.invoke(handle, {seg_of}); }}\n')
            buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
            buf.write('    }\n')
    elif t.kind == TypeKind.ARRAY:
        _emit_indexed_field(buf, f, short, classes, sym_base, module,
                            count_expr=f'Native.{sym_base}{f.name}_size.invoke()',
                            count_returns_long=True, has_resize=False, cap=cap)
    elif t.kind == TypeKind.VECTOR:
        if _is_bulk_flat_inner(t.element):
            _emit_bulk_vector_field(buf, f, sym_base, module, cap)
        else:
            _emit_indexed_field(buf, f, short, classes, sym_base, module,
                                count_expr=f'Native.{sym_base}get_{f.name}_count.invoke(handle)',
                                count_returns_long=True, has_resize=True, cap=cap)
    elif t.kind == TypeKind.NESTED_VEC:
        _emit_nested_vec_field(buf, f, short, classes, sym_base, module, cap)


def _emit_bulk_vector_field(buf: StringIO, f: BindField, sym_base: str,
                            module: BindModule, cap: str) -> None:
    """Flat-array accessors for vector<primitive> / vector<enum> /
    vector<Vector*/Quat> fields.  Returns the entire vector as one
    typed Java array (copied out, so it's safe across mutations);
    setter assigns from a similarly-flat array. Element-count for
    Vector*/Quat fields is `array.length / components`."""
    inner = f.type.element
    comp_short = _bulk_component_short(inner)
    jt, layout, sz = _SPAN_ELEM_INFO[comp_short]
    n = _bulk_components(inner)

    buf.write(f'    public int get{cap}Count() {{\n')
    buf.write(f'        try {{ return (int) (long) Native.{sym_base}get_{f.name}_count.invoke(handle); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')

    buf.write(f'    public {jt}[] get{cap}() {{\n')
    buf.write('        try {\n')
    buf.write(f'            long __count = (long) Native.{sym_base}get_{f.name}_count.invoke(handle);\n')
    buf.write(f'            MemorySegment __ptr = (MemorySegment) Native.{sym_base}get_{f.name}_data.invoke(handle);\n')
    buf.write(f'            if (__count == 0 || __ptr == null || __ptr.equals(MemorySegment.NULL)) return new {jt}[0];\n')
    buf.write(f'            long __scalars = __count * {n}L;\n')
    buf.write(f'            return __ptr.reinterpret(__scalars * {sz}L).toArray({layout});\n')
    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')

    buf.write(f'    public void set{cap}({jt}[] values) {{\n')
    buf.write('        try (Arena arena = Arena.ofConfined()) {\n')
    buf.write(f'            long __count = (long) values.length / {n};\n')
    buf.write(f'            MemorySegment __seg = arena.allocate((long) values.length * {sz}L);\n')
    buf.write(f'            if (values.length > 0) MemorySegment.copy(values, 0, __seg, {layout}, 0, values.length);\n')
    buf.write(f'            Native.{sym_base}assign_{f.name}.invoke(handle, __seg, __count);\n')
    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')

    buf.write(f'    public void resize{cap}(int count) {{\n')
    buf.write(f'        try {{ Native.{sym_base}resize_{f.name}.invoke(handle, (long) count); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')


def _emit_indexed_field(buf: StringIO, f: BindField, owner_short: str,
                        classes: dict[str, str], sym_base: str,
                        module: BindModule, count_expr: str,
                        count_returns_long: bool, has_resize: bool,
                        cap: str) -> None:
    inner = f.type.element
    cap_name = cap
    buf.write(f'    public int get{cap_name}Count() {{\n')
    buf.write(f'        try {{ return (int) (long) {count_expr}; }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')

    if inner.kind == TypeKind.PRIMITIVE:
        jt = _java_primitive(inner)
        cast = _native_to_java_primitive(jt)
        buf.write(f'    public {jt} get{cap_name}At(int index) {{\n')
        buf.write(f'        try {{ return {cast.format(invoke=f"Native.{sym_base}get_{f.name}_at.invoke(handle, (long) index)")}; }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        buf.write(f'    public void set{cap_name}At(int index, {jt} value) {{\n')
        buf.write(f'        try {{ Native.{sym_base}set_{f.name}_at.invoke(handle, (long) index, {_java_to_native_primitive(jt).format(value="value")}); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
    elif inner.kind == TypeKind.ENUM:
        java_cls = _resolve_java_class(inner.cpp_text, module, classes)
        buf.write(f'    public {java_cls} get{cap_name}At(int index) {{\n')
        buf.write(f'        try {{ return {java_cls}.fromInt((int) Native.{sym_base}get_{f.name}_at.invoke(handle, (long) index)); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        buf.write(f'    public void set{cap_name}At(int index, {java_cls} value) {{\n')
        buf.write(f'        try {{ Native.{sym_base}set_{f.name}_at.invoke(handle, (long) index, value.value); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
    elif inner.kind == TypeKind.NESTED:
        java_cls = _resolve_java_class(inner.cpp_text, module, classes)
        if java_cls in _SHARED_MATH_TYPES:
            wrap = f'Handles.wrap{java_cls}(__h, false)'
        else:
            wrap = _construct_handle_wrap(java_cls, '__h', 'false')
        buf.write(f'    public {java_cls} get{cap_name}At(int index) {{\n')
        buf.write(f'        try {{ MemorySegment __h = (MemorySegment) Native.{sym_base}get_{f.name}_at.invoke(handle, (long) index);\n')
        buf.write(f'            return {wrap}; }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')

    if has_resize:
        buf.write(f'    public void resize{cap_name}(int count) {{\n')
        buf.write(f'        try {{ Native.{sym_base}resize_{f.name}.invoke(handle, (long) count); }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')

    # Read-only List<T> view so callers can do for-each / streams over
    # the field without manually walking count + at.
    java_inner = _list_view_element_type(inner, module, classes)
    if java_inner is not None:
        buf.write(f'    public java.util.List<{java_inner}> {f.name}View() {{\n')
        buf.write(f'        return new java.util.AbstractList<{java_inner}>() {{\n')
        buf.write(f'            @Override public int size() {{ return get{cap_name}Count(); }}\n')
        buf.write(f'            @Override public {java_inner} get(int index) {{ return get{cap_name}At(index); }}\n')
        buf.write('        };\n')
        buf.write('    }\n')


def _emit_nested_vec_field(buf: StringIO, f: BindField, owner_short: str,
                           classes: dict[str, str], sym_base: str,
                           module: BindModule, cap: str) -> None:
    """Accessors for vector<vector<T>> fields. Each inner row surfaces as
    a Java primitive array (int[] / float[] / etc.) when T is bulk-flat,
    otherwise as per-element handle access. The outer collection is an
    ArrayList<row[]> built lazily by `get{Field}()`."""
    inner = f.type.element.element

    # ── outer/inner count + resize ────────────────────────────────────
    buf.write(f'    public int get{cap}Count() {{\n')
    buf.write(f'        try {{ return (int) (long) Native.{sym_base}get_{f.name}_count.invoke(handle); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')
    buf.write(f'    public int get{cap}InnerCount(int outerIdx) {{\n')
    buf.write(f'        try {{ return (int) (long) Native.{sym_base}get_{f.name}_inner_count.invoke(handle, (long) outerIdx); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')
    buf.write(f'    public void resize{cap}(int count) {{\n')
    buf.write(f'        try {{ Native.{sym_base}resize_{f.name}.invoke(handle, (long) count); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')
    buf.write(f'    public void resize{cap}Inner(int outerIdx, int innerCount) {{\n')
    buf.write(f'        try {{ Native.{sym_base}resize_{f.name}_inner.invoke(handle, (long) outerIdx, (long) innerCount); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')

    if _is_bulk_flat_inner(inner):
        # ── bulk-flat path: each inner row is a primitive array ───────
        comp_short = _bulk_component_short(inner)
        jt, layout, sz = _SPAN_ELEM_INFO[comp_short]
        n = _bulk_components(inner)
        # Single-row bulk getter.
        buf.write(f'    public {jt}[] get{cap}At(int outerIdx) {{\n')
        buf.write('        try {\n')
        buf.write(f'            long __count = (long) Native.{sym_base}get_{f.name}_inner_count.invoke(handle, (long) outerIdx);\n')
        buf.write(f'            MemorySegment __ptr = (MemorySegment) Native.{sym_base}get_{f.name}_inner_data.invoke(handle, (long) outerIdx);\n')
        buf.write(f'            if (__count == 0 || __ptr == null || __ptr.equals(MemorySegment.NULL)) return new {jt}[0];\n')
        buf.write(f'            long __scalars = __count * {n}L;\n')
        buf.write(f'            return __ptr.reinterpret(__scalars * {sz}L).toArray({layout});\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        # Single-row bulk setter.
        buf.write(f'    public void set{cap}At(int outerIdx, {jt}[] values) {{\n')
        buf.write('        try (Arena arena = Arena.ofConfined()) {\n')
        buf.write(f'            long __count = (long) values.length / {n};\n')
        buf.write(f'            MemorySegment __seg = arena.allocate((long) values.length * {sz}L);\n')
        buf.write(f'            if (values.length > 0) MemorySegment.copy(values, 0, __seg, {layout}, 0, values.length);\n')
        buf.write(f'            Native.{sym_base}assign_{f.name}_inner.invoke(handle, (long) outerIdx, __seg, __count);\n')
        buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')
        # Convenience: pull the whole thing as ArrayList<row[]>.
        buf.write(f'    public java.util.List<{jt}[]> get{cap}() {{\n')
        buf.write(f'        int __outer = get{cap}Count();\n')
        buf.write(f'        java.util.ArrayList<{jt}[]> __out = new java.util.ArrayList<>(__outer);\n')
        buf.write(f'        for (int __i = 0; __i < __outer; __i++) __out.add(get{cap}At(__i));\n')
        buf.write('        return __out;\n')
        buf.write('    }\n')
        # Convenience: replace everything from a List<row[]>.
        buf.write(f'    public void set{cap}(java.util.List<{jt}[]> values) {{\n')
        buf.write(f'        resize{cap}(values.size());\n')
        buf.write(f'        for (int __i = 0; __i < values.size(); __i++) set{cap}At(__i, values.get(__i));\n')
        buf.write('    }\n')
    elif inner.kind == TypeKind.NESTED:
        # ── per-element handle path (non-bulk-flat nested) ────────────
        java_cls = _resolve_java_class(inner.cpp_text, module, classes)
        if java_cls in _SHARED_MATH_TYPES:
            wrap = f'Handles.wrap{java_cls}(__h, false)'
        else:
            wrap = _construct_handle_wrap(java_cls, '__h', 'false')
        buf.write(f'    public {java_cls} get{cap}At(int outerIdx, int innerIdx) {{\n')
        buf.write(f'        try {{ MemorySegment __h = (MemorySegment) Native.{sym_base}get_{f.name}_at.invoke(handle, (long) outerIdx, (long) innerIdx);\n')
        buf.write(f'            return {wrap}; }}\n')
        buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
        buf.write('    }\n')


def _list_view_element_type(inner: TypeRef, module: BindModule,
                            classes: dict[str, str]) -> str | None:
    """Boxed Java type to use for a `List<T>` view of an indexed field;
    None means "skip the view" (primitives expose `XAt(int)` directly so
    a List<Boxed> view would just add boxing overhead)."""
    if inner.kind == TypeKind.NESTED:
        return _resolve_java_class(inner.cpp_text, module, classes)
    if inner.kind == TypeKind.ENUM:
        return _resolve_java_class(inner.cpp_text, module, classes)
    return None


def _can_emit_direct(f: BindField, module: BindModule) -> bool:
    """A field qualifies for direct-memory access only when libclang told
    us its byte offset and the parent class's total size — without size
    the segment isn't bounds-checked and `segment.get()` would throw."""
    if f.byte_offset is None:
        return False
    # The class context (parent struct) being-emitted always has a known
    # byte_size when we land here, because we only reinterpret-and-then-
    # direct-access when the parent's size came back from libclang. The
    # check is implicit: emit_class only sets BYTES + reinterpret when
    # byte_size is non-zero, so we rely on that. Same field also has to
    # use a primitive width we can map directly.
    if f.type.kind == TypeKind.PRIMITIVE:
        return _primitive_direct_info(f.type) is not None
    if f.type.kind == TypeKind.ENUM:
        return True
    if f.type.kind == TypeKind.NESTED:
        return _nested_byte_size(f.type, module) is not None
    return False


def _emit_direct_primitive_get_set(buf: StringIO, jt: str, cap: str,
                                   layout: str, offset: int,
                                   t: TypeRef) -> None:
    """`handle.get(LAYOUT, offset)` getter + `handle.set(...)` setter for
    a PRIMITIVE field. The JIT lowers each to a single load/store."""
    short = _short_name(t.cpp_text)
    if short == 'bool':
        # bool stays a byte in C++ but exposes as Java `boolean`.
        buf.write(f'    public {jt} get{cap}() {{\n')
        buf.write(f'        return handle.get({layout}, {offset}L) != 0;\n')
        buf.write('    }\n')
        buf.write(f'    public void set{cap}({jt} value) {{\n')
        buf.write(f'        handle.set({layout}, {offset}L, (byte) (value ? 1 : 0));\n')
        buf.write('    }\n')
        return
    buf.write(f'    public {jt} get{cap}() {{\n')
    buf.write(f'        return handle.get({layout}, {offset}L);\n')
    buf.write('    }\n')
    buf.write(f'    public void set{cap}({jt} value) {{\n')
    buf.write(f'        handle.set({layout}, {offset}L, value);\n')
    buf.write('    }\n')


def _emit_simple_get_set(buf: StringIO, jt: str, cap: str, sym_base: str,
                         fname: str, native_to_java: str,
                         java_to_native: str) -> None:
    buf.write(f'    public {jt} get{cap}() {{\n')
    buf.write(f'        try {{ return {native_to_java.format(invoke=f"Native.{sym_base}get_{fname}.invoke(handle)")}; }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')
    buf.write(f'    public void set{cap}({jt} value) {{\n')
    buf.write(f'        try {{ Native.{sym_base}set_{fname}.invoke(handle, {java_to_native.format(value="value")}); }}\n')
    buf.write('        catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n')


def _native_to_java_primitive(jt: str) -> str:
    """Format string mapping a native invocation result to its Java
    primitive form. `{invoke}` is the expression returning Object."""
    if jt == 'boolean':
        return '((int) {invoke}) != 0'
    return f'({jt}) {{invoke}}'


def _java_to_native_primitive(jt: str) -> str:
    """Format string mapping a Java primitive value to its native
    invocation arg. `{value}` is the Java expression."""
    if jt == 'boolean':
        return '({value} ? 1 : 0)'
    return '{value}'


def _emit_class_method(buf: StringIO, m: BindMethod, c: BindClass, short: str,
                       classes: dict[str, str], prefix: str,
                       module: BindModule):
    ret_java = _java_type(m.return_type, module, classes)

    # Build the Java parameter list (user-facing).
    java_params = []
    for p in m.params:
        if p.span_scalar is not None:
            jt, _layout, _sz = _span_elem(p.span_scalar)
            java_params.append(f'{jt}[] {p.name}')
        elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            jt = _java_type(p.type, module, classes)
            java_params.append(f'{jt} {p.name}')
        elif p.type.kind == TypeKind.ENUM:
            jt = _java_type(p.type, module, classes)
            java_params.append(f'{jt} {p.name}')
        else:
            java_params.append(f'{_java_type(p.type, module, classes)} {p.name}')

    params_str = ', '.join(java_params)

    # Always emit a Javadoc block. The C++ /// comment (if any) becomes
    # the brief; otherwise a generated brief is used. @param and @return
    # tags are derived from the signature so callers see parameter names
    # at the documentation level without having to read the source. We
    # skip auto-generated tags when the C++ doc already provides them,
    # so user-supplied docs aren't shadowed by boilerplate.
    has_return_tag = bool(m.doc) and '@return' in m.doc
    documented_params = (
        {line.split()[1] for line in m.doc.splitlines()
         if line.strip().startswith('@param ') and len(line.split()) >= 2}
        if m.doc else set()
    )
    buf.write('    /**\n')
    if m.doc:
        for line in m.doc.splitlines():
            buf.write(f'     * {line}\n')
    else:
        kind = 'Static factory' if m.is_static else 'Native method'
        buf.write(f'     * {kind} wrapping {{@code {c.cpp_qualifier}::{m.cpp_name}}}.\n')
    auto_params = [p for p in m.params if p.name not in documented_params]
    if auto_params:
        if m.doc:
            buf.write('     *\n')
        for p in auto_params:
            jt = _param_javadoc_type(p, module, classes)
            buf.write(f'     * @param {p.name} {jt} input.\n')
    if ret_java != 'void' and not has_return_tag:
        if ret_java == 'boolean':
            ret_desc = 'boolean result'
        elif ret_java in ('int', 'long', 'float', 'double', 'short', 'byte', 'String'):
            ret_desc = f'{ret_java} result'
        elif ret_java.startswith('Optional<'):
            ret_desc = f'{ret_java}; empty when the operation produced no value'
        elif ret_java.startswith('List<'):
            ret_desc = f'a borrowed {ret_java} view backed by the native container'
        elif ret_java.endswith('[]'):
            ret_desc = f'a fresh {ret_java} copied out of native memory'
        else:
            ret_desc = f'a fresh {ret_java} owning a native allocation'
        buf.write(f'     * @return {ret_desc}.\n')
    buf.write('     */\n')

    buf.write(f'    public {ret_java} {_java_method_name(m.name)}({params_str}) {{\n')

    # An Arena is needed for two reasons:
    #   - to marshal span params (Java byte[] â†’ native MemorySegment), and
    #   - to allocate a return slot for struct-returning C funcs
    #     (FFM prepends a SegmentAllocator param for those).
    ret = m.return_type
    returns_struct = (ret.kind == TypeKind.VECTOR or
                      (ret.kind == TypeKind.OPTIONAL and
                       ret.element.kind == TypeKind.VECTOR) or
                      _is_span_const_u8(ret))
    has_span = any(p.span_scalar is not None for p in m.params)
    needs_arena = has_span or returns_struct

    if needs_arena:
        buf.write('        try (Arena arena = Arena.ofConfined()) {\n')
        body_indent = '            '
    else:
        body_indent = '        '
        buf.write('        try {\n')

    # Prepare per-param Java->native conversions.
    invoke_args = []
    # Struct returns require a leading SegmentAllocator.
    if returns_struct:
        invoke_args.append('arena')
    if not m.is_static:
        invoke_args.append('handle')
    for p in m.params:
        if p.span_scalar is not None:
            _jt, layout, sz = _span_elem(p.span_scalar)
            buf.write(f'{body_indent}MemorySegment {p.name}_seg = arena.allocate({p.name}.length * {sz}L);\n')
            buf.write(f'{body_indent}MemorySegment.copy({p.name}, 0, {p.name}_seg, {layout}, 0, {p.name}.length);\n')
            invoke_args.append(f'{p.name}_seg')
            invoke_args.append(f'(long) {p.name}.length')
        elif p.type.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            java_cls = _resolve_java_class(p.type.cpp_text, module, classes)
            if java_cls in _SHARED_MATH_TYPES:
                # Cross-package read — go through Handles since the
                # math classes' `handle` field is private and the
                # accessor doesn't exist on the public surface.
                invoke_args.append(f'Handles.segmentOf({p.name})')
            else:
                # Per-module class — package-private field access since
                # both classes live in the same package.
                invoke_args.append(f'{p.name} == null ? MemorySegment.NULL : {p.name}.handle')
        elif p.type.kind == TypeKind.ENUM:
            invoke_args.append(f'{p.name}.value')
        elif _java_primitive(p.type) == 'boolean':
            invoke_args.append(f'({p.name} ? 1 : 0)')
        else:
            invoke_args.append(p.name)

    sym = f'{prefix}_{short}_{m.name}'
    invoke = f'Native.{sym}.invoke({", ".join(invoke_args)})'

    if ret.cpp_text == 'void':
        buf.write(f'{body_indent}{invoke};\n')
    elif _is_span_const_u8(ret):
        # span<const u8> â€” same Bytes-shaped struct return as vector<u8>,
        # but checked first so the generic UNKNOWN-handle branch doesn't
        # try to `new std::span<const u8>(handle)`.
        buf.write(f'{body_indent}MemorySegment __struct = (MemorySegment) {invoke};\n')
        buf.write(f'{body_indent}MemorySegment __data = __struct.get(ValueLayout.ADDRESS, 0);\n')
        buf.write(f'{body_indent}long __size = __struct.get(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS.byteSize());\n')
        buf.write(f'{body_indent}if (__data == null || __data.equals(MemorySegment.NULL)) return new byte[0];\n')
        buf.write(f'{body_indent}byte[] __out = __data.reinterpret(__size).toArray(ValueLayout.JAVA_BYTE);\n')
        buf.write(f'{body_indent}Native.whiteout_Bytes_free.invoke(__struct);\n')
        buf.write(f'{body_indent}return __out;\n')
    elif ret.kind == TypeKind.OPTIONAL and ret.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        # nullable handle -> java.util.Optional<T>.
        elem_short = _resolve_java_class(ret.element.cpp_text, module, classes)
        if elem_short in _SHARED_MATH_TYPES:
            wrap = f'Handles.wrap{elem_short}(__h, true)'
        else:
            wrap = _construct_handle_wrap(elem_short, '__h', 'true')
        buf.write(f'{body_indent}MemorySegment __h = (MemorySegment) {invoke};\n')
        buf.write(f'{body_indent}if (__h == null || __h.equals(MemorySegment.NULL)) return java.util.Optional.empty();\n')
        buf.write(f'{body_indent}return java.util.Optional.of({wrap});\n')
    elif ret.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        elem_short = _resolve_java_class(ret.cpp_text, module, classes)
        if elem_short in _SHARED_MATH_TYPES:
            wrap = f'Handles.wrap{elem_short}(__h, true)'
        else:
            wrap = _construct_handle_wrap(elem_short, '__h', 'true')
        buf.write(f'{body_indent}MemorySegment __h = (MemorySegment) {invoke};\n')
        buf.write(f'{body_indent}return {wrap};\n')
    elif ret.kind == TypeKind.ENUM:
        elem_short = _resolve_java_class(ret.cpp_text, module, classes)
        buf.write(f'{body_indent}return {elem_short}.fromInt((int) {invoke});\n')
    elif ret.kind == TypeKind.VECTOR or \
            (ret.kind == TypeKind.OPTIONAL and ret.element.kind == TypeKind.VECTOR):
        # Returns whiteout_Bytes struct â€” read data/size, copy out, then
        # invoke whiteout_Bytes_free (no-op when _owner is NULL, which is
        # how span<const u8> borrowed views are tagged).
        buf.write(f'{body_indent}MemorySegment __struct = (MemorySegment) {invoke};\n')
        buf.write(f'{body_indent}MemorySegment __data = __struct.get(ValueLayout.ADDRESS, 0);\n')
        buf.write(f'{body_indent}long __size = __struct.get(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS.byteSize());\n')
        buf.write(f'{body_indent}if (__data == null || __data.equals(MemorySegment.NULL)) return new byte[0];\n')
        buf.write(f'{body_indent}byte[] __out = __data.reinterpret(__size).toArray(ValueLayout.JAVA_BYTE);\n')
        buf.write(f'{body_indent}Native.whiteout_Bytes_free.invoke(__struct);\n')
        buf.write(f'{body_indent}return __out;\n')
    elif _java_primitive(ret) == 'boolean':
        buf.write(f'{body_indent}return ((int) {invoke}) != 0;\n')
    else:
        buf.write(f'{body_indent}return ({_java_primitive(ret)}) {invoke};\n')

    buf.write('        } catch (Throwable __ex) { throw new RuntimeException(__ex); }\n')
    buf.write('    }\n\n')


def _java_method_name(c_name: str) -> str:
    """Convert any C++ method name to camelCase Java. `hasIssues` stays
    as-is; `length_squared` becomes `lengthSquared`; `to_euler_angles`
    becomes `toEulerAngles`. The C symbol still uses the original
    snake_case name, so MethodHandle lookups are unaffected."""
    return _snake_to_camel(c_name)
