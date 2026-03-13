// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace whiteout {
namespace sno {

// ============================================================================
// Forward declarations
// ============================================================================
struct SnoValue;

/// Ordered map of field-name → value, preserving insertion order.
class SnoObject;

// ============================================================================
// Compound value types
// ============================================================================

/// A 2D float vector.
using SnoVec2 = Vector2f;

/// A 3D float vector.
using SnoVec3 = Vector3f;

/// A 4D float vector .
using SnoVec4 = Vector4f;

/// An integer 2D vector.
struct SnoIVec2 {
    i32 x, y;
};

/// An RGBA colour (0–255 per channel).
struct SnoColor {
    u8 r, g, b, a;
};

/// An RGBA colour (float per channel).
struct SnoColorF {
    f32 r, g, b, a;
};

/// An SNO cross-reference (DT_SNO or DT_SNO_NAME).
struct SnoRef {
    i32 group; ///< SnoGroup id (-1 if unknown)
    i32 snoId; ///< The referenced SNO ID
};

/// A GBID reference.
struct SnoGbid {
    i32 group; ///< The GameBalance group
    u32 raw;   ///< The raw GBID hash
};

// ============================================================================
// SnoValueType — discriminator for the tagged union
// ============================================================================

/// Identifies which type a SnoValue currently holds.
enum SnoValueType {
    SVT_NULL = 0, //  null / DT_NULL
    SVT_BOOL,     //  boolean (serializedBitCount == 1)
    SVT_INT,      //  DT_INT, DT_ENUM
    SVT_UINT,     //  DT_UINT, DT_STARTLOC_NAME
    SVT_FLOAT,    //  DT_FLOAT
    SVT_INT64,    //  DT_INT64
    SVT_UINT64,   //  DT_ACD_NETWORK_NAME, DT_SHARED_SERVER_DATA_ID
    SVT_BYTE,     //  DT_BYTE
    SVT_WORD,     //  DT_WORD
    SVT_STRING,   //  DT_CSTRING, DT_CHARARRAY, DT_STRING_FORMULA
    SVT_VEC2,     //  DT_VECTOR2D
    SVT_VEC3,     //  DT_VECTOR3D
    SVT_VEC4,     //  DT_VECTOR4D
    SVT_IVEC2,    //  DT_BCVEC2I
    SVT_COLOR,    //  DT_RGBACOLOR
    SVT_COLORF,   //  DT_RGBACOLORVALUE
    SVT_REF,      //  DT_SNO, DT_SNO_NAME
    SVT_GBID,     //  DT_GBID
    SVT_ARRAY,    //  DT_VARIABLEARRAY, DT_FIXEDARRAY, DT_POLYMORPHIC_VARIABLEARRAY (SnoArray)
    SVT_OBJECT    //  complex struct / DT_TAGMAP / DT_RANGE / DT_OPTIONAL / DT_BINDABLEPROPERTY
};

/// A typed array.  Homogeneous basic-type arrays store data in a single
/// std::vector<T>, while heterogeneous arrays (kind == SVT_ARRAY) store
/// data as std::vector<SnoValue>.  Discriminated by an SnoValueType tag.
///
/// Lifecycle semantics mirror SnoValue: the unrestricted union requires
/// manual placement-new / destructor calls handled via destroy/copyFrom/moveFrom.
/// Implementations live in sno_value.cpp.
struct SnoArray {

    // -- lifecycle -----------------------------------------------------------

    SnoArray();
    ~SnoArray();

    SnoArray(const SnoArray& o);
    SnoArray(SnoArray&& o) noexcept;

    SnoArray& operator=(const SnoArray& o);
    SnoArray& operator=(SnoArray&& o) noexcept;

    // -- convenience constructors --------------------------------------------

    explicit SnoArray(std::vector<u8> v);
    explicit SnoArray(std::vector<u16> v);
    explicit SnoArray(std::vector<u32> v);
    explicit SnoArray(std::vector<f32> v);
    explicit SnoArray(std::vector<i32> v);
    explicit SnoArray(std::vector<SnoVec2> v);
    explicit SnoArray(std::vector<SnoVec3> v);
    explicit SnoArray(std::vector<SnoVec4> v);
    explicit SnoArray(std::vector<SnoIVec2> v);
    explicit SnoArray(std::vector<SnoColor> v);
    explicit SnoArray(std::vector<SnoColorF> v);
    explicit SnoArray(std::vector<SnoRef> v);
    explicit SnoArray(std::vector<SnoGbid> v);
    explicit SnoArray(std::vector<SnoValue> v);

