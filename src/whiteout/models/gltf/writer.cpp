// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/gltf/writer.h"

#include <cmath>

#include "whiteout/models/gltf/json.h"

namespace whiteout {
namespace models {
namespace gltf {

namespace {

json::Value numberValue(f64 value) {
    return json::Value::number(value);
}

json::Value vec2Value(const Vector2f& v) {
    json::Value out = json::Value::array();
    out.push(numberValue(v.x));
    out.push(numberValue(v.y));
    return out;
}

json::Value vec3Value(const Vector3f& v) {
    json::Value out = json::Value::array();
    out.push(numberValue(v.x));
    out.push(numberValue(v.y));
    out.push(numberValue(v.z));
    return out;
}

json::Value vec4Value(const Vector4f& v) {
    json::Value out = json::Value::array();
    out.push(numberValue(v.x));
    out.push(numberValue(v.y));
    out.push(numberValue(v.z));
    out.push(numberValue(v.w));
    return out;
}

json::Value quatValue(const Quaternion& q) {
    json::Value out = json::Value::array();
    out.push(numberValue(q.x));
    out.push(numberValue(q.y));
    out.push(numberValue(q.z));
    out.push(numberValue(q.w));
    return out;
}

void setName(json::Value& target, const std::string& name) {
    if (!name.empty()) {
        target.set("name", json::Value::string(name));
    }
}

json::Value encodeTextureInfo(const TextureInfo& info, bool isNormal, bool isOcclusion) {
    json::Value out = json::Value::object();
    out.set("index", numberValue(info.index));
    if (info.texCoord != 0) {
        out.set("texCoord", numberValue(info.texCoord));
    }
    if (isNormal && info.scale != 1.0f) {
        out.set("scale", numberValue(info.scale));
    }
    if (isOcclusion && info.strength != 1.0f) {
        out.set("strength", numberValue(info.strength));
    }
    if (info.transform.has_value()) {
        const TextureTransform& transform = *info.transform;
        json::Value ext = json::Value::object();
        if (transform.offset.x != 0.0f || transform.offset.y != 0.0f) {
            ext.set("offset", vec2Value(transform.offset));
        }
        if (transform.rotation != 0.0f) {
            ext.set("rotation", numberValue(transform.rotation));
        }
        if (transform.scale.x != 1.0f || transform.scale.y != 1.0f) {
            ext.set("scale", vec2Value(transform.scale));
        }
        if (transform.texCoord != kNone) {
            ext.set("texCoord", numberValue(transform.texCoord));
        }
        json::Value extensions = json::Value::object();
        extensions.set("KHR_texture_transform", std::move(ext));
        out.set("extensions", std::move(extensions));
    }
    return out;
}

json::Value encodeMaterial(const Material& material) {
    json::Value out = json::Value::object();
    setName(out, material.name);

    json::Value pbr = json::Value::object();
    const PbrMetallicRoughness& mr = material.pbr;
    if (mr.baseColorFactor.x != 1.0f || mr.baseColorFactor.y != 1.0f ||
        mr.baseColorFactor.z != 1.0f || mr.baseColorFactor.w != 1.0f) {
        pbr.set("baseColorFactor", vec4Value(mr.baseColorFactor));
    }
    if (mr.baseColorTexture.present()) {
        pbr.set("baseColorTexture", encodeTextureInfo(mr.baseColorTexture, false, false));
    }
    if (mr.metallicFactor != 1.0f) {
        pbr.set("metallicFactor", numberValue(mr.metallicFactor));
    }
    if (mr.roughnessFactor != 1.0f) {
        pbr.set("roughnessFactor", numberValue(mr.roughnessFactor));
    }
    if (mr.metallicRoughnessTexture.present()) {
        pbr.set("metallicRoughnessTexture",
                encodeTextureInfo(mr.metallicRoughnessTexture, false, false));
    }
    if (pbr.size() != 0) {
        out.set("pbrMetallicRoughness", std::move(pbr));
    }

    if (material.normalTexture.present()) {
        out.set("normalTexture", encodeTextureInfo(material.normalTexture, true, false));
    }
    if (material.occlusionTexture.present()) {
        out.set("occlusionTexture", encodeTextureInfo(material.occlusionTexture, false, true));
    }
    if (material.emissiveTexture.present()) {
        out.set("emissiveTexture", encodeTextureInfo(material.emissiveTexture, false, false));
    }
    if (material.emissiveFactor.x != 0.0f || material.emissiveFactor.y != 0.0f ||
        material.emissiveFactor.z != 0.0f) {
        out.set("emissiveFactor", vec3Value(material.emissiveFactor));
    }
    if (material.alphaMode != AlphaMode::Opaque) {
        out.set("alphaMode", json::Value::string(ToString(material.alphaMode)));
    }
    if (material.alphaMode == AlphaMode::Mask && material.alphaCutoff != 0.5f) {
        out.set("alphaCutoff", numberValue(material.alphaCutoff));
    }
    if (material.doubleSided) {
        out.set("doubleSided", json::Value::boolean(true));
    }

    json::Value extensions = json::Value::object();
    if (material.unlit) {
        extensions.set("KHR_materials_unlit", json::Value::object());
    }
    if (material.emissiveStrength != 1.0f) {
        json::Value strength = json::Value::object();
        strength.set("emissiveStrength", numberValue(material.emissiveStrength));
        extensions.set("KHR_materials_emissive_strength", std::move(strength));
    }
    if (extensions.size() != 0) {
        out.set("extensions", std::move(extensions));
    }
    return out;
}

json::Value encodeNode(const Node& node) {
    json::Value out = json::Value::object();
    setName(out, node.name);
    if (!node.children.empty()) {
        json::Value children = json::Value::array();
        for (const u32 child : node.children) {
            children.push(numberValue(child));
        }
        out.set("children", std::move(children));
    }
    if (node.mesh != kNone) {
        out.set("mesh", numberValue(node.mesh));
    }
    if (node.skin != kNone) {
        out.set("skin", numberValue(node.skin));
    }
    if (node.camera != kNone) {
        out.set("camera", numberValue(node.camera));
    }
    if (node.light != kNone) {
        json::Value punctual = json::Value::object();
        punctual.set("light", numberValue(node.light));
        json::Value extensions = json::Value::object();
        extensions.set("KHR_lights_punctual", std::move(punctual));
        out.set("extensions", std::move(extensions));
    }
    if (node.hasMatrix) {
        json::Value matrix = json::Value::array();
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                matrix.push(numberValue(node.matrix.data[static_cast<std::size_t>(row)]
                                                        [static_cast<std::size_t>(col)]));
            }
        }
        out.set("matrix", std::move(matrix));
    } else {
        if (node.translation.x != 0.0f || node.translation.y != 0.0f ||
            node.translation.z != 0.0f) {
            out.set("translation", vec3Value(node.translation));
        }
        if (node.rotation.x != 0.0f || node.rotation.y != 0.0f || node.rotation.z != 0.0f ||
            node.rotation.w != 1.0f) {
            out.set("rotation", quatValue(node.rotation));
        }
        if (node.scale.x != 1.0f || node.scale.y != 1.0f || node.scale.z != 1.0f) {
            out.set("scale", vec3Value(node.scale));
        }
    }
    return out;
}

json::Value encodePrimitive(const Primitive& primitive) {
    json::Value out = json::Value::object();
    json::Value attributes = json::Value::object();
    for (const AttributeBinding& binding : primitive.attributes) {
        attributes.set(binding.semantic, numberValue(binding.accessor));
    }
    out.set("attributes", std::move(attributes));
    if (primitive.indices != kNone) {
        out.set("indices", numberValue(primitive.indices));
    }
    if (primitive.material != kNone) {
        out.set("material", numberValue(primitive.material));
    }
    if (primitive.mode != PrimitiveMode::Triangles) {
        out.set("mode", numberValue(static_cast<u32>(primitive.mode)));
    }
    return out;
}

json::Value encodeAccessor(const Accessor& accessor) {
    json::Value out = json::Value::object();
    if (accessor.bufferView != kNone) {
        out.set("bufferView", numberValue(accessor.bufferView));
    }
    if (accessor.byteOffset != 0) {
        out.set("byteOffset", numberValue(accessor.byteOffset));
    }
    out.set("componentType", numberValue(static_cast<u32>(accessor.componentType)));
    if (accessor.normalized) {
        out.set("normalized", json::Value::boolean(true));
    }
    out.set("count", numberValue(accessor.count));
    out.set("type", json::Value::string(ToString(accessor.type)));
    if (!accessor.min.empty()) {
        json::Value min = json::Value::array();
        for (const f64 value : accessor.min) {
            min.push(numberValue(value));
        }
        out.set("min", std::move(min));
    }
    if (!accessor.max.empty()) {
        json::Value max = json::Value::array();
        for (const f64 value : accessor.max) {
            max.push(numberValue(value));
        }
        out.set("max", std::move(max));
    }
    setName(out, accessor.name);
    return out;
}

json::Value encodeAnimation(const Animation& animation) {
    json::Value out = json::Value::object();
    setName(out, animation.name);
    json::Value samplers = json::Value::array();
    for (const AnimationSampler& sampler : animation.samplers) {
        json::Value entry = json::Value::object();
        entry.set("input", numberValue(sampler.input));
        if (sampler.interpolation != AnimInterpolation::Linear) {
            entry.set("interpolation", json::Value::string(ToString(sampler.interpolation)));
        }
        entry.set("output", numberValue(sampler.output));
        samplers.push(std::move(entry));
    }
    out.set("samplers", std::move(samplers));
    json::Value channels = json::Value::array();
    for (const AnimationChannel& channel : animation.channels) {
        json::Value entry = json::Value::object();
        entry.set("sampler", numberValue(channel.sampler));
        json::Value target = json::Value::object();
        if (channel.targetNode != kNone) {
            target.set("node", numberValue(channel.targetNode));
        }
        target.set("path", json::Value::string(ToString(channel.targetPath)));
        entry.set("target", std::move(target));
        channels.push(std::move(entry));
    }
    out.set("channels", std::move(channels));
    return out;
}

json::Value encodeLight(const Light& light) {
    json::Value out = json::Value::object();
    setName(out, light.name);
    const char* type = light.kind == LightKind::Directional ? "directional"
                       : light.kind == LightKind::Spot      ? "spot"
                                                            : "point";
    out.set("type", json::Value::string(type));
    if (light.color.x != 1.0f || light.color.y != 1.0f || light.color.z != 1.0f) {
        out.set("color", vec3Value(light.color));
    }
    if (light.intensity != 1.0f) {
        out.set("intensity", numberValue(light.intensity));
    }
    if (light.range != 0.0f) {
        out.set("range", numberValue(light.range));
    }
    if (light.kind == LightKind::Spot) {
        json::Value spot = json::Value::object();
        if (light.innerConeAngle != 0.0f) {
            spot.set("innerConeAngle", numberValue(light.innerConeAngle));
        }
        spot.set("outerConeAngle", numberValue(light.outerConeAngle));
        out.set("spot", std::move(spot));
    }
    return out;
}

json::Value encodeAsset(const Asset& asset) {
    json::Value root = json::Value::object();

    json::Value info = json::Value::object();
    info.set("version", json::Value::string(asset.asset.version));
    if (!asset.asset.minVersion.empty()) {
        info.set("minVersion", json::Value::string(asset.asset.minVersion));
    }
    if (!asset.asset.generator.empty()) {
        info.set("generator", json::Value::string(asset.asset.generator));
    }
    if (!asset.asset.copyright.empty()) {
        info.set("copyright", json::Value::string(asset.asset.copyright));
    }
    root.set("asset", std::move(info));

    if (!asset.extensionsUsed.empty()) {
        json::Value used = json::Value::array();
        for (const std::string& extension : asset.extensionsUsed) {
            used.push(json::Value::string(extension));
        }
        root.set("extensionsUsed", std::move(used));
    }
    if (!asset.extensionsRequired.empty()) {
        json::Value required = json::Value::array();
        for (const std::string& extension : asset.extensionsRequired) {
            required.push(json::Value::string(extension));
        }
        root.set("extensionsRequired", std::move(required));
    }

    if (asset.scene != kNone) {
        root.set("scene", numberValue(asset.scene));
    }
    if (!asset.scenes.empty()) {
        json::Value scenes = json::Value::array();
        for (const Scene& scene : asset.scenes) {
            json::Value entry = json::Value::object();
            setName(entry, scene.name);
            if (!scene.nodes.empty()) {
                json::Value nodes = json::Value::array();
                for (const u32 node : scene.nodes) {
                    nodes.push(numberValue(node));
                }
                entry.set("nodes", std::move(nodes));
            }
            scenes.push(std::move(entry));
        }
        root.set("scenes", std::move(scenes));
    }

    if (!asset.nodes.empty()) {
        json::Value nodes = json::Value::array();
        for (const Node& node : asset.nodes) {
            nodes.push(encodeNode(node));
        }
        root.set("nodes", std::move(nodes));
    }

    if (!asset.meshes.empty()) {
        json::Value meshes = json::Value::array();
        for (const Mesh& mesh : asset.meshes) {
            json::Value entry = json::Value::object();
            setName(entry, mesh.name);
            json::Value primitives = json::Value::array();
            for (const Primitive& primitive : mesh.primitives) {
                primitives.push(encodePrimitive(primitive));
            }
            entry.set("primitives", std::move(primitives));
            meshes.push(std::move(entry));
        }
        root.set("meshes", std::move(meshes));
    }

    if (!asset.skins.empty()) {
        json::Value skins = json::Value::array();
        for (const Skin& skin : asset.skins) {
            json::Value entry = json::Value::object();
            setName(entry, skin.name);
            if (skin.inverseBindMatrices != kNone) {
                entry.set("inverseBindMatrices", numberValue(skin.inverseBindMatrices));
            }
            if (skin.skeleton != kNone) {
                entry.set("skeleton", numberValue(skin.skeleton));
            }
            json::Value joints = json::Value::array();
            for (const u32 joint : skin.joints) {
                joints.push(numberValue(joint));
            }
            entry.set("joints", std::move(joints));
            skins.push(std::move(entry));
        }
        root.set("skins", std::move(skins));
    }

    if (!asset.animations.empty()) {
        json::Value animations = json::Value::array();
        for (const Animation& animation : asset.animations) {
            animations.push(encodeAnimation(animation));
        }
        root.set("animations", std::move(animations));
    }

    if (!asset.materials.empty()) {
        json::Value materials = json::Value::array();
        for (const Material& material : asset.materials) {
            materials.push(encodeMaterial(material));
        }
        root.set("materials", std::move(materials));
    }

    if (!asset.textures.empty()) {
        json::Value textures = json::Value::array();
        for (const Texture& texture : asset.textures) {
            json::Value entry = json::Value::object();
            if (texture.sampler != kNone) {
                entry.set("sampler", numberValue(texture.sampler));
            }
            if (texture.source != kNone) {
                entry.set("source", numberValue(texture.source));
            }
            setName(entry, texture.name);
            textures.push(std::move(entry));
        }
        root.set("textures", std::move(textures));
    }

    if (!asset.samplers.empty()) {
        json::Value samplers = json::Value::array();
        for (const Sampler& sampler : asset.samplers) {
            json::Value entry = json::Value::object();
            if (sampler.magFilter != 0) {
                entry.set("magFilter", numberValue(sampler.magFilter));
            }
            if (sampler.minFilter != 0) {
                entry.set("minFilter", numberValue(sampler.minFilter));
            }
            if (sampler.wrapS != WrapMode::Repeat) {
                entry.set("wrapS", numberValue(static_cast<u32>(sampler.wrapS)));
            }
            if (sampler.wrapT != WrapMode::Repeat) {
                entry.set("wrapT", numberValue(static_cast<u32>(sampler.wrapT)));
            }
            setName(entry, sampler.name);
            samplers.push(std::move(entry));
        }
        root.set("samplers", std::move(samplers));
    }

    if (!asset.images.empty()) {
        json::Value images = json::Value::array();
        for (const Image& image : asset.images) {
            json::Value entry = json::Value::object();
            if (image.bufferView != kNone) {
                entry.set("bufferView", numberValue(image.bufferView));
                if (!image.mimeType.empty()) {
                    entry.set("mimeType", json::Value::string(image.mimeType));
                }
            } else {
                entry.set("uri", json::Value::string(image.uri));
                if (!image.mimeType.empty()) {
                    entry.set("mimeType", json::Value::string(image.mimeType));
                }
            }
            setName(entry, image.name);
            images.push(std::move(entry));
        }
        root.set("images", std::move(images));
    }

    if (!asset.accessors.empty()) {
        json::Value accessors = json::Value::array();
        for (const Accessor& accessor : asset.accessors) {
            accessors.push(encodeAccessor(accessor));
        }
        root.set("accessors", std::move(accessors));
    }

    if (!asset.bufferViews.empty()) {
        json::Value views = json::Value::array();
        for (const BufferView& view : asset.bufferViews) {
            json::Value entry = json::Value::object();
            entry.set("buffer", numberValue(view.buffer));
            if (view.byteOffset != 0) {
                entry.set("byteOffset", numberValue(view.byteOffset));
            }
            entry.set("byteLength", numberValue(view.byteLength));
            if (view.byteStride != 0) {
                entry.set("byteStride", numberValue(view.byteStride));
            }
            if (view.target != BufferTarget::None) {
                entry.set("target", numberValue(static_cast<u32>(view.target)));
            }
            setName(entry, view.name);
            views.push(std::move(entry));
        }
        root.set("bufferViews", std::move(views));
    }

    if (!asset.buffers.empty()) {
        json::Value buffers = json::Value::array();
        for (std::size_t i = 0; i < asset.buffers.size(); ++i) {
            const Buffer& buffer = asset.buffers[i];
            json::Value entry = json::Value::object();
            if (!buffer.uri.empty()) {
                entry.set("uri", json::Value::string(buffer.uri));
            }
            const std::size_t length =
                (i == 0 && buffer.uri.empty() && !buffer.data.empty()) ? buffer.data.size()
                                                                       : buffer.byteLength;
            entry.set("byteLength", numberValue(static_cast<f64>(length)));
            setName(entry, buffer.name);
            buffers.push(std::move(entry));
        }
        root.set("buffers", std::move(buffers));
    }

    if (!asset.cameras.empty()) {
        json::Value cameras = json::Value::array();
        for (const Camera& camera : asset.cameras) {
            json::Value entry = json::Value::object();
            setName(entry, camera.name);
            entry.set("type", json::Value::string("perspective"));
            json::Value perspective = json::Value::object();
            if (camera.aspectRatio != 0.0f) {
                perspective.set("aspectRatio", numberValue(camera.aspectRatio));
            }
            perspective.set("yfov", numberValue(camera.yfov));
            if (camera.zfar != 0.0f) {
                perspective.set("zfar", numberValue(camera.zfar));
            }
            perspective.set("znear", numberValue(camera.znear));
            entry.set("perspective", std::move(perspective));
            cameras.push(std::move(entry));
        }
        root.set("cameras", std::move(cameras));
    }

    if (!asset.lights.empty()) {
        json::Value lights = json::Value::array();
        for (const Light& light : asset.lights) {
            lights.push(encodeLight(light));
        }
        json::Value punctual = json::Value::object();
        punctual.set("lights", std::move(lights));
        json::Value extensions = json::Value::object();
        extensions.set("KHR_lights_punctual", std::move(punctual));
        root.set("extensions", std::move(extensions));
    }

    return root;
}

void appendU32(std::vector<u8>& out, u32 value) {
    out.push_back(static_cast<u8>(value & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
    out.push_back(static_cast<u8>((value >> 16) & 0xFF));
    out.push_back(static_cast<u8>((value >> 24) & 0xFF));
}

} // namespace

std::string Writer::ToJsonText(const Asset& asset) {
    return json::Write(encodeAsset(asset));
}

std::vector<u8> Writer::ToGlb(const Asset& asset) {
    const std::string body = ToJsonText(asset);

    const std::size_t jsonPadded = (body.size() + 3u) & ~std::size_t(3);
    const bool hasBin =
        !asset.buffers.empty() && asset.buffers[0].uri.empty() && !asset.buffers[0].data.empty();
    const std::size_t binSize = hasBin ? asset.buffers[0].data.size() : 0;
    const std::size_t binPadded = (binSize + 3u) & ~std::size_t(3);

    std::vector<u8> out;
    std::size_t total = 12 + 8 + jsonPadded;
    if (hasBin) {
        total += 8 + binPadded;
    }
    out.reserve(total);

    appendU32(out, 0x46546C67); // "glTF"
    appendU32(out, 2);
    appendU32(out, static_cast<u32>(total));

    appendU32(out, static_cast<u32>(jsonPadded));
    appendU32(out, 0x4E4F534A); // "JSON"
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), jsonPadded - body.size(), u8(' '));

    if (hasBin) {
        appendU32(out, static_cast<u32>(binPadded));
        appendU32(out, 0x004E4942); // "BIN\0"
        const std::vector<u8>& bin = asset.buffers[0].data;
        out.insert(out.end(), bin.begin(), bin.end());
        out.insert(out.end(), binPadded - binSize, u8(0));
    }
    return out;
}

} // namespace gltf
} // namespace models
} // namespace whiteout
