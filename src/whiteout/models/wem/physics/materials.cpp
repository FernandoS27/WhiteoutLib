// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/physics/materials.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace whiteout {
namespace models {
namespace wem {

namespace {

// Density, friction, restitution; linear and angular damping. Sand's
// restitution, Snow's, Paper's angular damping and Hair's two dampings are the
// shipped art's, not the 2013 file's (0.5, 0.8, 0.25, 0.3 and 0.2).
constexpr std::array<PhysicsMaterialPreset, 22> kPresets{{
    {0, "Metal Heavy", {4000.0f, 0.5f, 0.1f}, 0.01f, 0.01f},
    {1, "Metal Light", {2500.0f, 0.5f, 0.1f}, 0.01f, 0.01f},
    {2, "Metal Protoss", {2400.0f, 0.5f, 0.1f}, 0.01f, 0.01f},
    {3, "Rock", {2600.0f, 0.5f, 0.1f}, 0.01f, 0.01f},
    {4, "Flesh", {800.0f, 0.7f, 0.1f}, 0.01f, 0.01f},
    {5, "Bone", {1050.0f, 0.6f, 0.1f}, 0.01f, 0.01f},
    {6, "LightArmor", {950.0f, 0.6f, 0.1f}, 0.01f, 0.01f},
    {7, "Sand", {1800.0f, 0.5f, 0.05f}, 0.1f, 0.1f},
    {8, "Dirt", {1600.0f, 0.4f, 0.1f}, 0.01f, 0.01f},
    {9, "Water", {1000.0f, 0.0f, 0.5f}, 0.0f, 0.0f},
    {10, "Lava", {2000.0f, 0.2f, 0.0f}, 0.1f, 0.1f},
    {11, "Creep", {1200.0f, 0.8f, 0.1f}, 0.1f, 0.1f},
    {12, "Plastic", {800.0f, 0.2f, 0.3f}, 0.01f, 0.01f},
    {13, "Rubber", {900.0f, 0.8f, 0.5f}, 0.2f, 0.2f},
    {14, "Wood", {600.0f, 0.2f, 0.2f}, 0.2f, 0.2f},
    {15, "Snow", {500.0f, 0.2f, 0.0f}, 0.2f, 0.0f},
    {16, "Ice", {800.0f, 0.01f, 0.0f}, 0.0f, 0.0f},
    {17, "Paper", {200.0f, 0.5f, 0.0f}, 0.5f, 0.3f},
    {18, "Cloth Heavy", {800.0f, 0.5f, 0.0f}, 0.3f, 0.2f},
    {19, "Cloth Light", {400.0f, 0.5f, 0.0f}, 0.4f, 0.25f},
    {20, "Hair", {300.0f, 0.4f, 0.0f}, 0.4f, 0.3f},
    {21, "EnergyShield", {1000.0f, 0.2f, 0.5f}, 0.01f, 0.01f},
}};

bool Near(f32 a, f32 b) {
    return std::abs(a - b) <= 1e-4f * std::max(1.0f, std::abs(b));
}

} // namespace

std::span<const PhysicsMaterialPreset> Sc2PhysicsMaterials() {
    return kPresets;
}

std::optional<u32> MatchSc2Preset(const PhysicsBody& body) {
    if (body.shapes.empty())
        return std::nullopt;
    const PhysicsMaterial& first = body.shapes.front().material;
    for (const PhysicsShape& shape : body.shapes)
        if (!Near(shape.material.density, first.density) || !Near(shape.material.friction, first.friction) ||
            !Near(shape.material.restitution, first.restitution))
            return std::nullopt;
    for (const PhysicsMaterialPreset& preset : kPresets)
        if (Near(first.density, preset.material.density) && Near(first.friction, preset.material.friction) &&
            Near(first.restitution, preset.material.restitution) && Near(body.linearDamping, preset.linearDamping) &&
            Near(body.angularDamping, preset.angularDamping))
            return preset.id;
    return std::nullopt;
}

} // namespace wem
} // namespace models
} // namespace whiteout
