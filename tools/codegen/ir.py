# SPDX-License-Identifier: BSD-3-Clause
"""Backend-neutral IR for binding generation.

The parser produces a Module from C++ headers; emitters consume it to
generate Embind, pybind11, or any other binding code. Nothing in this
file should mention Embind or Python-binding specifics.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional


class TypeKind(Enum):
    PRIMITIVE   = 'primitive'    # u32, f32, bool, ...
    STRING      = 'string'       # std::string
    ENUM        = 'enum'         # bound enum
    NESTED      = 'nested'       # bound class/struct (passed by value/copy);
                                 # also: @bind value_template instantiations,
                                 # in which case .element holds the template
                                 # argument
    VECTOR      = 'vector'       # std::vector<T>
    NESTED_VEC  = 'nested_vec'   # std::vector<std::vector<T>>
    ARRAY       = 'array'        # std::array<T, N>  (needs getter/setter helper)
    OPTIONAL    = 'optional'     # std::optional<T>  (Embind has built-in support)
    UNKNOWN     = 'unknown'


@dataclass
class TypeRef:
    """A reference to a C++ type, in a form the emitter can use."""
    cpp_text: str            # raw C++ ("std::vector<Bone>", "Track<Vector3f>", "u32")
    kind: TypeKind = TypeKind.UNKNOWN
    element: Optional['TypeRef'] = None   # for VECTOR / NESTED_VEC / ARRAY / TRACK
    array_size: Optional[int] = None      # for ARRAY


@dataclass
class BindField:
    name: str                # JS name (after rename)
    cpp_name: str            # raw C++ field name
    type: TypeRef
    array_with_view: bool = False   # vector<u8>: also emit *View() helper
    doc: str = ''            # extracted from C++ /// comment
    # Byte offset of this field within its containing struct, computed
    # by libclang. `None` when libclang couldn't determine it (templated
    # type, anonymous bitfield, etc.). Backends use this to read/write
    # the field directly from a MemorySegment without going through the
    # C wrapper — only safe for plain-old-data fields (PRIMITIVE/ENUM
    # and bit-compatible nested PODs like Vector*).
    byte_offset: Optional[int] = None


@dataclass
class BindMethodParam:
    name: str
    type: TypeRef
    has_default: bool = False
    # Raw C++ argument type spelling (preserves const/ref qualifiers).
    # Used when the emit-side needs the full signature: ctor parameter
    # types (`Storage&`), `py::overload_cast` disambiguation, etc.
    cpp_raw: str = ''
    # If the param is `std::span<const X>` for some primitive scalar X,
    # this is the (short_name, canonical_cpp) tuple for X — e.g.
    # ("f32", "float") or ("u8", "unsigned char"). Used by the codegen to
    # marshal numpy arrays / typed-arrays directly into the C++ span.
    # None when the param is not a span-of-primitive.
    span_scalar: Optional[tuple[str, str]] = None


@dataclass
class BindMethod:
    name: str                  # JS/Python name
    cpp_name: str              # raw C++ method name
    return_type: TypeRef
    params: list[BindMethodParam] = field(default_factory=list)
    is_const: bool = False
    is_static: bool = False
    # When True, the first parameter is `std::span<const u8>` and should
    # be wrapped in backend-specific bytes-to-vector glue.
    bytes_in: bool = False
    # When True, the return type is `std::vector<u8>` and should be wrapped
    # in backend-specific vector-to-bytes glue. Also set when the return
    # is `std::optional<std::vector<u8>>` (→ bytes or None).
    bytes_out: bool = False
    # When True, even without bytes_in/out, the emitter must produce a
    # lambda wrapper rather than `&Class::method`. Set when trailing
    # default-value params were dropped (their C++ defaults stay in effect
    # at call time) and for static methods with non-trivial return types.
    needs_wrapper: bool = False
    # When True, the C++ source has multiple overloads of this method;
    # `&Class::method` is ambiguous and the emitter must disambiguate via
    # `py::overload_cast<...>(&Class::method)` (or a lambda wrapper).
    is_overloaded: bool = False
    # When True, the return type is a move-only class. WASM/Embind needs
    # an explicit `val(std::move(...))` wrap or the binding fails to
    # compile (no copy ctor to call). Filled in by a parser post-pass that
    # cross-references the class registry.
    return_is_move_only: bool = False
    # When True, the return type carries a `&` qualifier — e.g. methods
    # like `Builder& declareX(...)` that return `*this` for chaining.
    # Both backends bind reference returns natively (no `to_heap_ptr`
    # wrapper needed) since the JS/Python wrapper for the class type is
    # already a handle/reference.
    return_is_reference: bool = False
    doc: str = ''


@dataclass
class BindEnumValue:
    js_name: str
    cpp_qualifier: str       # "Layer::ShaderType::HD"
    doc: str = ''


@dataclass
class BindEnum:
    cpp_qualifier: str       # "Layer::ShaderType" or "InterpolationType"
    js_name: str             # "MdxLayerShaderType" or "MdxInterpolationType"
    values: list[BindEnumValue] = field(default_factory=list)
    cpp_namespace: str = ''  # full namespace, e.g. "whiteout::textures::blp"
    doc: str = ''


@dataclass
class BindConstructor:
    """Public constructor signature; all params come through unmodified."""
    params: list[BindMethodParam] = field(default_factory=list)


@dataclass
class BindClass:
    cpp_qualifier: str       # "Layer", "Layer::SubTexture"
    js_name: str             # "MdxLayer", "MdxLayerSubTexture"
    is_value_object: bool = False
    fields: list[BindField] = field(default_factory=list)
    methods: list[BindMethod] = field(default_factory=list)
    constructors: list[BindConstructor] = field(default_factory=list)
    cpp_namespace: str = ''  # full namespace, e.g. "whiteout::textures::blp"
    doc: str = ''
    # Optional base class — fully-qualified C++ name. When present, the
    # backends emit the inheritance relationship: `py::class_<C, Base>(...)`
    # in pybind11; `.base<Base>()` in Embind. The Base type must already be
    # registered before this class binds. Set via `@bind extends=Foo::Bar`.
    base_class: str = ''
    # If True, suppress the default `py::init<>()` no-arg constructor. Used
    # for classes that are only constructed via static factory methods
    # (move-only types like `mpq::Storage`). Set via `@bind no_default_ctor`.
    no_default_ctor: bool = False
    # If True, the class has a deleted copy constructor and can only be
    # produced by-value via move. WASM/Embind needs explicit `val(std::move(...))`
    # for these so the binding doesn't try to invoke the absent copy.
    # Auto-detected: any class with an explicit `= delete`d copy ctor.
    is_move_only: bool = False
    # Total byte size of the struct as libclang sees it (sizeof in the
    # current compilation environment). `None` when libclang couldn't
    # determine it (incomplete type, template).
    byte_size: Optional[int] = None
    # True when the C++ type is a Plain Old Data type per libclang
    # (`Type.is_pod()`): trivial copy / trivial destructor / standard
    # layout / no non-POD members. Backends use this to decide whether
    # a bitwise `memcpy` is a safe replacement for the class's
    # copy-assignment operator.
    is_pod: bool = False


@dataclass
class BindConstant:
    js_name: str             # "MdxNoParent"
    cpp_expr: str            # "Node::NO_PARENT"
    cpp_type: str = 'u32'
    doc: str = ''


@dataclass
class BindTemplateField:
    """A field on a class template, with the template parameter left as a
    text placeholder (typically 'T'). Used by the parser to synthesise
    concrete BindClasses by substituting 'T → X' into each field's
    `cpp_text_template` and re-classifying the result."""
    name: str
    cpp_name: str
    cpp_text_template: str   # raw spelling with 'T' (or whatever) preserved
    doc: str = ''


@dataclass
class BindTemplate:
    """A C++ class template marked `@bind value_template, instantiate=A;B;…`.
    The parser captures its field list with the type parameter as a
    placeholder, then synthesises one regular `BindClass` per requested
    instantiation. Emitters never see the template directly — they see the
    concrete classes and treat them like any other handle type."""
    cpp_short: str                       # 'Track', 'AnimationTrack', 'AnimRef'
    cpp_qualifier: str                   # 'whiteout::mdx::Track' (no <T>)
    cpp_namespace: str                   # 'whiteout::mdx'
    type_param: str = 'T'                # template parameter spelling
    fields: list[BindTemplateField] = field(default_factory=list)
    instantiate: list[str] = field(default_factory=list)  # raw cpp_text per T
    # Fully-qualified base class to inline (e.g. 'whiteout::m2::AnimationTrackBase');
    # the base's fields are flattened into every concrete instantiation. Empty
    # string when no base.
    base_cpp_qualifier: str = ''
    doc: str = ''


@dataclass
class BindModule:
    name: str                            # "mdx"
    js_prefix: str                       # "Mdx"
    cpp_namespace: str                   # "whiteout::mdx"
    embind_block: str                    # EMSCRIPTEN_BINDINGS(<this>)
    headers: list[str]                   # for the #include block
    classes: list[BindClass] = field(default_factory=list)
    enums: list[BindEnum] = field(default_factory=list)
    constants: list[BindConstant] = field(default_factory=list)
    # Auto-discovered from field types:
    vector_types: list[TypeRef] = field(default_factory=list)
    templates: list[BindTemplate] = field(default_factory=list)
    skip_vector_js_names: list[str] = field(default_factory=list)
    skip_class_js_names: list[str] = field(default_factory=list)


@dataclass
class ModuleConfig:
    name: str
    cpp_namespace: str
    js_prefix: str
    embind_block: str
    headers: list[str]               # paths relative to repo root
    output_path: str                 # Embind output (path relative to repo root)
    pybind_output_path: str = ''     # pybind11 output (defaults to bindings/python/<name>_bindings.cpp)
    dts_output_path: str = ''        # TypeScript .d.ts output (defaults to packages/js-ts/types/<name>.d.ts)
    pyi_output_path: str = ''        # Python .pyi stub (defaults to packages/python/whiteout-stubs/<name>.pyi)
    c_header_output_path: str = ''   # C ABI header (defaults to bindings/c/whiteout_<name>.h)
    c_source_output_path: str = ''   # C ABI source (defaults to bindings/c/whiteout_<name>.cpp)
    include_dirs: list[str] = field(default_factory=list)
    # JS names of vector containers already registered elsewhere (e.g. by a
    # hand-written bindings file). The codegen will discover the same C++
    # type from field walks but skip the register_vector call so we don't
    # double-register at module init.
    skip_vector_js_names: list[str] = field(default_factory=list)
    # When True, every top-level struct/class/enum found in `cpp_namespace`
    # is auto-bound — no explicit `@bind` needed. Override per-type with
    # `@bind skip` (drop) or `@bind value_object` (change kind).
    auto_bind: bool = False
    # Type names (short, like "Format" or "TrackHeader") to exclude from
    # auto_bind. Useful for internal helper types you don't want exposed.
    auto_bind_skip: list[str] = field(default_factory=list)
    # JS class names to exclude from emission (because another module owns
    # them — e.g. shared math types live in mdx_bindings.cpp).
    skip_class_js_names: list[str] = field(default_factory=list)
