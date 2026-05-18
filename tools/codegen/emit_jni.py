# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
"""IR -> JNI bridge emitter (C++ wrapper + Java interface + Java factory).

The JNI bridge lets Java code IMPLEMENT C++ abstract interfaces (the
ones marked `@bind subclassable` in include/whiteout/interfaces.h).
Library calls reach the Java override through a thin C++ class that
derives from the interface, caches JNI method IDs at construction, and
forwards each virtual override via `Call*Method` into the Java object.

Output layout (one set of files per subclassable class):

    bindings/java/jni/<lower>_bridge.cpp       # C++ wrapper + JNI exports
    bindings/java/src/main/java/whiteout/interfaces/<Class>.java         # interface
    bindings/java/src/main/java/whiteout/interfaces/<Class>Handlers.java # factory

Value-object types that subclassable methods reference (e.g. HttpResponse
as a sync_call return) get a Java `record` emitted in the same package.

Per-method directives the emitter recognises (set via `@bind`):

    sync_call=<TypeName>   The C++ signature ends with a
                           `std::function<void(<TypeName>)>` callback.
                           The emitter generates a Java method that
                           drops the callback param and returns
                           <TypeName> synchronously; the C++ override
                           collects the returned value and fires the
                           std::function on the calling thread.

    skip                   Method is omitted from the bridge entirely
                           (still pure-virtual in the C++ wrapper;
                           library code that calls it will assert).
"""

from __future__ import annotations

import re
from io import StringIO

from .ir import BindClass, BindMethod, BindMethodParam, BindModule, TypeKind, TypeRef
from .parser import _short_name


# ── Type mapping tables ──────────────────────────────────────────────────

_PRIMITIVE_JNI_SIG = {
    'u8': 'B', 'i8': 'B', 'unsigned char': 'B', 'char': 'B',
    'u16': 'S', 'i16': 'S', 'short': 'S', 'unsigned short': 'S',
    'u32': 'I', 'i32': 'I', 'int': 'I', 'unsigned int': 'I',
    'u64': 'J', 'i64': 'J', 'long long': 'J', 'unsigned long long': 'J',
    'size_t': 'J', 'std::size_t': 'J',
    'f32': 'F', 'float': 'F',
    'f64': 'D', 'double': 'D',
    'bool': 'Z',
    'void': 'V',
}

_PRIMITIVE_JAVA_TYPE = {
    'u8': 'byte', 'i8': 'byte', 'unsigned char': 'byte', 'char': 'byte',
    'u16': 'short', 'i16': 'short', 'short': 'short', 'unsigned short': 'short',
    'u32': 'int', 'i32': 'int', 'int': 'int', 'unsigned int': 'int',
    'u64': 'long', 'i64': 'long', 'long long': 'long', 'unsigned long long': 'long',
    'size_t': 'long', 'std::size_t': 'long',
    'f32': 'float', 'float': 'float',
    'f64': 'double', 'double': 'double',
    'bool': 'boolean',
    'void': 'void',
}

_PRIMITIVE_JNI_C_TYPE = {
    'u8': 'jbyte', 'i8': 'jbyte', 'unsigned char': 'jbyte', 'char': 'jbyte',
    'u16': 'jshort', 'i16': 'jshort', 'short': 'jshort', 'unsigned short': 'jshort',
    'u32': 'jint', 'i32': 'jint', 'int': 'jint', 'unsigned int': 'jint',
    'u64': 'jlong', 'i64': 'jlong', 'long long': 'jlong', 'unsigned long long': 'jlong',
    'size_t': 'jlong', 'std::size_t': 'jlong',
    'f32': 'jfloat', 'float': 'jfloat',
    'f64': 'jdouble', 'double': 'jdouble',
    'bool': 'jboolean',
}

_PRIMITIVE_BOX = {
    'u8': 'Byte', 'i8': 'Byte',
    'u16': 'Short', 'i16': 'Short',
    'u32': 'Integer', 'i32': 'Integer',
    'u64': 'Long', 'i64': 'Long', 'size_t': 'Long',
    'f32': 'Float', 'f64': 'Double',
    'bool': 'Boolean',
}

# `Call<T>Method` suffixes for non-Object returns. Object returns use
# CallObjectMethod regardless of declared type.
_PRIMITIVE_JNI_CALL_VERB = {
    'u8': 'Byte', 'i8': 'Byte',
    'u16': 'Short', 'i16': 'Short',
    'u32': 'Int', 'i32': 'Int',
    'u64': 'Long', 'i64': 'Long', 'size_t': 'Long',
    'f32': 'Float', 'f64': 'Double',
    'bool': 'Boolean',
    'void': 'Void',
}


def _jni_package(c: BindClass) -> str:
    return c.jni_package or 'whiteout.interfaces'


def _jni_package_slash(c: BindClass) -> str:
    return _jni_package(c).replace('.', '/')


def _class_short(c: BindClass) -> str:
    """Java class / file name. Strips the namespace bits."""
    return _short_name(c.cpp_qualifier)


def _is_byte_vector(t: TypeRef) -> bool:
    if t.kind != TypeKind.VECTOR:
        return False
    return t.element.kind == TypeKind.PRIMITIVE and \
           _short_name(t.element.cpp_text) in ('u8', 'unsigned char')


def _jni_signature(t: TypeRef, value_records: set[str]) -> str | None:
    """JNI signature fragment for one Java type. None when the codegen
    can't surface this type."""
    if t.kind == TypeKind.PRIMITIVE:
        return _PRIMITIVE_JNI_SIG.get(_short_name(t.cpp_text))
    if t.kind == TypeKind.STRING:
        return 'Ljava/lang/String;'
    if _is_byte_vector(t):
        return '[B'
    if t.kind == TypeKind.OPTIONAL:
        if t.element.kind == TypeKind.PRIMITIVE:
            box = _PRIMITIVE_BOX.get(_short_name(t.element.cpp_text))
            return f'Ljava/lang/{box};' if box else None
        if t.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            short = _short_name(t.element.cpp_text)
            if short in value_records:
                return f'Lwhiteout/interfaces/{short};'
        return None
    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        short = _short_name(t.cpp_text)
        if short in value_records:
            return f'Lwhiteout/interfaces/{short};'
    return None


def _java_type(t: TypeRef, value_records: set[str]) -> str | None:
    if t.kind == TypeKind.PRIMITIVE:
        return _PRIMITIVE_JAVA_TYPE.get(_short_name(t.cpp_text))
    if t.kind == TypeKind.STRING:
        return 'String'
    if _is_byte_vector(t):
        return 'byte[]'
    if t.kind == TypeKind.OPTIONAL:
        if t.element.kind == TypeKind.PRIMITIVE:
            return _PRIMITIVE_BOX.get(_short_name(t.element.cpp_text))
        if t.element.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
            short = _short_name(t.element.cpp_text)
            return short if short in value_records else None
        return None
    if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
        short = _short_name(t.cpp_text)
        return short if short in value_records else None
    return None


# Whiteout primitives are typedefs under namespace `whiteout::` —
# qualifying them is required when the bridge sits outside that
# namespace. Built-in types (bool / int / long / size_t / float etc.)
# are keywords / built-ins and must NOT be prefixed.
_WHITEOUT_PRIMITIVES = {
    'u8', 'u16', 'u32', 'u64',
    'i8', 'i16', 'i32', 'i64',
    'f32', 'f64',
}


