// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/gltf/parser.h"

#include <cmath>
#include <cstring>

#include "whiteout/models/gltf/json.h"

namespace whiteout {
namespace models {
namespace gltf {

namespace {

constexpr u32 kGlbMagic = 0x46546C67;     // "glTF"
constexpr u32 kGlbChunkJson = 0x4E4F534A; // "JSON"
constexpr u32 kGlbChunkBin = 0x004E4942;  // "BIN\0"

u32 readU32(std::span<const u8> bytes, std::size_t offset) {
    return static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8) |
           (static_cast<u32>(bytes[offset + 2]) << 16) |
           (static_cast<u32>(bytes[offset + 3]) << 24);
}

// ============================================================================
// Decode context
// ============================================================================

struct Decoder {
    const ParseOptions& options;
    Asset asset;
    std::vector<std::string> warnings;
    std::string error;

    explicit Decoder(const ParseOptions& opts) : options(opts) {}

    bool fail(std::string message) {
        if (error.empty()) {
            error = std::move(message);
        }
        return false;
    }

    void warn(std::string message) {
        warnings.push_back(std::move(message));
    }

    // --- URIs ----------------------------------------------------------------

    static bool isDataUri(const std::string& uri) {
        return uri.rfind("data:", 0) == 0;
    }

    bool decodeDataUri(const std::string& uri, std::vector<u8>& out, const char* what) {
        const std::size_t comma = uri.find(',');
        if (comma == std::string::npos) {
            warn(std::string(what) + ": malformed data: URI");
            return false;
        }
        const std::string_view head = std::string_view(uri).substr(0, comma);
        if (head.find(";base64") == std::string_view::npos) {
            warn(std::string(what) + ": non-base64 data: URI");
            return false;
        }
        if (!DecodeBase64(std::string_view(uri).substr(comma + 1), out)) {
            warn(std::string(what) + ": bad base64 payload");
            return false;
        }
        return true;
    }

    bool resolveExternal(const std::string& uri, std::vector<u8>& out) {
        if (!options.resolveUri) {
            return false;
        }
        return options.resolveUri(uri, out);
    }

    // --- sections ------------------------------------------------------------

    bool decodeAssetInfo(const json::Value& root) {
        const json::Value* info = root.find("asset");
        if (info == nullptr || !info->isObject()) {
            return fail("no asset object — not a glTF body");
        }
        asset.asset.version = info->stringOf("version");
        asset.asset.minVersion = info->stringOf("minVersion");
        asset.asset.generator = info->stringOf("generator");
        asset.asset.copyright = info->stringOf("copyright");
        if (asset.asset.version.rfind("2.", 0) != 0) {
            return fail("unsupported glTF version '" + asset.asset.version + "'");
        }
        return true;
    }

    void decodeStringArray(const json::Value& root, const char* key,
                           std::vector<std::string>& out) {
        const json::Value& array = root.arrayOf(key);
        for (std::size_t i = 0; i < array.size(); ++i) {
            out.push_back(array.at(i).asString());
        }
    }

    bool decodeBuffers(const json::Value& root, std::span<const u8> binChunk) {
        const json::Value& buffers = root.arrayOf("buffers");
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            const json::Value& entry = buffers.at(i);
            Buffer buffer;
            buffer.uri = entry.stringOf("uri");
            buffer.byteLength = entry.u32Of("byteLength");
            buffer.name = entry.stringOf("name");

            if (buffer.uri.empty()) {
                if (i == 0 && !binChunk.empty()) {
                    // The chunk's length includes its 4-byte zero padding;
                    // `byteLength` is the buffer's real size and trims it.
                    std::size_t size = binChunk.size();
                    if (buffer.byteLength != 0 && buffer.byteLength <= size) {
                        size = buffer.byteLength;
                    }
                    buffer.data.assign(binChunk.begin(), binChunk.begin() + size);
                } else {
                    warn("buffer " + std::to_string(i) + " has no URI and no BIN chunk");
                }
            } else if (isDataUri(buffer.uri)) {
                decodeDataUri(buffer.uri, buffer.data, "buffer");
            } else if (!resolveExternal(buffer.uri, buffer.data)) {
                warn("buffer URI '" + buffer.uri + "' unresolved");
            }

            if (!buffer.data.empty() && buffer.data.size() < buffer.byteLength) {
                return fail("buffer " + std::to_string(i) + " shorter than its byteLength");
            }
            asset.buffers.push_back(std::move(buffer));
        }
        return true;
    }

