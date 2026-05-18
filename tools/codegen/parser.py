# SPDX-License-Identifier: BSD-3-Clause
"""libclang-based parser that builds a BindModule from annotated headers."""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Optional

from clang import cindex
from clang.cindex import CursorKind, TypeKind as CXTypeKind

from .annotations import parse as parse_annotations, is_bound, extract_doc
from .ir import (
    BindClass, BindConstant, BindConstructor, BindEnum, BindEnumValue,
    BindField, BindMethod, BindMethodParam, BindModule, BindTemplate,
    BindTemplateField, ModuleConfig, TypeKind, TypeRef,
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


def classify_type(cpp_text: str, known_classes: set[str], known_enums: set[str],
                  known_templates: dict[str, str] | None = None) -> TypeRef:
    """Classify a raw C++ type spelling into a TypeRef.

    `known_templates` maps every spelling under which a `@bind value_template`
    template might appear (`'Track'`, `'mdx::Track'`, `'whiteout::mdx::Track'`)
    to its canonical fully-qualified name (`'whiteout::mdx::Track'`). When a
    type matches one of these templates, we treat the instantiation as a
    NESTED reference to a synthetic concrete class — the codegen later
    materialises that class by substituting the template parameter."""
    if known_templates is None:
        known_templates = {}
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
            inner = classify_type(args[0], known_classes, known_enums, known_templates)
            kind = TypeKind.NESTED_VEC if inner.kind == TypeKind.VECTOR else TypeKind.VECTOR
            return TypeRef(cpp_text=t, kind=kind, element=inner)
        # std::array<X, N>
        if base in ('std::array', 'array'):
            inner = classify_type(args[0], known_classes, known_enums, known_templates)
            try:
                n = int(args[1].strip())
            except (ValueError, IndexError):
                n = None
            return TypeRef(cpp_text=t, kind=TypeKind.ARRAY, element=inner, array_size=n)
        # std::optional<X> — Embind has built-in JS<->std::optional conversion.
        if base in ('std::optional', 'optional'):
            inner = classify_type(args[0], known_classes, known_enums, known_templates)
            return TypeRef(cpp_text=t, kind=TypeKind.OPTIONAL, element=inner)
        # `@bind value_template` instantiation — synthesise a NESTED ref to a
        # concrete class the parser will materialise later. The element TypeRef
        # carries the template argument so naming helpers (js_name_for_type)
        # can reach it.
        if base in known_templates:
            template_qual = known_templates[base]
            inner = classify_type(args[0], known_classes, known_enums, known_templates)
            instantiation = f'{template_qual}<{inner.cpp_text}>'
            return TypeRef(cpp_text=instantiation, kind=TypeKind.NESTED,
                           element=inner)

    if t in known_classes or short in known_classes:
        return TypeRef(cpp_text=t, kind=TypeKind.NESTED)

    return TypeRef(cpp_text=t, kind=TypeKind.UNKNOWN)


def _template_for_instantiation(cpp_text: str,
                                templates: dict[str, BindTemplate]
                                ) -> Optional[BindTemplate]:
    """Map an instantiation cpp_text like 'whiteout::mdx::Track<float>' back
    to the BindTemplate that produced it. Returns None for plain
    (non-template) NESTED types."""
    m = TEMPLATE_RE.match(cpp_text)
    if not m:
        return None
    base = m.group('base')
    # Match by short name (last `::` component) since that's how templates
    # are keyed.
    short = base.split('::')[-1]
    return templates.get(short)


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
                value=int(child.enum_value),
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
        # `value_template` instantiation: classify_type produced a NESTED
        # with .element set to the template argument. Name it as
        # `<prefix><TemplateShort><T-as-js-name>` so e.g. `AnimRef<f32>`
        # becomes `M3AnimRefF32`, matching the synthetic BindClass.
        if t.kind == TypeKind.NESTED and t.element is not None and '<' in t.cpp_text:
            m = TEMPLATE_RE.match(t.cpp_text)
            if m:
                template_short = m.group('base').split('::')[-1]
                return prefix + template_short + js_name_for_type(t.element, prefix)
        # Shared math types stay un-prefixed; everything else gets js_prefix.
        short = _short_name(t.cpp_text)
        if short in ('Vector2f', 'Vector3f', 'Vector4f', 'Quaternion'):
            return short
        return prefix + short.replace('::', '')
    if t.kind == TypeKind.VECTOR:
        return 'Vector' + js_name_for_type(t.element, prefix)
    if t.kind == TypeKind.NESTED_VEC:
        return 'Vector' + js_name_for_type(t.element, prefix)
    # UNKNOWN / fallback: scrub `::` and lower-cased namespace parts so the
    # output is a valid C++/JS identifier (e.g. `whiteout::interfaces::Foo`
    # collapses to `Foo`).
    short = _short_name(t.cpp_text).replace('::', '')
    return short or 'Unknown'


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
    # A value_template instantiation (NESTED with .element set) whose T
    # is itself unbindable — `Track<UnboundType>` — has nowhere to point.
    if t.kind == TypeKind.NESTED and t.element is not None:
        return _has_unbindable_inner(t.element)
    return False


def _is_span_const_u8(t: TypeRef) -> bool:
    """Matches `std::span<const u8>` specifically — the bytes-in idiom.
    Kept for the bytes-marshalling path which uses py::bytes / val."""
    if t.kind != TypeKind.UNKNOWN:
        return False
    s = t.cpp_text
    return s.startswith('std::span<') and ('uint8_t' in s or ' u8' in s
                                            or 'whiteout::u8' in s
                                            or 'unsigned char' in s)


# Map C++ scalar spellings (as they appear inside `std::span<const X>`) to a
# (short_name, fundamental) tuple used by the codegen for buffer marshalling.
# Short name matches `_short_name(...)` output for the type so the rest of
# the pipeline can re-resolve it.
_SPAN_SCALAR_TABLE = {
    'unsigned char':      ('u8',  'unsigned char'),
    'uint8_t':            ('u8',  'unsigned char'),
    'u8':                 ('u8',  'unsigned char'),
    'unsigned short':     ('u16', 'unsigned short'),
    'uint16_t':           ('u16', 'unsigned short'),
    'u16':                ('u16', 'unsigned short'),
    'unsigned int':       ('u32', 'unsigned int'),
    'uint32_t':           ('u32', 'unsigned int'),
    'u32':                ('u32', 'unsigned int'),
    'unsigned long long': ('u64', 'unsigned long long'),
    'uint64_t':           ('u64', 'unsigned long long'),
    'u64':                ('u64', 'unsigned long long'),
    'signed char':        ('i8',  'signed char'),
    'int8_t':             ('i8',  'signed char'),
    'i8':                 ('i8',  'signed char'),
    'short':              ('i16', 'short'),
    'int16_t':            ('i16', 'short'),
    'i16':                ('i16', 'short'),
    'int':                ('i32', 'int'),
    'int32_t':            ('i32', 'int'),
    'i32':                ('i32', 'int'),
    'long long':          ('i64', 'long long'),
    'int64_t':            ('i64', 'long long'),
    'i64':                ('i64', 'long long'),
    'float':              ('f32', 'float'),
    'f32':                ('f32', 'float'),
    'double':             ('f64', 'double'),
    'f64':                ('f64', 'double'),
}


def _span_scalar(t: TypeRef) -> Optional[tuple[str, str]]:
    """If `t` is `std::span<const X>` for a recognised scalar X, return
    `(short_name, canonical_cpp)`. Otherwise None.

    Generalises `_is_span_const_u8` — used by the codegen to marshal a
    span of any primitive directly from a numpy array / typed array,
    skipping an opaque-vector round trip.
    """
    if t.kind != TypeKind.UNKNOWN:
        return None
    s = t.cpp_text
    if not s.startswith('std::span<'):
        return None
    # Strip `std::span<` ... `>` and any leading `const`.
    inner = s[len('std::span<'):]
    if inner.endswith('>'):
        inner = inner[:-1]
    inner = inner.strip()
    if inner.startswith('const '):
        inner = inner[len('const '):].strip()
    # Drop any trailing template arg (e.g. extent), keep just the element.
    if ',' in inner:
        inner = inner.split(',', 1)[0].strip()
    # Normalise leading `whiteout::` typedef so the lookup table hits.
    inner_short = inner.removeprefix('whiteout::')
    return _SPAN_SCALAR_TABLE.get(inner_short) or _SPAN_SCALAR_TABLE.get(inner)


def _is_vector_u8(t: TypeRef) -> bool:
    if t.kind != TypeKind.VECTOR:
        return False
    return t.element.kind == TypeKind.PRIMITIVE and \
           _short_name(t.element.cpp_text) in ('u8', 'unsigned char')


def _is_string_param(t: TypeRef) -> bool:
    return t.kind == TypeKind.STRING


# Capture `T` from canonical spellings like
#   `std::function<void(whiteout::interfaces::HttpResponse)>`
#   `std::function<void()>`
# Returns the short name of T (e.g. "HttpResponse"), or "void" for the
# no-arg form, or '' when the spelling isn't a function callback.
_FUNCTION_CALLBACK_RE = re.compile(r'std::function\s*<\s*void\s*\((?P<args>[^)]*)\)\s*>')


def _extract_callback_target(cpp_text: str) -> str:
    m = _FUNCTION_CALLBACK_RE.search(cpp_text)
    if m is None:
        return ''
    args = m.group('args').strip()
    if not args:
        return 'void'  # std::function<void()> — Runnable in Java
    # Strip the parameter name if present (`HttpResponse r` → `HttpResponse`).
    # The canonical form is just the type — but be defensive.
    args = args.split()[0]
    return _short_name(args)


def collect_methods(cursor, bind_class, known_classes, known_enums,
                    remember_containers, mode: str | bool,
                    known_templates: dict[str, str] | None = None,
                    class_ann: dict | None = None):
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
        tref = classify_type(cpp, known_classes, known_enums, known_templates)
        remember_containers(tref)
        # Detect std::function<...> callback shapes. The JNI backend
        # turns these into Java functional-interface params (Consumer<T>,
        # Runnable, etc.) and generates the matching wrapper class.
        callback_target = _extract_callback_target(cpp)
        # A default value shows up as an expression child of the param
        # cursor. Different default forms produce different cursor kinds
        # (UNEXPOSED_EXPR for `pool = nullptr`, INIT_LIST_EXPR for `opts =
        # {}`, CALL_EXPR for `opts = CreateOptions()`, INTEGER_LITERAL for
        # `n = 0`, …); accept any cursor whose kind name carries `_EXPR`
        # or `_LITERAL` so we don't have to enumerate them all.
        def _is_default_expr(c):
            kn = c.kind.name
            return '_EXPR' in kn or '_LITERAL' in kn
        return BindMethodParam(
            name=arg_cursor.spelling or 'arg',
            type=tref,
            has_default=any(_is_default_expr(c) for c in arg_cursor.get_children()),
            cpp_raw=cpp_raw,
            span_scalar=_span_scalar(tref),
            callback_target=callback_target,
        )

    def _has_pointer_param(o) -> bool:
        """True if the overload has any pointer parameter (`Foo*`).
        Pointers are typically used for diagnostic outputs (`std::string*`)
        or unbindable resources (`WorkerPool*`); skip them."""
        for a in o.get_arguments():
            spelling = a.type.spelling
            if '*' in spelling and 'span' not in spelling:
                return True
        return False

    def _is_interface_pointer(p):
        return 'interfaces::' in p.cpp_raw

    def _bindable(p):
        if p.span_scalar is not None:
            return True
        if '*' in p.cpp_raw:
            return _is_interface_pointer(p)
        return True

    def _try_emit_overload(name, chosen, method_ann, total_overloads,
                            name_suffix=''):
        """Render one overload into a BindMethod and append it to
        bind_class.methods. Returns True if emitted, False if filtered
        (unbindable return, pointer-return, vector<array>, etc.)."""
        ret_cpp = chosen.result_type.get_canonical().spelling
        ret_raw = chosen.result_type.spelling
        if ('*' in ret_cpp and 'span' not in ret_cpp) \
                or ('*' in ret_raw and 'span' not in ret_raw):
            return False
        ret = classify_type(ret_cpp, known_classes, known_enums, known_templates)
        return_is_reference = ret_cpp.rstrip().endswith('&')

        def _vector_of_array(t):
            if t.kind in (TypeKind.VECTOR, TypeKind.NESTED_VEC):
                if t.element.kind == TypeKind.ARRAY:
                    return True
                return _vector_of_array(t.element)
            return False
        if _vector_of_array(ret):
            return False
        remember_containers(ret)

        all_params = [make_param(a) for a in chosen.get_arguments()]
        params = list(all_params)
        while params and params[-1].has_default and '*' in params[-1].cpp_raw \
                and not _is_interface_pointer(params[-1]):
            params.pop()
        trimmed = len(params) < len(all_params)

        if not all(_bindable(p) for p in params):
            return False

        bytes_in = any(p.span_scalar is not None for p in params)
        bytes_out = _is_vector_u8(ret) or (
            ret.kind == TypeKind.OPTIONAL and _is_vector_u8(ret.element))
        optional_class_return = (
            ret.kind == TypeKind.OPTIONAL
            and ret.element is not None
            and ret.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN)
        )

        try:
            from clang.cindex import ExceptionSpecificationKind as _ESK
            _spec = chosen.exception_specification_kind
            is_noexcept = _spec in (
                _ESK.BASIC_NOEXCEPT,
                _ESK.COMPUTED_NOEXCEPT,
                _ESK.DYNAMIC_NONE,
                _ESK.UNEVALUATED,
            )
        except Exception:
            is_noexcept = False

        bind_class.methods.append(BindMethod(
            name=method_ann.get('rename', name) + name_suffix,
            cpp_name=name,
            return_type=ret,
            params=params,
            is_const=chosen.is_const_method(),
            is_static=chosen.is_static_method(),
            bytes_in=bytes_in,
            bytes_out=bytes_out,
            needs_wrapper=trimmed or optional_class_return,
            is_overloaded=(total_overloads > 1),
            return_is_reference=return_is_reference,
            is_noexcept=is_noexcept,
            annotations=dict(method_ann),
            doc=extract_doc(chosen.raw_comment),
        ))
        return True

    for name, overloads in methods_by_name.items():
        method_ann = parse_annotations(
            overloads[0].raw_comment) if overloads else {}
        if method_ann.get('skip'):
            continue

        # `buffer_only` mode (BLP/JPEG/PNG writers): pick exactly ONE
        # buffer-returning overload to expose. Everywhere else we emit
        # every bindable overload, disambiguated by the first non-default
        # param name(s) — Java natively supports overloading by signature,
        # so the C symbol is the only thing that needs uniqueness.
        if mode == 'buffer_only' and len(overloads) > 1:
            chosen = None
            for o in overloads:
                ps = list(o.get_arguments())
                if not ps:
                    continue
                first_cpp = ps[0].type.spelling
                if 'span' in first_cpp and ('u8' in first_cpp
                                             or 'uint8_t' in first_cpp):
                    chosen = o
                    break
            if chosen is None:
                for o in overloads:
                    ps = list(o.get_arguments())
                    if ps and 'string' not in ps[0].type.spelling:
                        chosen = o
                        break
            if chosen is None:
                chosen = overloads[0]
            _try_emit_overload(name, chosen, method_ann, len(overloads))
            continue

        # Collapse const/non-const overload pairs with identical param
        # type signatures: Java has no const-overloading, so a single
        # entry suffices. Prefer the const variant — it's the read-only
        # face, matches reasonable callers' default expectation.
        by_sig: dict[str, list] = {}
        for o in overloads:
            type_sig = '|'.join(
                a.type.get_canonical().spelling for a in o.get_arguments())
            by_sig.setdefault(type_sig, []).append(o)
        deduped: list = []
        for _sig, group in by_sig.items():
            if len(group) > 1:
                const_first = sorted(
                    group, key=lambda o: not o.is_const_method())
                deduped.append(const_first[0])
            else:
                deduped.append(group[0])
        overloads_to_emit = deduped

        # Standard path: walk every (deduped) overload, skipping ones
        # with raw pointer params (diagnostic outputs / unbindable
        # resources).
        emitted = 0
        seen_param_sigs: set = set()
        for o in overloads_to_emit:
            if _has_pointer_param(o):
                # Allow whiteout::interfaces::X* pointers — those route
                # through the NativeHandled / *Bridge dispatch layer.
                params_iter = list(o.get_arguments())
                if not all(('interfaces::' in p.type.spelling)
                           or '*' not in p.type.spelling
                           or 'span' in p.type.spelling
                           for p in params_iter):
                    continue

            # Build a coarse signature key (param-name list) so two
            # overloads producing the same C symbol collide cleanly. The
            # _N suffix is only added once we hit a real collision.
            sig_key = '_'.join(
                (a.spelling or 'arg') for a in o.get_arguments())
            suffix = ''
            if sig_key in seen_param_sigs:
                # Tiebreaker if param names collide too — shouldn't
                # happen in our codebase but keep the symbol unique.
                suffix = f'_dup{emitted + 1}'
            elif emitted > 0:
                # First overload: no suffix (keeps the common name).
                # Subsequent ones: use the param-name list as a suffix
                # (mirrors the ctor pattern, e.g. `write_frames_opts`).
                suffix = f'_{sig_key}' if sig_key else f'_overload{emitted + 1}'
            seen_param_sigs.add(sig_key)

            if _try_emit_overload(name, o, method_ann, len(overloads), suffix):
                emitted += 1
        continue

    # Non-default constructors. Skip:
    #   - the implicit default ctor (already emitted separately)
    #   - copy / move constructors (PImpl classes have these deleted, and
    #     binding them would fail to compile)
    #   - any ctor taking std::string& (path-based)  — unless the class is
    #     `@bind ctors=string`, used for openers like `OsFileSystem(root)`.
    #   - any ctor taking an UNKNOWN type (e.g. WorkerPool* — not bound)
    ctors_modes = (class_ann or {}).get('ctors', '')
    if not isinstance(ctors_modes, str):
        ctors_modes = ''
    allow_string_ctors = 'string' in ctors_modes
    for ctor in constructors:
        if ctor.is_copy_constructor() or ctor.is_move_constructor():
            continue
        params = list(ctor.get_arguments())
        if not params:
            continue
        param_objs = [make_param(p) for p in params]
        if any(_is_string_param(p.type) for p in param_objs) and not allow_string_ctors:
            continue
        # UNKNOWN params are typically un-bindable (raw pointers to types
        # the codegen doesn't model). EXCEPTION: `whiteout::interfaces::X*`
        # is routed through the host bindings' NativeHandled/*Bridge
        # dispatch layer — those are bindable.
        if any(p.type.kind == TypeKind.UNKNOWN and not _is_interface_pointer(p)
               for p in param_objs):
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
    args = [
        '-std=c++20', '-x', 'c++',
        '-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH',
        # Storage backends have #error guards that require their feature
        # macro. Define them here so the codegen can parse the headers
        # regardless of how the cmake build is configured.
        '-DWHITEOUT_HAS_MPQ=1',
        '-DWHITEOUT_HAS_CASC=1',
    ]
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
    # @bind value_template — class templates whose instantiations the parser
    # later materialises as concrete classes. Stored separately so they
    # don't accidentally get bound as if they were complete classes.
    raw_templates: list[tuple[cindex.Cursor, dict, str, str]] = []

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
                    if (child.kind == CursorKind.CLASS_TEMPLATE
                            and ann.get('value_template')):
                        raw_templates.append((child, ann, qual, cpp_ns))
                    elif (_should_bind(ann, qual, scope_qual, cpp_ns)
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

    # Build BindTemplate IR for every `@bind value_template` class template.
    # We record the field list with the type-parameter spelling preserved so
    # we can substitute it later when synthesising concrete instantiations.
    templates: dict[str, BindTemplate] = {}             # cpp_short -> BindTemplate
    known_templates: dict[str, str] = {}                # spelling -> fully-qualified
    for cursor, ann, qual, ns in raw_templates:
        # The template parameter spelling (almost always 'T').
        type_param = 'T'
        for c in cursor.get_children():
            if c.kind == CursorKind.TEMPLATE_TYPE_PARAMETER:
                type_param = c.spelling or 'T'
                break
        # Detect a single public base class — its fields are flattened into
        # every instantiation so AnimationTrack<T> picks up
        # AnimationTrackBase's interpolationType/globalSequenceId/timestamps.
        base_qual = ''
        for c in cursor.get_children():
            if c.kind == CursorKind.CXX_BASE_SPECIFIER:
                base_qual = c.type.get_canonical().spelling
                break
        # Walk fields, preserving the raw template-parameter spelling in
        # cpp_text_template (libclang gives us 'std::vector<T>' literally for
        # template members).
        tpl_fields: list[BindTemplateField] = []
        def collect_template_fields(c):
            for member in c.get_children():
                if member.kind == CursorKind.FIELD_DECL:
                    if member.access_specifier in (
                            cindex.AccessSpecifier.PRIVATE,
                            cindex.AccessSpecifier.PROTECTED):
                        continue
                    f_ann = parse_annotations(member.raw_comment)
                    if f_ann.get('skip'):
                        continue
                    tpl_fields.append(BindTemplateField(
                        name=f_ann.get('rename', member.spelling),
                        cpp_name=member.spelling,
                        cpp_text_template=member.type.spelling,
                        doc=extract_doc(member.raw_comment),
                    ))
                elif member.kind in (CursorKind.UNION_DECL,
                                     CursorKind.STRUCT_DECL) \
                        and not member.spelling:
                    collect_template_fields(member)
        collect_template_fields(cursor)
        # Parse the explicit instantiate=A;B;C list (canonicalising the
        # tokens lightly — we just trim whitespace; classify_type will do
        # the rest).
        inst_raw = ann.get('instantiate', '')
        instantiate = [s.strip() for s in inst_raw.split(';') if s.strip()]
        tpl = BindTemplate(
            cpp_short=cursor.spelling,
            cpp_qualifier=qual,
            cpp_namespace=ns,
            type_param=type_param,
            fields=tpl_fields,
            instantiate=instantiate,
            base_cpp_qualifier=base_qual,
            doc=extract_doc(cursor.raw_comment),
        )
        templates[cursor.spelling] = tpl
        # Spelling variants classify_type may see for this template — by the
        # short name, namespace-prefixed, or fully-qualified.
        full_qual = (ns + '::' + qual) if ns else qual
        known_templates[cursor.spelling] = full_qual
        known_templates[qual] = full_qual
        known_templates[full_qual] = full_qual

    # Second pass: build the IR objects and discover container types.
    vector_types: dict[str, TypeRef] = {}    # cpp_text -> TypeRef
    # Collected during field walking; consumed by the third pass that
    # materialises a concrete BindClass per (template, T) tuple.
    instantiations: dict[str, tuple[BindTemplate, TypeRef]] = {}  # cpp_text -> (template, T-TypeRef)

    def remember_containers(t: TypeRef):
        """Walk a type and remember every container we'll need to register."""
        if t.kind in (TypeKind.VECTOR, TypeKind.NESTED_VEC):
            vector_types.setdefault(t.cpp_text, t)
            remember_containers(t.element)
        elif t.kind == TypeKind.ARRAY:
            remember_containers(t.element)
        elif t.kind == TypeKind.NESTED and t.element is not None:
            # A NESTED with .element set is a value_template instantiation
            # produced by classify_type. Record it for later synthesis,
            # but only when the template argument is itself bindable —
            # `Track<UnboundType>` would generate a class name like
            # `MdxTrack<UnboundType>` (containing `<>` chars) which breaks
            # downstream identifier generation and filenames.
            tpl = _template_for_instantiation(t.cpp_text, templates)
            if tpl is not None and t.element.kind != TypeKind.UNKNOWN:
                instantiations.setdefault(t.cpp_text, (tpl, t.element))
            remember_containers(t.element)

    # Build enums.
    for cursor, ann, qual, ns in raw_enums:
        bind_enum = BindEnum(
            cpp_qualifier=qual,
            cpp_namespace=ns,
            js_name=ann.get('js_name') or js_name_for_enum(qual, config.js_prefix),
            doc=extract_doc(cursor.raw_comment),
            values=[
                BindEnumValue(
                    js_name=v.spelling,
                    cpp_qualifier=f'{qual}::{v.spelling}',
                    doc=extract_doc(v.raw_comment),
                    value=int(v.enum_value),
                )
                for v in cursor.get_children()
                if v.kind == CursorKind.ENUM_CONSTANT_DECL
            ],
        )
        module.enums.append(bind_enum)

    # Build classes.
    for cursor, ann, qual, ns in raw_classes:
        # Auto-detect missing public default constructor: when the class
        # declares any ctor and none of them are no-arg public, py::init<>()
        # would fail to compile. Annotating @bind no_default_ctor overrides
        # (forces suppression even if the class technically has one).
        declared_ctors = [c for c in cursor.get_children()
                          if c.kind == CursorKind.CONSTRUCTOR]
        has_public_default = any(
            c.is_default_constructor()
            and c.access_specifier == cindex.AccessSpecifier.PUBLIC
            for c in declared_ctors
        ) or not declared_ctors   # implicit-default when no ctors declared
        auto_no_default = not has_public_default

        # Move-only detection: a class is move-only if its copy constructor
        # is explicitly deleted. libclang exposes this via
        # `is_deleted_method()` on the cursor (where available).
        def _ctor_is_deleted(c):
            return getattr(c, 'is_deleted_method', lambda: False)()
        auto_move_only = any(
            c.is_copy_constructor() and _ctor_is_deleted(c)
            for c in declared_ctors
        )

        bind_class = BindClass(
            cpp_qualifier=qual,
            cpp_namespace=ns,
            js_name=ann.get('js_name') or js_name_for_class(qual, config.js_prefix),
            is_value_object='value_object' in ann,
            doc=extract_doc(cursor.raw_comment),
            base_class=ann.get('extends', ''),
            no_default_ctor=auto_no_default or 'no_default_ctor' in ann,
            is_move_only=auto_move_only or 'move_only' in ann,
            is_subclassable='subclassable' in ann,
            jni_package=ann.get('jni_package', '') or '',
            java_package=ann.get('java_package', '') or '',
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
                    tref = classify_type(cpp, known_classes, known_enums,
                                         known_templates)
                    # Skip fields whose type can't be cleanly bound across
                    # backends — e.g. vector<array<T,N>> needs a specially-
                    # named container plus per-element conversions.
                    if _has_unbindable_inner(tref):
                        continue
                    remember_containers(tref)
                    # libclang reports the offset in BITS; we want bytes.
                    # Negative values mean libclang couldn't determine the
                    # offset (templated, bitfield in a packed struct, …).
                    try:
                        bit_off = member.get_field_offsetof()
                    except Exception:
                        bit_off = -1
                    byte_off = bit_off // 8 if bit_off is not None and bit_off >= 0 \
                                            else None
                    bind_class.fields.append(BindField(
                        name=field_ann.get('rename', member.spelling),
                        cpp_name=member.spelling,
                        type=tref,
                        array_with_view=bool(field_ann.get('array_with_view')),
                        doc=extract_doc(member.raw_comment),
                        byte_offset=byte_off,
                    ))
                elif member.kind in (CursorKind.UNION_DECL, CursorKind.STRUCT_DECL) \
                        and not member.spelling:
                    collect_fields(member)
        collect_fields(cursor)

        # Whole-struct sizeof. libclang returns negative numbers for
        # incomplete / template-instantiated types where it can't compute.
        try:
            type_size = cursor.type.get_size()
        except Exception:
            type_size = -1
        if type_size is not None and type_size > 0:
            bind_class.byte_size = type_size

        # POD-ness drives whether downstream backends can safely use
        # bitwise copy (memcpy) instead of the C++ copy-assignment
        # operator. libclang's `is_pod()` inspects *every* member
        # (including private/protected) so a class with a private
        # `std::string` correctly reports False even when we didn't
        # bind that member.
        try:
            bind_class.is_pod = bool(cursor.type.is_pod())
        except Exception:
            bind_class.is_pod = False

        # Walk methods + non-default constructors when the class has been
        # explicitly @bind'd with `methods` (we don't auto-bind methods on
        # auto_bind'd classes — too easy to expose internal implementation
        # details).
        if ann.get('methods'):
            collect_methods(cursor, bind_class, known_classes, known_enums,
                            remember_containers, ann.get('methods'),
                            known_templates=known_templates,
                            class_ann=ann)

        module.classes.append(bind_class)

    # Build constants.
    for cursor, ann, qual, ns in raw_constants:
        module.constants.append(BindConstant(
            js_name=ann.get('js_name') or (config.js_prefix + cursor.spelling),
            cpp_expr=ann.get('cpp_expr', qual),
            cpp_type='u32',
            doc=extract_doc(cursor.raw_comment),
        ))

    # Synthesise concrete classes for every (template, T) instantiation
    # discovered during field walking. Each concrete class is a regular
    # BindClass — emitters treat it identically to a hand-bound struct.
    # We also flatten any base-class fields into the synthesised class so
    # AnimationTrack<f32> picks up AnimationTrackBase's members.
    def _resolve_base_fields(base_qual: str) -> list[BindField]:
        if not base_qual:
            return []
        # Strip qualifiers so 'whiteout::m2::AnimationTrackBase' and
        # 'AnimationTrackBase' both match.
        candidates = {base_qual, _short_name(base_qual)}
        for cls in module.classes:
            if cls.cpp_qualifier in candidates:
                return list(cls.fields)
        return []

    def _substitute(cpp_template: str, type_param: str, value: str) -> str:
        # Whole-word substitution of the template parameter — avoid replacing
        # 'T' inside identifiers like 'TangentKey' or 'std::vector<TKey>'.
        return re.sub(rf'\b{re.escape(type_param)}\b', value, cpp_template)

    for inst_cpp_text, (tpl, t_ref) in instantiations.items():
        # Store cpp_qualifier in the relative-to-namespace form
        # ('Track<whiteout::Vector3f>') so emitters can splice in
        # cpp_namespace the same way they do for regular classes. The
        # full instantiation text (`whiteout::mdx::Track<...>`) is what
        # libclang gives us in field cpp_text — keep that around to
        # build map entries below.
        ns_prefix = tpl.cpp_namespace + '::' if tpl.cpp_namespace else ''
        synthetic_qual = (inst_cpp_text[len(ns_prefix):]
                          if inst_cpp_text.startswith(ns_prefix)
                          else inst_cpp_text)
        synthetic_js   = (config.js_prefix + tpl.cpp_short
                          + js_name_for_type(t_ref, config.js_prefix))
        bind_class = BindClass(
            cpp_qualifier=synthetic_qual,
            cpp_namespace=tpl.cpp_namespace,
            js_name=synthetic_js,
            is_value_object=False,
            doc=tpl.doc,
        )
        # Inherited base-class fields first (so the layout reads top-down
        # the way the C++ struct does).
        bind_class.fields.extend(_resolve_base_fields(tpl.base_cpp_qualifier))
        # Template-defined fields with `T` substituted.
        for tf in tpl.fields:
            substituted = _substitute(tf.cpp_text_template, tpl.type_param,
                                      t_ref.cpp_text)
            tref = classify_type(substituted, known_classes, known_enums,
                                 known_templates)
            if _has_unbindable_inner(tref):
                continue
            remember_containers(tref)
            bind_class.fields.append(BindField(
                name=tf.name,
                cpp_name=tf.cpp_name,
                type=tref,
                doc=tf.doc,
            ))
        module.classes.append(bind_class)

    module.vector_types = list(vector_types.values())
    module.templates    = list(templates.values())

    # Post-pass: cross-reference each method's return type against the
    # class registry to flag move-only returns. Used by emit_embind to
    # decide whether the binding needs an explicit `val(std::move(...))`
    # wrap. Match by both bare qualifier ("Storage") and fully-qualified
    # ("whiteout::storages::mpq::Storage") since classify_type's cpp_text
    # form depends on whether the type lived in `known_classes`.
    move_only_keys: set[str] = set()
    for c in module.classes:
        if not c.is_move_only:
            continue
        full = (f'{c.cpp_namespace}::{c.cpp_qualifier}'
                if c.cpp_namespace else c.cpp_qualifier)
        move_only_keys.add(c.cpp_qualifier)
        move_only_keys.add(full)
    for c in module.classes:
        for m in c.methods:
            # Reference returns (e.g. `Builder& declareX(...)`) are bound
            # natively by both backends — no move-only wrapping. Skip.
            if m.return_is_reference:
                continue
            ret = m.return_type
            # Both `T` and `std::optional<T>` returns benefit — for plain T,
            # the lambda wraps with `val(std::move(...))`; for optional<T>,
            # to_optional_val<T> already moves.
            target = ret.element if ret.kind == TypeKind.OPTIONAL and ret.element else ret
            if target.cpp_text in move_only_keys:
                m.return_is_move_only = True

    return module