    // -- discriminator access ------------------------------------------------

    SnoValueType kind() const {
        return kind_;
    }

    // -- type checks ---------------------------------------------------------

    bool isByte() const {
        return kind_ == SVT_BYTE;
    }
    bool isWord() const {
        return kind_ == SVT_WORD;
    }
    bool isUint() const {
        return kind_ == SVT_UINT;
    }
    bool isFloat() const {
        return kind_ == SVT_FLOAT;
    }
    bool isInt() const {
        return kind_ == SVT_INT;
    }
    bool isVec2() const {
        return kind_ == SVT_VEC2;
    }
    bool isVec3() const {
        return kind_ == SVT_VEC3;
    }
    bool isVec4() const {
        return kind_ == SVT_VEC4;
    }
    bool isIVec2() const {
        return kind_ == SVT_IVEC2;
    }
    bool isColor() const {
        return kind_ == SVT_COLOR;
    }
    bool isColorF() const {
        return kind_ == SVT_COLORF;
    }
    bool isRef() const {
        return kind_ == SVT_REF;
    }
    bool isGbid() const {
        return kind_ == SVT_GBID;
    }
    bool isArray() const {
        return kind_ == SVT_ARRAY;
    }
    bool isObject() const {
        return kind_ == SVT_OBJECT;
    }

    // -- accessors (undefined behaviour on type mismatch — check first!) -----

    std::vector<u8>& asByteData() {
        assert(isByte());
        return s_.byteData;
    }
    const std::vector<u8>& asByteData() const {
        assert(isByte());
        return s_.byteData;
    }
    std::vector<u16>& asWordData() {
        assert(isWord());
        return s_.wordData;
    }
    const std::vector<u16>& asWordData() const {
        assert(isWord());
        return s_.wordData;
    }
    std::vector<u32>& asUintData() {
        assert(isUint());
        return s_.uintData;
    }
    const std::vector<u32>& asUintData() const {
        assert(isUint());
        return s_.uintData;
    }
    std::vector<f32>& asFloatData() {
        assert(isFloat());
        return s_.floatData;
    }
    const std::vector<f32>& asFloatData() const {
        assert(isFloat());
        return s_.floatData;
    }
    std::vector<i32>& asIntData() {
        assert(isInt());
        return s_.intData;
    }
    const std::vector<i32>& asIntData() const {
        assert(isInt());
        return s_.intData;
    }
    std::vector<SnoVec2>& asVec2Data() {
        assert(isVec2());
        return s_.vec2Data;
    }
    const std::vector<SnoVec2>& asVec2Data() const {
        assert(isVec2());
        return s_.vec2Data;
    }
    std::vector<SnoVec3>& asVec3Data() {
        assert(isVec3());
        return s_.vec3Data;
    }
    const std::vector<SnoVec3>& asVec3Data() const {
        assert(isVec3());
        return s_.vec3Data;
    }
    std::vector<SnoVec4>& asVec4Data() {
        assert(isVec4());
        return s_.vec4Data;
    }
    const std::vector<SnoVec4>& asVec4Data() const {
        assert(isVec4());
        return s_.vec4Data;
    }
    std::vector<SnoIVec2>& asIVec2Data() {
        assert(isIVec2());
        return s_.ivec2Data;
    }
    const std::vector<SnoIVec2>& asIVec2Data() const {
        assert(isIVec2());
        return s_.ivec2Data;
    }
    std::vector<SnoColor>& asColorData() {
        assert(isColor());
        return s_.colorData;
    }
    const std::vector<SnoColor>& asColorData() const {
        assert(isColor());
        return s_.colorData;
    }
    std::vector<SnoColorF>& asColorFData() {
        assert(isColorF());
        return s_.colorFData;
    }
    const std::vector<SnoColorF>& asColorFData() const {
        assert(isColorF());
        return s_.colorFData;
    }
    std::vector<SnoRef>& asRefData() {
        assert(isRef());
        return s_.refData;
    }
    const std::vector<SnoRef>& asRefData() const {
        assert(isRef());
        return s_.refData;
    }
    std::vector<SnoGbid>& asGbidData() {
        assert(isGbid());
        return s_.gbidData;
    }
    const std::vector<SnoGbid>& asGbidData() const {
        assert(isGbid());
        return s_.gbidData;
    }
    std::vector<SnoValue>& asValueData() {
        assert(isArray() || isObject());
        return s_.genericData;
    }
    const std::vector<SnoValue>& asValueData() const {
        assert(isArray() || isObject());
        return s_.genericData;
    }