    void decodeBufferViews(const json::Value& root) {
        const json::Value& views = root.arrayOf("bufferViews");
        for (std::size_t i = 0; i < views.size(); ++i) {
            const json::Value& entry = views.at(i);
            BufferView view;
            view.buffer = entry.u32Of("buffer", kNone);
            view.byteOffset = entry.u32Of("byteOffset");
            view.byteLength = entry.u32Of("byteLength");
            view.byteStride = entry.u32Of("byteStride");
            view.target = static_cast<BufferTarget>(entry.u32Of("target"));
            view.name = entry.stringOf("name");
            asset.bufferViews.push_back(std::move(view));
        }
    }

    void decodeAccessors(const json::Value& root) {
        const json::Value& accessors = root.arrayOf("accessors");
        for (std::size_t i = 0; i < accessors.size(); ++i) {
            const json::Value& entry = accessors.at(i);
            Accessor accessor;
            accessor.bufferView = entry.u32Of("bufferView", kNone);
            accessor.byteOffset = entry.u32Of("byteOffset");
            accessor.componentType = static_cast<ComponentType>(entry.u32Of("componentType"));
            accessor.normalized = entry.boolOf("normalized");
            accessor.count = entry.u32Of("count");
            accessor.type = AccessorTypeFromName(entry.stringOf("type"));
            accessor.name = entry.stringOf("name");
            const json::Value& minArray = entry.arrayOf("min");
            for (std::size_t c = 0; c < minArray.size(); ++c) {
                accessor.min.push_back(minArray.at(c).asNumber());
            }
            const json::Value& maxArray = entry.arrayOf("max");
            for (std::size_t c = 0; c < maxArray.size(); ++c) {
                accessor.max.push_back(maxArray.at(c).asNumber());
            }
            if (const json::Value* sparse = entry.find("sparse");
                sparse != nullptr && sparse->isObject()) {
                AccessorSparse out;
                out.count = sparse->u32Of("count");
                if (const json::Value* indices = sparse->find("indices"); indices != nullptr) {
                    out.indicesBufferView = indices->u32Of("bufferView", kNone);
                    out.indicesByteOffset = indices->u32Of("byteOffset");
                    out.indicesComponentType =
                        static_cast<ComponentType>(indices->u32Of("componentType"));
                }
                if (const json::Value* values = sparse->find("values"); values != nullptr) {
                    out.valuesBufferView = values->u32Of("bufferView", kNone);
                    out.valuesByteOffset = values->u32Of("byteOffset");
                }
                accessor.sparse = out;
            }
            if (accessor.type == AccessorType::Count) {
                warn("accessor " + std::to_string(i) + " has an unknown type; treated as SCALAR");
                accessor.type = AccessorType::Scalar;
            }
            asset.accessors.push_back(std::move(accessor));
        }
    }

    void decodeImages(const json::Value& root) {
        const json::Value& images = root.arrayOf("images");
        for (std::size_t i = 0; i < images.size(); ++i) {
            const json::Value& entry = images.at(i);
            Image image;
            image.uri = entry.stringOf("uri");
            image.mimeType = entry.stringOf("mimeType");
            image.bufferView = entry.u32Of("bufferView", kNone);
            image.name = entry.stringOf("name");

            if (image.bufferView != kNone) {
                std::span<const u8> bytes = viewBytes(image.bufferView);
                image.data.assign(bytes.begin(), bytes.end());
                if (image.data.empty()) {
                    warn("image " + std::to_string(i) + " buffer view has no bytes");
                }
            } else if (isDataUri(image.uri)) {
                decodeDataUri(image.uri, image.data, "image");
                image.uri.clear(); // The bytes are the value; the URI was transport.
            } else if (!image.uri.empty() && resolveExternal(image.uri, image.data)) {
                // Resolved; the URI is kept as the name of where it came from.
            }
            asset.images.push_back(std::move(image));
        }
    }

    void decodeSamplers(const json::Value& root) {
        const json::Value& samplers = root.arrayOf("samplers");
        for (std::size_t i = 0; i < samplers.size(); ++i) {
            const json::Value& entry = samplers.at(i);
            Sampler sampler;
            sampler.magFilter = entry.u32Of("magFilter");
            sampler.minFilter = entry.u32Of("minFilter");
            sampler.wrapS = static_cast<WrapMode>(
                entry.u32Of("wrapS", static_cast<u32>(WrapMode::Repeat)));
            sampler.wrapT = static_cast<WrapMode>(
                entry.u32Of("wrapT", static_cast<u32>(WrapMode::Repeat)));
            sampler.name = entry.stringOf("name");
            asset.samplers.push_back(std::move(sampler));
        }
    }