def _cpp_full_type(t: TypeRef, ns: str) -> str:
    """Fully-qualified C++ type spelling for parameter / return rendering.
    Uses absolute (`::`-rooted) qualifiers so name lookup inside
    `namespace whiteout::jni` doesn't ambiguate `whiteout::u8` against
    a hypothetical `whiteout::jni::whiteout`. A leading space before
    `::` keeps MSVC from misparsing the digraph `<:` (= `[`) inside
    template argument lists like `std::vector< ::whiteout::u8>`."""
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        if short in _WHITEOUT_PRIMITIVES:
            return f' ::whiteout::{short}'
        return t.cpp_text
    if t.kind == TypeKind.STRING:
        return 'std::string'
    if t.kind == TypeKind.VECTOR:
        return f'std::vector<{_cpp_full_type(t.element, ns)}>'
    if t.kind == TypeKind.OPTIONAL:
        return f'std::optional<{_cpp_full_type(t.element, ns)}>'
    return t.cpp_text


# ── Callback param mapping (std::function ⇄ Java functional interface) ──

def _callback_consumer_class(target: str) -> str:
    """Java class name of the codegen-produced wrapper for a callback
    with target type `target` (e.g. 'HttpResponse' → 'HttpResponseConsumer',
    'void' → 'CallbackRunnable')."""
    if target == 'void':
        return 'CallbackRunnable'
    return f'{target}Consumer'


def _callback_java_iface(target: str) -> str:
    """Java functional-interface type the wrapper implements."""
    if target == 'void':
        return 'Runnable'
    return f'Consumer<{target}>'


def _callback_param_java_type(target: str) -> str:
    """Java type used in the method signature. Surface the standard
    functional interface so users see idiomatic types; the runtime
    instance is a codegen-produced subclass."""
    return _callback_java_iface(target)


def _is_submit_workertask(m: BindMethod) -> bool:
    """Method tagged `@bind submit_workertask`: keep it in the bridge
    even though its `const WorkerTask&` param classifies as UNKNOWN,
    and emit a custom Java signature + C++ override that constructs
    the Java WorkerTask record and forwards through to Java's submit."""
    return bool(m.annotations.get('submit_workertask'))


def _collect_callback_targets(module: BindModule) -> set[str]:
    """Unique callback target types across all subclassable interfaces.
    One wrapper class is generated per unique target. `submit_workertask`
    methods implicitly need the `void` (Runnable) wrapper for the
    embedded `std::function<void()>` field, even though they don't have
    a direct std::function param."""
    targets: set[str] = set()
    for c in module.classes:
        if not c.is_subclassable:
            continue
        for m in c.methods:
            for p in m.params:
                if p.callback_target:
                    targets.add(p.callback_target)
            if _is_submit_workertask(m):
                targets.add('void')
    return targets


# ── Method emission helpers ──────────────────────────────────────────────

def _is_void(t: TypeRef) -> bool:
    """libclang surfaces `void` as both PRIMITIVE and UNKNOWN depending on
    context; check both kinds against the textual spelling."""
    return _short_name(t.cpp_text) == 'void'


def _java_method_signature(m: BindMethod, value_records: set[str]) -> str | None:
    """Render a Java method signature: `ReturnType name(ParamType p, ...)`.
    Returns None if any type can't be expressed.

    Callback params (`std::function<void(T)>`) surface as `Consumer<T>`
    (or `Runnable` for void/void). The bridge invokes the C++ callback
    when Java calls `.accept(...)` / `.run()` on the wrapper.

    `submit_workertask` short-circuits to `void <name>(WorkerTask <param>)`,
    using the hand-written WorkerTask record in whiteout.interfaces."""
    if _is_submit_workertask(m):
        pname = m.params[0].name if m.params else 'task'
        return f'void {m.name}(WorkerTask {pname})'
    if _is_void(m.return_type):
        ret_java = 'void'
    else:
        ret_java = _java_type(m.return_type, value_records)
        if ret_java is None:
            return None
    param_bits = []
    for p in m.params:
        if p.callback_target:
            param_bits.append(f'{_callback_param_java_type(p.callback_target)} {p.name}')
            continue
        pj = _java_type(p.type, value_records)
        if pj is None:
            return None
        param_bits.append(f'{pj} {p.name}')
    return f'{ret_java} {m.name}({", ".join(param_bits)})'


def _jni_method_signature(m: BindMethod, value_records: set[str]) -> str | None:
    """JNI signature string like `(Ljava/lang/String;)Lwhiteout/interfaces/HttpResponse;`.
    Callback params encode as the standard functional-interface type
    (`Ljava/util/function/Consumer;` or `Ljava/lang/Runnable;`).
    `submit_workertask` short-circuits to `(Lwhiteout/interfaces/WorkerTask;)V`."""
    if _is_submit_workertask(m):
        return '(Lwhiteout/interfaces/WorkerTask;)V'
    if _is_void(m.return_type):
        ret_sig = 'V'
    else:
        ret_sig = _jni_signature(m.return_type, value_records)
        if ret_sig is None:
            return None
    param_sigs = []
    for p in m.params:
        if p.callback_target:
            if p.callback_target == 'void':
                param_sigs.append('Ljava/lang/Runnable;')
            else:
                param_sigs.append('Ljava/util/function/Consumer;')
            continue
        s = _jni_signature(p.type, value_records)
        if s is None:
            return None
        param_sigs.append(s)
    return f'({"".join(param_sigs)}){ret_sig}'


def _supported_methods(c: BindClass, value_records: set[str]) -> list[BindMethod]:
    """Methods this emitter can bridge. Skip-annotated and unsupported-type
    methods drop out silently — the C++ wrapper still has them as pure
    virtual but library code that calls them will abort.

    `submit_workertask` methods are kept even when their `const WorkerTask&`
    param classifies as UNKNOWN — the special-case Java + C++ paths
    handle the marshalling."""
    out = []
    for m in c.methods:
        if m.annotations.get('skip'):
            continue
        if _is_submit_workertask(m):
            out.append(m)
            continue
        if _java_method_signature(m, value_records) is None:
            continue
        if _jni_method_signature(m, value_records) is None:
            continue
        out.append(m)
    return out


def _emit_cpp_stub_overrides(buf, iface_short: str, iface_ns: str) -> None:
    """Hand-coded overrides for pure-virtual or default-returning methods
    that `@bind skip` filtered out of the IR. Without these the JavaXxx
    wrapper class would stay abstract or default to wrong behaviour."""
    if iface_short == 'VirtualPathFileSystem':
        buf.write('    std::vector< ::whiteout::interfaces::DirectoryEntry> listDirectory(const std::string& /*path*/) const override {\n')
        buf.write('        return {};\n')
        buf.write('    }\n\n')
    elif iface_short == 'WorkerPool':
        # `submit` is no longer a stub — `@bind submit_workertask` routes
        # it through the WorkerTask record + Runnable bridge path. The
        # only remaining stub is createTimelineSemaphore, which library
        # code may call on a Java-backed pool; return a real C++ semaphore
        # so callers always get a working one regardless of pool source.
        buf.write('    std::unique_ptr< ::whiteout::interfaces::TimelineSemaphore> createTimelineSemaphore() override {\n')
        buf.write('        return std::make_unique< ::whiteout::utils::TimelineSemaphore>();\n')
        buf.write('    }\n\n')