    /// Return the number of elements in the active vector.
    size_t size() const;

private:
    SnoValueType kind_; ///< Element type (SVT_*)

    union Storage {
        std::vector<u8> byteData;          ///< For byte arrays (DT_CHARARRAY, DT_BYTE)
        std::vector<u16> wordData;         ///< For word arrays (DT_WORD)
        std::vector<u32> uintData;         ///< For uint arrays (DT_UINT)
        std::vector<f32> floatData;        ///< For float arrays (DT_FLOAT)
        std::vector<i32> intData;          ///< For int arrays (DT_INT, DT_ENUM)
        std::vector<SnoVec2> vec2Data;     ///< For DT_VECTOR2D
        std::vector<SnoVec3> vec3Data;     ///< For DT_VECTOR3D
        std::vector<SnoVec4> vec4Data;     ///< For DT_VECTOR4D
        std::vector<SnoIVec2> ivec2Data;   ///< For DT_BCVEC2I
        std::vector<SnoColor> colorData;   ///< For DT_RGBACOLOR
        std::vector<SnoColorF> colorFData; ///< For DT_RGBACOLORVALUE
        std::vector<SnoRef> refData;       ///< For DT_SNO, DT_SNO_NAME
        std::vector<SnoGbid> gbidData;     ///< For DT_GBID
        std::vector<SnoValue> genericData; ///< For untyped arrays (DT_VARIABLEARRAY, etc.)

        Storage() {}  // no-op — SnoArray manages the active member
        ~Storage() {} // no-op — SnoArray manages the active member
    };

    Storage s_;

    void destroy();
    void copyFrom(const SnoArray& o);
    void moveFrom(SnoArray&& o);
};

// ============================================================================
// SnoValue — the generic DOM node  (C++11 compatible)
// ============================================================================

/// A tagged union that can hold any value produced by the SNO deserializer.
///
/// Modelled after a JSON DOM: scalars, arrays, and objects (maps).
/// Compound game-specific types (vectors, colours, references) get their own
/// alternatives so the caller can work with them directly.
///
/// Implemented as a C++11 unrestricted union with a discriminator tag.
/// Copy/move/destroy are handled manually for the non-trivial
/// members (std::string, SnoArray, SnoObject).  Implementations live in
/// sno_value.cpp.
struct SnoValue {

    // -- lifecycle (defined in sno_value.cpp) --------------------------------

    SnoValue();
    ~SnoValue();

    SnoValue(const SnoValue& o);
    SnoValue(SnoValue&& o);

    SnoValue& operator=(const SnoValue& o);
    SnoValue& operator=(SnoValue&& o);

    // -- convenience constructors (defined in sno_value.cpp) -----------------

    explicit SnoValue(bool v);
    SnoValue(i32 v);
    SnoValue(u32 v);
    SnoValue(f32 v);
    SnoValue(i64 v);
    SnoValue(u64 v);
    SnoValue(u8 v);
    SnoValue(u16 v);
    SnoValue(std::string v);
    SnoValue(SnoVec2 v);
    SnoValue(SnoVec3 v);
    SnoValue(SnoVec4 v);
    SnoValue(SnoIVec2 v);
    SnoValue(SnoColor v);
    SnoValue(SnoColorF v);
    SnoValue(SnoRef v);
    SnoValue(SnoGbid v);
    SnoValue(SnoArray v);
    SnoValue(SnoObject v);

    // -- discriminator access -----------------------------------------------

    SnoValueType type() const {
        return type_;
    }

    // -- type checks --------------------------------------------------------