    void decodeTextures(const json::Value& root) {
        const json::Value& textures = root.arrayOf("textures");
        for (std::size_t i = 0; i < textures.size(); ++i) {
            const json::Value& entry = textures.at(i);
            Texture texture;
            texture.sampler = entry.u32Of("sampler", kNone);
            texture.source = entry.u32Of("source", kNone);
            texture.name = entry.stringOf("name");
            asset.textures.push_back(std::move(texture));
        }
    }

    TextureInfo decodeTextureInfo(const json::Value* entry) {
        TextureInfo info;
        if (entry == nullptr || !entry->isObject()) {
            return info;
        }
        info.index = entry->u32Of("index", kNone);
        info.texCoord = entry->u32Of("texCoord");
        info.scale = entry->f32Of("scale", 1.0f);
        info.strength = entry->f32Of("strength", 1.0f);
        if (const json::Value* extensions = entry->find("extensions"); extensions != nullptr) {
            if (const json::Value* ext = extensions->find("KHR_texture_transform");
                ext != nullptr) {
                TextureTransform transform;
                const json::Value& offset = ext->arrayOf("offset");
                if (offset.size() == 2) {
                    transform.offset = {offset.at(0).asF32(), offset.at(1).asF32()};
                }
                transform.rotation = ext->f32Of("rotation");
                const json::Value& scale = ext->arrayOf("scale");
                if (scale.size() == 2) {
                    transform.scale = {scale.at(0).asF32(1.0f), scale.at(1).asF32(1.0f)};
                }
                transform.texCoord = ext->u32Of("texCoord", kNone);
                info.transform = transform;
            }
        }
        return info;
    }

    static Vector3f decodeVec3(const json::Value& array, const Vector3f& fallback) {
        if (array.size() != 3) {
            return fallback;
        }
        return {array.at(0).asF32(), array.at(1).asF32(), array.at(2).asF32()};
    }

    void decodeMaterials(const json::Value& root) {
        const json::Value& materials = root.arrayOf("materials");
        for (std::size_t i = 0; i < materials.size(); ++i) {
            const json::Value& entry = materials.at(i);
            Material material;
            material.name = entry.stringOf("name");
            if (const json::Value* pbr = entry.find("pbrMetallicRoughness"); pbr != nullptr) {
                const json::Value& base = pbr->arrayOf("baseColorFactor");
                if (base.size() == 4) {
                    material.pbr.baseColorFactor = {base.at(0).asF32(1.0f), base.at(1).asF32(1.0f),
                                                    base.at(2).asF32(1.0f), base.at(3).asF32(1.0f)};
                }
                material.pbr.baseColorTexture = decodeTextureInfo(pbr->find("baseColorTexture"));
                material.pbr.metallicFactor = pbr->f32Of("metallicFactor", 1.0f);
                material.pbr.roughnessFactor = pbr->f32Of("roughnessFactor", 1.0f);
                material.pbr.metallicRoughnessTexture =
                    decodeTextureInfo(pbr->find("metallicRoughnessTexture"));
            }
            material.normalTexture = decodeTextureInfo(entry.find("normalTexture"));
            material.occlusionTexture = decodeTextureInfo(entry.find("occlusionTexture"));
            material.emissiveTexture = decodeTextureInfo(entry.find("emissiveTexture"));
            material.emissiveFactor =
                decodeVec3(entry.arrayOf("emissiveFactor"), Vector3f{0, 0, 0});
            const std::string& alphaMode = entry.stringOf("alphaMode");
            if (alphaMode == "MASK") {
                material.alphaMode = AlphaMode::Mask;
            } else if (alphaMode == "BLEND") {
                material.alphaMode = AlphaMode::Blend;
            }
            material.alphaCutoff = entry.f32Of("alphaCutoff", 0.5f);
            material.doubleSided = entry.boolOf("doubleSided");
            if (const json::Value* extensions = entry.find("extensions"); extensions != nullptr) {
                material.unlit = extensions->find("KHR_materials_unlit") != nullptr;
                if (const json::Value* strength =
                        extensions->find("KHR_materials_emissive_strength");
                    strength != nullptr) {
                    material.emissiveStrength = strength->f32Of("emissiveStrength", 1.0f);
                }
            }
            asset.materials.push_back(std::move(material));
        }
    }