def _records_referenced_by_class(c: BindClass, candidates: set[str]) -> set[str]:
    """Records referenced specifically by one interface's methods
    DIRECTLY (return / param types). Callback target records aren't
    counted here — the consumer wrapper file handles their accessors."""
    out: set[str] = set()
    for m in c.methods:
        for t in [m.return_type] + [p.type for p in m.params]:
            if t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN):
                s = _short_name(t.cpp_text)
                if s in candidates:
                    out.add(s)
            if t.kind == TypeKind.OPTIONAL and t.element is not None:
                s = _short_name(t.element.cpp_text)
                if s in candidates:
                    out.add(s)
    return out


def _value_records_referenced(module: BindModule) -> set[str]:
    """Union across all subclassable interfaces — used to decide which
    Java record files to emit at all. Includes records used as direct
    method return types AND as the target of callback params (since the
    consumer wrappers need the same Java record type)."""
    candidates = {_short_name(c.cpp_qualifier) for c in module.classes
                  if c.is_value_object}
    referenced: set[str] = set()
    for c in module.classes:
        if not c.is_subclassable:
            continue
        referenced |= _records_referenced_by_class(c, candidates)
        # Callback target records also need a Java record class so the
        # consumer wrapper can hand back HttpResponse etc.
        for m in c.methods:
            for p in m.params:
                if p.callback_target and p.callback_target in candidates:
                    referenced.add(p.callback_target)
    return referenced


# ── C++ bridge emission ──────────────────────────────────────────────────

_CPP_HEADER = '''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// AUTOGENERATED by tools/codegen/emit_jni.py — do not edit by hand.
// Regenerate via:  python -m tools.codegen.codegen {module} --backend jni
//
// JNI bridge for {fqn}.
// Lets Java code implement the abstract interface; library code reaches
// the override via this C++ wrapper class.

#include <jni.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <whiteout/interfaces.h>
{extra_includes}
#include "jni_common.h"

'''


def _cpp_jni_method_id_field(m: BindMethod) -> str:
    return f'm_{m.cpp_name}'


def _emit_cpp_method_id_decls(buf: StringIO, methods: list[BindMethod]):
    for m in methods:
        buf.write(f'    jmethodID {_cpp_jni_method_id_field(m)} = nullptr;\n')


def _emit_cpp_method_id_lookups(buf: StringIO, methods: list[BindMethod],
                                value_records: set[str],
                                iface_jni_class: str):
    for m in methods:
        sig = _jni_method_signature(m, value_records)
        buf.write(f'        {_cpp_jni_method_id_field(m)} = env->GetMethodID('
                  f'ifaceCls, "{m.name}", "{sig}");\n')


def _emit_cpp_record_accessor_lookups(buf: StringIO,
                                      value_records: set[str],
                                      module: BindModule):
    """For each referenced value record, look up accessor jmethodIDs."""
    for short in sorted(value_records):
        rec_cls = _record_for_short(module, short)
        if rec_cls is None:
            continue
        buf.write(f'        {{\n')
        buf.write(f'            jclass cls = env->FindClass('
                  f'"{_jni_package_slash(rec_cls)}/{short}");\n')
        buf.write(f'            if (cls == nullptr) throw std::runtime_error('
                  f'"{short}: FindClass failed");\n')
        for f in rec_cls.fields:
            sig = _jni_signature(f.type, value_records)
            buf.write(f'            m_rec_{short}_{f.cpp_name} = '
                      f'env->GetMethodID(cls, "{f.cpp_name}", "(){sig}");\n')
        buf.write(f'            env->DeleteLocalRef(cls);\n')
        buf.write(f'        }}\n')


def _emit_cpp_record_accessor_decls(buf: StringIO, value_records: set[str],
                                    module: BindModule):
    for short in sorted(value_records):
        rec_cls = _record_for_short(module, short)
        if rec_cls is None:
            continue
        for f in rec_cls.fields:
            buf.write(f'    jmethodID m_rec_{short}_{f.cpp_name} = nullptr;\n')


def _record_for_short(module: BindModule, short: str) -> BindClass | None:
    for c in module.classes:
        if c.is_value_object and _short_name(c.cpp_qualifier) == short:
            return c
    return None


# ── C++ method body emission ─────────────────────────────────────────────

def _emit_cpp_marshal_param(buf: StringIO, p: BindMethodParam,
                            value_records: set[str], indent: str):
    """Emit C++ code that converts a C++ parameter into a `jvalue`
    suitable for passing to Call<T>Method. Sets a local `arg_<name>` of
    the appropriate JNI type."""
    # Callback param: heap-allocate the std::function and wrap in the
    # corresponding Java functional-interface bridge object. The Java
    # wrapper holds the heap pointer as a long; when Java invokes
    # accept() / run() the JNI _fire export reads the std::function back
    # and frees it (single-shot semantics).
    if p.callback_target:
        wrapper = _callback_consumer_class(p.callback_target)
        buf.write(f'{indent}auto* cbHeap_{p.name} = new ::whiteout::interfaces::HttpCallback(std::move({p.name}));\n'
                  if p.callback_target == 'HttpResponse'
                  else f'{indent}// callback heap-alloc for {p.callback_target} — only HttpCallback wired today\n')
        buf.write(f'{indent}jobject arg_{p.name} = e->NewObject(m_cb_{p.callback_target}_class, '
                  f'm_cb_{p.callback_target}_ctor, reinterpret_cast<jlong>(cbHeap_{p.name}));\n')
        return
    t = p.type
    if t.kind == TypeKind.PRIMITIVE:
        short = _short_name(t.cpp_text)
        jc = _PRIMITIVE_JNI_C_TYPE.get(short, 'jint')
        buf.write(f'{indent}{jc} arg_{p.name} = static_cast<{jc}>({p.name});\n')
    elif t.kind == TypeKind.STRING:
        buf.write(f'{indent}jstring arg_{p.name} = whiteout::jni::stringToJstring(e, {p.name});\n')
    elif _is_byte_vector(t):
        buf.write(f'{indent}jbyteArray arg_{p.name} = whiteout::jni::vecToJbyteArray(e, {p.name});\n')
    elif t.kind in (TypeKind.NESTED, TypeKind.UNKNOWN) \
            and _short_name(t.cpp_text) in value_records:
        # Would require record-construction marshalling — not currently
        # exercised by any subclassable interface (no method takes a
        # struct param). Stub for future use.
        buf.write(f'{indent}// TODO: marshal {p.name} as record (unsupported)\n')


def _emit_cpp_param_release(buf: StringIO, p: BindMethodParam,
                            value_records: set[str], indent: str):
    """Free any local refs from the marshalling step."""
    if p.callback_target:
        # The callback wrapper is a fresh local jobject from NewObject;
        # release the local ref. The heap-allocated std::function it
        # references stays alive — Java owns its lifetime via the
        # _fire/_cancel JNI exports.
        buf.write(f'{indent}if (arg_{p.name} != nullptr) e->DeleteLocalRef(arg_{p.name});\n')
        return
    t = p.type
    if t.kind == TypeKind.STRING or _is_byte_vector(t):
        buf.write(f'{indent}if (arg_{p.name} != nullptr) e->DeleteLocalRef(arg_{p.name});\n')