    bool isNull() const {
        return type_ == SVT_NULL;
    }
    bool isBool() const {
        return type_ == SVT_BOOL;
    }
    bool isInt() const {
        return type_ == SVT_INT;
    }
    bool isUint() const {
        return type_ == SVT_UINT;
    }
    bool isFloat() const {
        return type_ == SVT_FLOAT;
    }
    bool isInt64() const {
        return type_ == SVT_INT64;
    }
    bool isUint64() const {
        return type_ == SVT_UINT64;
    }
    bool isByte() const {
        return type_ == SVT_BYTE;
    }
    bool isWord() const {
        return type_ == SVT_WORD;
    }
    bool isString() const {
        return type_ == SVT_STRING;
    }
    bool isVec2() const {
        return type_ == SVT_VEC2;
    }
    bool isVec3() const {
        return type_ == SVT_VEC3;
    }
    bool isVec4() const {
        return type_ == SVT_VEC4;
    }
    bool isIVec2() const {
        return type_ == SVT_IVEC2;
    }
    bool isColor() const {
        return type_ == SVT_COLOR;
    }
    bool isColorF() const {
        return type_ == SVT_COLORF;
    }
    bool isRef() const {
        return type_ == SVT_REF;
    }
    bool isGbid() const {
        return type_ == SVT_GBID;
    }
    bool isArray() const {
        return type_ == SVT_ARRAY;
    }
    bool isObject() const {
        return type_ == SVT_OBJECT;
    }

    // -- accessors (undefined behaviour on type mismatch — check first!) ----

    bool& asBool() {
        assert(isBool());
        return s_.b;
    }
    const bool& asBool() const {
        assert(isBool());
        return s_.b;
    }
    i32& asInt() {
        assert(isInt());
        return s_.i;
    }
    const i32& asInt() const {
        assert(isInt());
        return s_.i;
    }
    u32& asUint() {
        assert(isUint());
        return s_.u;
    }
    const u32& asUint() const {
        assert(isUint());
        return s_.u;
    }
    f32& asFloat() {
        assert(isFloat());
        return s_.f;
    }
    const f32& asFloat() const {
        assert(isFloat());
        return s_.f;
    }
    i64& asInt64() {
        assert(isInt64());
        return s_.i64v;
    }
    const i64& asInt64() const {
        assert(isInt64());
        return s_.i64v;
    }
    u64& asUint64() {
        assert(isUint64());
        return s_.u64v;
    }
    const u64& asUint64() const {
        assert(isUint64());
        return s_.u64v;
    }
    u8& asByte() {
        assert(isByte());
        return s_.byte;
    }
    const u8& asByte() const {
        assert(isByte());
        return s_.byte;
    }
    u16& asWord() {
        assert(isWord());
        return s_.word;
    }
    const u16& asWord() const {
        assert(isWord());
        return s_.word;
    }
    std::string& asString() {
        assert(isString());
        return *s_.str;
    }
    const std::string& asString() const {
        assert(isString());
        return *s_.str;
    }
    SnoVec2& asVec2() {
        assert(isVec2());
        return s_.vec2;
    }
    const SnoVec2& asVec2() const {
        assert(isVec2());
        return s_.vec2;
    }
    SnoVec3& asVec3() {
        assert(isVec3());
        return s_.vec3;
    }
    const SnoVec3& asVec3() const {
        assert(isVec3());
        return s_.vec3;
    }
    SnoVec4& asVec4() {
        assert(isVec4());
        return *s_.vec4;
    }
    const SnoVec4& asVec4() const {
        assert(isVec4());
        return *s_.vec4;
    }
    SnoIVec2& asIVec2() {
        assert(isIVec2());
        return s_.ivec2;
    }
    const SnoIVec2& asIVec2() const {
        assert(isIVec2());
        return s_.ivec2;
    }
    SnoColor& asColor() {
        assert(isColor());
        return s_.color;
    }
    const SnoColor& asColor() const {
        assert(isColor());
        return s_.color;
    }
    SnoColorF& asColorF() {
        assert(isColorF());
        return *s_.colorf;
    }
    const SnoColorF& asColorF() const {
        assert(isColorF());
        return *s_.colorf;
    }
    SnoRef& asRef() {
        assert(isRef());
        return s_.ref;
    }
    const SnoRef& asRef() const {
        assert(isRef());
        return s_.ref;
    }
    SnoGbid& asGbid() {
        assert(isGbid());
        return s_.gbid;
    }
    const SnoGbid& asGbid() const {
        assert(isGbid());
        return s_.gbid;
    }
    SnoArray& asArray() {
        assert(isArray());
        return *s_.arr;
    }
    const SnoArray& asArray() const {
        assert(isArray());
        return *s_.arr;
    }
    SnoObject& asObject();
    const SnoObject& asObject() const;

