# SPDX-License-Identifier: BSD-3-Clause
"""libclang-based parser that builds a BindModule from annotated headers."""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Optional

from clang import cindex
from clang.cindex import CursorKind, TypeKind as CXTypeKind

from .annotations import parse as parse_annotations, is_bound
from .ir import (
    BindClass, BindConstant, BindConstructor, BindEnum, BindEnumValue,
    BindField, BindMethod, BindMethodParam, BindModule, ModuleConfig,
    TypeKind, TypeRef,
)


# ── Type-string → TypeRef classification ───────────────────────────────────

# C++ scalar names that map to Embind primitives. Anything in u/i/f integer or
# float family qualifies; bool too.
PRIMITIVES = {
    'bool',
    'char', 'signed char', 'unsigned char',
    'short', 'unsigned short',
    'int', 'unsigned int',
    'long', 'unsigned long', 'long long', 'unsigned long long',
    'float', 'double',
    'std::int8_t', 'std::int16_t', 'std::int32_t', 'std::int64_t',
    'std::uint8_t', 'std::uint16_t', 'std::uint32_t', 'std::uint64_t',
    'int8_t', 'int16_t', 'int32_t', 'int64_t',
    'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t',
    'size_t', 'std::size_t',
    'u8', 'u16', 'u32', 'u64', 'i8', 'i16', 'i32', 'i64', 'f32', 'f64',
}

# When the canonical type comes back from libclang as a built-in, normalise
# it back to the whiteout alias so the emitter and JS naming stay consistent
# with bindings.cpp's hand-written register_vector calls.
_CANONICAL_TO_ALIAS = {
    'unsigned char': 'whiteout::u8',
    'unsigned short': 'whiteout::u16',
    'unsigned int': 'whiteout::u32',
    'unsigned long long': 'whiteout::u64',
    'signed char': 'whiteout::i8',
    'short': 'whiteout::i16',
    'int': 'whiteout::i32',
    'long long': 'whiteout::i64',
    'float': 'whiteout::f32',
    'double': 'whiteout::f64',
}

# Class templates with well-known typedef aliases. libclang reports field
# types using the canonical template instantiation (Vector3<float>); map
# back to the alias (Vector3f) so naming and lookup stay stable.
_CANONICAL_CLASS_ALIASES = {
    'whiteout::Vector2<float>': 'whiteout::Vector2f',
    'whiteout::Vector3<float>': 'whiteout::Vector3f',
    'whiteout::Vector4<float>': 'whiteout::Vector4f',
}

STRING_TYPES = {'std::string', 'std::__cxx11::basic_string<char>'}


def _strip_qualifiers(t: str) -> str:
    """Drop leading const/volatile and trailing &/*."""
    t = t.strip()
    for prefix in ('const ', 'volatile '):
        if t.startswith(prefix):
            t = t[len(prefix):].strip()
    while t.endswith(('&', '*')):
        t = t[:-1].strip()
    return t


def _short_name(qualified: str) -> str:
    """'whiteout::mdx::Sequence::Flag' -> 'Sequence::Flag'.

    Strips LEADING namespace components that start with a lowercase
    letter — they're conventionally namespaces (whiteout, mdx, m2, m3,
    std, models) while user-defined types use PascalCase. We always keep
    at least one component so `whiteout::u8` -> `u8` (not empty), which
    matters for primitive detection.
    """
    parts = qualified.split('::')
    while len(parts) > 1 and parts[0] and parts[0][0].islower():
        parts = parts[1:]
    return '::'.join(parts)


# Match templates: foo<bar, baz<qux>>
TEMPLATE_RE = re.compile(r'^(?P<base>[\w:]+)<(?P<args>.+)>$')