    void decodeMeshes(const json::Value& root) {
        const json::Value& meshes = root.arrayOf("meshes");
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const json::Value& entry = meshes.at(i);
            Mesh mesh;
            mesh.name = entry.stringOf("name");
            const json::Value& primitives = entry.arrayOf("primitives");
            for (std::size_t p = 0; p < primitives.size(); ++p) {
                const json::Value& source = primitives.at(p);
                Primitive primitive;
                if (const json::Value* attributes = source.find("attributes");
                    attributes != nullptr && attributes->isObject()) {
                    for (std::size_t a = 0; a < attributes->size(); ++a) {
                        const json::Member& member = attributes->memberAt(a);
                        primitive.attributes.push_back(
                            AttributeBinding{member.key, member.value.asU32(kNone)});
                    }
                }
                primitive.indices = source.u32Of("indices", kNone);
                primitive.material = source.u32Of("material", kNone);
                primitive.mode = static_cast<PrimitiveMode>(
                    source.u32Of("mode", static_cast<u32>(PrimitiveMode::Triangles)));
                if (source.find("targets") != nullptr) {
                    warn("mesh " + std::to_string(i) + " primitive " + std::to_string(p) +
                         ": morph targets dropped");
                }
                mesh.primitives.push_back(std::move(primitive));
            }
            const json::Value& weights = entry.arrayOf("weights");
            for (std::size_t w = 0; w < weights.size(); ++w) {
                mesh.weights.push_back(weights.at(w).asF32());
            }
            asset.meshes.push_back(std::move(mesh));
        }
    }