    /// Look up a field by name on an object value. Returns nullptr if this is
    /// not an object or the field is not present.
    const SnoValue* field(const std::string& name) const;

    /// Index into a typed array with generic SnoValue elements.
    /// Returns nullptr if out of range or not a generic typed array.
    const SnoValue* at(size_t index) const;

    /// Return element count for arrays, field count for objects, 0 otherwise.
    size_t size() const;

private:
    // -- heap-indirected pointer aliases ------------------------------------
    // Members larger than 12 bytes are stored via std::unique_ptr to keep the
    // union (and therefore every SnoValue) small.
    using StrPtr = std::unique_ptr<std::string>;
    using Vec4Ptr = std::unique_ptr<SnoVec4>;
    using ColorFPtr = std::unique_ptr<SnoColorF>;
    using ArrPtr = std::unique_ptr<SnoArray>;
    using ObjPtr = std::unique_ptr<SnoObject>;

    // -- unrestricted union -------------------------------------------------
    // Non-trivial members require explicit placement-new construction and
    // manual destructor calls.  Members >12 bytes are heap-indirected through
    // std::unique_ptr so the union stays at 12 bytes (≤ SnoVec3).
    union Storage {
        bool b;
        i32 i;
        u32 u;
        f32 f;
        i64 i64v;
        u64 u64v;
        u8 byte;
        u16 word;
        StrPtr str; // std::string  (~32 B → 8 B ptr)
        SnoVec2 vec2;
        SnoVec3 vec3;
        Vec4Ptr vec4; // SnoVec4      (16 B  → 8 B ptr)
        SnoIVec2 ivec2;
        SnoColor color;
        ColorFPtr colorf; // SnoColorF    (16 B  → 8 B ptr)
        SnoRef ref;
        SnoGbid gbid;
        ArrPtr arr; // SnoArray    (~28 B → 8 B ptr)
        ObjPtr obj; // SnoObject    (~48 B → 8 B ptr)

        Storage() {}  // no-op — SnoValue manages the active member
        ~Storage() {} // no-op — SnoValue manages the active member
    };

    SnoValueType type_;
    Storage s_;

    void destroy();
    void copyFrom(const SnoValue& o);
    void moveFrom(SnoValue&& o);
};

// ============================================================================
// SnoObject — insertion-order-preserving string→SnoValue map  (C++11 compatible)
// ============================================================================

class SnoObject {
public:
    using value_type = std::pair<std::string, SnoValue>;
    using container_type = std::vector<value_type>;
    using iterator = container_type::iterator;
    using const_iterator = container_type::const_iterator;
    using size_type = container_type::size_type;

    SnoObject() = default;

    /// Access or insert a field by name (preserves insertion order).
    SnoValue& operator[](const std::string& key) {
        auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const value_type& p) { return p.first == key; });
        if (it != entries_.end())
            return it->second;
        entries_.emplace_back(key, SnoValue());
        return entries_.back().second;
    }

    iterator find(const std::string& key) {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&](const value_type& p) { return p.first == key; });
    }

    const_iterator find(const std::string& key) const {
        return std::find_if(entries_.begin(), entries_.end(),
                            [&](const value_type& p) { return p.first == key; });
    }

    iterator begin() {
        return entries_.begin();
    }
    iterator end() {
        return entries_.end();
    }
    const_iterator begin() const {
        return entries_.begin();
    }
    const_iterator end() const {
        return entries_.end();
    }

    size_type size() const {
        return entries_.size();
    }
    bool empty() const {
        return entries_.empty();
    }

private:
    container_type entries_;
};

// -- deferred inline definitions (need both SnoValue and SnoObject complete) --

inline SnoObject& SnoValue::asObject() {
    assert(isObject());
    return *s_.obj;
}
inline const SnoObject& SnoValue::asObject() const {
    assert(isObject());
    return *s_.obj;
}

} // namespace sno
} // namespace whiteout