def _split_template_args(args: str) -> list[str]:
    """Split a template argument list, respecting nested <>."""
    out, depth, current = [], 0, []
    for ch in args:
        if ch == '<':
            depth += 1
            current.append(ch)
        elif ch == '>':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            out.append(''.join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        out.append(''.join(current).strip())
    return out


def classify_type(cpp_text: str, known_classes: set[str], known_enums: set[str]) -> TypeRef:
    t = _strip_qualifiers(cpp_text)
    # Map canonical built-ins / template instantiations back to their
    # whiteout aliases so the emitter and JS naming stay stable.
    t = _CANONICAL_TO_ALIAS.get(t, _CANONICAL_CLASS_ALIASES.get(t, t))
    short = _short_name(t)

    if t in PRIMITIVES or short in PRIMITIVES:
        return TypeRef(cpp_text=t, kind=TypeKind.PRIMITIVE)
    if t in STRING_TYPES or short == 'string' or short == 'basic_string<char>':
        return TypeRef(cpp_text='std::string', kind=TypeKind.STRING)
    if t in known_enums or short in known_enums:
        return TypeRef(cpp_text=t, kind=TypeKind.ENUM)

    m = TEMPLATE_RE.match(t)
    if m:
        base = m.group('base').replace('std::__1::', 'std::').replace('std::__cxx11::', 'std::')
        args = _split_template_args(m.group('args'))
        # std::vector<X>
        if base in ('std::vector', 'vector'):
            inner = classify_type(args[0], known_classes, known_enums)
            kind = TypeKind.NESTED_VEC if inner.kind == TypeKind.VECTOR else TypeKind.VECTOR
            return TypeRef(cpp_text=t, kind=kind, element=inner)
        # std::array<X, N>
        if base in ('std::array', 'array'):
            inner = classify_type(args[0], known_classes, known_enums)
            try:
                n = int(args[1].strip())
            except (ValueError, IndexError):
                n = None
            return TypeRef(cpp_text=t, kind=TypeKind.ARRAY, element=inner, array_size=n)
        # std::optional<X> — Embind has built-in JS<->std::optional conversion.
        if base in ('std::optional', 'optional'):
            inner = classify_type(args[0], known_classes, known_enums)
            return TypeRef(cpp_text=t, kind=TypeKind.OPTIONAL, element=inner)
        # whiteout::mdx::Track<X> — keyframe track template owned by MDX.
        # Only the exact MDX Track template is treated as TRACK; M2's
        # AnimationTrack<T> and M3's AnimBlock<T> are different templates
        # that need their own handling.
        if base in ('whiteout::mdx::Track', 'mdx::Track', 'Track'):
            inner = classify_type(args[0], known_classes, known_enums)
            return TypeRef(cpp_text=t, kind=TypeKind.TRACK, element=inner)

    if t in known_classes or short in known_classes:
        return TypeRef(cpp_text=t, kind=TypeKind.NESTED)

    return TypeRef(cpp_text=t, kind=TypeKind.UNKNOWN)


# ── AST walking helpers ────────────────────────────────────────────────────

def _is_under_paths(cursor: cindex.Cursor, paths: list[Path]) -> bool:
    """True if cursor's location is under any of the given file paths."""
    loc = cursor.location.file
    if loc is None:
        return False
    cf = Path(loc.name).resolve()
    return any(cf == p for p in paths)


def _enum_values(cursor: cindex.Cursor) -> list[BindEnumValue]:
    out = []
    parent_qual = _short_name(cursor.type.spelling)
    for child in cursor.get_children():
        if child.kind == CursorKind.ENUM_CONSTANT_DECL:
            out.append(BindEnumValue(
                js_name=child.spelling,
                cpp_qualifier=f'{parent_qual}::{child.spelling}',
            ))
    return out


# ── Naming ─────────────────────────────────────────────────────────────────

_SHARED_MATH = {'Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'}


def _apply_prefix(name: str, prefix: str) -> str:
    """Prepend `prefix` to `name` unless `name` already begins with it.

    Avoids "M2M2Box" (where the C++ class is `whiteout::m2::M2Box` and
    the module's prefix is `M2`). Without this guard the codegen would
    emit `prefix + name = "M2" + "M2Box" = "M2M2Box"`.
    """
    if not prefix or name.startswith(prefix):
        return name
    return prefix + name


def js_name_for_class(cpp_qual: str, prefix: str) -> str:
    """Sequence -> MdxSequence;  Layer::SubTexture -> MdxLayerSubTexture.

    Shared math types (Vector*, Quaternion) keep their short C++ name with
    no prefix so different modules can share the registration.
    """
    if cpp_qual in _SHARED_MATH:
        return cpp_qual
    return _apply_prefix(cpp_qual.replace('::', ''), prefix)


def js_name_for_enum(cpp_qual: str, prefix: str) -> str:
    """Layer::FilterMode -> MdxLayerFilterMode;
    Node::NodeType -> MdxNodeType (drop redundant outer-name overlap).
    """
    parts = cpp_qual.split('::')
    if len(parts) > 1 and parts[-1].startswith(parts[-2]):
        # e.g. Node::NodeType -> NodeType   (then prefix with module)
        joined = ''.join(parts[:-2] + [parts[-1]])
    else:
        joined = ''.join(parts)
    return _apply_prefix(joined, prefix)


_PRIMITIVE_JS_NAME = {
    # Whiteout aliases
    'u8': 'U8', 'u16': 'U16', 'u32': 'U32', 'u64': 'U64',
    'i8': 'I8', 'i16': 'I16', 'i32': 'I32', 'i64': 'I64',
    'f32': 'F32', 'f64': 'F64', 'bool': 'Bool',
    # Canonical names (libclang's get_canonical().spelling produces these)
    'unsigned char': 'U8', 'unsigned short': 'U16',
    'unsigned int': 'U32', 'unsigned long': 'U32', 'unsigned long long': 'U64',
    'signed char': 'I8', 'short': 'I16', 'int': 'I32',
    'long': 'I32', 'long long': 'I64',
    'float': 'F32', 'double': 'F64', 'char': 'I8',
}


def js_name_for_type(t: TypeRef, prefix: str) -> str:
    """Stable JS-side name used to derive container names (e.g. VectorMdxBone)."""
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        return _PRIMITIVE_JS_NAME.get(short, short.title())
    if t.kind == TypeKind.STRING:
        return 'String'
    if t.kind in (TypeKind.NESTED, TypeKind.ENUM):
        # Shared math types stay un-prefixed; everything else gets js_prefix.
        short = _short_name(t.cpp_text)
        if short in ('Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'):
            return short
        return prefix + short.replace('::', '')
    if t.kind == TypeKind.VECTOR:
        return 'Vector' + js_name_for_type(t.element, prefix)
    if t.kind == TypeKind.NESTED_VEC:
        return 'Vector' + js_name_for_type(t.element, prefix)
    if t.kind == TypeKind.TRACK:
        return prefix + 'Track' + js_name_for_type(t.element, prefix)
    return t.cpp_text


# ── Parser entrypoint ──────────────────────────────────────────────────────

def _has_unbindable_inner(t: TypeRef) -> bool:
    """Return True for type shapes the codegen can't cleanly name across
    backends: vector<array<...>>, vector<unbound>, etc. Used to skip
    fields rather than emit broken `Vectorstd::array<...>` names.
    """
    if t.kind == TypeKind.UNKNOWN:
        return True
    if t.kind in (TypeKind.VECTOR, TypeKind.NESTED_VEC):
        if t.element.kind == TypeKind.ARRAY:
            return True
        return _has_unbindable_inner(t.element)
    return False


def _is_span_const_u8(t: TypeRef) -> bool:
    if t.kind != TypeKind.UNKNOWN:
        return False
    s = t.cpp_text
    return s.startswith('std::span<') and ('uint8_t' in s or ' u8' in s
                                            or 'whiteout::u8' in s
                                            or 'unsigned char' in s)


def _is_vector_u8(t: TypeRef) -> bool:
    if t.kind != TypeKind.VECTOR:
        return False
    return t.element.kind == TypeKind.PRIMITIVE and \
           _short_name(t.element.cpp_text) in ('u8', 'unsigned char')


def _is_string_param(t: TypeRef) -> bool:
    return t.kind == TypeKind.STRING


def collect_methods(cursor, bind_class, known_classes, known_enums,
                    remember_containers, mode: str | bool):
    """Walk public CXX_METHOD / CONSTRUCTOR cursors on a class and add them
    to bind_class.methods / .constructors.

    `mode` controls overload selection:
        'buffer_only' — when multiple overloads exist, prefer ones taking
                        std::span<const u8> over std::string (path-based).
        True (or any) — bind everything not marked @bind skip.
    """
    from clang import cindex
    from clang.cindex import CursorKind

    # Group method cursors by name to detect overloads.
    methods_by_name: dict[str, list] = {}
    constructors: list = []

    for child in cursor.get_children():
        if child.access_specifier != cindex.AccessSpecifier.PUBLIC:
            continue
        if child.kind == CursorKind.CXX_METHOD:
            # Skip C++ operators (operator=, operator==, ...) — they need
            # special handling per backend and are rarely useful in JS/Py.
            if child.spelling.startswith('operator'):
                continue
            methods_by_name.setdefault(child.spelling, []).append(child)
        elif child.kind == CursorKind.CONSTRUCTOR:
            constructors.append(child)

    def make_param(arg_cursor) -> BindMethodParam:
        cpp = arg_cursor.type.get_canonical().spelling
        # Preserve std::span<...> and std::string& signatures literally —
        # canonical strips the const-ref but the basic shape is fine.
        cpp_raw = arg_cursor.type.spelling
        if 'span' in cpp_raw:
            cpp = cpp_raw  # don't canonicalise span (it'd lose template args)
        tref = classify_type(cpp, known_classes, known_enums)
        remember_containers(tref)
        return BindMethodParam(
            name=arg_cursor.spelling or 'arg',
            type=tref,
            has_default=any(c.kind == CursorKind.UNEXPOSED_EXPR
                            for c in arg_cursor.get_children()),
        )

    for name, overloads in methods_by_name.items():
        method_ann = parse_annotations(
            overloads[0].raw_comment) if overloads else {}
        if method_ann.get('skip'):
            continue

        chosen = None
        if len(overloads) > 1 and mode == 'buffer_only':
            # Prefer the overload whose first param is std::span<const u8>.
            for o in overloads:
                params = list(o.get_arguments())
                if not params:
                    continue
                first_cpp = params[0].type.spelling
                if 'span' in first_cpp and ('u8' in first_cpp
                                             or 'uint8_t' in first_cpp):
                    chosen = o
                    break
            if chosen is None:
                # Fall back: prefer any non-string-first-arg overload.
                for o in overloads:
                    params = list(o.get_arguments())
                    if params and 'string' not in params[0].type.spelling:
                        chosen = o
                        break
        if chosen is None:
            chosen = overloads[0]

        ret_cpp = chosen.result_type.get_canonical().spelling
        ret = classify_type(ret_cpp, known_classes, known_enums)
        remember_containers(ret)

        params = [make_param(a) for a in chosen.get_arguments()]
        bytes_in = bool(params) and _is_span_const_u8(params[0].type)
        bytes_out = _is_vector_u8(ret)

        bind_class.methods.append(BindMethod(
            name=method_ann.get('rename', name),
            cpp_name=name,
            return_type=ret,
            params=params,
            is_const=chosen.is_const_method(),
            is_static=chosen.is_static_method(),
            bytes_in=bytes_in,
            bytes_out=bytes_out,
        ))

    # Non-default constructors. Skip:
    #   - the implicit default ctor (already emitted separately)
    #   - copy / move constructors (PImpl classes have these deleted, and
    #     binding them would fail to compile)
    #   - any ctor taking std::string& (path-based)
    #   - any ctor taking an UNKNOWN type (e.g. WorkerPool* — not bound)
    for ctor in constructors:
        if ctor.is_copy_constructor() or ctor.is_move_constructor():
            continue
        params = list(ctor.get_arguments())
        if not params:
            continue
        param_objs = [make_param(p) for p in params]
        if any(_is_string_param(p.type) for p in param_objs):
            continue
        if any(p.type.kind == TypeKind.UNKNOWN for p in param_objs):
            continue
        bind_class.constructors.append(BindConstructor(params=param_objs))


def parse_module(config: ModuleConfig, repo_root: Path) -> BindModule:
    repo_root = repo_root.resolve()
    headers = [(repo_root / h).resolve() for h in config.headers]

    idx = cindex.Index.create()

    # Compose a tiny umbrella TU that includes every header. Faster than parsing
    # each header in isolation and avoids "header doesn't include what it uses"
    # follow-on issues.
    umbrella = '\n'.join(f'#include "{h.as_posix()}"' for h in headers) + '\n'
    args = ['-std=c++20', '-x', 'c++', '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH']
    for inc in config.include_dirs:
        args.append(f'-I{(repo_root / inc).as_posix()}')

    tu = idx.parse('umbrella.cpp', args=args,
                   unsaved_files=[('umbrella.cpp', umbrella)],
                   options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)

    # Surface any hard errors (warnings are fine).
    for d in tu.diagnostics:
        if d.severity >= 3:
            print(f'  clang error: {d.spelling} @ {d.location}')

    module = BindModule(
        name=config.name,
        js_prefix=config.js_prefix,
        cpp_namespace=config.cpp_namespace,
        embind_block=config.embind_block,
        headers=config.headers,
        skip_vector_js_names=list(config.skip_vector_js_names),
        skip_class_js_names=list(config.skip_class_js_names),
    )

    # First pass: collect every @bind-annotated class and enum so we know
    # which names are "known" when classifying field types.
    # Each tuple: (cursor, annotations, cpp_qualifier, cpp_namespace).
    raw_classes:   list[tuple[cindex.Cursor, dict, str, str]] = []
    raw_enums:     list[tuple[cindex.Cursor, dict, str, str]] = []
    raw_constants: list[tuple[cindex.Cursor, dict, str, str]] = []

    auto_skip = set(config.auto_bind_skip)

    def _should_bind(ann: dict, child_qual: str, scope_qual: str, cpp_ns: str) -> bool:
        """Decide whether a type at `scope_qual::child_qual` is bound.

        `cpp_ns` is the FULL C++ namespace the type lives in, e.g.
        'whiteout::m2'. Auto-bind only fires when this matches the module's
        configured cpp_namespace.
        """
        if ann.get('skip'):
            return False
        if is_bound(ann):
            return True
        if not config.auto_bind:
            return False
        if cpp_ns != config.cpp_namespace:
            return False
        if scope_qual:
            return False  # nested types must opt in explicitly
        if child_qual.split('::')[-1] in auto_skip:
            return False
        return True

    def visit(cursor: cindex.Cursor, scope_qual: str = '', cpp_ns: str = ''):
        for child in cursor.get_children():
            if not _is_under_paths(child, headers):
                if child.kind == CursorKind.NAMESPACE:
                    new_ns = (cpp_ns + '::' if cpp_ns else '') + child.spelling
                    visit(child, scope_qual, new_ns)
                continue
            ann = parse_annotations(child.raw_comment)

            if child.kind == CursorKind.NAMESPACE:
                new_ns = (cpp_ns + '::' if cpp_ns else '') + child.spelling
                visit(child, scope_qual, new_ns)

            elif child.kind in (CursorKind.STRUCT_DECL, CursorKind.CLASS_DECL,
                                CursorKind.CLASS_TEMPLATE):
                if not child.spelling:
                    continue
                qual = (scope_qual + '::' if scope_qual else '') + child.spelling
                is_def = child.is_definition() or child.kind == CursorKind.CLASS_TEMPLATE
                if is_def:
                    if (_should_bind(ann, qual, scope_qual, cpp_ns)
                            and child.kind != CursorKind.CLASS_TEMPLATE):
                        raw_classes.append((child, ann, qual, cpp_ns))
                    # Recurse INTO the struct/class for nested types — keep
                    # cpp_ns the same (we're inside a class, not a namespace).
                    visit(child, qual, cpp_ns)

            elif child.kind == CursorKind.ENUM_DECL:
                if not child.spelling:
                    continue
                qual = (scope_qual + '::' if scope_qual else '') + child.spelling
                if child.is_definition() and _should_bind(ann, qual, scope_qual, cpp_ns):
                    raw_enums.append((child, ann, qual, cpp_ns))

            elif child.kind == CursorKind.TYPE_ALIAS_DECL:
                if is_bound(ann):
                    raw_classes.append((child, ann, child.spelling, cpp_ns))

            elif child.kind == CursorKind.VAR_DECL:
                if is_bound(ann):
                    qual = (scope_qual + '::' if scope_qual else '') + child.spelling
                    raw_constants.append((child, ann, qual, cpp_ns))

    visit(tu.cursor)

    known_classes = {q for (_, _, q, _) in raw_classes}
    known_enums   = {q for (_, _, q, _) in raw_enums}

    # Second pass: build the IR objects and discover container types.
    vector_types: dict[str, TypeRef] = {}    # cpp_text -> TypeRef
    track_types:  dict[str, TypeRef] = {}

    def remember_containers(t: TypeRef):
        """Walk a type and remember every container we'll need to register."""
        if t.kind in (TypeKind.VECTOR, TypeKind.NESTED_VEC):
            vector_types.setdefault(t.cpp_text, t)
            remember_containers(t.element)
        elif t.kind == TypeKind.TRACK:
            track_types.setdefault(t.cpp_text, t)
            # Track<T>::keys_data is std::vector<T>, so we need vector<T> too.
            inner_vec = TypeRef(cpp_text=f'std::vector<{t.element.cpp_text}>',
                                kind=TypeKind.VECTOR, element=t.element)
            vector_types.setdefault(inner_vec.cpp_text, inner_vec)
            # Timestamps are vector<u32>.
            ts = TypeRef(cpp_text='std::vector<u32>', kind=TypeKind.VECTOR,
                         element=TypeRef(cpp_text='u32', kind=TypeKind.PRIMITIVE))
            vector_types.setdefault(ts.cpp_text, ts)
        elif t.kind == TypeKind.ARRAY:
            remember_containers(t.element)

    # Build enums.
    for cursor, ann, qual, ns in raw_enums:
        bind_enum = BindEnum(
            cpp_qualifier=qual,
            cpp_namespace=ns,
            js_name=ann.get('js_name') or js_name_for_enum(qual, config.js_prefix),
            values=[
                BindEnumValue(
                    js_name=v.spelling,
                    cpp_qualifier=f'{qual}::{v.spelling}',
                )
                for v in cursor.get_children()
                if v.kind == CursorKind.ENUM_CONSTANT_DECL
            ],
        )
        module.enums.append(bind_enum)

    # Build classes.
    for cursor, ann, qual, ns in raw_classes:
        bind_class = BindClass(
            cpp_qualifier=qual,
            cpp_namespace=ns,
            js_name=ann.get('js_name') or js_name_for_class(qual, config.js_prefix),
            is_value_object='value_object' in ann,
        )

        # `fields=x;y;z` on the class annotation overrides AST inspection.
        # Required for types whose members live in anonymous unions/structs
        # (Vector3f, Quaternion) — libclang doesn't expose them as direct
        # FIELD_DECLs of the parent struct.
        explicit_fields = ann.get('fields')
        if explicit_fields:
            for fname in explicit_fields.split(';'):
                fname = fname.strip()
                if not fname:
                    continue
                bind_class.fields.append(BindField(
                    name=fname,
                    cpp_name=fname,
                    type=TypeRef(cpp_text='f32', kind=TypeKind.PRIMITIVE),
                ))
            module.classes.append(bind_class)
            continue

        # type_alias decls have no children; require explicit fields=.
        if cursor.kind == CursorKind.TYPE_ALIAS_DECL:
            module.classes.append(bind_class)
            continue

        # Walk struct fields. Recurse into anonymous unions/structs so we pick
        # up named members hidden inside them. Skip private/protected fields
        # (Embind can't take the address of them anyway).
        def collect_fields(c):
            for member in c.get_children():
                if member.kind == CursorKind.FIELD_DECL:
                    if member.access_specifier in (
                            cindex.AccessSpecifier.PRIVATE,
                            cindex.AccessSpecifier.PROTECTED):
                        continue
                    field_ann = parse_annotations(member.raw_comment)
                    if field_ann.get('skip'):
                        continue
                    cpp = member.type.get_canonical().spelling
                    tref = classify_type(cpp, known_classes, known_enums)
                    # Skip fields whose type can't be cleanly bound across
                    # backends — e.g. vector<array<T,N>> needs a specially-
                    # named container plus per-element conversions.
                    if _has_unbindable_inner(tref):
                        continue
                    remember_containers(tref)
                    bind_class.fields.append(BindField(
                        name=field_ann.get('rename', member.spelling),
                        cpp_name=member.spelling,
                        type=tref,
                        array_with_view=bool(field_ann.get('array_with_view')),
                    ))
                elif member.kind in (CursorKind.UNION_DECL, CursorKind.STRUCT_DECL) \
                        and not member.spelling:
                    collect_fields(member)
        collect_fields(cursor)

        # Walk methods + non-default constructors when the class has been
        # explicitly @bind'd with `methods` (we don't auto-bind methods on
        # auto_bind'd classes — too easy to expose internal implementation
        # details).
        if ann.get('methods'):
            collect_methods(cursor, bind_class, known_classes, known_enums,
                            remember_containers, ann.get('methods'))

        module.classes.append(bind_class)

    # Build constants.
    for cursor, ann, qual, ns in raw_constants:
        module.constants.append(BindConstant(
            js_name=ann.get('js_name') or (config.js_prefix + cursor.spelling),
            cpp_expr=ann.get('cpp_expr', qual),
            cpp_type='u32',
        ))

    module.vector_types = list(vector_types.values())
    module.track_types  = list(track_types.values())

    return module