    void decodeNodes(const json::Value& root) {
        const json::Value& nodes = root.arrayOf("nodes");
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const json::Value& entry = nodes.at(i);
            Node node;
            node.name = entry.stringOf("name");
            const json::Value& children = entry.arrayOf("children");
            for (std::size_t c = 0; c < children.size(); ++c) {
                node.children.push_back(children.at(c).asU32(kNone));
            }
            node.mesh = entry.u32Of("mesh", kNone);
            node.skin = entry.u32Of("skin", kNone);
            node.camera = entry.u32Of("camera", kNone);
            if (const json::Value* extensions = entry.find("extensions"); extensions != nullptr) {
                if (const json::Value* lights = extensions->find("KHR_lights_punctual");
                    lights != nullptr) {
                    node.light = lights->u32Of("light", kNone);
                }
            }
            const json::Value& matrix = entry.arrayOf("matrix");
            if (matrix.size() == 16) {
                node.hasMatrix = true;
                // Column-major on the wire.
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        node.matrix.data[static_cast<std::size_t>(row)]
                                        [static_cast<std::size_t>(col)] =
                            matrix.at(static_cast<std::size_t>(col * 4 + row)).asF32();
                    }
                }
            } else {
                node.translation =
                    decodeVec3(entry.arrayOf("translation"), Vector3f{0, 0, 0});
                const json::Value& rotation = entry.arrayOf("rotation");
                if (rotation.size() == 4) {
                    node.rotation = {rotation.at(0).asF32(), rotation.at(1).asF32(),
                                     rotation.at(2).asF32(), rotation.at(3).asF32(1.0f)};
                }
                node.scale = decodeVec3(entry.arrayOf("scale"), Vector3f{1, 1, 1});
            }
            asset.nodes.push_back(std::move(node));
        }
    }

    void decodeScenes(const json::Value& root) {
        asset.scene = root.u32Of("scene", kNone);
        const json::Value& scenes = root.arrayOf("scenes");
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            const json::Value& entry = scenes.at(i);
            Scene scene;
            scene.name = entry.stringOf("name");
            const json::Value& nodes = entry.arrayOf("nodes");
            for (std::size_t n = 0; n < nodes.size(); ++n) {
                scene.nodes.push_back(nodes.at(n).asU32(kNone));
            }
            asset.scenes.push_back(std::move(scene));
        }
    }

    void decodeSkins(const json::Value& root) {
        const json::Value& skins = root.arrayOf("skins");
        for (std::size_t i = 0; i < skins.size(); ++i) {
            const json::Value& entry = skins.at(i);
            Skin skin;
            skin.name = entry.stringOf("name");
            skin.inverseBindMatrices = entry.u32Of("inverseBindMatrices", kNone);
            skin.skeleton = entry.u32Of("skeleton", kNone);
            const json::Value& joints = entry.arrayOf("joints");
            for (std::size_t j = 0; j < joints.size(); ++j) {
                skin.joints.push_back(joints.at(j).asU32(kNone));
            }
            asset.skins.push_back(std::move(skin));
        }
    }

    void decodeAnimations(const json::Value& root) {
        const json::Value& animations = root.arrayOf("animations");
        for (std::size_t i = 0; i < animations.size(); ++i) {
            const json::Value& entry = animations.at(i);
            Animation animation;
            animation.name = entry.stringOf("name");
            const json::Value& samplers = entry.arrayOf("samplers");
            for (std::size_t s = 0; s < samplers.size(); ++s) {
                const json::Value& source = samplers.at(s);
                AnimationSampler sampler;
                sampler.input = source.u32Of("input", kNone);
                sampler.output = source.u32Of("output", kNone);
                const std::string& interp = source.stringOf("interpolation");
                if (interp == "STEP") {
                    sampler.interpolation = AnimInterpolation::Step;
                } else if (interp == "CUBICSPLINE") {
                    sampler.interpolation = AnimInterpolation::CubicSpline;
                }
                animation.samplers.push_back(sampler);
            }
            const json::Value& channels = entry.arrayOf("channels");
            for (std::size_t c = 0; c < channels.size(); ++c) {
                const json::Value& source = channels.at(c);
                AnimationChannel channel;
                channel.sampler = source.u32Of("sampler", kNone);
                if (const json::Value* target = source.find("target"); target != nullptr) {
                    channel.targetNode = target->u32Of("node", kNone);
                    const std::string& path = target->stringOf("path");
                    if (path == "rotation") {
                        channel.targetPath = AnimPath::Rotation;
                    } else if (path == "scale") {
                        channel.targetPath = AnimPath::Scale;
                    } else if (path == "weights") {
                        channel.targetPath = AnimPath::Weights;
                    }
                }
                animation.channels.push_back(channel);
            }
            asset.animations.push_back(std::move(animation));
        }
    }

    void decodeCameras(const json::Value& root) {
        const json::Value& cameras = root.arrayOf("cameras");
        for (std::size_t i = 0; i < cameras.size(); ++i) {
            const json::Value& entry = cameras.at(i);
            Camera camera;
            camera.name = entry.stringOf("name");
            camera.perspective = entry.stringOf("type") != "orthographic";
            if (const json::Value* perspective = entry.find("perspective");
                perspective != nullptr) {
                camera.yfov = perspective->f32Of("yfov");
                camera.aspectRatio = perspective->f32Of("aspectRatio");
                camera.znear = perspective->f32Of("znear");
                camera.zfar = perspective->f32Of("zfar");
            }
            asset.cameras.push_back(std::move(camera));
        }
    }

    void decodeLights(const json::Value& root) {
        const json::Value* extensions = root.find("extensions");
        if (extensions == nullptr) {
            return;
        }
        const json::Value* punctual = extensions->find("KHR_lights_punctual");
        if (punctual == nullptr) {
            return;
        }
        const json::Value& lights = punctual->arrayOf("lights");
        for (std::size_t i = 0; i < lights.size(); ++i) {
            const json::Value& entry = lights.at(i);
            Light light;
            light.name = entry.stringOf("name");
            const std::string& type = entry.stringOf("type");
            if (type == "directional") {
                light.kind = LightKind::Directional;
            } else if (type == "spot") {
                light.kind = LightKind::Spot;
            }
            light.color = decodeVec3(entry.arrayOf("color"), Vector3f{1, 1, 1});
            light.intensity = entry.f32Of("intensity", 1.0f);
            light.range = entry.f32Of("range");
            if (const json::Value* spot = entry.find("spot"); spot != nullptr) {
                light.innerConeAngle = spot->f32Of("innerConeAngle");
                light.outerConeAngle = spot->f32Of("outerConeAngle", 0.7853981633974483f);
            }
            asset.lights.push_back(std::move(light));
        }
    }

    std::span<const u8> viewBytes(u32 viewIndex) const {
        if (viewIndex >= asset.bufferViews.size()) {
            return {};
        }
        const BufferView& view = asset.bufferViews[viewIndex];
        if (view.buffer >= asset.buffers.size()) {
            return {};
        }
        const Buffer& buffer = asset.buffers[view.buffer];
        if (buffer.data.size() < view.byteOffset ||
            buffer.data.size() - view.byteOffset < view.byteLength) {
            return {};
        }
        return std::span<const u8>(buffer.data.data() + view.byteOffset, view.byteLength);
    }

    bool run(const json::Value& root, std::span<const u8> binChunk) {
        if (!root.isObject()) {
            return fail("root is not an object");
        }
        if (!decodeAssetInfo(root)) {
            return false;
        }
        decodeStringArray(root, "extensionsUsed", asset.extensionsUsed);
        decodeStringArray(root, "extensionsRequired", asset.extensionsRequired);
        for (const std::string& required : asset.extensionsRequired) {
            if (required != "KHR_materials_unlit" && required != "KHR_texture_transform" &&
                required != "KHR_materials_emissive_strength" &&
                required != "KHR_lights_punctual") {
                warn("required extension '" + required + "' is not supported");
            }
        }
        if (!decodeBuffers(root, binChunk)) {
            return false;
        }
        decodeBufferViews(root);
        decodeAccessors(root);
        decodeImages(root);
        decodeSamplers(root);
        decodeTextures(root);
        decodeMaterials(root);
        decodeMeshes(root);
        decodeNodes(root);
        decodeScenes(root);
        decodeSkins(root);
        decodeAnimations(root);
        decodeCameras(root);
        decodeLights(root);
        return true;
    }
};

} // namespace