def _emit_cpp_unpack_return(buf: StringIO, ret: TypeRef,
                            value_records: set[str], indent: str,
                            into_var: str):
    """Take a `jReturn` (jobject/jboolean/jint/...) and assign the
    converted C++ value to `<into_var>`. Caller handles
    `if (consumePendingException(e)) return ...;` between the JNI call
    and this conversion."""
    if ret.kind == TypeKind.PRIMITIVE and _short_name(ret.cpp_text) == 'void':
        return  # nothing to do
    if ret.kind == TypeKind.PRIMITIVE:
        buf.write(f'{indent}{into_var} = static_cast<{_cpp_full_type(ret, "")}>(jReturn);\n')
    elif ret.kind == TypeKind.STRING:
        buf.write(f'{indent}{into_var} = whiteout::jni::jstringToString(e, '
                  f'static_cast<jstring>(jReturn));\n')
        buf.write(f'{indent}if (jReturn != nullptr) e->DeleteLocalRef(jReturn);\n')
    elif _is_byte_vector(ret):
        buf.write(f'{indent}{into_var} = whiteout::jni::jbyteArrayToVec(e, '
                  f'static_cast<jbyteArray>(jReturn));\n')
        buf.write(f'{indent}if (jReturn != nullptr) e->DeleteLocalRef(jReturn);\n')
    elif ret.kind == TypeKind.OPTIONAL and ret.element.kind == TypeKind.PRIMITIVE:
        # Nullable boxed return — unwrap via the Box.<type>Value() accessor.
        short = _short_name(ret.element.cpp_text)
        box = _PRIMITIVE_BOX.get(short, 'Integer')
        verb = _PRIMITIVE_JNI_CALL_VERB.get(short, 'Int')
        cpp_elem = _cpp_full_type(ret.element, "")
        buf.write(f'{indent}if (jReturn == nullptr) {{\n')
        buf.write(f'{indent}    {into_var} = std::nullopt;\n')
        buf.write(f'{indent}}} else {{\n')
        buf.write(f'{indent}    jclass boxCls = e->GetObjectClass(jReturn);\n')
        buf.write(f'{indent}    jmethodID unbox = e->GetMethodID(boxCls, "{verb.lower()}Value", "(){_PRIMITIVE_JNI_SIG[short]}");\n')
        buf.write(f'{indent}    {into_var} = static_cast<{cpp_elem}>(e->Call{verb}Method(jReturn, unbox));\n')
        buf.write(f'{indent}    e->DeleteLocalRef(boxCls);\n')
        buf.write(f'{indent}    e->DeleteLocalRef(jReturn);\n')
        buf.write(f'{indent}}}\n')


def _emit_cpp_unpack_record(buf: StringIO, short: str, into_var: str,
                            module: BindModule, value_records: set[str],
                            indent: str):
    """Unpack a Java record (jobject in `jReturn`) into a C++ struct
    assigned to `<into_var>`."""
    rec_cls = _record_for_short(module, short)
    if rec_cls is None:
        buf.write(f'{indent}// missing record class for {short}\n')
        return
    for f in rec_cls.fields:
        # Field accessor returns a value matching the field's TypeRef.
        sig_char = _jni_signature(f.type, value_records)
        verb = 'Object'
        if f.type.kind == TypeKind.PRIMITIVE:
            verb = _PRIMITIVE_JNI_CALL_VERB.get(_short_name(f.type.cpp_text), 'Int')
        buf.write(f'{indent}{{\n')
        buf.write(f'{indent}    auto jv = e->Call{verb}Method(jReturn, m_rec_{short}_{f.cpp_name});\n')
        if f.type.kind == TypeKind.PRIMITIVE:
            buf.write(f'{indent}    {into_var}.{f.cpp_name} = static_cast<{_cpp_full_type(f.type, "")}>(jv);\n')
        elif f.type.kind == TypeKind.STRING:
            buf.write(f'{indent}    {into_var}.{f.cpp_name} = whiteout::jni::jstringToString(e, static_cast<jstring>(jv));\n')
            buf.write(f'{indent}    if (jv != nullptr) e->DeleteLocalRef(jv);\n')
        elif _is_byte_vector(f.type):
            buf.write(f'{indent}    {into_var}.{f.cpp_name} = whiteout::jni::jbyteArrayToVec(e, static_cast<jbyteArray>(jv));\n')
            buf.write(f'{indent}    if (jv != nullptr) e->DeleteLocalRef(jv);\n')
        buf.write(f'{indent}}}\n')


