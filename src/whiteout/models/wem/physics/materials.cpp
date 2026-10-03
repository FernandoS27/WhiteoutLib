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

std::span<const ClothPreset> Sc2ClothPresets() {
    // The medians of the four clusters C0's census found in the 216 shipped
    // cages (tests/physics_cloth_census_test.cpp): Light, the soft capes
    // (Arthas, Artanis, Anduin); Medium, capes and skirts with body
    // (Alexstrasza, the Diablo III heroines); Heavy, leather and banners
    // (Butcher, Azmodan, the Mengsk buildings); Flowing, scarves the air
    // carries (Genji, Hanzo). Every shipped cage drags explosions and wind at
    // 1 and has skin offset, exponent and stiffness 1 and no skin collision.
    const auto make = [](const char* name, f32 density, f32 damping, f32 friction, f32 stretch, f32 bend,
                         f32 tracking, f32 horizontal, f32 shear, f32 drag, f32 sphere) {
        ClothPreset preset;
        preset.name = name;
        preset.density = density;
        preset.damping = damping;
        preset.friction = friction;
        preset.stretchStiffness = stretch;
        preset.bendStiffness = bend;
        preset.gravityScale = 1.0f;
        preset.sc2.tracking = tracking;
        preset.sc2.horizontalStiffness = horizontal;
        preset.sc2.shearStiffness = shear;
        preset.sc2.explosionScale = 1.0f;
        preset.sc2.windScale = 1.0f;
        preset.sc2.dragFactor = drag;
        preset.sc2.liftFactor = 1.0f;
        preset.sc2.sphereStiffness = sphere;
        preset.sc2.skinOffset = 1.0f;
        preset.sc2.skinExponent = 1.0f;
        preset.sc2.skinStiffness = 1.0f;
        return preset;
    };
    static const std::array<ClothPreset, 4> presets{
        make("light", 2.0f, 2.0f, 0.1f, 0.5f, 0.2f, 0.5f, 0.3f, 0.2f, 1.0f, 0.3f),
        make("medium", 2.0f, 2.0f, 0.2f, 0.9f, 0.5f, 0.5f, 0.6f, 0.1f, 1.0f, 0.2f),
        make("heavy", 15.0f, 10.0f, 0.5f, 0.9f, 0.9f, 0.25f, 0.9f, 1.0f, 1.0f, 1.0f),
        make("flowing", 2.0f, 1.0f, 0.3f, 0.9f, 0.25f, 0.1f, 0.9f, 0.8f, 5.0f, 0.2f),
    };
    return presets;
}

void ApplyClothPreset(Cloth& cloth, const ClothPreset& preset) {
    cloth.density = preset.density;
    cloth.damping = preset.damping;
    cloth.friction = preset.friction;
    cloth.stretchStiffness = preset.stretchStiffness;
    cloth.bendStiffness = preset.bendStiffness;
    cloth.gravityScale = preset.gravityScale;
    cloth.sc2 = preset.sc2;
}

std::optional<u32> MatchClothPreset(const Cloth& cloth) {
    const Sc2ClothParams sc2 = cloth.sc2.value_or(Sc2ClothParams{});
    const std::span<const ClothPreset> presets = Sc2ClothPresets();
    for (u32 i = 0; i < presets.size(); ++i) {
        const ClothPreset& p = presets[i];
        if (Near(cloth.density, p.density) && Near(cloth.damping, p.damping) && Near(cloth.friction, p.friction) &&
            Near(cloth.stretchStiffness, p.stretchStiffness) && Near(cloth.bendStiffness, p.bendStiffness) &&
            Near(cloth.gravityScale, p.gravityScale) && Near(sc2.tracking, p.sc2.tracking) &&
            Near(sc2.horizontalStiffness, p.sc2.horizontalStiffness) &&
            Near(sc2.shearStiffness, p.sc2.shearStiffness) && Near(sc2.dragFactor, p.sc2.dragFactor) &&
            Near(sc2.liftFactor, p.sc2.liftFactor) && Near(sc2.sphereStiffness, p.sc2.sphereStiffness))
            return i;
    }
    return std::nullopt;
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
