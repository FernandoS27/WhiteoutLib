// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file scene.h
 * @brief What an FBX record tree means: objects, connections and properties
 *        (FBX_OBJ_DESIGN §1, §10).
 *
 * Every object is a record under `Objects` with an id, a name and a subclass;
 * how they relate is the `Connections` list alone (object→object, and
 * object→property). A property an object does not state takes its value from
 * the `Definitions` template for its kind — the SDK writes only what differs —
 * and the table here answers through that fall-back.
 *
 * The binary encoding names an object `Name\0\1Class`, the ASCII one
 * `Class::Name`; both come out as the bare name.
 */

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fbx.h"

namespace whiteout {
namespace models {
namespace fbx {

/// FbxTime: 46 186 158 000 ticks to the second.
inline constexpr i64 kTicksPerSecond = 46186158000LL;

/// One `P:` row: name, type, label, flags, values.
struct PropertyRow {
    std::string name;
    std::string type;
    std::string flags;
    std::vector<const Property*> values;
};

class PropertyTable {
public:
    void read(const Node* properties70);
    void setTemplate(const PropertyTable* fallback) {
        template_ = fallback;
    }

    const PropertyRow* find(std::string_view name) const;
    bool has(std::string_view name) const {
        return find(name) != nullptr;
    }
    f64 number(std::string_view name, f64 fallback = 0.0) const;
    i64 integer(std::string_view name, i64 fallback = 0) const;
    std::array<f64, 3> vec3(std::string_view name, std::array<f64, 3> fallback = {0, 0, 0}) const;
    std::string_view string(std::string_view name) const;
    /// The rows this object states itself, template rows not included.
    std::span<const PropertyRow> own() const {
        return rows_;
    }

private:
    std::vector<PropertyRow> rows_;
    const PropertyTable* template_ = nullptr;
};

struct Object {
    i64 id = 0;
    /// The record name: `Model`, `Geometry`, `Material`, `Deformer`, …
    std::string kind;
    std::string name;
    /// The third value: `Mesh`, `LimbNode`, `Null`, `Skin`, `Cluster`, …
    std::string subclass;
    const Node* node = nullptr;
    PropertyTable properties;
};

struct Connection {
    /// `OP`: the child feeds one of the parent's properties, named here.
    bool toProperty = false;
    i64 child = 0;
    i64 parent = 0;
    std::string property;
};

struct GlobalSettings {
    i32 upAxis = 1;
    i32 upAxisSign = 1;
    i32 frontAxis = 2;
    i32 frontAxisSign = 1;
    i32 coordAxis = 0;
    i32 coordAxisSign = 1;
    /// Centimetres per unit.
    f64 unitScaleFactor = 1.0;
    i32 timeMode = 0;
    f64 customFrameRate = -1.0;
    i64 timeSpanStart = 0;
    i64 timeSpanStop = 0;
    /// Frames per second, from `TimeMode` (or `CustomFrameRate`); 30 when unknown.
    f64 frameRate() const;
};

/// `Name\0\1Class` or `Class::Name`, as the bare name.
std::string ObjectName(std::string_view raw);

struct SceneOutcome;

/// The objects of one file. Move-only: it holds pointers into its own records.
class Scene {
public:
    static SceneOutcome Build(File file);

    Scene(Scene&&) = default;
    Scene& operator=(Scene&&) = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    const File& file() const {
        return file_;
    }
    std::span<const Object> objects() const {
        return objects_;
    }
    const Object* object(i64 id) const;
    const GlobalSettings& globals() const {
        return globals_;
    }

    /// The objects connected under @p parent object-to-object, of @p kind when given.
    std::vector<const Object*> children(i64 parent, std::string_view kind = {}) const;
    /// The objects connected to @p parent's property @p property.
    std::vector<const Object*> propertyChildren(i64 parent, std::string_view property) const;
    /// Every object-to-property connection into @p parent: (child, property name).
    std::vector<std::pair<const Object*, std::string_view>> propertyInputs(i64 parent) const;
    /// What @p child is connected under, object-to-object, of @p kind when given.
    std::vector<const Object*> parents(i64 child, std::string_view kind = {}) const;
    /// The property of a parent @p child feeds, with the parent: for an
    /// animation curve node, which channel of which model it drives.
    std::vector<std::pair<const Object*, std::string_view>> propertyParents(i64 child) const;

private:
    Scene() = default;

    File file_;
    std::vector<Object> objects_;
    std::unordered_map<i64, std::size_t> byId_;
    std::vector<Connection> connections_;
    std::unordered_map<i64, std::vector<std::size_t>> byParent_;
    std::unordered_map<i64, std::vector<std::size_t>> byChild_;
    std::vector<std::pair<std::string, PropertyTable>> templates_;
    GlobalSettings globals_;
};

struct SceneOutcome {
    std::optional<Scene> scene;
    std::string error;
    std::vector<std::string> warnings;
    bool ok() const {
        return scene.has_value();
    }
};

} // namespace fbx
} // namespace models
} // namespace whiteout