// ============================================================================
// Parser
// ============================================================================

bool Parser::LooksLikeGlb(std::span<const u8> bytes) {
    return bytes.size() >= 12 && readU32(bytes, 0) == kGlbMagic;
}

ParseOutcome Parser::FromBytes(std::span<const u8> bytes, const ParseOptions& options) {
    if (!LooksLikeGlb(bytes)) {
        return FromJsonText(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {},
            options);
    }

    ParseOutcome outcome;
    const u32 version = readU32(bytes, 4);
    if (version != 2) {
        outcome.error = "GLB version " + std::to_string(version) + " is not supported";
        return outcome;
    }
    const u32 declaredLength = readU32(bytes, 8);
    if (declaredLength > bytes.size()) {
        outcome.error = "GLB header length exceeds the file";
        return outcome;
    }

    std::span<const u8> jsonChunk;
    std::span<const u8> binChunk;
    std::size_t offset = 12;
    while (offset + 8 <= declaredLength) {
        const u32 chunkLength = readU32(bytes, offset);
        const u32 chunkType = readU32(bytes, offset + 4);
        offset += 8;
        if (chunkLength > declaredLength - offset) {
            outcome.error = "GLB chunk overruns the file";
            return outcome;
        }
        const std::span<const u8> chunk = bytes.subspan(offset, chunkLength);
        if (chunkType == kGlbChunkJson && jsonChunk.empty()) {
            jsonChunk = chunk;
        } else if (chunkType == kGlbChunkBin && binChunk.empty()) {
            binChunk = chunk;
        }
        // Chunks are 4-aligned; the padding is inside the *next* chunk's
        // position, not this one's length.
        offset += (chunkLength + 3u) & ~3u;
    }
    if (jsonChunk.empty()) {
        outcome.error = "GLB carries no JSON chunk";
        return outcome;
    }
    return FromJsonText(
        std::string_view(reinterpret_cast<const char*>(jsonChunk.data()), jsonChunk.size()),
        binChunk, options);
}

ParseOutcome Parser::FromJsonText(std::string_view text, std::span<const u8> binChunk,
                                  const ParseOptions& options) {
    ParseOutcome outcome;
    json::ParseResult parsed = json::Parse(text);
    if (!parsed.ok()) {
        outcome.error =
            "JSON: " + parsed.error + " at byte " + std::to_string(parsed.offset);
        return outcome;
    }

    Decoder decoder(options);
    if (!decoder.run(*parsed.value, binChunk)) {
        outcome.error = decoder.error;
        outcome.warnings = std::move(decoder.warnings);
        return outcome;
    }
    outcome.asset = std::move(decoder.asset);
    outcome.warnings = std::move(decoder.warnings);
    return outcome;
}

// ============================================================================
// Base64
// ============================================================================

