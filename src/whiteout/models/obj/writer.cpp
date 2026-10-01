// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/obj/writer.h"

#include "../../common/text.h"

namespace whiteout {
namespace models {
namespace obj {

namespace {

void Header(std::string& out, std::string_view header) {
    text::ForEachLine(header, [&](std::string_view line) {
        out += "# ";
        out += line;
        out += '\n';
    });
}

void Floats(std::string& out, const char* keyword, const f32* values, u32 count) {
    out += keyword;
    for (u32 i = 0; i < count; ++i) {
        out += ' ';
        text::AppendF32(out, values[i]);
    }
    out += '\n';
}

void Color(std::string& out, const char* keyword, const std::optional<Vector3f>& value) {
    if (value.has_value()) {
        const f32 values[3] = {value->x, value->y, value->z};
        Floats(out, keyword, values, 3);
    }
}

void Scalar(std::string& out, const char* keyword, const std::optional<f32>& value) {
    if (value.has_value()) {
        Floats(out, keyword, &*value, 1);
    }
}

void Map(std::string& out, const char* keyword, const TextureMap& map) {
    if (!map.present()) {
        return;
    }
    out += keyword;
    if (map.bumpMultiplier != 1.0f) {
        out += " -bm ";
        text::AppendF32(out, map.bumpMultiplier);
    }
    if (map.clamp) {
        out += " -clamp on";
    }
    if (map.offset.x != 0.0f || map.offset.y != 0.0f) {
        out += " -o ";
        text::AppendF32(out, map.offset.x);
        out += ' ';
        text::AppendF32(out, map.offset.y);
    }
    if (map.scale.x != 1.0f || map.scale.y != 1.0f) {
        out += " -s ";
        text::AppendF32(out, map.scale.x);
        out += ' ';
        text::AppendF32(out, map.scale.y);
    }
    out += ' ';
    out += map.file;
    out += '\n';
}

} // namespace

std::string Writer::ToText(const Asset& asset, std::string_view header) {
    std::string out;
    out.reserve(asset.positions.size() * 40 + asset.corners.size() * 16);
    Header(out, header);
    for (const std::string& library : asset.materialLibraries) {
        out += "mtllib ";
        out += library;
        out += '\n';
    }

    const bool colored = asset.colors.size() == asset.positions.size() && !asset.colors.empty();
    for (std::size_t i = 0; i < asset.positions.size(); ++i) {
        const Vector3f& p = asset.positions[i];
        if (colored) {
            const Vector3f& c = asset.colors[i];
            const f32 values[6] = {p.x, p.y, p.z, c.x, c.y, c.z};
            Floats(out, "v", values, 6);
        } else {
            const f32 values[3] = {p.x, p.y, p.z};
            Floats(out, "v", values, 3);
        }
    }
    for (const Vector2f& uv : asset.uvs) {
        const f32 values[2] = {uv.x, uv.y};
        Floats(out, "vt", values, 2);
    }
    for (const Vector3f& n : asset.normals) {
        const f32 values[3] = {n.x, n.y, n.z};
        Floats(out, "vn", values, 3);
    }

    const std::string* object = nullptr;
    const std::string* group = nullptr;
    u32 material = kNone;
    u32 smoothing = kNone;
    for (const Face& face : asset.faces) {
        if (face.group < asset.groups.size()) {
            const Group& g = asset.groups[face.group];
            if (object == nullptr || *object != g.object) {
                object = &g.object;
                group = nullptr;
                if (!g.object.empty()) {
                    out += "o ";
                    out += g.object;
                    out += '\n';
                }
            }
            if (group == nullptr || *group != g.group) {
                group = &g.group;
                if (!g.group.empty()) {
                    out += "g ";
                    out += g.group;
                    out += '\n';
                }
            }
        }
        if (face.material != material && face.material < asset.materials.size()) {
            material = face.material;
            out += "usemtl ";
            out += asset.materials[material];
            out += '\n';
        }
        if (face.smoothing != smoothing) {
            smoothing = face.smoothing;
            if (smoothing == 0) {
                out += "s off\n";
            } else {
                out += "s ";
                out += std::to_string(smoothing);
                out += '\n';
            }
        }
        out += 'f';
        for (u32 k = 0; k < face.cornerCount; ++k) {
            const Corner& corner = asset.corners[face.firstCorner + k];
            out += ' ';
            out += std::to_string(static_cast<u64>(corner.position) + 1);
            if (corner.uv != kNone || corner.normal != kNone) {
                out += '/';
                if (corner.uv != kNone) {
                    out += std::to_string(static_cast<u64>(corner.uv) + 1);
                }
                if (corner.normal != kNone) {
                    out += '/';
                    out += std::to_string(static_cast<u64>(corner.normal) + 1);
                }
            }
        }
        out += '\n';
    }
    return out;
}

std::string Writer::MaterialsToText(const MaterialLibrary& library, std::string_view header) {
    std::string out;
    Header(out, header);
    for (const Material& material : library.materials) {
        out += "\nnewmtl ";
        out += material.name;
        out += '\n';
        Color(out, "Ka", material.ambient);
        Color(out, "Kd", material.diffuse);
        Color(out, "Ks", material.specular);
        Color(out, "Ke", material.emissive);
        Scalar(out, "Ns", material.exponent);
        Scalar(out, "Ni", material.ior);
        Scalar(out, "d", material.dissolve);
        if (material.illum.has_value()) {
            out += "illum ";
            out += std::to_string(*material.illum);
            out += '\n';
        }
        Scalar(out, "Pr", material.roughness);
        Scalar(out, "Pm", material.metallic);
        Map(out, "map_Kd", material.diffuseMap);
        Map(out, "map_Ks", material.specularMap);
        Map(out, "map_Ns", material.exponentMap);
        Map(out, "map_d", material.dissolveMap);
        Map(out, "map_Ke", material.emissiveMap);
        Map(out, "map_Bump", material.bumpMap);
        Map(out, "norm", material.normalMap);
        Map(out, "map_Pr", material.roughnessMap);
        Map(out, "map_Pm", material.metallicMap);
    }
    return out;
}

} // namespace obj
} // namespace models
} // namespace whiteout
