// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fbx.h
 * @brief The FBX record tree, as both encodings store it (FBX_OBJ_DESIGN §10).
 *
 * An FBX file — binary or ASCII — is a tree of named records, each holding a
 * list of typed values and a list of child records. This layer is that tree and
 * nothing more: lossless, so read → write → read is the codec's own gate, and
 * format-neutral, so the binary and ASCII readers produce the same thing. What
 * the records mean (objects, connections, the transform stack, curves) is
 * `scene.h`'s business.
 *
 * Every read is bounded: record offsets must stay inside their parent, an
 * array's inflated size must equal its declared count and is checked against
 * DEFLATE's own ceiling before anything is allocated, and nesting is capped.
 * No exceptions; a failure is a `ReadOutcome` with an error.
 */

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace fbx {

enum class PropertyType : u8 {
    Bool,   ///< `C`
    I16,    ///< `Y`
    I32,    ///< `I`
    I64,    ///< `L`
    F32,    ///< `F`
    F64,    ///< `D`
    String, ///< `S` — bytes, not necessarily UTF-8; object names carry `\0\1`.
    Raw,    ///< `R`
    BoolArray,
    I32Array,
    I64Array,
    F32Array,
    F64Array,
};

/// One typed value. Scalars live in `integer` / `number`, strings and raw bytes
/// in `text`, arrays in the vector matching their type.
struct Property {
    PropertyType type = PropertyType::I64;
    i64 integer = 0;
    f64 number = 0.0;
    std::string text;
    std::vector<u8> bools;
    std::vector<i32> i32s;
    std::vector<i64> i64s;
    std::vector<f32> f32s;
    std::vector<f64> f64s;

    static Property Bool(bool value);
    /// A `C` field holding a character rather than 0/1, as `Shading` holds 'T'.
    static Property Char(char value);
    static Property Short(i16 value);
    static Property Int(i32 value);
    static Property Long(i64 value);
    static Property Float(f32 value);
    static Property Double(f64 value);
    static Property Str(std::string value);
    static Property RawBytes(std::string bytes);
    static Property Ints(std::vector<i32> values);
    static Property Longs(std::vector<i64> values);
    static Property Floats(std::vector<f32> values);
    static Property Doubles(std::vector<f64> values);

    bool isArray() const;
    bool isNumber() const;
    std::size_t arraySize() const;
    /// Any scalar number as f64 / i64 (a bool is 0 or 1; a string 0).
    f64 toF64() const;
    i64 toI64() const;
    std::string_view toString() const;
    /// Any numeric array, converted element by element.
    std::vector<f64> toF64s() const;
    std::vector<i64> toI64s() const;
    std::vector<i32> toI32s() const;
};

struct Node {
    std::string name;
    std::vector<Property> properties;
    std::vector<Node> children;
    /// Closed with a null record even with no children. The SDK writes one on
    /// every object and section record — an empty `AnimationLayer`, the empty
    /// `References` — and its reader depends on it: without, 3ds Max skinned
    /// a re-written file differently from its own original.
    bool block = false;

    Node() = default;
    explicit Node(std::string nodeName) : name(std::move(nodeName)) {}

    const Node* child(std::string_view childName) const;
    /// Appends a child and returns it.
    Node& add(std::string childName);
    Node& add(std::string childName, std::vector<Property> values);
};

struct File {
    /// 7100–7700. The writer's encoding follows it: 7500 and later use 64-bit
    /// record offsets.
    u32 version = 7400;
    bool binary = true;
    std::vector<Node> nodes;
    /// The 16 bytes a binary file carries after its last record, which the SDK
    /// derives from the header's creation time; kept so a re-write is
    /// byte-faithful, and set by a writer that states its own header.
    std::vector<u8> footerCode;

    const Node* find(std::string_view name) const;
};

struct ReadOutcome {
    std::optional<File> file;
    std::string error;
    std::vector<std::string> warnings;

    bool ok() const {
        return file.has_value();
    }
};

/// `Kaydara FBX Binary` at the start.
bool LooksLikeBinaryFbx(std::span<const u8> data);
/// The `; FBX` comment ASCII files open with, or an `FBXHeaderExtension:` record.
bool LooksLikeAsciiFbx(std::span<const u8> data);

/// Either encoding, by its magic. Files older than 7.0 (FBX 6, SDK 2010 and
/// before) are refused: their object model is another format.
ReadOutcome Read(std::span<const u8> data);
ReadOutcome ReadBinary(std::span<const u8> data);
ReadOutcome ReadAscii(std::string_view text);

/// The binary encoding of @p file at `file.version`: deterministic, arrays of a
/// kilobyte and more deflated as the SDK does, the footer the SDK writes.
std::vector<u8> WriteBinary(const File& file);

} // namespace fbx
} // namespace models
} // namespace whiteout
