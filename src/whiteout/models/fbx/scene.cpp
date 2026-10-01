// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/fbx/scene.h"

namespace whiteout {
namespace models {
namespace fbx {

namespace {

std::string_view StringOf(const Property* p) {
    return p != nullptr ? p->toString() : std::string_view();
}

} // namespace

// ============================================================================
// PropertyTable
// ============================================================================

void PropertyTable::read(const Node* properties70) {
    if (properties70 == nullptr) {
        return;
    }
    rows_.reserve(properties70->children.size());
    for (const Node& p : properties70->children) {
        if (p.name != "P" || p.properties.size() < 4) {
            continue;
        }
        PropertyRow row;
        row.name = std::string(p.properties[0].toString());
        row.type = std::string(p.properties[1].toString());
        row.flags = std::string(p.properties[3].toString());
        for (std::size_t i = 4; i < p.properties.size(); ++i) {
            row.values.push_back(&p.properties[i]);
        }
        rows_.push_back(std::move(row));
    }
}

const PropertyRow* PropertyTable::find(std::string_view name) const {
    for (const PropertyRow& row : rows_) {
        if (row.name == name) {
            return &row;
        }
    }
    return template_ != nullptr ? template_->find(name) : nullptr;
}

f64 PropertyTable::number(std::string_view name, f64 fallback) const {
    const PropertyRow* row = find(name);
    return row != nullptr && !row->values.empty() ? row->values[0]->toF64() : fallback;
}

i64 PropertyTable::integer(std::string_view name, i64 fallback) const {
    const PropertyRow* row = find(name);
    return row != nullptr && !row->values.empty() ? row->values[0]->toI64() : fallback;
}

std::array<f64, 3> PropertyTable::vec3(std::string_view name, std::array<f64, 3> fallback) const {
    const PropertyRow* row = find(name);
    if (row == nullptr || row->values.size() < 3) {
        return fallback;
    }
    return {row->values[0]->toF64(), row->values[1]->toF64(), row->values[2]->toF64()};
}

std::string_view PropertyTable::string(std::string_view name) const {
    const PropertyRow* row = find(name);
    return row != nullptr && !row->values.empty() ? StringOf(row->values[0]) : std::string_view();
}

// ============================================================================
// GlobalSettings
// ============================================================================

f64 GlobalSettings::frameRate() const {
    // FbxTime::EMode, in its own order.
    constexpr f64 kRates[] = {30.0, 120.0, 100.0, 60.0, 50.0,  48.0, 30.0,    30.0,  29.97, 29.97,
                              25.0, 24.0,  1000.0, 23.976, -1.0, 96.0, 72.0, 59.94, 119.88};
    if (timeMode == 14) {
        return customFrameRate > 0.0 ? customFrameRate : 30.0;
    }
    if (timeMode >= 0 && timeMode < static_cast<i32>(sizeof(kRates) / sizeof(kRates[0]))) {
        return kRates[timeMode];
    }
    return 30.0;
}

std::string ObjectName(std::string_view raw) {
    const std::size_t separator = raw.find(std::string_view("\0\1", 2));
    if (separator != std::string_view::npos) {
        return std::string(raw.substr(0, separator));
    }
    const std::size_t colons = raw.find("::");
    if (colons != std::string_view::npos) {
        return std::string(raw.substr(colons + 2));
    }
    return std::string(raw);
}

// ============================================================================
// Scene
// ============================================================================

SceneOutcome Scene::Build(File file) {
    SceneOutcome outcome;
    Scene scene;
    scene.file_ = std::move(file);
    const File& f = scene.file_;

    // The templates first: objects point at them.
    if (const Node* definitions = f.find("Definitions")) {
        for (const Node& type : definitions->children) {
            if (type.name != "ObjectType" || type.properties.empty()) {
                continue;
            }
            const Node* propertyTemplate = type.child("PropertyTemplate");
            if (propertyTemplate == nullptr) {
                continue;
            }
            PropertyTable table;
            table.read(propertyTemplate->child("Properties70"));
            scene.templates_.emplace_back(std::string(type.properties[0].toString()),
                                          std::move(table));
        }
    }
    const auto templateFor = [&](std::string_view kind) -> const PropertyTable* {
        for (const auto& [name, table] : scene.templates_) {
            if (name == kind) {
                return &table;
            }
        }
        return nullptr;
    };

    if (const Node* objects = f.find("Objects")) {
        scene.objects_.reserve(objects->children.size());
        for (const Node& record : objects->children) {
            if (record.properties.empty()) {
                continue;
            }
            Object object;
            object.kind = record.name;
            object.id = record.properties[0].toI64();
            if (record.properties.size() > 1) {
                object.name = ObjectName(record.properties[1].toString());
            }
            if (record.properties.size() > 2) {
                object.subclass = std::string(record.properties[2].toString());
            }
            object.node = &record;
            object.properties.read(record.child("Properties70"));
            object.properties.setTemplate(templateFor(record.name));
            if (scene.byId_.count(object.id) != 0) {
                outcome.warnings.push_back("object id " + std::to_string(object.id) +
                                           " appears twice; the first one keeps it");
                continue;
            }
            scene.byId_.emplace(object.id, scene.objects_.size());
            scene.objects_.push_back(std::move(object));
        }
    } else {
        outcome.error = "no Objects record";
        return outcome;
    }

    if (const Node* connections = f.find("Connections")) {
        for (const Node& c : connections->children) {
            if (c.name != "C" || c.properties.size() < 3) {
                continue;
            }
            Connection connection;
            const std::string_view kind = c.properties[0].toString();
            connection.toProperty = kind == "OP";
            if (kind != "OO" && kind != "OP") {
                continue; // PO / PP: property-to-property, nothing here reads them.
            }
            connection.child = c.properties[1].toI64();
            connection.parent = c.properties[2].toI64();
            if (connection.toProperty && c.properties.size() > 3) {
                connection.property = std::string(c.properties[3].toString());
            }
            const std::size_t index = scene.connections_.size();
            scene.byParent_[connection.parent].push_back(index);
            scene.byChild_[connection.child].push_back(index);
            scene.connections_.push_back(std::move(connection));
        }
    }

    if (const Node* global = f.find("GlobalSettings")) {
        PropertyTable table;
        table.read(global->child("Properties70"));
        GlobalSettings& g = scene.globals_;
        g.upAxis = static_cast<i32>(table.integer("UpAxis", 1));
        g.upAxisSign = static_cast<i32>(table.integer("UpAxisSign", 1));
        g.frontAxis = static_cast<i32>(table.integer("FrontAxis", 2));
        g.frontAxisSign = static_cast<i32>(table.integer("FrontAxisSign", 1));
        g.coordAxis = static_cast<i32>(table.integer("CoordAxis", 0));
        g.coordAxisSign = static_cast<i32>(table.integer("CoordAxisSign", 1));
        g.unitScaleFactor = table.number("UnitScaleFactor", 1.0);
        g.timeMode = static_cast<i32>(table.integer("TimeMode", 0));
        g.customFrameRate = table.number("CustomFrameRate", -1.0);
        g.timeSpanStart = table.integer("TimeSpanStart", 0);
        g.timeSpanStop = table.integer("TimeSpanStop", 0);
    } else {
        outcome.warnings.push_back("no GlobalSettings; Y up, centimetres, 30 fps assumed");
    }

    outcome.scene.emplace(std::move(scene));
    return outcome;
}

const Object* Scene::object(i64 id) const {
    const auto found = byId_.find(id);
    return found != byId_.end() ? &objects_[found->second] : nullptr;
}

std::vector<const Object*> Scene::children(i64 parent, std::string_view kind) const {
    std::vector<const Object*> out;
    const auto found = byParent_.find(parent);
    if (found == byParent_.end()) {
        return out;
    }
    for (const std::size_t index : found->second) {
        const Connection& c = connections_[index];
        if (c.toProperty) {
            continue;
        }
        const Object* child = object(c.child);
        if (child != nullptr && (kind.empty() || child->kind == kind)) {
            out.push_back(child);
        }
    }
    return out;
}

std::vector<const Object*> Scene::propertyChildren(i64 parent, std::string_view property) const {
    std::vector<const Object*> out;
    const auto found = byParent_.find(parent);
    if (found == byParent_.end()) {
        return out;
    }
    for (const std::size_t index : found->second) {
        const Connection& c = connections_[index];
        if (!c.toProperty || c.property != property) {
            continue;
        }
        if (const Object* child = object(c.child)) {
            out.push_back(child);
        }
    }
    return out;
}

std::vector<std::pair<const Object*, std::string_view>> Scene::propertyInputs(i64 parent) const {
    std::vector<std::pair<const Object*, std::string_view>> out;
    const auto found = byParent_.find(parent);
    if (found == byParent_.end()) {
        return out;
    }
    for (const std::size_t index : found->second) {
        const Connection& c = connections_[index];
        if (!c.toProperty) {
            continue;
        }
        if (const Object* child = object(c.child)) {
            out.emplace_back(child, c.property);
        }
    }
    return out;
}

std::vector<const Object*> Scene::parents(i64 child, std::string_view kind) const {
    std::vector<const Object*> out;
    const auto found = byChild_.find(child);
    if (found == byChild_.end()) {
        return out;
    }
    for (const std::size_t index : found->second) {
        const Connection& c = connections_[index];
        if (c.toProperty) {
            continue;
        }
        const Object* parent = object(c.parent);
        if (parent != nullptr && (kind.empty() || parent->kind == kind)) {
            out.push_back(parent);
        }
    }
    return out;
}

std::vector<std::pair<const Object*, std::string_view>> Scene::propertyParents(i64 child) const {
    std::vector<std::pair<const Object*, std::string_view>> out;
    const auto found = byChild_.find(child);
    if (found == byChild_.end()) {
        return out;
    }
    for (const std::size_t index : found->second) {
        const Connection& c = connections_[index];
        if (!c.toProperty) {
            continue;
        }
        if (const Object* parent = object(c.parent)) {
            out.emplace_back(parent, c.property);
        }
    }
    return out;
}

} // namespace fbx
} // namespace models
} // namespace whiteout
