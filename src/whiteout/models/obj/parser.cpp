// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/obj/parser.h"

#include <unordered_map>

#include "../../common/text.h"

namespace whiteout {
namespace models {
namespace obj {

namespace {

using text::NextToken;
using text::Trim;

/// More warnings than this say nothing new; the count still does.
constexpr std::size_t kWarningCap = 32;

void Warn(std::vector<std::string>& warnings, u32 line, const std::string& message) {
    if (warnings.size() < kWarningCap) {
        warnings.push_back("line " + std::to_string(line) + ": " + message);
    }
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if ((a[i] | 0x20) != (b[i] | 0x20)) {
            return false;
        }
    }
    return true;
}

bool EndsWithNoCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           EqualsNoCase(text.substr(text.size() - suffix.size()), suffix);
}

/// Up to @p count floats off @p rest; returns how many parsed.
u32 ReadFloats(std::string_view& rest, f32* out, u32 count, bool& malformed) {
    u32 read = 0;
    while (read < count) {
        const std::string_view token = NextToken(rest);
        if (token.empty()) {
            break;
        }
        if (!text::ParseF32(token, out[read])) {
            malformed = true;
            break;
        }
        ++read;
    }
    return read;
}

/// A 1-based (or negative, relative) pool index to 0-based; `kNone` if bad.
u32 ResolveIndex(std::string_view token, std::size_t poolSize) {
    i64 value = 0;
    if (token.empty() || !text::ParseI64(token, value) || value == 0) {
        return kNone;
    }
    const i64 resolved = value > 0 ? value - 1 : static_cast<i64>(poolSize) + value;
    if (resolved < 0 || resolved >= static_cast<i64>(poolSize)) {
        return kNone;
    }
    return static_cast<u32>(resolved);
}

/// Joins `\`-continued lines; the format allows a statement to wrap.
template <class Fn>
void ForEachStatement(std::string_view source, Fn&& fn) {
    std::string joined;
    u32 lineNumber = 0;
    u32 statementLine = 0;
    text::ForEachLine(source, [&](std::string_view line) {
        ++lineNumber;
        if (joined.empty()) {
            statementLine = lineNumber;
        }
        const std::string_view trimmed = Trim(line);
        if (!trimmed.empty() && trimmed.back() == '\\') {
            joined.append(trimmed.substr(0, trimmed.size() - 1));
            joined.push_back(' ');
            return;
        }
        if (!joined.empty()) {
            joined.append(line);
            fn(std::string_view(joined), statementLine);
            joined.clear();
            return;
        }
        fn(line, statementLine);
    });
    if (!joined.empty()) {
        fn(std::string_view(joined), statementLine);
    }
}

/// The statement with its comment cut off, and its keyword popped.
std::string_view SplitKeyword(std::string_view line, std::string_view& rest) {
    const std::size_t hash = line.find('#');
    if (hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    rest = line;
    return NextToken(rest);
}

bool LooksLikeText(std::string_view source) {
    const std::size_t probe = source.size() < 4096 ? source.size() : 4096;
    for (std::size_t i = 0; i < probe; ++i) {
        if (source[i] == '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

ParseOutcome Parser::FromText(std::string_view source) {
    ParseOutcome outcome;
    if (!LooksLikeText(source)) {
        outcome.error = "not an OBJ text file (NUL bytes in the header)";
        return outcome;
    }
    Asset asset;
    std::unordered_map<std::string, u32> groupIndex;
    std::unordered_map<std::string, u32> materialIndex;
    std::string object;
    std::string group;
    u32 currentGroup = kNone;
    u32 currentMaterial = kNone;
    u32 currentSmoothing = 0;
    u32 droppedFaces = 0;
    std::vector<Corner> scratch;

    const auto groupFor = [&]() {
        if (currentGroup == kNone) {
            std::string key = object;
            key.push_back('\0');
            key += group;
            auto found = groupIndex.find(key);
            if (found == groupIndex.end()) {
                asset.groups.push_back(Group{object, group});
                found = groupIndex.emplace(std::move(key), static_cast<u32>(asset.groups.size() - 1))
                            .first;
            }
            currentGroup = found->second;
        }
        return currentGroup;
    };

    ForEachStatement(source, [&](std::string_view line, u32 lineNumber) {
        std::string_view rest;
        const std::string_view keyword = SplitKeyword(line, rest);
        if (keyword.empty()) {
            return;
        }
        bool malformed = false;
        if (keyword == "v") {
            f32 values[7] = {0, 0, 0, 0, 0, 0, 0};
            const u32 read = ReadFloats(rest, values, 7, malformed);
            if (read < 3) {
                Warn(outcome.warnings, lineNumber, "a vertex with fewer than three coordinates");
                asset.positions.push_back(Vector3f{0, 0, 0});
            } else {
                asset.positions.push_back(Vector3f{values[0], values[1], values[2]});
            }
            // `x y z r g b` is the colour extension; `x y z w` is a rational
            // weight nothing here can use.
            if (read == 6 || read == 7) {
                const u32 base = read == 7 ? 4 : 3;
                if (asset.colors.size() + 1 < asset.positions.size()) {
                    asset.colors.resize(asset.positions.size() - 1, Vector3f{1, 1, 1});
                }
                asset.colors.push_back(Vector3f{values[base], values[base + 1], values[base + 2]});
            } else if (!asset.colors.empty()) {
                asset.colors.push_back(Vector3f{1, 1, 1});
            }
        } else if (keyword == "vt") {
            f32 values[3] = {0, 0, 0};
            if (ReadFloats(rest, values, 3, malformed) < 1) {
                Warn(outcome.warnings, lineNumber, "a texture coordinate with no value");
            }
            asset.uvs.push_back(Vector2f{values[0], values[1]});
        } else if (keyword == "vn") {
            f32 values[3] = {0, 0, 0};
            if (ReadFloats(rest, values, 3, malformed) < 3) {
                Warn(outcome.warnings, lineNumber, "a normal with fewer than three components");
            }
            asset.normals.push_back(Vector3f{values[0], values[1], values[2]});
        } else if (keyword == "f" || keyword == "fo") {
            scratch.clear();
            bool bad = false;
            for (std::string_view token = NextToken(rest); !token.empty();
                 token = NextToken(rest)) {
                Corner corner;
                const std::size_t slash1 = token.find('/');
                const std::string_view p = token.substr(0, slash1);
                corner.position = ResolveIndex(p, asset.positions.size());
                if (corner.position == kNone) {
                    bad = true;
                }
                if (slash1 != std::string_view::npos) {
                    const std::string_view tail = token.substr(slash1 + 1);
                    const std::size_t slash2 = tail.find('/');
                    const std::string_view t = tail.substr(0, slash2);
                    if (!t.empty()) {
                        corner.uv = ResolveIndex(t, asset.uvs.size());
                        bad = bad || corner.uv == kNone;
                    }
                    if (slash2 != std::string_view::npos) {
                        const std::string_view n = tail.substr(slash2 + 1);
                        if (!n.empty()) {
                            corner.normal = ResolveIndex(n, asset.normals.size());
                            bad = bad || corner.normal == kNone;
                        }
                    }
                }
                scratch.push_back(corner);
            }
            if (bad || scratch.size() < 3) {
                ++droppedFaces;
                Warn(outcome.warnings, lineNumber,
                     bad ? "a face naming a vertex, UV or normal past the end of its pool"
                         : "a face with fewer than three corners");
                return;
            }
            Face face;
            face.firstCorner = static_cast<u32>(asset.corners.size());
            face.cornerCount = static_cast<u32>(scratch.size());
            face.group = groupFor();
            face.material = currentMaterial;
            face.smoothing = currentSmoothing;
            asset.corners.insert(asset.corners.end(), scratch.begin(), scratch.end());
            asset.faces.push_back(face);
        } else if (keyword == "o") {
            object = std::string(Trim(rest));
            currentGroup = kNone;
        } else if (keyword == "g") {
            group = std::string(Trim(rest));
            currentGroup = kNone;
        } else if (keyword == "s") {
            asset.smoothingStated = true;
            const std::string_view value = NextToken(rest);
            i64 number = 0;
            if (EqualsNoCase(value, "off") || value.empty()) {
                currentSmoothing = 0;
            } else if (text::ParseI64(value, number) && number >= 0 && number <= 0xFFFFFFFF) {
                currentSmoothing = static_cast<u32>(number);
            } else {
                Warn(outcome.warnings, lineNumber, "an unreadable smoothing group; taken as off");
                currentSmoothing = 0;
            }
        } else if (keyword == "usemtl") {
            const std::string name(Trim(rest));
            auto found = materialIndex.find(name);
            if (found == materialIndex.end()) {
                asset.materials.push_back(name);
                found = materialIndex.emplace(name, static_cast<u32>(asset.materials.size() - 1))
                            .first;
            }
            currentMaterial = found->second;
        } else if (keyword == "mtllib") {
            // Several libraries, or one name with spaces in it: Blender writes
            // `mtllib My Model.mtl`. Only a list whose every entry is an .mtl
            // is a list.
            const std::string_view all = Trim(rest);
            std::vector<std::string_view> tokens;
            std::string_view scan = all;
            for (std::string_view token = NextToken(scan); !token.empty();
                 token = NextToken(scan)) {
                tokens.push_back(token);
            }
            bool list = tokens.size() > 1;
            for (const std::string_view token : tokens) {
                list = list && EndsWithNoCase(token, ".mtl");
            }
            if (list) {
                for (const std::string_view token : tokens) {
                    asset.materialLibraries.emplace_back(token);
                }
            } else if (!all.empty()) {
                asset.materialLibraries.emplace_back(all);
            }
        } else if (keyword == "l" || keyword == "p" || keyword == "curv" || keyword == "curv2" ||
                   keyword == "surf") {
            ++asset.skippedElements;
        } else if (keyword == "vp" || keyword == "cstype" || keyword == "deg" ||
                   keyword == "bmat" || keyword == "step" || keyword == "parm" ||
                   keyword == "trim" || keyword == "hole" || keyword == "scrv" ||
                   keyword == "sp" || keyword == "end" || keyword == "con" ||
                   keyword == "usemap" || keyword == "maplib" || keyword == "mg" ||
                   keyword == "lod" || keyword == "bevel" || keyword == "c_interp" ||
                   keyword == "d_interp" || keyword == "shadow_obj" || keyword == "trace_obj" ||
                   keyword == "ctech" || keyword == "stech") {
            // Free-form and display statements: nothing a mesh holds.
        } else {
            Warn(outcome.warnings, lineNumber,
                 "unknown statement '" + std::string(keyword) + "' ignored");
        }
        if (malformed) {
            Warn(outcome.warnings, lineNumber, "a number that does not parse");
        }
    });

    if (droppedFaces > kWarningCap) {
        outcome.warnings.push_back(std::to_string(droppedFaces) + " malformed faces dropped");
    }
    if (!asset.colors.empty() && asset.colors.size() < asset.positions.size()) {
        asset.colors.resize(asset.positions.size(), Vector3f{1, 1, 1});
    }
    outcome.asset = std::move(asset);
    return outcome;
}

namespace {

/// Splits `-option args… file` into its options and the file name. Options
/// this module does not keep are skipped with their arguments; the file is the
/// rest of the statement, spaces and all.
TextureMap ReadTextureMap(std::string_view rest, u32 lineNumber,
                          std::vector<std::string>& warnings) {
    TextureMap map;
    for (;;) {
        std::string_view probe = rest;
        const std::string_view token = NextToken(probe);
        if (token.size() < 2 || token[0] != '-') {
            break;
        }
        rest = probe;
        const std::string_view option = token.substr(1);
        // How many numeric arguments each option takes (at most).
        u32 arguments = 1;
        if (option == "o" || option == "s" || option == "t") {
            arguments = 3;
        } else if (option == "mm") {
            arguments = 2;
        }
        f32 values[3] = {0, 0, 0};
        u32 read = 0;
        if (option == "clamp" || option == "blendu" || option == "blendv" || option == "cc") {
            map.clamp = option == "clamp" ? EqualsNoCase(NextToken(rest), "on") : map.clamp;
            if (option != "clamp") {
                NextToken(rest);
            }
            continue;
        }
        if (option == "imfchan" || option == "type") {
            NextToken(rest);
            continue;
        }
        while (read < arguments) {
            std::string_view look = rest;
            const std::string_view value = NextToken(look);
            f32 parsed = 0.0f;
            if (value.empty() || !text::ParseF32(value, parsed)) {
                break;
            }
            values[read++] = parsed;
            rest = look;
        }
        if (option == "o" && read >= 2) {
            map.offset = Vector2f{values[0], values[1]};
        } else if (option == "s" && read >= 2) {
            map.scale = Vector2f{values[0], values[1]};
        } else if (option == "bm" && read >= 1) {
            map.bumpMultiplier = values[0];
        } else if (read == 0) {
            Warn(warnings, lineNumber, "texture option -" + std::string(option) + " has no value");
        }
    }
    map.file = std::string(Trim(rest));
    return map;
}

bool ReadColor(std::string_view rest, Vector3f& out) {
    f32 values[3] = {0, 0, 0};
    bool malformed = false;
    const u32 read = ReadFloats(rest, values, 3, malformed);
    if (read == 0 || malformed) {
        return false;
    }
    // `Kd 0.5` is a grey: one value fills all three.
    out = read < 3 ? Vector3f{values[0], values[0], values[0]}
                   : Vector3f{values[0], values[1], values[2]};
    return true;
}

bool ReadScalar(std::string_view rest, f32& out) {
    return text::ParseF32(NextToken(rest), out);
}

} // namespace

MtlParseOutcome Parser::MaterialsFromText(std::string_view source) {
    MtlParseOutcome outcome;
    Material* current = nullptr;
    ForEachStatement(source, [&](std::string_view line, u32 lineNumber) {
        std::string_view rest;
        const std::string_view keyword = SplitKeyword(line, rest);
        if (keyword.empty()) {
            return;
        }
        if (keyword == "newmtl") {
            outcome.library.materials.push_back(Material{});
            current = &outcome.library.materials.back();
            current->name = std::string(Trim(rest));
            return;
        }
        if (current == nullptr) {
            Warn(outcome.warnings, lineNumber, "a statement before any newmtl");
            return;
        }
        Vector3f color;
        f32 scalar = 0.0f;
        bool understood = true;
        if (keyword == "Ka") {
            understood = ReadColor(rest, color);
            current->ambient = color;
        } else if (keyword == "Kd") {
            understood = ReadColor(rest, color);
            current->diffuse = color;
        } else if (keyword == "Ks") {
            understood = ReadColor(rest, color);
            current->specular = color;
        } else if (keyword == "Ke") {
            understood = ReadColor(rest, color);
            current->emissive = color;
        } else if (keyword == "Ns") {
            understood = ReadScalar(rest, scalar);
            current->exponent = scalar;
        } else if (keyword == "d") {
            // `d -halo f` is a facing-ratio dissolve nothing renders; keep f.
            std::string_view probe = rest;
            if (NextToken(probe) == "-halo") {
                rest = probe;
            }
            understood = ReadScalar(rest, scalar);
            current->dissolve = scalar;
        } else if (keyword == "Tr") {
            understood = ReadScalar(rest, scalar);
            current->dissolve = 1.0f - scalar;
        } else if (keyword == "Ni") {
            understood = ReadScalar(rest, scalar);
            current->ior = scalar;
        } else if (keyword == "illum") {
            i64 value = 0;
            understood = text::ParseI64(NextToken(rest), value);
            current->illum = static_cast<i32>(value);
        } else if (keyword == "Pr") {
            understood = ReadScalar(rest, scalar);
            current->roughness = scalar;
        } else if (keyword == "Pm") {
            understood = ReadScalar(rest, scalar);
            current->metallic = scalar;
        } else if (keyword == "map_Kd") {
            current->diffuseMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_Ks") {
            current->specularMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_Ns") {
            current->exponentMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_d") {
            current->dissolveMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_Ke") {
            current->emissiveMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (EqualsNoCase(keyword, "map_Bump") || keyword == "bump") {
            current->bumpMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "norm" || keyword == "map_Kn") {
            current->normalMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_Pr") {
            current->roughnessMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else if (keyword == "map_Pm") {
            current->metallicMap = ReadTextureMap(rest, lineNumber, outcome.warnings);
        } else {
            // Tf, sharpness, refl, map_Ka, disp, decal, Ps, Pc, aniso…: no WEM home.
            return;
        }
        if (!understood) {
            Warn(outcome.warnings, lineNumber,
                 "'" + std::string(keyword) + "' has a value that does not parse");
        }
    });
    return outcome;
}

} // namespace obj
} // namespace models
} // namespace whiteout
