// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/fbx/fbx.h"

#include <cstring>

namespace whiteout {
namespace models {
namespace fbx {

namespace {

template <class Out, class In>
std::vector<Out> Convert(const std::vector<In>& in) {
    std::vector<Out> out(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        out[i] = static_cast<Out>(in[i]);
    }
    return out;
}

template <class Out>
std::vector<Out> ConvertArray(const Property& p) {
    switch (p.type) {
    case PropertyType::BoolArray:
        return Convert<Out>(p.bools);
    case PropertyType::I32Array:
        return Convert<Out>(p.i32s);
    case PropertyType::I64Array:
        return Convert<Out>(p.i64s);
    case PropertyType::F32Array:
        return Convert<Out>(p.f32s);
    case PropertyType::F64Array:
        return Convert<Out>(p.f64s);
    default:
        break;
    }
    return {};
}

} // namespace

Property Property::Bool(bool value) {
    Property p;
    p.type = PropertyType::Bool;
    p.integer = value ? 1 : 0;
    return p;
}

Property Property::Char(char value) {
    Property p;
    p.type = PropertyType::Bool;
    p.integer = static_cast<u8>(value);
    return p;
}

Property Property::Short(i16 value) {
    Property p;
    p.type = PropertyType::I16;
    p.integer = value;
    return p;
}

Property Property::Int(i32 value) {
    Property p;
    p.type = PropertyType::I32;
    p.integer = value;
    return p;
}

Property Property::Long(i64 value) {
    Property p;
    p.type = PropertyType::I64;
    p.integer = value;
    return p;
}

Property Property::Float(f32 value) {
    Property p;
    p.type = PropertyType::F32;
    p.number = value;
    return p;
}

Property Property::Double(f64 value) {
    Property p;
    p.type = PropertyType::F64;
    p.number = value;
    return p;
}

Property Property::Str(std::string value) {
    Property p;
    p.type = PropertyType::String;
    p.text = std::move(value);
    return p;
}

Property Property::RawBytes(std::string bytes) {
    Property p;
    p.type = PropertyType::Raw;
    p.text = std::move(bytes);
    return p;
}

Property Property::Ints(std::vector<i32> values) {
    Property p;
    p.type = PropertyType::I32Array;
    p.i32s = std::move(values);
    return p;
}

Property Property::Longs(std::vector<i64> values) {
    Property p;
    p.type = PropertyType::I64Array;
    p.i64s = std::move(values);
    return p;
}

Property Property::Floats(std::vector<f32> values) {
    Property p;
    p.type = PropertyType::F32Array;
    p.f32s = std::move(values);
    return p;
}

Property Property::Doubles(std::vector<f64> values) {
    Property p;
    p.type = PropertyType::F64Array;
    p.f64s = std::move(values);
    return p;
}

bool Property::isArray() const {
    return type >= PropertyType::BoolArray;
}

bool Property::isNumber() const {
    return type <= PropertyType::F64;
}

std::size_t Property::arraySize() const {
    switch (type) {
    case PropertyType::BoolArray:
        return bools.size();
    case PropertyType::I32Array:
        return i32s.size();
    case PropertyType::I64Array:
        return i64s.size();
    case PropertyType::F32Array:
        return f32s.size();
    case PropertyType::F64Array:
        return f64s.size();
    default:
        return 0;
    }
}

f64 Property::toF64() const {
    if (type == PropertyType::F32 || type == PropertyType::F64) {
        return number;
    }
    if (isNumber()) {
        return static_cast<f64>(integer);
    }
    return 0.0;
}

i64 Property::toI64() const {
    if (type == PropertyType::F32 || type == PropertyType::F64) {
        return static_cast<i64>(number);
    }
    if (isNumber()) {
        return integer;
    }
    return 0;
}

std::string_view Property::toString() const {
    return type == PropertyType::String || type == PropertyType::Raw ? std::string_view(text)
                                                                     : std::string_view();
}

std::vector<f64> Property::toF64s() const {
    return ConvertArray<f64>(*this);
}

std::vector<i64> Property::toI64s() const {
    return ConvertArray<i64>(*this);
}

std::vector<i32> Property::toI32s() const {
    return ConvertArray<i32>(*this);
}

const Node* Node::child(std::string_view childName) const {
    for (const Node& node : children) {
        if (node.name == childName) {
            return &node;
        }
    }
    return nullptr;
}

Node& Node::add(std::string childName) {
    children.emplace_back(std::move(childName));
    return children.back();
}

Node& Node::add(std::string childName, std::vector<Property> values) {
    Node& node = add(std::move(childName));
    node.properties = std::move(values);
    return node;
}

const Node* File::find(std::string_view name) const {
    for (const Node& node : nodes) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

namespace {

constexpr char kBinaryMagic[] = "Kaydara FBX Binary  ";

} // namespace

bool LooksLikeBinaryFbx(std::span<const u8> data) {
    return data.size() >= 27 && std::memcmp(data.data(), kBinaryMagic, 20) == 0;
}

bool LooksLikeAsciiFbx(std::span<const u8> data) {
    // Skip a UTF-8 byte-order mark and leading blank space.
    std::size_t i = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        i = 3;
    }
    while (i < data.size() && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' ||
                               data[i] == '\n')) {
        ++i;
    }
    const std::string_view rest(reinterpret_cast<const char*>(data.data()) + i,
                                data.size() - i < 64 ? data.size() - i : 64);
    return rest.starts_with("; FBX") || rest.starts_with("FBXHeaderExtension:");
}

ReadOutcome Read(std::span<const u8> data) {
    if (LooksLikeBinaryFbx(data)) {
        return ReadBinary(data);
    }
    if (LooksLikeAsciiFbx(data)) {
        return ReadAscii(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    }
    ReadOutcome outcome;
    outcome.error = "not an FBX file";
    return outcome;
}

} // namespace fbx
} // namespace models
} // namespace whiteout