bool DecodeBase64(std::string_view text, std::vector<u8>& out) {
    static constexpr i8 kInvalid = -1;
    i8 table[256];
    for (i32 i = 0; i < 256; ++i) {
        table[i] = kInvalid;
    }
    for (i8 i = 0; i < 26; ++i) {
        table['A' + i] = i;
        table['a' + i] = static_cast<i8>(26 + i);
    }
    for (i8 i = 0; i < 10; ++i) {
        table['0' + i] = static_cast<i8>(52 + i);
    }
    table['+'] = 62;
    table['/'] = 63;

    out.clear();
    out.reserve((text.size() / 4) * 3);
    u32 accumulator = 0;
    u32 bits = 0;
    std::size_t padding = 0;
    for (const char c : text) {
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding != 0) {
            return false; // Data after padding.
        }
        const i8 value = table[static_cast<unsigned char>(c)];
        if (value == kInvalid) {
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<u32>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<u8>((accumulator >> bits) & 0xFF));
        }
    }
    return padding <= 2;
}

// ============================================================================
// Accessor decode
// ============================================================================

namespace {

struct AccessorLayout {
    const u8* base = nullptr; ///< First element; null = zero-filled accessor.
    std::size_t stride = 0;
    u32 componentSize = 0;
    u32 components = 0;
    u32 count = 0;
    ComponentType componentType = ComponentType::None;
    bool normalized = false;
};

bool layoutOf(const Asset& asset, const Accessor& accessor, AccessorLayout& out) {
    out.componentSize = ComponentSize(accessor.componentType);
    out.components = TypeComponentCount(accessor.type);
    out.count = accessor.count;
    out.componentType = accessor.componentType;
    out.normalized = accessor.normalized;
    if (out.componentSize == 0 || out.components == 0) {
        return false;
    }
    const std::size_t packed = static_cast<std::size_t>(out.componentSize) * out.components;
    if (accessor.bufferView == kNone) {
        out.base = nullptr; // Legal: a sparse or zero accessor.
        out.stride = packed;
        return true;
    }
    if (accessor.bufferView >= asset.bufferViews.size()) {
        return false;
    }
    const BufferView& view = asset.bufferViews[accessor.bufferView];
    if (view.buffer >= asset.buffers.size()) {
        return false;
    }
    const Buffer& buffer = asset.buffers[view.buffer];
    if (buffer.data.empty()) {
        return false;
    }
    out.stride = view.byteStride != 0 ? view.byteStride : packed;
    const std::size_t start = static_cast<std::size_t>(view.byteOffset) + accessor.byteOffset;
    if (accessor.count == 0) {
        out.base = buffer.data.data() + start;
        return start <= buffer.data.size();
    }
    const std::size_t last = start + out.stride * (accessor.count - 1) + packed;
    if (view.byteOffset > buffer.data.size() ||
        static_cast<std::size_t>(view.byteOffset) + view.byteLength > buffer.data.size() ||
        last > buffer.data.size()) {
        return false;
    }
    out.base = buffer.data.data() + start;
    return true;
}

f64 readComponent(const u8* bytes, ComponentType type) {
    switch (type) {
    case ComponentType::I8:
        return static_cast<f64>(static_cast<i8>(bytes[0]));
    case ComponentType::U8:
        return static_cast<f64>(bytes[0]);
    case ComponentType::I16: {
        i16 value;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f64>(value);
    }
    case ComponentType::U16: {
        u16 value;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f64>(value);
    }
    case ComponentType::U32: {
        u32 value;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f64>(value);
    }
    case ComponentType::F32: {
        f32 value;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<f64>(value);
    }
    case ComponentType::None:
        break;
    }
    return 0.0;
}

f32 normalizeComponent(f64 raw, ComponentType type) {
    switch (type) {
    case ComponentType::I8:
        return static_cast<f32>(raw < -127.0 ? -1.0 : raw / 127.0);
    case ComponentType::U8:
        return static_cast<f32>(raw / 255.0);
    case ComponentType::I16:
        return static_cast<f32>(raw < -32767.0 ? -1.0 : raw / 32767.0);
    case ComponentType::U16:
        return static_cast<f32>(raw / 65535.0);
    default:
        return static_cast<f32>(raw);
    }
}

/// Applies the sparse overlay onto @p values (already sized count*components).
bool applySparse(const Asset& asset, const Accessor& accessor, std::vector<f32>& values,
                 u32 components) {
    if (!accessor.sparse.has_value()) {
        return true;
    }
    const AccessorSparse& sparse = *accessor.sparse;
    const u32 indexSize = ComponentSize(sparse.indicesComponentType);
    if (indexSize == 0 || sparse.indicesBufferView >= asset.bufferViews.size() ||
        sparse.valuesBufferView >= asset.bufferViews.size()) {
        return false;
    }
    const BufferView& indexView = asset.bufferViews[sparse.indicesBufferView];
    const BufferView& valueView = asset.bufferViews[sparse.valuesBufferView];
    if (indexView.buffer >= asset.buffers.size() || valueView.buffer >= asset.buffers.size()) {
        return false;
    }
    const Buffer& indexBuffer = asset.buffers[indexView.buffer];
    const Buffer& valueBuffer = asset.buffers[valueView.buffer];
    const u32 componentSize = ComponentSize(accessor.componentType);
    const std::size_t indexStart =
        static_cast<std::size_t>(indexView.byteOffset) + sparse.indicesByteOffset;
    const std::size_t valueStart =
        static_cast<std::size_t>(valueView.byteOffset) + sparse.valuesByteOffset;
    if (indexStart + static_cast<std::size_t>(indexSize) * sparse.count > indexBuffer.data.size() ||
        valueStart + static_cast<std::size_t>(componentSize) * components * sparse.count >
            valueBuffer.data.size()) {
        return false;
    }
    for (u32 i = 0; i < sparse.count; ++i) {
        const f64 rawIndex = readComponent(indexBuffer.data.data() + indexStart +
                                               static_cast<std::size_t>(indexSize) * i,
                                           sparse.indicesComponentType);
        const u32 element = static_cast<u32>(rawIndex);
        if (element >= accessor.count) {
            return false;
        }
        for (u32 c = 0; c < components; ++c) {
            const u8* bytes = valueBuffer.data.data() + valueStart +
                              (static_cast<std::size_t>(i) * components + c) * componentSize;
            const f64 raw = readComponent(bytes, accessor.componentType);
            values[static_cast<std::size_t>(element) * components + c] =
                accessor.normalized ? normalizeComponent(raw, accessor.componentType)
                                    : static_cast<f32>(raw);
        }
    }
    return true;
}

} // namespace