def _emit_cpp_method_override(buf: StringIO, c: BindClass, m: BindMethod,
                              module: BindModule, value_records: set[str],
                              iface_ns: str):
    """Emit a single virtual override for the JavaXxx wrapper class."""
    iface_qual = f'{iface_ns}::{_short_name(c.cpp_qualifier)}'

    # Build the C++ method signature using libclang's spellings.
    cpp_params = []
    for p in m.params:
        # Render the param type as raw spelling for accuracy
        # (`const std::string&`, etc.). Falls back to TypeRef render.
        raw = p.cpp_raw if p.cpp_raw else _cpp_full_type(p.type, iface_ns)
        cpp_params.append(f'{raw} {p.name}')
    cpp_ret = _cpp_full_type(m.return_type, iface_ns)
    if _is_void(m.return_type):
        cpp_ret = 'void'
    const_qual = ' const' if m.is_const else ''
    noexcept_qual = ' noexcept' if m.is_noexcept else ''
    buf.write(f'    {cpp_ret} {m.cpp_name}({", ".join(cpp_params)}){const_qual}{noexcept_qual} override {{\n')

    # `submit_workertask` is the WorkerTask-flavoured override. The Java
    # side sees `submit(WorkerTask task)`; the bridge unpacks the C++
    # struct into a Java WorkerTask record + Runnable + opaque
    # TimelineSemaphore handles.
    if _is_submit_workertask(m):
        task_name = m.params[0].name if m.params else 'task'
        buf.write('        whiteout::jni::AttachedEnv env;\n')
        buf.write(f'        if (!env || {_cpp_jni_method_id_field(m)} == nullptr) return;\n')
        buf.write('        JNIEnv* e = env.env();\n')
        buf.write(f'        auto* fnHeap = new std::function<void()>({task_name}.fn);\n')
        buf.write('        jobject jFn = e->NewObject(m_cb_void_class, m_cb_void_ctor, reinterpret_cast<jlong>(fnHeap));\n')
        buf.write(f'        jobject jWaitSem = ({task_name}.waitSemaphore == nullptr)\n')
        buf.write('            ? nullptr\n')
        buf.write(f'            : e->NewObject(m_timelineSem_class, m_timelineSem_ctor, reinterpret_cast<jlong>({task_name}.waitSemaphore));\n')
        buf.write(f'        jobject jSignalSem = ({task_name}.signalSemaphore == nullptr)\n')
        buf.write('            ? nullptr\n')
        buf.write(f'            : e->NewObject(m_timelineSem_class, m_timelineSem_ctor, reinterpret_cast<jlong>({task_name}.signalSemaphore));\n')
        buf.write('        jobject jTask = e->NewObject(m_workerTask_class, m_workerTask_ctor,\n')
        buf.write(f'            jFn, jWaitSem, static_cast<jlong>({task_name}.waitValue),\n')
        buf.write(f'            jSignalSem, static_cast<jlong>({task_name}.signalValue));\n')
        buf.write(f'        e->CallVoidMethod(m_impl, {_cpp_jni_method_id_field(m)}, jTask);\n')
        buf.write('        e->DeleteLocalRef(jFn);\n')
        buf.write('        if (jWaitSem != nullptr) e->DeleteLocalRef(jWaitSem);\n')
        buf.write('        if (jSignalSem != nullptr) e->DeleteLocalRef(jSignalSem);\n')
        buf.write('        e->DeleteLocalRef(jTask);\n')
        buf.write('        whiteout::jni::consumePendingException(e);\n')
        buf.write('    }\n\n')
        return

    # ── method body ──
    buf.write('        whiteout::jni::AttachedEnv env;\n')

    # Default error return.
    if _is_void(m.return_type):
        err_return = 'return;'
    elif _is_byte_vector(m.return_type):
        err_return = 'return {};'
    elif m.return_type.kind == TypeKind.PRIMITIVE:
        err_return = 'return {};'
    elif m.return_type.kind == TypeKind.OPTIONAL:
        err_return = 'return std::nullopt;'
    elif m.return_type.kind == TypeKind.STRING:
        err_return = 'return {};'
    else:
        err_return = 'return {};'

    buf.write(f'        if (!env || {_cpp_jni_method_id_field(m)} == nullptr) {{ {err_return} }}\n')
    buf.write('        JNIEnv* e = env.env();\n')
    for p in m.params:
        _emit_cpp_marshal_param(buf, p, value_records, '        ')
    java_args = ', '.join(f'arg_{p.name}' for p in m.params)
    java_args = f', {java_args}' if java_args else ''

    # Pick CallXxxMethod based on return type.
    if _is_void(m.return_type):
        buf.write(f'        e->CallVoidMethod(m_impl, {_cpp_jni_method_id_field(m)}{java_args});\n')
        for p in m.params:
            _emit_cpp_param_release(buf, p, value_records, '        ')
        buf.write('        whiteout::jni::consumePendingException(e);\n')
        buf.write('    }\n\n')
        return
    if m.return_type.kind == TypeKind.PRIMITIVE:
        verb = _PRIMITIVE_JNI_CALL_VERB.get(_short_name(m.return_type.cpp_text), 'Int')
        jc = _PRIMITIVE_JNI_C_TYPE.get(_short_name(m.return_type.cpp_text), 'jint')
        buf.write(f'        {jc} jReturn = e->Call{verb}Method(m_impl, {_cpp_jni_method_id_field(m)}{java_args});\n')
    else:
        buf.write(f'        jobject jReturn = e->CallObjectMethod(m_impl, {_cpp_jni_method_id_field(m)}{java_args});\n')

    for p in m.params:
        _emit_cpp_param_release(buf, p, value_records, '        ')
    buf.write(f'        if (whiteout::jni::consumePendingException(e)) {{ {err_return} }}\n')

    # Build C++ return value.
    if m.return_type.kind == TypeKind.OPTIONAL and m.return_type.element.kind == TypeKind.PRIMITIVE:
        buf.write(f'        {_cpp_full_type(m.return_type, iface_ns)} cpp_result;\n')
        _emit_cpp_unpack_return(buf, m.return_type, value_records, '        ', 'cpp_result')
        buf.write('        return cpp_result;\n')
    elif m.return_type.kind == TypeKind.PRIMITIVE:
        buf.write(f'        return static_cast<{_cpp_full_type(m.return_type, iface_ns)}>(jReturn);\n')
    elif _is_byte_vector(m.return_type):
        # Avoid `__name`: identifiers starting with two underscores are
        # reserved for the implementation and MSVC silently rejects the
        # declaration, surfacing as a misleading C2513 ("no variable
        # declared before '='") on the following line.
        buf.write('        std::vector< ::whiteout::u8> outVec = whiteout::jni::jbyteArrayToVec(e, static_cast<jbyteArray>(jReturn));\n')
        buf.write('        if (jReturn != nullptr) e->DeleteLocalRef(jReturn);\n')
        buf.write('        return outVec;\n')
    elif m.return_type.kind == TypeKind.STRING:
        buf.write('        std::string outStr = whiteout::jni::jstringToString(e, static_cast<jstring>(jReturn));\n')
        buf.write('        if (jReturn != nullptr) e->DeleteLocalRef(jReturn);\n')
        buf.write('        return outStr;\n')
    else:
        buf.write(f'        {err_return}\n')
    buf.write('    }\n\n')


