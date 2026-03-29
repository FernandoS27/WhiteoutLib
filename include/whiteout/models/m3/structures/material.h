// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file material.h
 * @brief Material structures — standard, displacement, composite, terrain, volume, and more
 *
 * Defines all M3 material chunk types: MaterialMap (MATM) for type+index
 * dispatch, TextureLayer (LAYR) for animated texture references, and the
 * full set of material types: StandardMaterial (MAT_), DisplacementMaterial
 * (DIS_), CompositeMaterial (CMP_), TerrainMaterial (TER_), VolumeMaterial
 * (VOL_), HairMaterial (HAI_, defunct), VolumeNoiseMaterial (VON_),
 * CreepMaterial (CREP), STBMaterial (STBM), ReflectionMaterial (REF_),
 * LensFlare (LFLR), and MaterialAddData (MADD).
 *
 * @see M3_FILE_FORMAT_SPECIFICATION.md §11 Materials
 */

#include "base.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Materials
// ============================================================================

/**
 * @brief MATM — Material map entry (v0, 8 bytes)
 *
 * Maps a material type enum to an index into the corresponding material array.
 * The MODL root references an array of these; the renderer uses materialType
 * to dispatch to the correct material vector.
 */
struct MaterialMap {
    MaterialType materialType; ///< Material type (1=standard, 2=displacement, etc.)
    u32 materialIndex;         ///< Index into the typed material array
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief LAYR — Texture layer (v0–v26, 352–464 bytes)
 *
 * A single texture binding with animated color tint, UV transforms, flipbook
 * parameters, fresnel settings, and AVI video playback controls. Materials
 * embed multiple optional TextureLayer instances for diffuse, specular,
 * emissive, normal, and other texture slots.
 */
struct TextureLayer {
    u32 id;                                          ///< Layer identifier
    std::string texturePath;                         ///< Texture file path (Ref<CHAR>)
    AnimRef<ColorBGRA> color;                        ///< Animated color tint
    TextureLayerFlag flags = TextureLayerFlag::None; ///< Layer flags (wrap, flipbook, video, etc.)
    UVMappingMode uvMapping = UVMappingMode::ExplicitUV0;   ///< UV mapping source
    ColorChannelSelect colorType = ColorChannelSelect::RGB; ///< Channel selection
    AnimRef<f32> rgbMultiply;                               ///< RGB multiply factor
    AnimRef<f32> rgbAdd;                                    ///< RGB additive factor
    u32 pocTexture;                                         ///< POC texture reference
    f32 noiseAmplitude;                                     ///< Noise amplitude (v24+)
    f32 noiseFrequency;                                     ///< Noise frequency (v24+)
    u32 textureSource;                                      ///< Texture source override
    u32 aviFrameRate;                                       ///< AVI playback frame rate
    u32 aviStart;                                           ///< AVI start frame
    u32 aviStop;                                            ///< AVI stop frame
    u32 aviLoop;                                            ///< AVI loop mode
    u32 aviSync;                                            ///< AVI sync mode
    AnimRef<u32> aviPlay;                                   ///< AVI play control
    AnimRef<u32> aviRestart;                                ///< AVI restart control
    u32 flipbookRows;                                       ///< Flipbook grid rows
    u32 flipbookColumns;                                    ///< Flipbook grid columns
    AnimRef<u16> currentFrame;                              ///< Animated flipbook frame index
    AnimRef<Vector2f> uvOffset;                             ///< Animated UV offset
    AnimRef<Vector3f> uvAngle;                              ///< Animated UV rotation angles
    AnimRef<Vector2f> uvTiling;                             ///< Animated UV tiling
    AnimRef<f32> wOffset;                                   ///< Animated W offset (3D textures)
    AnimRef<f32> wTiling;                                   ///< Animated W tiling (3D textures)
    AnimRef<f32> mapAlpha;                                  ///< Animated map alpha
    AnimRef<Vector3f> triplanarOffset;                      ///< Tri-planar UV offset (v23+)
    AnimRef<Vector3f> triplanarScale;                       ///< Tri-planar UV scale (v23+)
    u32 uvSourceRelated;                                    ///< UV source related field
    FresnelMode fresnelMode = FresnelMode::None;            ///< Fresnel effect mode
    f32 fresnelExponent;                                    ///< Fresnel exponent (edge sharpness)
    f32 fresnelMin;                                         ///< Fresnel minimum intensity
    f32 fresnelMax;                                         ///< Fresnel maximum intensity
    Vector3f fresnelTranslation;                            ///< Fresnel UV translation (v25+)
    Vector3f fresnelMask;                                   ///< Fresnel mask vector (v25+)
    Vector2f fresnelRotation;                               ///< Fresnel UV rotation (v25+)
    u32 uvDensity; ///< UV density hint (v0–v25, absent in v26)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief MAT_ — Standard material (v0–v20, 268–352 bytes)
 *
 * The primary material type with up to 18 texture layers (diffuse, specular,
 * emissive, normal, height, etc.), blend mode, HDR multipliers, and
 * per-version extensions for normal-blend and gloss layers.
 */
struct StandardMaterial {
    std::string name; ///< Material name (Ref<CHAR>)
    MaterialAdditionalFlag additionalFlags = MaterialAdditionalFlag::None; ///< Additional flags
    MaterialFlag flags = MaterialFlag::None; ///< Material rendering flags
    BlendMode blendMode = BlendMode::Opaque; ///< Alpha blend mode
    i32 priority;                            ///< Render priority (lower = earlier)
    u32 rttChannels;                         ///< RTT channel mask
    f32 specularExponent;                    ///< Specular highlight exponent
    f32 depthBlendFalloff;                   ///< Depth blend falloff distance
    u32 alphaTestThreshold;                  ///< Alpha test cut-off value
    f32 hdrSpecularMultiplier;               ///< HDR specular multiplier
    f32 hdrEmissiveMultiplier;               ///< HDR emissive multiplier
    f32 hdrEnvironmentConstant;              ///< HDR environment constant (v20)
    f32 hdrEnvironmentDiffuse;               ///< HDR environment diffuse (v20)
    f32 hdrEnvironmentSpecular;              ///< HDR environment specular (v20)
    // Texture layers (13-18 depending on version)
    std::optional<TextureLayer> diffuseLayer;            ///< Diffuse / albedo texture
    std::optional<TextureLayer> decalLayer;              ///< Decal overlay texture
    std::optional<TextureLayer> specularLayer;           ///< Specular map texture
    std::optional<TextureLayer> glossLayer;              ///< Gloss map texture (v16+)
    std::optional<TextureLayer> emissiveLayer1;          ///< Emissive layer 1
    std::optional<TextureLayer> emissiveLayer2;          ///< Emissive layer 2
    std::optional<TextureLayer> environmentLayer;        ///< Environment reflection map
    std::optional<TextureLayer> environmentMaskLayer;    ///< Environment mask
    std::optional<TextureLayer> alphaLayer1;             ///< Alpha mask layer 1
    std::optional<TextureLayer> alphaLayer2;             ///< Alpha mask layer 2
    std::optional<TextureLayer> normalLayer;             ///< Normal / bump map
    std::optional<TextureLayer> heightLayer;             ///< Height / parallax map
    std::optional<TextureLayer> lightMapLayer;           ///< Light map
    std::optional<TextureLayer> ambientOcclusionLayer;   ///< Ambient occlusion map
    std::optional<TextureLayer> normalBlend1MaskLayer;   ///< Normal blend 1 mask (v19+)
    std::optional<TextureLayer> normalBlend2MaskLayer;   ///< Normal blend 2 mask (v19+)
    std::optional<TextureLayer> normalBlend1Layer;       ///< Normal blend 1 map (v19+)
    std::optional<TextureLayer> normalBlend2Layer;       ///< Normal blend 2 map (v19+)
    MaterialClass materialClass = MaterialClass::Unit;   ///< Material class (unit, building, etc.)
    LayerBlendOp layerBlendMode = LayerBlendOp::Mod;     ///< Layer blend operation
    LayerBlendOp emissiveBlendMode1 = LayerBlendOp::Mod; ///< Emissive layer 1 blend mode
    LayerBlendOp emissiveBlendMode2 = LayerBlendOp::Mod; ///< Emissive layer 2 blend mode
    SpecularMode specularMode = SpecularMode::RGB;       ///< Specular computation mode
    AnimRef<f32> parallaxHeight;                         ///< Animated parallax height
    AnimRef<f32> motionBlurAmount;                       ///< Animated motion blur amount
    std::vector<AnimRef<f32>> normalBlendFactors;        ///< Normal blend factors (v19+)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief DIS_ — Displacement material (v0–v4, 68 bytes)
 *
 * Applies vertex displacement via a normal map and animated strength.
 */
struct DisplacementMaterial {
    std::string name;                        ///< Material name (Ref<CHAR>)
    u32 unknown;                             ///< Unknown field
    AnimRef<f32> strength;                   ///< Animated displacement strength
    std::optional<TextureLayer> normalMap;   ///< Normal / displacement direction map
    std::optional<TextureLayer> strengthMap; ///< Strength mask texture
    Flag flags;                              ///< Displacement material flags
    u32 priority;                            ///< Render priority
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief CMS_ — Composite material section (v0, 24 bytes)
 *
 * A single section within a composite material, referencing another material
 * index with an animated blend multiplier.
 */
struct CompositeSection {
    u32 materialIndex;          ///< Index into MATM array
    AnimRef<f32> mapMultiplier; ///< Animated blend weight
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief CMP_ — Composite material (v0–v2, 28 bytes)
 *
 * Blends multiple sub-materials via CompositeSection entries.
 */
struct CompositeMaterial {
    std::string name;                       ///< Material name (Ref<CHAR>)
    u32 priority;                           ///< Render priority
    std::vector<CompositeSection> sections; ///< Sub-material sections (CMS_)
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief TER_ — Terrain material (v0–v1, 28 bytes)
 *
 * Simple terrain-specific material with a single texture layer.
 */
struct TerrainMaterial {
    std::string name;                       ///< Material name (Ref<CHAR>)
    std::optional<TextureLayer> terrainMap; ///< Terrain texture layer
    u32 unknown;                            ///< Unknown field
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief VOL_ — Volume material (v0, 84 bytes)
 *
 * Volumetric rendering material with density falloff, color map, and
 * two noise maps for procedural volumetric effects.
 */
struct VolumeMaterial {
    std::string name;                      ///< Material name (Ref<CHAR>)
    u32 blendMode;                         ///< Blend mode
    VolumeFalloffType falloffType;         ///< Density falloff type
    AnimRef<f32> density;                  ///< Animated density
    std::optional<TextureLayer> colorMap;  ///< Color map texture
    std::optional<TextureLayer> noiseMap1; ///< Noise map 1
    std::optional<TextureLayer> noiseMap2; ///< Noise map 2
    u32 alphaThreshold;                    ///< Alpha test threshold
    Flag flags;                            ///< Volume material flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief HAI_ — Hair material (defunct, v0, 116 bytes)
 *
 * Anisotropic hair rendering material with specular shift and AO.
 * Always null in observed corpus data.
 */
struct HairMaterial {
    std::string name;                           ///< Material name (Ref<CHAR>)
    std::optional<TextureLayer> layerBase;      ///< Base color / diffuse texture
    std::optional<TextureLayer> layerSpecShift; ///< Anisotropic specular shift map
    std::optional<TextureLayer> layerSpecNoise; ///< Specular noise / break-up map
    std::optional<TextureLayer> layerAO;        ///< Ambient occlusion map
    f32 shiftPrimary;                           ///< Primary specular shift
    f32 shiftSecondary;                         ///< Secondary specular shift
    AnimRef<ColorBGRA> colorDiffuse;            ///< Animated diffuse tint
    AnimRef<ColorBGRA> colorSpec;               ///< Animated specular tint
    f32 specExponent0;                          ///< Primary specular exponent
    f32 specExponent1;                          ///< Secondary specular exponent
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief VON_ — Volume noise material (v0, 268 bytes)
 *
 * Volumetric noise-based rendering material with animated density, falloff,
 * scroll rate, position, scale, and rotation. Used for gas/smoke/cloud effects.
 */
struct VolumeNoiseMaterial {
    std::string name;                       ///< Material name (Ref<CHAR>)
    VolumeFalloffType falloffType;          ///< Density falloff type
    VolumeNoiseCameraMode drawTransparency; ///< Camera position mode (inside/outside)
    AnimRef<f32> density;                   ///< Animated density
    AnimRef<f32> nearPlane;                 ///< Animated near-plane clip
    AnimRef<f32> falloff;                   ///< Animated falloff distance
    std::optional<TextureLayer> colorMap;   ///< Color map texture
    std::optional<TextureLayer> noiseMap1;  ///< Noise map 1
    std::optional<TextureLayer> noiseMap2;  ///< Noise map 2
    AnimRef<Vector3f> scrollRate;           ///< Animated noise scroll rate
    AnimRef<Vector3f> position;             ///< Animated volume position
    AnimRef<Vector3f> scale;                ///< Animated volume scale
    AnimRef<Vector3f> rotation;             ///< Animated volume rotation
    u32 alphaThreshold;                     ///< Alpha test threshold
    VolumeNoiseMaterialFlag flags;          ///< Volume noise material flags
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief CREP — Creep material (v0–v1, 28 bytes)
 *
 * Material for Zerg creep rendering with a mask map and creep-low parameter.
 */
struct CreepMaterial {
    std::string name;                    ///< Material name (Ref<CHAR>)
    std::optional<TextureLayer> maskMap; ///< Creep mask texture
    u32 creepLow;                        ///< Creep low parameter
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief STBM — Splat terrain bake material (v0, 48 bytes)
 *
 * Material for baked terrain splat rendering with diffuse, normal, and
 * specular texture layers.
 */
struct STBMaterial {
    std::string name;                        ///< Material name (Ref<CHAR>)
    std::optional<TextureLayer> diffuseMap;  ///< Diffuse / albedo map
    std::optional<TextureLayer> normalMap;   ///< Normal map
    std::optional<TextureLayer> specularMap; ///< Specular map
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief REF_ — Reflection material (v0–v3, 84–160 bytes)
 *
 * Planar or cube-map reflection material with animated reflection/displacement
 * strength, blur, and multiple texture layers.
 */
struct ReflectionMaterial {
    std::string name;                            ///< Material name (Ref<CHAR>)
    u32 unknown;                                 ///< Unknown field
    AnimRef<f32> reflectionStrength;             ///< Animated reflection strength (v2+)
    AnimRef<f32> displacementStrength;           ///< Animated displacement strength (v2+)
    AnimRef<f32> reflectionOffset;               ///< Animated reflection offset (v2+)
    AnimRef<f32> blurAngle;                      ///< Animated blur angle (v2+)
    AnimRef<f32> blurDistanceMax;                ///< Animated max blur distance (v2+)
    std::optional<TextureLayer> reflectionMap;   ///< Reflection map texture
    std::optional<TextureLayer> displacementMap; ///< Displacement map texture
    std::optional<TextureLayer> blurMap;         ///< Blur map texture
    ReflectionMaterialFlag flags;                ///< Reflection flags (v2+)
    u32 unknown2;                                ///< Unknown field
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief LFSB — Sub-flare element (v0–v2, 56 bytes)
 *
 * A single flare element within a LensFlare material, with position,
 * size, scale, fade, color, and offset parameters.
 */
struct SubFlare {
    u32 index;            ///< Flare element index
    f32 position;         ///< Position along the flare axis (0–1)
    Vector2f sizeXY;      ///< Base size (width, height)
    Vector2f scaleXY;     ///< Scale multiplier (width, height)
    Vector2f fadeIn;      ///< Fade-in range (start, end)
    Vector2f fadeOut;     ///< Fade-out range (start, end)
    ColorBGRA colorAlpha; ///< Flare color and alpha
    u32 faceCenter;       ///< Whether to face the flare center
    Vector2f offset;      ///< Offset from flare center
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief LFLR — Lens flare material (v0–v3, 152 bytes)
 *
 * Lens flare effect with animated intensity, color, HDR, size, sub-flare
 * elements, and flipbook texture grid parameters.
 */
struct LensFlare {
    std::string name;                     ///< Flare name (Ref<CHAR>)
    std::optional<TextureLayer> flareMap; ///< Flare texture atlas
    std::optional<TextureLayer> maskMap;  ///< Flare mask texture
    std::vector<SubFlare> subFlares;      ///< Sub-flare elements (LFSB)
    u32 columns;                          ///< Flipbook grid columns
    u32 rows;                             ///< Flipbook grid rows
    f32 distanceFade;                     ///< Distance fade start
    std::string libName;                  ///< Library name (Ref<CHAR>)
    AnimRef<f32> intensity;               ///< Animated intensity
    AnimRef<ColorBGRA> color;             ///< Animated color
    AnimRef<f32> hdr;                     ///< Animated HDR multiplier
    AnimRef<f32> size;                    ///< Animated size
    M3_DEFINE_VERSION_ACCESSORS()
};

/**
 * @brief MADD — Material additional data (v0–v3, 140–160 bytes)
 *
 * Buffer-style material extension storing key–value pairs, hashes, and
 * animation parameters. Added in MODL v30.
 */
struct MaterialAddData {
    std::string keyName;                ///< Key name (Ref<CHAR>)
    std::vector<u32> keyHash;           ///< Key hash values (U32_)
    std::vector<u32> extraHash;         ///< Extra hash values (U32_, v2+)
    std::string valuePath;              ///< Value file path (Ref<CHAR>)
    std::vector<std::string> valueData; ///< Value data strings
    std::array<Reference, 4> reserved;  ///< Reserved references
    f32 frequency;                      ///< Animation frequency
    f32 intensity;                      ///< Effect intensity
    f32 holdTime;                       ///< Hold time duration
    u32 randomHash;                     ///< Random seed hash
    u32 animationType;                  ///< Animation type code
    u32 padding0;                       ///< Alignment padding
    i32 loopCount;                      ///< Loop count (-1 = infinite)
    u32 flags;                          ///< Flags
    u32 subType;                        ///< Sub-type identifier
    u32 configA;                        ///< Configuration parameter A
    u32 configB;                        ///< Configuration parameter B
    u32 extraId0;                       ///< Extra identifier 0 (v3+)
    u32 extraId1;                       ///< Extra identifier 1 (v3+)
    M3_DEFINE_VERSION_ACCESSORS()
};

} // namespace m3
} // namespace whiteout
