// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/common_types.h>

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace whiteout {
namespace mdx {

// ─── MDL Parse Tree ──────────────────────────────────────────────────────────
// A generic, format-agnostic AST for the MDL text format.  The parser knows
// only syntax (blocks, properties, animation tracks, vectors) — semantic
// meaning is left to higher-level consumers.
//
// Grammar summary (both v800 and v1000):
//
//   file       ::= block*
//   block      ::= IDENT [STRING] [NUMBER*] '{' (block | property | keyframe_track)* '}'
//   property   ::= ['static'] IDENT value+ ','
//                 | IDENT ','                           (bare flag)
//   value      ::= NUMBER | STRING | IDENT | '{' value (',' value)* '}'
//   keyframe_track ::= IDENT NUMBER '{' IDENT ','  keyframe* '}'
//   keyframe   ::= NUMBER ':' value ','
//                   ['InTan'  value ',']
//                   ['OutTan' value ',']

// Forward declarations
struct MdlNode;

// ─── Value types ─────────────────────────────────────────────────────────────

// An atomic value: number, string, identifier, or a curly-brace-delimited
// list of atomic values (used for vectors like { 1, 2, 3 }).
struct MdlValue {
    using Array = std::vector<MdlValue>;
    std::variant<f64, std::string, Array> data;

    bool isNumber() const { return std::holds_alternative<f64>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isArray() const { return std::holds_alternative<Array>(data); }

    f64 asNumber() const { return std::get<f64>(data); }
    const std::string& asString() const { return std::get<std::string>(data); }
    const Array& asArray() const { return std::get<Array>(data); }
};

// ─── Keyframe ────────────────────────────────────────────────────────────────

struct MdlKeyframe {
    i32 time;
    MdlValue value;             // scalar or { vector }
    MdlValue inTan;             // empty (no data member) when not present
    MdlValue outTan;
    bool hasTangents = false;
};

// ─── Property ────────────────────────────────────────────────────────────────
// A named property with zero or more values, terminated by comma.
// Examples:
//   MinimumExtent { -1, -2, -3 },   →  name="MinimumExtent", values=[{-1,-2,-3}]
//   static TextureID 5,             →  name="TextureID", values=[5], isStatic=true
//   NonLooping,                     →  name="NonLooping", values=[]  (bare flag)
//   Image "foo.blp",               →  name="Image", values=["foo.blp"]

struct MdlProperty {
    std::string name;
    std::vector<MdlValue> values;
    bool isStatic = false;
};

// ─── Animation Track ─────────────────────────────────────────────────────────
// A named track with an interpolation type and a list of keyframes.
// Examples:
//   Translation 402 { Linear, 300: { 0.2, 0.9, -2.3 }, ... }
//   EventTrack 1 { 0, }        (special: no interp type, bare values)

struct MdlAnimTrack {
    std::string name;
    u32 count = 0;
    std::string interpolation; // "Linear", "Hermite", "Bezier", "DontInterp", or "" for EventTrack
    std::vector<MdlKeyframe> keyframes;
};

// ─── Node ────────────────────────────────────────────────────────────────────
// A named block that may carry string/number header parameters and contains
// an ordered list of children: properties, animation tracks, or nested blocks.
//
// Examples:
//   Version { ... }                       →  name="Version", headerParams=[]
//   Anim "Stand 1" { ... }               →  name="Anim", headerParams=["Stand 1"]
//   Sequences 8 { ... }                  →  name="Sequences", headerParams=[8]
//   Model "HeroPaladinBoss" { ... }      →  name="Model", headerParams=["Hero..."]
//   Faces 1 10572 { ... }               →  name="Faces", headerParams=[1, 10572]
//   Bone "bone_turret" { ... }           →  name="Bone", headerParams=["bone_turret"]

using MdlChild = std::variant<MdlProperty, MdlAnimTrack, MdlNode>;

struct MdlNode {
    std::string name;
    std::vector<MdlValue> headerParams; // string/number params before the '{'
    std::vector<MdlChild> children;
};

// ─── Parse Result ────────────────────────────────────────────────────────────

struct MdlParseError {
    std::string message;
    u32 line = 0;
    u32 column = 0;
};

struct MdlDocument {
    std::vector<MdlNode> roots; // top-level blocks (Version, Model, Sequences, ...)
    std::vector<MdlParseError> errors;

    bool hasErrors() const { return !errors.empty(); }
};

// ─── Parser ──────────────────────────────────────────────────────────────────

class MdlParser {
public:
    /// Parse an MDL source string into a document tree.
    /// The source data must remain valid for the duration of parsing
    /// (string views in tokens reference it), but the returned MdlDocument
    /// owns all its data and is independent of the source buffer.
    static MdlDocument parse(std::string_view source);
};

} // namespace mdx
} // namespace whiteout