def emit_bridge_cpp(c: BindClass, module: BindModule,
                    value_records: set[str]) -> str:
    iface_short = _short_name(c.cpp_qualifier)
    iface_ns    = c.cpp_namespace
    iface_qual  = f'{iface_ns}::{iface_short}'
    pkg_slash   = _jni_package_slash(c)
    iface_jni_cls = f'{pkg_slash}/{iface_short}'
    # The bridge factory now lives in whiteout.interfaces.internal — the JNI
    # mangled name reflects the runtime-class path of the native method.
    handlers_jni_cls = f'{pkg_slash}/internal/{iface_short}Bridge'
    handlers_jni_mangled = handlers_jni_cls.replace('/', '_')
    methods = _supported_methods(c, value_records)
    # Records this specific interface uses — emit lookups / member
    # declarations only for these, not for every record in the module.
    own_records = _records_referenced_by_class(c, value_records)
    # Callback target types this interface uses. The bridge caches the
    # corresponding Java wrapper class + ctor at init so NewObject is
    # one method-ID lookup, not a class search per call.
    own_callbacks: set[str] = set()
    needs_workertask = False
    for m in methods:
        for p in m.params:
            if p.callback_target:
                own_callbacks.add(p.callback_target)
        if _is_submit_workertask(m):
            # The bridge needs both the WorkerTask record class + ctor
            # and the TimelineSemaphore class + ctor cached. The
            # embedded `fn` field uses the void-flavoured CallbackRunnable
            # — pull it into own_callbacks so the cache + global ref slot
            # exist on the class.
            needs_workertask = True
            own_callbacks.add('void')

    # WorkerPool's stub override for createTimelineSemaphore returns a
    # utils::TimelineSemaphore — the header needs that include.
    extra_includes = ''
    if iface_short == 'WorkerPool':
        extra_includes = '#include <whiteout/utils/timeline_semaphore.h>\n'

    buf = StringIO()
    buf.write(_CPP_HEADER.format(module=module.name, fqn=iface_qual,
                                 extra_includes=extra_includes))
    buf.write('namespace whiteout::jni {\n\n')
    # Bring whiteout primitives (u32/u64/...) and interface aliases
    # (HttpCallback, HttpResponse, HttpCapability, ...) into scope so
    # the libclang-spelled override signatures resolve without manual
    # qualification at every site.
    buf.write('using namespace ::whiteout;\n')
    buf.write(f'using namespace ::{iface_ns};\n\n')
    buf.write(f'class Java{iface_short} : public {iface_qual} {{\n')
    buf.write('public:\n')
    buf.write(f'    Java{iface_short}(JNIEnv* env, jobject impl) {{\n')
    buf.write('        m_impl = env->NewGlobalRef(impl);\n')
    buf.write(f'        if (m_impl == nullptr) throw std::runtime_error("Java{iface_short}: NewGlobalRef failed");\n')
    buf.write('        jclass localCls = env->GetObjectClass(impl);\n')
    buf.write('        m_class = static_cast<jclass>(env->NewGlobalRef(localCls));\n')
    buf.write('        env->DeleteLocalRef(localCls);\n')
    buf.write(f'        if (m_class == nullptr) {{ env->DeleteGlobalRef(m_impl); m_impl = nullptr; throw std::runtime_error("Java{iface_short}: NewGlobalRef(class) failed"); }}\n')
    buf.write(f'        jclass ifaceCls = env->FindClass("{iface_jni_cls}");\n')
    buf.write(f'        if (ifaceCls == nullptr) throw std::runtime_error("Java{iface_short}: FindClass({iface_jni_cls}) failed");\n')
    _emit_cpp_method_id_lookups(buf, methods, value_records, iface_jni_cls)
    buf.write('        env->DeleteLocalRef(ifaceCls);\n')
    _emit_cpp_record_accessor_lookups(buf, own_records, module)
    # Cache each callback wrapper class + its (long) ctor.
    for target in sorted(own_callbacks):
        wrapper = _callback_consumer_class(target)
        buf.write(f'        {{\n')
        buf.write(f'            jclass cls = env->FindClass("whiteout/interfaces/{wrapper}");\n')
        buf.write(f'            if (cls == nullptr) throw std::runtime_error("{wrapper}: FindClass failed");\n')
        buf.write(f'            m_cb_{target}_class = static_cast<jclass>(env->NewGlobalRef(cls));\n')
        buf.write(f'            env->DeleteLocalRef(cls);\n')
        buf.write(f'            m_cb_{target}_ctor = env->GetMethodID(m_cb_{target}_class, "<init>", "(J)V");\n')
        buf.write(f'        }}\n')
    if needs_workertask:
        buf.write('        {\n')
        buf.write('            jclass cls = env->FindClass("whiteout/interfaces/WorkerTask");\n')
        buf.write('            if (cls == nullptr) throw std::runtime_error("WorkerTask: FindClass failed");\n')
        buf.write('            m_workerTask_class = static_cast<jclass>(env->NewGlobalRef(cls));\n')
        buf.write('            env->DeleteLocalRef(cls);\n')
        # Record's canonical ctor matches the field order: Runnable fn,
        # TimelineSemaphore waitSemaphore, long waitValue, TimelineSemaphore signalSemaphore, long signalValue.
        buf.write('            m_workerTask_ctor = env->GetMethodID(m_workerTask_class, "<init>",\n')
        buf.write('                "(Ljava/lang/Runnable;Lwhiteout/interfaces/TimelineSemaphore;JLwhiteout/interfaces/TimelineSemaphore;J)V");\n')
        buf.write('        }\n')
        buf.write('        {\n')
        buf.write('            jclass cls = env->FindClass("whiteout/interfaces/TimelineSemaphore");\n')
        buf.write('            if (cls == nullptr) throw std::runtime_error("TimelineSemaphore: FindClass failed");\n')
        buf.write('            m_timelineSem_class = static_cast<jclass>(env->NewGlobalRef(cls));\n')
        buf.write('            env->DeleteLocalRef(cls);\n')
        buf.write('            m_timelineSem_ctor = env->GetMethodID(m_timelineSem_class, "<init>", "(J)V");\n')
        buf.write('        }\n')
    buf.write('    }\n\n')
    buf.write(f'    ~Java{iface_short}() override {{\n')
    buf.write('        AttachedEnv env;\n')
    buf.write('        if (env.env() != nullptr) {\n')
    buf.write('            if (m_impl  != nullptr) env.env()->DeleteGlobalRef(m_impl);\n')
    buf.write('            if (m_class != nullptr) env.env()->DeleteGlobalRef(m_class);\n')
    for target in sorted(own_callbacks):
        buf.write(f'            if (m_cb_{target}_class != nullptr) env.env()->DeleteGlobalRef(m_cb_{target}_class);\n')
    if needs_workertask:
        buf.write('            if (m_workerTask_class != nullptr) env.env()->DeleteGlobalRef(m_workerTask_class);\n')
        buf.write('            if (m_timelineSem_class != nullptr) env.env()->DeleteGlobalRef(m_timelineSem_class);\n')
    buf.write('        }\n')
    buf.write('    }\n\n')
    for m in methods:
        _emit_cpp_method_override(buf, c, m, module, value_records, iface_ns)
    # Hand-coded stubs for pure-virtual base methods that the parser
    # filtered out via `@bind skip`. Without them the wrapper class
    # stays abstract and the JNI factory can't `new` it. The stubs are
    # no-ops / sane defaults; library code that actually exercises
    # them on a JNI-backed instance will get surprising behaviour, but
    # at least the binding loads. Track the matrix of {interface, method}
    # we need stubs for here — small, explicit, easier to audit than a
    # generic "synthesise stubs from headers" path.
    _emit_cpp_stub_overrides(buf, iface_short, iface_ns)
    buf.write('private:\n')
    buf.write('    jobject m_impl  = nullptr;\n')
    buf.write('    jclass  m_class = nullptr;\n')
    _emit_cpp_method_id_decls(buf, methods)
    _emit_cpp_record_accessor_decls(buf, own_records, module)
    for target in sorted(own_callbacks):
        buf.write(f'    jclass    m_cb_{target}_class = nullptr;\n')
        buf.write(f'    jmethodID m_cb_{target}_ctor  = nullptr;\n')
    if needs_workertask:
        buf.write('    jclass    m_workerTask_class = nullptr;\n')
        buf.write('    jmethodID m_workerTask_ctor  = nullptr;\n')
        buf.write('    jclass    m_timelineSem_class = nullptr;\n')
        buf.write('    jmethodID m_timelineSem_ctor  = nullptr;\n')
    buf.write('};\n\n')
    buf.write('} // namespace whiteout::jni\n\n')
    buf.write('// ── JNI exports — called from the Java factory ──────────────────────\n\n')
    buf.write('extern "C" JNIEXPORT jlong JNICALL\n')
    buf.write(f'Java_{handlers_jni_mangled}__1createBridge(JNIEnv* env, jclass, jobject impl) {{\n')
    buf.write('    if (impl == nullptr) {\n')
    buf.write('        jclass npe = env->FindClass("java/lang/NullPointerException");\n')
    buf.write(f'        if (npe != nullptr) env->ThrowNew(npe, "{iface_short} impl must not be null");\n')
    buf.write('        return 0;\n')
    buf.write('    }\n')
    buf.write('    try {\n')
    buf.write(f'        auto* h = new whiteout::jni::Java{iface_short}(env, impl);\n')
    buf.write(f'        return reinterpret_cast<jlong>(static_cast<{iface_qual}*>(h));\n')
    buf.write('    } catch (const std::exception& e) {\n')
    buf.write('        jclass re = env->FindClass("java/lang/RuntimeException");\n')
    buf.write('        if (re != nullptr) env->ThrowNew(re, e.what());\n')
    buf.write('        return 0;\n')
    buf.write('    }\n')
    buf.write('}\n\n')
    buf.write('extern "C" JNIEXPORT void JNICALL\n')
    buf.write(f'Java_{handlers_jni_mangled}__1destroyBridge(JNIEnv*, jclass, jlong handle) {{\n')
    buf.write('    if (handle == 0) return;\n')
    buf.write(f'    auto* h = reinterpret_cast<{iface_qual}*>(handle);\n')
    buf.write('    delete h;\n')
    buf.write('}\n')
    return buf.getvalue()


# ── Java emission ────────────────────────────────────────────────────────

_JAVA_HEADER = '''// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// AUTOGENERATED by tools/codegen/emit_jni.py — do not edit by hand.

package {pkg};

'''


