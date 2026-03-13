// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/sno/sno_value.h>

#include <new>
#include <utility>

namespace whiteout {
namespace sno {

// ============================================================================
// Lifecycle
// ============================================================================

SnoValue::SnoValue() : type_(SVT_NULL) {}

SnoValue::~SnoValue() {
    destroy();
}

SnoValue::SnoValue(const SnoValue& o) : type_(SVT_NULL) {
    copyFrom(o);
}

SnoValue::SnoValue(SnoValue&& o) : type_(SVT_NULL) {
    moveFrom(static_cast<SnoValue&&>(o));
}

SnoValue& SnoValue::operator=(const SnoValue& o) {
    if (this != &o) {
        destroy();
        copyFrom(o);
    }
    return *this;
}

SnoValue& SnoValue::operator=(SnoValue&& o) {
    if (this != &o) {
        destroy();
        moveFrom(static_cast<SnoValue&&>(o));
    }
    return *this;
}

// ============================================================================
// Convenience constructors
// ============================================================================

SnoValue::SnoValue(bool v) : type_(SVT_BOOL) {
    s_.b = v;
}
SnoValue::SnoValue(i32 v) : type_(SVT_INT) {
    s_.i = v;
}
SnoValue::SnoValue(u32 v) : type_(SVT_UINT) {
    s_.u = v;
}
SnoValue::SnoValue(f32 v) : type_(SVT_FLOAT) {
    s_.f = v;
}
SnoValue::SnoValue(i64 v) : type_(SVT_INT64) {
    s_.i64v = v;
}
SnoValue::SnoValue(u64 v) : type_(SVT_UINT64) {
    s_.u64v = v;
}
SnoValue::SnoValue(u8 v) : type_(SVT_BYTE) {
    s_.byte = v;
}
SnoValue::SnoValue(u16 v) : type_(SVT_WORD) {
    s_.word = v;
}
SnoValue::SnoValue(std::string v) : type_(SVT_STRING) {
    new (&s_.str) StrPtr(std::make_unique<std::string>(std::move(v)));
}
SnoValue::SnoValue(SnoVec2 v) : type_(SVT_VEC2) {
    s_.vec2 = v;
}
SnoValue::SnoValue(SnoVec3 v) : type_(SVT_VEC3) {
    s_.vec3 = v;
}
SnoValue::SnoValue(SnoVec4 v) : type_(SVT_VEC4) {
    new (&s_.vec4) Vec4Ptr(std::make_unique<SnoVec4>(v));
}
SnoValue::SnoValue(SnoIVec2 v) : type_(SVT_IVEC2) {
    s_.ivec2 = v;
}
SnoValue::SnoValue(SnoColor v) : type_(SVT_COLOR) {
    s_.color = v;
}
SnoValue::SnoValue(SnoColorF v) : type_(SVT_COLORF) {
    new (&s_.colorf) ColorFPtr(std::make_unique<SnoColorF>(v));
}
SnoValue::SnoValue(SnoRef v) : type_(SVT_REF) {
    s_.ref = v;
}
SnoValue::SnoValue(SnoGbid v) : type_(SVT_GBID) {
    s_.gbid = v;
}
SnoValue::SnoValue(SnoArray v) : type_(SVT_ARRAY) {
    new (&s_.arr) ArrPtr(std::make_unique<SnoArray>(std::move(v)));
}
SnoValue::SnoValue(SnoObject v) : type_(SVT_OBJECT) {
    new (&s_.obj) ObjPtr(std::make_unique<SnoObject>(std::move(v)));
}

// ============================================================================
// Lookup helpers
// ============================================================================

const SnoValue* SnoValue::field(const std::string& name) const {
    if (!isObject())
        return nullptr;
    const SnoObject& obj = asObject();
    SnoObject::const_iterator it = obj.find(name);
    return it != obj.end() ? &it->second : nullptr;
}

const SnoValue* SnoValue::at(size_t index) const {
    if (!isArray())
        return nullptr;
    auto& ta = asArray();
    if (!ta.isArray())
        return nullptr;
    auto& arr = ta.asValueData();
    return index < arr.size() ? &arr[index] : nullptr;
}

size_t SnoValue::size() const {
    if (isArray())
        return asArray().size();
    if (isObject())
        return asObject().size();
    return 0;
}

// ============================================================================
// Private lifetime helpers
// ============================================================================

void SnoValue::destroy() {
    switch (type_) {
    case SVT_STRING:
        s_.str.~StrPtr();
        break;
    case SVT_VEC4:
        s_.vec4.~Vec4Ptr();
        break;
    case SVT_COLORF:
        s_.colorf.~ColorFPtr();
        break;
    case SVT_ARRAY:
        s_.arr.~ArrPtr();
        break;
    case SVT_OBJECT:
        s_.obj.~ObjPtr();
        break;
    default:
        break;
    }
    type_ = SVT_NULL;
}

void SnoValue::copyFrom(const SnoValue& o) {
    type_ = o.type_;
    switch (type_) {
    case SVT_NULL:
        break;
    case SVT_BOOL:
        s_.b = o.s_.b;
        break;
    case SVT_INT:
        s_.i = o.s_.i;
        break;
    case SVT_UINT:
        s_.u = o.s_.u;
        break;
    case SVT_FLOAT:
        s_.f = o.s_.f;
        break;
    case SVT_INT64:
        s_.i64v = o.s_.i64v;
        break;
    case SVT_UINT64:
        s_.u64v = o.s_.u64v;
        break;
    case SVT_BYTE:
        s_.byte = o.s_.byte;
        break;
    case SVT_WORD:
        s_.word = o.s_.word;
        break;
    case SVT_STRING:
        new (&s_.str) StrPtr(std::make_unique<std::string>(*o.s_.str));
        break;
    case SVT_VEC2:
        s_.vec2 = o.s_.vec2;
        break;
    case SVT_VEC3:
        s_.vec3 = o.s_.vec3;
        break;
    case SVT_VEC4:
        new (&s_.vec4) Vec4Ptr(std::make_unique<SnoVec4>(*o.s_.vec4));
        break;
    case SVT_IVEC2:
        s_.ivec2 = o.s_.ivec2;
        break;
    case SVT_COLOR:
        s_.color = o.s_.color;
        break;
    case SVT_COLORF:
        new (&s_.colorf) ColorFPtr(std::make_unique<SnoColorF>(*o.s_.colorf));
        break;
    case SVT_REF:
        s_.ref = o.s_.ref;
        break;
    case SVT_GBID:
        s_.gbid = o.s_.gbid;
        break;
    case SVT_ARRAY:
        new (&s_.arr) ArrPtr(std::make_unique<SnoArray>(*o.s_.arr));
        break;
    case SVT_OBJECT:
        new (&s_.obj) ObjPtr(std::make_unique<SnoObject>(*o.s_.obj));
        break;
    }
}

void SnoValue::moveFrom(SnoValue&& o) {
    type_ = o.type_;
    switch (type_) {
    case SVT_NULL:
        break;
    case SVT_BOOL:
        s_.b = o.s_.b;
        break;
    case SVT_INT:
        s_.i = o.s_.i;
        break;
    case SVT_UINT:
        s_.u = o.s_.u;
        break;
    case SVT_FLOAT:
        s_.f = o.s_.f;
        break;
    case SVT_INT64:
        s_.i64v = o.s_.i64v;
        break;
    case SVT_UINT64:
        s_.u64v = o.s_.u64v;
        break;
    case SVT_BYTE:
        s_.byte = o.s_.byte;
        break;
    case SVT_WORD:
        s_.word = o.s_.word;
        break;
    case SVT_STRING:
        new (&s_.str) StrPtr(std::move(o.s_.str));
        break;
    case SVT_VEC2:
        s_.vec2 = o.s_.vec2;
        break;
    case SVT_VEC3:
        s_.vec3 = o.s_.vec3;
        break;
    case SVT_VEC4:
        new (&s_.vec4) Vec4Ptr(std::move(o.s_.vec4));
        break;
    case SVT_IVEC2:
        s_.ivec2 = o.s_.ivec2;
        break;
    case SVT_COLOR:
        s_.color = o.s_.color;
        break;
    case SVT_COLORF:
        new (&s_.colorf) ColorFPtr(std::move(o.s_.colorf));
        break;
    case SVT_REF:
        s_.ref = o.s_.ref;
        break;
    case SVT_GBID:
        s_.gbid = o.s_.gbid;
        break;
    case SVT_ARRAY:
        new (&s_.arr) ArrPtr(std::move(o.s_.arr));
        break;
    case SVT_OBJECT:
        new (&s_.obj) ObjPtr(std::move(o.s_.obj));
        break;
    }
    o.destroy();
}

// ============================================================================
// SnoArray — Lifecycle
// ============================================================================

SnoArray::SnoArray() : kind_(SVT_NULL) {}

SnoArray::~SnoArray() {
    destroy();
}

SnoArray::SnoArray(const SnoArray& o) : kind_(SVT_NULL) {
    copyFrom(o);
}

SnoArray::SnoArray(SnoArray&& o) noexcept : kind_(SVT_NULL) {
    moveFrom(static_cast<SnoArray&&>(o));
}

SnoArray& SnoArray::operator=(const SnoArray& o) {
    if (this != &o) {
        destroy();
        copyFrom(o);
    }
    return *this;
}

SnoArray& SnoArray::operator=(SnoArray&& o) noexcept {
    if (this != &o) {
        destroy();
        moveFrom(static_cast<SnoArray&&>(o));
    }
    return *this;
}

// ============================================================================
// SnoArray — Convenience constructors
// ============================================================================

// Helper macro to reduce boilerplate for placement-new vector construction.
#define STA_CTOR(Type, tag, field)                                                                 \
    SnoArray::SnoArray(std::vector<Type> v) : kind_(tag) {                                         \
        new (&s_.field) std::vector<Type>(std::move(v));                                           \
    }

STA_CTOR(u8, SVT_BYTE, byteData)
STA_CTOR(u16, SVT_WORD, wordData)
STA_CTOR(u32, SVT_UINT, uintData)
STA_CTOR(f32, SVT_FLOAT, floatData)
STA_CTOR(i32, SVT_INT, intData)
STA_CTOR(SnoVec2, SVT_VEC2, vec2Data)
STA_CTOR(SnoVec3, SVT_VEC3, vec3Data)
STA_CTOR(SnoVec4, SVT_VEC4, vec4Data)
STA_CTOR(SnoIVec2, SVT_IVEC2, ivec2Data)
STA_CTOR(SnoColor, SVT_COLOR, colorData)
STA_CTOR(SnoColorF, SVT_COLORF, colorFData)
STA_CTOR(SnoRef, SVT_REF, refData)
STA_CTOR(SnoGbid, SVT_GBID, gbidData)

SnoArray::SnoArray(std::vector<SnoValue> v) : kind_(SVT_ARRAY) {
    new (&s_.genericData) std::vector<SnoValue>(std::move(v));
}

#undef STA_CTOR

// ============================================================================
// SnoArray — size()
// ============================================================================

size_t SnoArray::size() const {
    switch (kind_) {
    case SVT_BYTE:
        return s_.byteData.size();
    case SVT_WORD:
        return s_.wordData.size();
    case SVT_UINT:
        return s_.uintData.size();
    case SVT_FLOAT:
        return s_.floatData.size();
    case SVT_INT:
        return s_.intData.size();
    case SVT_VEC2:
        return s_.vec2Data.size();
    case SVT_VEC3:
        return s_.vec3Data.size();
    case SVT_VEC4:
        return s_.vec4Data.size();
    case SVT_IVEC2:
        return s_.ivec2Data.size();
    case SVT_COLOR:
        return s_.colorData.size();
    case SVT_COLORF:
        return s_.colorFData.size();
    case SVT_REF:
        return s_.refData.size();
    case SVT_GBID:
        return s_.gbidData.size();
    case SVT_ARRAY:
    case SVT_OBJECT:
        return s_.genericData.size();
    default:
        return 0;
    }
}

// ============================================================================
// SnoArray — Private lifetime helpers
// ============================================================================

// Helper macro — call the destructor on the active vector member.
#define STA_DESTROY(tag, Type, field)                                                              \
    case tag:                                                                                      \
        s_.field.~vector();                                                                        \
        break;

void SnoArray::destroy() {
    switch (kind_) {
        STA_DESTROY(SVT_BYTE, u8, byteData)
        STA_DESTROY(SVT_WORD, u16, wordData)
        STA_DESTROY(SVT_UINT, u32, uintData)
        STA_DESTROY(SVT_FLOAT, f32, floatData)
        STA_DESTROY(SVT_INT, i32, intData)
        STA_DESTROY(SVT_VEC2, SnoVec2, vec2Data)
        STA_DESTROY(SVT_VEC3, SnoVec3, vec3Data)
        STA_DESTROY(SVT_VEC4, SnoVec4, vec4Data)
        STA_DESTROY(SVT_IVEC2, SnoIVec2, ivec2Data)
        STA_DESTROY(SVT_COLOR, SnoColor, colorData)
        STA_DESTROY(SVT_COLORF, SnoColorF, colorFData)
        STA_DESTROY(SVT_REF, SnoRef, refData)
        STA_DESTROY(SVT_GBID, SnoGbid, gbidData)
        STA_DESTROY(SVT_ARRAY, SnoValue, genericData)
        STA_DESTROY(SVT_OBJECT, SnoValue, genericData)
    default:
        break;
    }
    kind_ = SVT_NULL;
}

#undef STA_DESTROY

// Helper macro — placement-new copy-construct the vector member.
#define STA_COPY(tag, Type, field)                                                                 \
    case tag:                                                                                      \
        new (&s_.field) std::vector<Type>(o.s_.field);                                             \
        break;

void SnoArray::copyFrom(const SnoArray& o) {
    kind_ = o.kind_;
    switch (kind_) {
        STA_COPY(SVT_BYTE, u8, byteData)
        STA_COPY(SVT_WORD, u16, wordData)
        STA_COPY(SVT_UINT, u32, uintData)
        STA_COPY(SVT_FLOAT, f32, floatData)
        STA_COPY(SVT_INT, i32, intData)
        STA_COPY(SVT_VEC2, SnoVec2, vec2Data)
        STA_COPY(SVT_VEC3, SnoVec3, vec3Data)
        STA_COPY(SVT_VEC4, SnoVec4, vec4Data)
        STA_COPY(SVT_IVEC2, SnoIVec2, ivec2Data)
        STA_COPY(SVT_COLOR, SnoColor, colorData)
        STA_COPY(SVT_COLORF, SnoColorF, colorFData)
        STA_COPY(SVT_REF, SnoRef, refData)
        STA_COPY(SVT_GBID, SnoGbid, gbidData)
        STA_COPY(SVT_ARRAY, SnoValue, genericData)
        STA_COPY(SVT_OBJECT, SnoValue, genericData)
    default:
        break;
    }
}

#undef STA_COPY

// Helper macro — placement-new move-construct the vector member.
#define STA_MOVE(tag, Type, field)                                                                 \
    case tag:                                                                                      \
        new (&s_.field) std::vector<Type>(std::move(o.s_.field));                                  \
        break;

void SnoArray::moveFrom(SnoArray&& o) {
    kind_ = o.kind_;
    switch (kind_) {
        STA_MOVE(SVT_BYTE, u8, byteData)
        STA_MOVE(SVT_WORD, u16, wordData)
        STA_MOVE(SVT_UINT, u32, uintData)
        STA_MOVE(SVT_FLOAT, f32, floatData)
        STA_MOVE(SVT_INT, i32, intData)
        STA_MOVE(SVT_VEC2, SnoVec2, vec2Data)
        STA_MOVE(SVT_VEC3, SnoVec3, vec3Data)
        STA_MOVE(SVT_VEC4, SnoVec4, vec4Data)
        STA_MOVE(SVT_IVEC2, SnoIVec2, ivec2Data)
        STA_MOVE(SVT_COLOR, SnoColor, colorData)
        STA_MOVE(SVT_COLORF, SnoColorF, colorFData)
        STA_MOVE(SVT_REF, SnoRef, refData)
        STA_MOVE(SVT_GBID, SnoGbid, gbidData)
        STA_MOVE(SVT_ARRAY, SnoValue, genericData)
        STA_MOVE(SVT_OBJECT, SnoValue, genericData)
    default:
        break;
    }
    o.destroy();
}

#undef STA_MOVE

} // namespace sno
} // namespace whiteout
