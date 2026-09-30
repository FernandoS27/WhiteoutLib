// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file fields.h
 * @brief Force fields and vertex warps (WEM_PHYSICS_DESIGN.md §3.8).
 *
 * Placed, animated objects like lights and emitters, so each is a node kind
 * whose payload is the record whole. StarCraft II and Heroes carry both
 * (`kSc2NodeKinds`); no other game ships either.
 *
 * A payload keeps StarCraft II's axis naming, as the StarCraft II emitter
 * payloads do: the node is rebased, and the sizes are read as the game reads
 * them in its bone's frame. The animated properties are `EmitterProperty`
 * channels on the node, `sub` naming the property below
 * (`FindEmitterProperty`); the payload field is the rest value.
 */

#include <whiteout/common_types.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace wem {

/// Appended, never reordered.
enum class ForceKind : u8 { Directional, Radial, Drag, Vortex, Count };

/// Appended, never reordered.
enum class ForceVolume : u8 { Sphere, Cylinder, Box, Hemisphere, Cone, Count };

/// A force field's animatable properties, in `EmitterProperty` sub order.
enum class ForceFieldProperty : u32 { Strength, Width, Height, Length, Count };

/// A vertex warp's animatable properties, in `EmitterProperty` sub order.
enum class VertexWarpProperty : u32 { Radius, Height, Strength, Angular, Axial, Radial, Count };

/// StarCraft II `FOR_`: pushes what is inside a volume. Particles and ribbons
/// feel it when `affectsParticles` is set, rigid bodies when `affectsBodies`
/// is and `channels` shares a bit with their `PhysicsBody::forceChannels`.
/// Cloth has no force-field path.
struct ForceFieldPayload {
    ForceKind kind = ForceKind::Radial;
    ForceVolume volume = ForceVolume::Sphere;
    bool falloff = false;
    bool heightGradient = false;
    bool unbounded = false;
    bool affectsParticles = true;
    bool affectsBodies = true;
    u32 channels = 0;
    /// `FOR_` +8, read as the field's scope (0 = the model's own); no reader
    /// is traced yet, so it crosses verbatim.
    u32 scope = 0;
    f32 strength = 0.0f;
    f32 width = 0.0f;
    f32 height = 0.0f;
    f32 length = 0.0f;
    /// Own frame -> the node's, as a shape's is: a Warcraft III node cannot turn
    /// at rest, so the field's axis lives here. `FOR_` has none, so the
    /// StarCraft II export gives a turned field a bone of its own. `NODE` v15.
    /// @bind skip — no value binding has a `Matrix44f` shape (`Node::poseMatrices`).
    Matrix44f transform = Matrix44f::identity();

    template <class V>
    void reflect(V& v) {
        v.field("kind", kind);
        v.field("volume", volume);
        v.field("falloff", falloff);
        v.field("heightGradient", heightGradient);
        v.field("unbounded", unbounded);
        v.field("affectsParticles", affectsParticles);
        v.field("affectsBodies", affectsBodies);
        v.field("channels", channels);
        v.field("scope", scope);
        v.field("strength", strength);
        v.field("width", width);
        v.field("height", height);
        v.field("length", length);
        v.since(15).field("transform", transform);
    }
};

/// StarCraft II `WRP_`: a vertex-shader deformation particles and ribbons opt
/// into. An effect, not physics.
struct VertexWarpPayload {
    u32 type = 0;
    /// `WRP_` +8; no reader is traced yet, so it crosses verbatim.
    u32 reserved = 0;
    f32 radius = 0.0f;
    f32 height = 0.0f;
    f32 strength = 0.0f;
    f32 angular = 0.0f;
    f32 axial = 0.0f;
    f32 radial = 0.0f;

    template <class V>
    void reflect(V& v) {
        v.field("type", type);
        v.field("reserved", reserved);
        v.field("radius", radius);
        v.field("height", height);
        v.field("strength", strength);
        v.field("angular", angular);
        v.field("axial", axial);
        v.field("radial", radial);
    }
};

} // namespace wem
} // namespace models
} // namespace whiteout