bool ReadAccessorF32(const Asset& asset, u32 index, std::vector<f32>& out) {
    out.clear();
    if (index >= asset.accessors.size()) {
        return false;
    }
    const Accessor& accessor = asset.accessors[index];
    AccessorLayout layout;
    if (!layoutOf(asset, accessor, layout)) {
        return false;
    }
    out.assign(static_cast<std::size_t>(layout.count) * layout.components, 0.0f);
    if (layout.base != nullptr) {
        std::size_t write = 0;
        for (u32 i = 0; i < layout.count; ++i) {
            const u8* element = layout.base + layout.stride * i;
            for (u32 c = 0; c < layout.components; ++c) {
                const f64 raw =
                    readComponent(element + static_cast<std::size_t>(c) * layout.componentSize,
                                  layout.componentType);
                out[write++] = layout.normalized
                                   ? normalizeComponent(raw, layout.componentType)
                                   : static_cast<f32>(raw);
            }
        }
    }
    return applySparse(asset, accessor, out, layout.components);
}

bool ReadAccessorU32(const Asset& asset, u32 index, std::vector<u32>& out) {
    out.clear();
    if (index >= asset.accessors.size()) {
        return false;
    }
    const Accessor& accessor = asset.accessors[index];
    if (accessor.componentType == ComponentType::F32) {
        return false;
    }
    AccessorLayout layout;
    if (!layoutOf(asset, accessor, layout)) {
        return false;
    }
    out.assign(static_cast<std::size_t>(layout.count) * layout.components, 0u);
    if (layout.base != nullptr) {
        std::size_t write = 0;
        for (u32 i = 0; i < layout.count; ++i) {
            const u8* element = layout.base + layout.stride * i;
            for (u32 c = 0; c < layout.components; ++c) {
                const f64 raw =
                    readComponent(element + static_cast<std::size_t>(c) * layout.componentSize,
                                  layout.componentType);
                out[write++] = raw <= 0.0 ? 0u : static_cast<u32>(raw);
            }
        }
    }
    if (accessor.sparse.has_value()) {
        std::vector<f32> overlay(out.size());
        for (std::size_t i = 0; i < out.size(); ++i) {
            overlay[i] = static_cast<f32>(out[i]);
        }
        if (!applySparse(asset, accessor, overlay, layout.components)) {
            return false;
        }
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = overlay[i] <= 0.0f ? 0u : static_cast<u32>(overlay[i]);
        }
    }
    return true;
}

} // namespace gltf
} // namespace models
} // namespace whiteout
