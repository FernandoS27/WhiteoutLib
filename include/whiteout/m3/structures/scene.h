// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file scene.h
 * @brief Scene structures — lights and cameras
 *
 * Defines the M3 light (LITE) and camera (CAM_) chunk types with animated
 * color, intensity, attenuation, field of view, depth of field, and
 * version-dependent bokeh parameters.
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §14.1 Lights, §14.2 Cameras
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Lights, Cameras
// ============================================================================

/**
 * @brief LITE — Light source (v0–v7, 212 bytes)
 *
 * Omni, spot, or directional light with animated diffuse/specular colors,
 * intensity, decay, attenuation start/end, and spot-light hot-spot/falloff.
 */
struct Light {
    static constexpr u32 tag = TAG_LITE;  ///< FourCC tag
    static constexpr u32 max_version = 7; ///< Latest known LITE version
    LightType lightType;                  ///< Light type (omni/spot/directional)
    u16 boneIndex;                        ///< Index into BONE array
    LightFlag flags = LightFlag::None;    ///< Light flags (shadows, specular, AO, etc.)
    u32 lodCut;                           ///< LOD cut-off level
    u32 shadowLodCut;                     ///< Shadow LOD cut-off level
    AnimRef<Vector3f> diffuseColor;       ///< Animated diffuse color (RGB)
    AnimRef<f32> intensityMultiplier;     ///< Animated intensity multiplier
    AnimRef<Vector3f> specularColor;      ///< Animated specular color (RGB)
    AnimRef<f32> specularMultiplier;      ///< Animated specular multiplier
    AnimRef<f32> decay;                   ///< Animated distance decay exponent
    f32 attenuationEnd;                   ///< Attenuation end distance
    AnimRef<f32> attenuationStart;        ///< Animated attenuation start distance
    AnimRef<f32> hotSpot;                 ///< Animated spot inner cone angle
    AnimRef<f32> falloff;                 ///< Animated spot outer cone falloff
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief CAM_ — Camera (v2–v5, 144–264 bytes)
 *
 * Bone-attached camera with animated FOV, clip planes, shadow clip distance,
 * depth-of-field parameters, and version-dependent bokeh settings.
 */
struct Camera {
    static constexpr u32 tag = TAG_CAM;   ///< FourCC tag
    static constexpr u32 max_version = 5; ///< Latest known CAM_ version
    u32 boneIndex;                        ///< Index into BONE array
    std::string name;                     ///< Camera name (Ref<CHAR>)
    AnimRef<f32> fieldOfView;             ///< Animated FOV in radians (v2+)
    u32 useVerticalFOV;                   ///< Use vertical FOV (0 or 1, v2+)
    u32 dofType;                          ///< DOF type (v5 only, default 3)
    AnimRef<f32> farClip;                 ///< Animated far clip plane (v3+)
    AnimRef<f32> nearClip;                ///< Animated near clip plane (v3+)
    AnimRef<f32> shadowClipDistance;      ///< Animated shadow clip distance (v2+)
    AnimRef<f32> focusDistance;           ///< Animated DOF focal point distance (v2+)
    AnimRef<f32> farFocusRange;           ///< Animated DOF far focus range (v2+)
    AnimRef<f32> nearFocusRange;          ///< Animated DOF near focus range (v2+)
    AnimRef<f32> nearFalloffStart;        ///< Animated near falloff start (v4+)
    AnimRef<f32> nearFalloffEnd;          ///< Animated near falloff end (v4+)
    AnimRef<f32> dofAmount;               ///< Animated DOF strength (v2+)
    AnimRef<f32> bokehFStop;              ///< Animated bokeh f-stop (v5+)
    AnimRef<f32> bokehMaxCoCDiameter;     ///< Animated bokeh max CoC diameter (v5+)
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout
