// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file gltf.h
 * @brief `gltf::Asset` — glTF 2.0, decoded (GLTF_DESIGN §10).
 *
 * The house pattern: a native in-memory struct with its own `Parser`/`Writer`,
 * and a WEM converter that maps struct ⇄ `Document` — the same split as
 * `mdx::Model`/`MdxConverter`. This header is the struct: the JSON body's
 * arrays, index-linked exactly as the spec links them, plus the buffer bytes
 * once a chunk or resolver supplied them. Nothing in here interprets geometry;
 * accessor decode lives on `Parser`, basis changes live in the converter.
 *
 * Everything the two shipped phases do not read yet still parses and survives
 * (`extensionsUsed` carries unknown extension names through a diagnostic, not
 * a failure); what the struct has no field for is dropped by the parser with a
 * warning, never silently.
 */

#include <optional>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/compatibility.h>
#include <whiteout/vector_types.h>

namespace whiteout {
namespace models {
namespace gltf {

/// "No index" on every optional cross-reference — glTF spells absence by
/// omitting the member, and 2^32-1 is not a legal index in any array.
inline constexpr u32 kNone = 0xFFFFFFFFu;

// ============================================================================
// Buffers, buffer views, accessors
// ============================================================================

struct Buffer {
    std::string uri; ///< Empty for the GLB BIN chunk (buffer 0 only).
    u32 byteLength = 0;
    /// The bytes, once known: the BIN chunk, a decoded data: URI, or whatever
    /// the parse-time resolver supplied. Empty when the URI never resolved.
    std::vector<u8> data;
    std::string name;
};

/// GL enums, verbatim — the values are the wire format.
enum class BufferTarget : u32 {
    None = 0,
    ArrayBuffer = 34962,
    ElementArrayBuffer = 34963,
};

struct BufferView {
    u32 buffer = kNone;
    u32 byteOffset = 0;
    u32 byteLength = 0;
    u32 byteStride = 0; ///< 0 = tightly packed.
    BufferTarget target = BufferTarget::None;
    std::string name;
};

enum class ComponentType : u32 {
    None = 0,
    I8 = 5120,
    U8 = 5121,
    I16 = 5122,
    U16 = 5123,
    U32 = 5125,
    F32 = 5126,
};

enum class AccessorType : u8 { Scalar, Vec2, Vec3, Vec4, Mat2, Mat3, Mat4, Count };

/// Bytes of one component — 1 for I8/U8, 2 for I16/U16, 4 for U32/F32.
u32 ComponentSize(ComponentType type);
/// Components of one element — 3 for Vec3, 16 for Mat4.
u32 TypeComponentCount(AccessorType type);
/// "SCALAR", "VEC3", … — the wire spelling.
const char* ToString(AccessorType type);
/// Inverse of `ToString`; `Count` when nothing matches.
AccessorType AccessorTypeFromName(const std::string& name);

struct AccessorSparse {
    u32 count = 0;
    u32 indicesBufferView = kNone;
    u32 indicesByteOffset = 0;
    ComponentType indicesComponentType = ComponentType::None;
    u32 valuesBufferView = kNone;
    u32 valuesByteOffset = 0;
};

struct Accessor {
    u32 bufferView = kNone; ///< `kNone` = all zeros (or sparse-only).
    u32 byteOffset = 0;
    ComponentType componentType = ComponentType::None;
    bool normalized = false;
    u32 count = 0;
    AccessorType type = AccessorType::Scalar;
    std::vector<f64> min; ///< Sized to the component count when present.
    std::vector<f64> max;
    std::optional<AccessorSparse> sparse;
    std::string name;
};

// ============================================================================
// Images, samplers, textures, materials
// ============================================================================

struct Image {
    std::string uri; ///< As written; empty when the image is a buffer view.
    std::string mimeType;
    u32 bufferView = kNone;
    /// Decoded-from-container bytes (buffer view or data: URI); still encoded
    /// PNG/JPEG — pixels are the texture module's business, not this struct's.
    std::vector<u8> data;
    std::string name;
};

/// GL wrap enums, verbatim.
enum class WrapMode : u32 {
    ClampToEdge = 33071,
    MirroredRepeat = 33648,
    Repeat = 10497,
};

struct Sampler {
    u32 magFilter = 0; ///< 0 = unstated; filter choice is the renderer's.
    u32 minFilter = 0;
    WrapMode wrapS = WrapMode::Repeat;
    WrapMode wrapT = WrapMode::Repeat;
    std::string name;
};

struct Texture {
    u32 sampler = kNone;
    u32 source = kNone; ///< -> `images`.
    std::string name;
};

/// KHR_texture_transform, decomposed exactly as the extension states it.
struct TextureTransform {
    Vector2f offset{0, 0};
    f32 rotation = 0; ///< Radians, counter-clockwise about the origin.
    Vector2f scale{1, 1};
    u32 texCoord = kNone; ///< Overrides the info's `texCoord` when set.
};

struct TextureInfo {
    u32 index = kNone; ///< -> `textures`; `kNone` = the member was absent.
    u32 texCoord = 0;
    f32 scale = 1;    ///< normalTexture only.
    f32 strength = 1; ///< occlusionTexture only.
    std::optional<TextureTransform> transform;

    bool present() const {
        return index != kNone;
    }
};

enum class AlphaMode : u8 { Opaque, Mask, Blend };

const char* ToString(AlphaMode mode); ///< "OPAQUE" / "MASK" / "BLEND".

struct PbrMetallicRoughness {
    Vector4f baseColorFactor{1, 1, 1, 1};
    TextureInfo baseColorTexture;
    f32 metallicFactor = 1;
    f32 roughnessFactor = 1;
    TextureInfo metallicRoughnessTexture; ///< G = roughness, B = metalness.
};

struct Material {
    std::string name;
    PbrMetallicRoughness pbr;
    TextureInfo normalTexture;
    TextureInfo occlusionTexture;
    TextureInfo emissiveTexture;
    Vector3f emissiveFactor{0, 0, 0};
    AlphaMode alphaMode = AlphaMode::Opaque;
    f32 alphaCutoff = 0.5f;
    bool doubleSided = false;
    bool unlit = false;          ///< KHR_materials_unlit.
    f32 emissiveStrength = 1.0f; ///< KHR_materials_emissive_strength.
};

// ============================================================================
// Meshes
// ============================================================================

/// One named vertex stream — "POSITION", "TEXCOORD_2" — and its accessor.
/// A vector of pairs rather than a map so the writer's output order is the
/// insertion order and therefore deterministic.
struct AttributeBinding {
    std::string semantic;
    u32 accessor = kNone;
};

enum class PrimitiveMode : u32 {
    Points = 0,
    Lines = 1,
    LineLoop = 2,
    LineStrip = 3,
    Triangles = 4,
    TriangleStrip = 5,
    TriangleFan = 6,
};

struct Primitive {
    std::vector<AttributeBinding> attributes;
    u32 indices = kNone; ///< `kNone` = non-indexed.
    u32 material = kNone;
    PrimitiveMode mode = PrimitiveMode::Triangles;

    u32 attribute(const std::string& semantic) const; ///< `kNone` when absent.
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
    /// Morph-target weights, kept so the parser can say how many it dropped.
    std::vector<f32> weights;
};

// ============================================================================
// Nodes, scenes, skins
// ============================================================================

struct Node {
    std::string name;
    std::vector<u32> children;
    u32 mesh = kNone;
    u32 skin = kNone;
    u32 camera = kNone;
    u32 light = kNone; ///< KHR_lights_punctual.

    /// A node states its transform as TRS or as one matrix, never both; the
    /// flag says which the file did, because "identity TRS" and "no transform
    /// at all" round-trip differently.
    bool hasMatrix = false;
    Matrix44f matrix = Matrix44f::identity(); ///< Column-major on the wire.
    Vector3f translation{0, 0, 0};
    Quaternion rotation{0, 0, 0, 1};
    Vector3f scale{1, 1, 1};
};

struct Scene {
    std::string name;
    std::vector<u32> nodes; ///< Roots.
};

struct Skin {
    std::string name;
    u32 inverseBindMatrices = kNone; ///< Accessor of Mat4; absent = identities.
    u32 skeleton = kNone;
    std::vector<u32> joints;
};

// ============================================================================
// Animation
// ============================================================================

enum class AnimInterpolation : u8 { Linear, Step, CubicSpline };

const char* ToString(AnimInterpolation interp); ///< Wire spelling.

enum class AnimPath : u8 { Translation, Rotation, Scale, Weights, Count };

const char* ToString(AnimPath path); ///< "translation" / … / "weights".

struct AnimationSampler {
    u32 input = kNone;  ///< Accessor of key times, seconds.
    u32 output = kNone; ///< Accessor of values.
    AnimInterpolation interpolation = AnimInterpolation::Linear;
};

struct AnimationChannel {
    u32 sampler = kNone;
    u32 targetNode = kNone;
    AnimPath targetPath = AnimPath::Translation;
};

struct Animation {
    std::string name;
    std::vector<AnimationSampler> samplers;
    std::vector<AnimationChannel> channels;
};

// ============================================================================
// Cameras and lights
// ============================================================================

struct Camera {
    std::string name;
    bool perspective = true; ///< False = orthographic, carried but unread.
    f32 yfov = 0;            ///< Radians, vertical.
    f32 aspectRatio = 0;     ///< 0 = unstated.
    f32 znear = 0;
    f32 zfar = 0; ///< 0 = infinite projection.
};

enum class LightKind : u8 { Directional, Point, Spot };

/// KHR_lights_punctual.
struct Light {
    std::string name;
    LightKind kind = LightKind::Point;
    Vector3f color{1, 1, 1};
    f32 intensity = 1;
    f32 range = 0; ///< 0 = unlimited.
    f32 innerConeAngle = 0;
    f32 outerConeAngle = 0.7853981633974483f; ///< The spec's default, pi/4.
};

// ============================================================================
// Asset
// ============================================================================

struct AssetInfo {
    std::string version = "2.0";
    std::string minVersion;
    std::string generator;
    std::string copyright;
};

struct Asset {
    AssetInfo asset;
    u32 scene = kNone;

    std::vector<Scene> scenes;
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<Accessor> accessors;
    std::vector<BufferView> bufferViews;
    std::vector<Buffer> buffers;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    std::vector<Image> images;
    std::vector<Skin> skins;
    std::vector<Animation> animations;
    std::vector<Camera> cameras;
    std::vector<Light> lights; ///< KHR_lights_punctual's own array.

    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
};

} // namespace gltf
} // namespace models
} // namespace whiteout