def emit_java_interface(c: BindClass, value_records: set[str]) -> str:
    iface_short = _short_name(c.cpp_qualifier)
    methods = _supported_methods(c, value_records)
    buf = StringIO()
    buf.write(_JAVA_HEADER.format(pkg=_jni_package(c)))
    # Standard-library imports for callback params.
    cb_targets = {p.callback_target for m in methods for p in m.params
                  if p.callback_target}
    if any(t != 'void' for t in cb_targets):
        buf.write('import java.util.function.Consumer;\n\n')
    if c.doc:
        buf.write('/**\n')
        for line in c.doc.splitlines():
            buf.write(f' * {line}\n')
        buf.write(' */\n')
    buf.write(f'public interface {iface_short} {{\n\n')
    for m in methods:
        sig = _java_method_signature(m, value_records)
        if sig is None:
            continue
        if m.doc:
            buf.write('    /**\n')
            for line in m.doc.splitlines():
                buf.write(f'     * {line}\n')
            buf.write('     */\n')
        # Methods that originally had a std::function callback might
        # throw — let users propagate exceptions naturally.
        # Methods with callback params can return immediately and fire
        # the callback later — but `accept()` on the Consumer wrapper
        # may still throw if Java code chains user-supplied logic into
        # it. Add `throws Exception` to keep checked exceptions allowed.
        has_callback = any(p.callback_target for p in m.params)
        throws = ' throws Exception' if has_callback else ''
        # `capabilities` is a non-pure-virtual with a default — emit as
        # default method so impls don't have to override it.
        if m.cpp_name == 'capabilities':
            buf.write(f'    default {sig} {{ return 0; }}\n\n')
        else:
            buf.write(f'    {sig}{throws};\n\n')
    buf.write('}\n')
    return buf.getvalue()


def emit_java_record(c: BindClass) -> str:
    short = _short_name(c.cpp_qualifier)
    pkg = _jni_package(c) if c.jni_package else 'whiteout.interfaces'
    buf = StringIO()
    buf.write(_JAVA_HEADER.format(pkg=pkg))
    if c.doc:
        buf.write('/**\n')
        for line in c.doc.splitlines():
            buf.write(f' * {line}\n')
        buf.write(' */\n')
    # Field list in declaration order with codegen type mapping.
    rec_fields = []
    for f in c.fields:
        jt = _java_type(f.type, set())
        if jt is None:
            jt = 'Object'  # fallback
        rec_fields.append(f'{jt} {f.cpp_name}')
    buf.write(f'public record {short}({", ".join(rec_fields)}) {{\n\n')
    # Compact ctor: null-normalise byte[] and String fields.
    has_normalisation = any(
        f.type.kind == TypeKind.STRING or _is_byte_vector(f.type)
        for f in c.fields
    )
    if has_normalisation:
        buf.write(f'    public {short} {{\n')
        for f in c.fields:
            if f.type.kind == TypeKind.STRING:
                buf.write(f'        if ({f.cpp_name} == null) {f.cpp_name} = "";\n')
            elif _is_byte_vector(f.type):
                buf.write(f'        if ({f.cpp_name} == null) {f.cpp_name} = new byte[0];\n')
        buf.write('    }\n')
    buf.write('}\n')
    return buf.getvalue()


def emit_java_consumer(target: str) -> str:
    """Java wrapper that implements `Consumer<T>` (or `Runnable` for void)
    and forwards `accept` / `run` into the C++ `std::function` heap copy
    we handed it. Single-shot: subsequent invocations throw; an explicit
    `cancel()` releases the C++ allocation without firing."""
    wrapper = _callback_consumer_class(target)
    iface = _callback_java_iface(target)
    buf = StringIO()
    buf.write(_JAVA_HEADER.format(pkg='whiteout.interfaces'))
    if target != 'void':
        buf.write('import java.util.function.Consumer;\n')
    buf.write('import whiteout.interfaces.internal.JniLoader;\n\n')
    buf.write('/**\n')
    buf.write(f' * Single-shot wrapper around a C++ {"std::function<void(" + target + ")>" if target != "void" else "std::function<void()>"}.\n')
    buf.write(' * Each instance fires at most once; subsequent invocations throw.\n')
    buf.write(' * Call {@link #cancel()} to release the C++ allocation without firing.\n')
    buf.write(' */\n')
    buf.write(f'public final class {wrapper} implements {iface} {{\n')
    buf.write('    private final long handle;\n')
    buf.write('    private volatile boolean fired = false;\n\n')
    buf.write(f'    public {wrapper}(long handle) {{\n')
    buf.write('        JniLoader.ensureLoaded();\n')
    buf.write('        this.handle = handle;\n')
    buf.write('    }\n\n')

    if target == 'void':
        buf.write('    @Override public void run() {\n')
        buf.write('        if (fired) throw new IllegalStateException("CallbackRunnable fired twice");\n')
        buf.write('        fired = true;\n')
        buf.write('        _fire(handle);\n')
        buf.write('    }\n\n')
        buf.write('    public void cancel() {\n')
        buf.write('        if (fired) return;\n')
        buf.write('        fired = true;\n')
        buf.write('        _cancel(handle);\n')
        buf.write('    }\n\n')
        buf.write('    private static native void _fire(long handle);\n')
        buf.write('    private static native void _cancel(long handle);\n')
    else:
        # HttpResponse-specific accept signature.
        # For now we hard-code the HttpResponse field shape — generalising
        # to arbitrary records would require record-class introspection
        # here and matching unpack code on the JNI side. HttpResponse is
        # the only target in scope.
        buf.write(f'    @Override public void accept({target} response) {{\n')
        buf.write(f'        if (fired) throw new IllegalStateException("{wrapper} fired twice");\n')
        buf.write('        fired = true;\n')
        if target == 'HttpResponse':
            buf.write('        _fire(handle, response.statusCode(), response.body(), response.error());\n')
        else:
            buf.write('        // unsupported callback target — _fire signature would need extending\n')
        buf.write('    }\n\n')
        buf.write('    public void cancel() {\n')
        buf.write('        if (fired) return;\n')
        buf.write('        fired = true;\n')
        buf.write('        _cancel(handle);\n')
        buf.write('    }\n\n')
        if target == 'HttpResponse':
            buf.write('    private static native void _fire(long handle, int statusCode, byte[] body, String error);\n')
        buf.write('    private static native void _cancel(long handle);\n')
    buf.write('}\n')
    return buf.getvalue()


def emit_cpp_consumer_callback(target: str) -> str:
    """JNI exports backing the `_fire` / `_cancel` natives on the Java
    consumer wrapper. Knows about the named target type's C++ struct
    layout (currently only HttpResponse)."""
    wrapper = _callback_consumer_class(target)
    buf = StringIO()
    buf.write('// SPDX-License-Identifier: BSD-3-Clause\n')
    buf.write('// Copyright (c) 2026 Fernando Sahmkow\n')
    buf.write('//\n')
    buf.write('// AUTOGENERATED by tools/codegen/emit_jni.py — do not edit by hand.\n')
    buf.write(f'// Callback bridge for std::function<void({target})>.\n\n')
    buf.write('#include <jni.h>\n\n')
    buf.write('#include <cstdint>\n')
    buf.write('#include <string>\n')
    buf.write('#include <utility>\n')
    buf.write('#include <vector>\n\n')
    buf.write('#include <whiteout/interfaces.h>\n')
    buf.write('#include "jni_common.h"\n\n')

    if target == 'void':
        buf.write('extern "C" JNIEXPORT void JNICALL\n')
        buf.write(f'Java_whiteout_interfaces_{wrapper}__1fire(JNIEnv* /*env*/, jclass, jlong handle) {{\n')
        buf.write('    if (handle == 0) return;\n')
        buf.write('    auto* fn = reinterpret_cast<std::function<void()>*>(handle);\n')
        buf.write('    (*fn)();\n')
        buf.write('    delete fn;\n')
        buf.write('}\n\n')
        buf.write('extern "C" JNIEXPORT void JNICALL\n')
        buf.write(f'Java_whiteout_interfaces_{wrapper}__1cancel(JNIEnv* /*env*/, jclass, jlong handle) {{\n')
        buf.write('    if (handle == 0) return;\n')
        buf.write('    delete reinterpret_cast<std::function<void()>*>(handle);\n')
        buf.write('}\n')
        return buf.getvalue()

    # HttpResponse-shape callback.
    buf.write('extern "C" JNIEXPORT void JNICALL\n')
    buf.write(f'Java_whiteout_interfaces_{wrapper}__1fire(JNIEnv* env, jclass, jlong handle,\n')
    buf.write('                                              jint statusCode, jbyteArray body, jstring error) {\n')
    buf.write('    if (handle == 0) return;\n')
    buf.write('    auto* fn = reinterpret_cast< ::whiteout::interfaces::HttpCallback*>(handle);\n')
    buf.write('    ::whiteout::interfaces::HttpResponse r;\n')
    buf.write('    r.statusCode = static_cast< ::whiteout::i32>(statusCode);\n')
    buf.write('    r.body = whiteout::jni::jbyteArrayToVec(env, body);\n')
    buf.write('    r.error = whiteout::jni::jstringToString(env, error);\n')
    buf.write('    (*fn)(std::move(r));\n')
    buf.write('    delete fn;\n')
    buf.write('}\n\n')
    buf.write('extern "C" JNIEXPORT void JNICALL\n')
    buf.write(f'Java_whiteout_interfaces_{wrapper}__1cancel(JNIEnv* /*env*/, jclass, jlong handle) {{\n')
    buf.write('    if (handle == 0) return;\n')
    buf.write('    delete reinterpret_cast< ::whiteout::interfaces::HttpCallback*>(handle);\n')
    buf.write('}\n')
    return buf.getvalue()


def emit_java_internal_bridge(c: BindClass) -> str:
    """Internal factory living under `whiteout.interfaces.internal`. Library
    entry points (Java-side methods that accept these interfaces) call
    this to obtain a native handle and pin the Java impl to an owner via
    Cleaner — the cleanup runs when the owner becomes unreachable.

    Users don't see this class. The natural usage pattern from a
    library-side wrapper:

        public static MyStorage open(..., HttpHandler handler) {
            Object[] pinned = { handler };
            long handlerPtr = HttpHandlerBridge.createPinned(handler, pinned);
            long storage    = nativeOpen(..., handlerPtr);
            return new MyStorage(storage, pinned);
        }

    When the MyStorage instance is GC'd, `pinned` becomes unreachable,
    the Cleaner fires `_destroyBridge`, the C++ wrapper releases the
    JNI global ref to the Java handler.
    """
    iface_short = _short_name(c.cpp_qualifier)
    pkg = _jni_package(c)  # whiteout.interfaces
    buf = StringIO()
    buf.write(_JAVA_HEADER.format(pkg=f'{pkg}.internal'))
    buf.write('import java.lang.ref.Cleaner;\n\n')
    buf.write(f'import {pkg}.{iface_short};\n\n')
    buf.write(f'/** Internal: native bridge factory for {{@link {iface_short}}}.\n')
    buf.write(f' *  Not exported by module-info — only library entry points should\n')
    buf.write(f' *  invoke this. */\n')
    buf.write(f'public final class {iface_short}Bridge {{\n\n')
    buf.write('    private static final Cleaner CLEANER = Cleaner.create();\n\n')
    buf.write('    static { JniLoader.ensureLoaded(); }\n\n')
    buf.write(f'    private {iface_short}Bridge() {{}}\n\n')
    buf.write(f'    /** Wrap {{@code impl}} as a native handle. The handle is freed when\n')
    buf.write('     *  {@code owner} is collected — keep a strong reference to {@code owner}\n')
    buf.write('     *  (typically the storage / wrapper object that consumes the handle)\n')
    buf.write('     *  for at least as long as the C++ side needs the bridge. */\n')
    buf.write(f'    public static long createPinned({iface_short} impl, Object owner) {{\n')
    buf.write('        if (impl == null) throw new NullPointerException("impl");\n')
    buf.write('        if (owner == null) throw new NullPointerException("owner");\n')
    buf.write('        long ptr = _createBridge(impl);\n')
    buf.write('        CLEANER.register(owner, new Releaser(ptr));\n')
    buf.write('        return ptr;\n')
    buf.write('    }\n\n')
    buf.write('    // Cleanup is a static-nested class (not a lambda) so the captured\n')
    buf.write('    // pointer doesn\'t accidentally hold a strong reference back to the\n')
    buf.write('    // owner — that would defeat the Cleaner.\n')
    buf.write('    private static final class Releaser implements Runnable {\n')
    buf.write('        private final long ptr;\n')
    buf.write('        Releaser(long ptr) { this.ptr = ptr; }\n')
    buf.write('        @Override public void run() { _destroyBridge(ptr); }\n')
    buf.write('    }\n\n')
    buf.write('    private static native long _createBridge(Object impl);\n')
    buf.write('    private static native void _destroyBridge(long handle);\n')
    buf.write('}\n')
    return buf.getvalue()


# ── Entry point ──────────────────────────────────────────────────────────

def emit(module: BindModule) -> dict[str, str]:
    """Return {repo-relative path: content} for every file the JNI
    backend should produce for this module. Empty when no subclassable
    classes are present."""
    files: dict[str, str] = {}
    value_records = _value_records_referenced(module)
    callback_targets = _collect_callback_targets(module)

    # Stamp value records into whiteout.interfaces so the per-interface bridges
    # have a record class to construct/unpack.
    for short in sorted(value_records):
        rec = _record_for_short(module, short)
        if rec is None:
            continue
        if not rec.jni_package:
            rec.jni_package = 'whiteout.interfaces'
        files[f'bindings/java/src/main/java/whiteout/interfaces/{short}.java'] = \
            emit_java_record(rec)

    # One Consumer<T> / Runnable wrapper per unique callback target. The
    # `_fire` / `_cancel` JNI exports live in the matching *_callback.cpp
    # alongside whiteout_jni.dll's other native methods.
    for target in sorted(callback_targets):
        wrapper = _callback_consumer_class(target)
        files[f'bindings/java/src/main/java/whiteout/interfaces/{wrapper}.java'] = \
            emit_java_consumer(target)
        files[f'bindings/java/jni/{wrapper.lower()}_callback.cpp'] = \
            emit_cpp_consumer_callback(target)

    for c in module.classes:
        if not c.is_subclassable:
            continue
        if not c.jni_package:
            c.jni_package = 'whiteout.interfaces'
        iface_short = _short_name(c.cpp_qualifier)
        files[f'bindings/java/jni/{iface_short.lower()}_bridge.cpp'] = \
            emit_bridge_cpp(c, module, value_records)
        files[f'bindings/java/src/main/java/whiteout/interfaces/{iface_short}.java'] = \
            emit_java_interface(c, value_records)
        files[f'bindings/java/src/main/java/whiteout/interfaces/internal/{iface_short}Bridge.java'] = \
            emit_java_internal_bridge(c)
    return files
