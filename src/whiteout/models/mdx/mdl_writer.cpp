// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "mdl_writer.h"

#include <whiteout/models/mdx/structures.h>
#include <whiteout/vector_types.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>

namespace whiteout {
namespace mdx {

namespace {

// ============================================================================
// Text formatting helpers
// ============================================================================

class MdlTextWriter {
public:
    MdlTextWriter(const Model& model) : m_model(model) {}

    std::string write() {
        writeVersion();
        writeModel();
        writeSequences();
        writeGlobalSequences();
        writeTextures();
        writeMaterials();
        writeTextureAnims();
        writeGeosets();
        writeGeosetAnims();
        writeBones();
        writeLights();
        writeHelpers();
        writeAttachments();
        writePivotPoints();
        writeParticleEmitters();
        writeParticleEmitters2();
        writeRibbonEmitters();
        writeCornEmitters();
        writeEventObjects();
        writeCameras();
        writeCollisionShapes();
        writeFaceEffects();
        writeBindPose();
        return m_out.str();
    }

private:
    const Model& m_model;
    std::ostringstream m_out;
    int m_indent = 0;

    // ========================================================================
    // Indentation / basic output
    // ========================================================================

    void indent() { ++m_indent; }
    void dedent() { --m_indent; }

    void writeIndent() {
        for (int i = 0; i < m_indent; ++i)
            m_out << '\t';
    }

    void line(const std::string& s) {
        writeIndent();
        m_out << s << '\n';
    }

    void openBlock(const std::string& header) {
        writeIndent();
        m_out << header << " {\n";
        indent();
    }

    void closeBlock() {
        dedent();
        line("}");
    }

    // ========================================================================
    // Value formatting
    // ========================================================================

    static std::string fmtFloat(f32 v) {
        // Use enough precision but strip trailing zeros
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
        // Trim trailing zeros after decimal point
        char* dot = std::strchr(buf, '.');
        if (dot) {
            char* end = buf + std::strlen(buf) - 1;
            while (end > dot && *end == '0')
                --end;
            if (end == dot)
                *(end + 1) = '\0'; // keep one zero: "1.0"
            else
                *(end + 1) = '\0';
            // Ensure at least one digit after dot
            if (buf[std::strlen(buf) - 1] == '.')
                std::strcat(buf, "0");
        }
        return buf;
    }

    static std::string fmtVec3(const Vector3f& v) {
        return "{ " + fmtFloat(v.x) + ", " + fmtFloat(v.y) + ", " + fmtFloat(v.z) + " }";
    }

    static std::string fmtVec4(const Vector4f& v) {
        return "{ " + fmtFloat(v.x) + ", " + fmtFloat(v.y) + ", " + fmtFloat(v.z) + ", " +
               fmtFloat(v.w) + " }";
    }

    static std::string fmtQuat(const Quaternion& q) {
        return "{ " + fmtFloat(q.x) + ", " + fmtFloat(q.y) + ", " + fmtFloat(q.z) + ", " +
               fmtFloat(q.w) + " }";
    }

    static std::string fmtVec2(const Vector2f& v) {
        return "{ " + fmtFloat(v.x) + ", " + fmtFloat(v.y) + " }";
    }

    static std::string fmtExtent(const Extent& e, const std::string& prefix) {
        return prefix + "MinimumExtent " + fmtVec3(e.minimum) + ",\n" + prefix +
               "MaximumExtent " + fmtVec3(e.maximum) + ",\n" + prefix + "BoundsRadius " +
               fmtFloat(e.boundsRadius) + ",";
    }

    void writeExtent(const Extent& e) {
        line("MinimumExtent " + fmtVec3(e.minimum) + ",");
        line("MaximumExtent " + fmtVec3(e.maximum) + ",");
        line("BoundsRadius " + fmtFloat(e.boundsRadius) + ",");
    }

    static std::string quoted(const std::string& s) { return "\"" + s + "\""; }

    // ========================================================================
    // Interpolation type string
    // ========================================================================

    static const char* interpName(InterpolationType t) {
        switch (t) {
            case InterpolationType::Linear: return "Linear";
            case InterpolationType::Hermite: return "Hermite";
            case InterpolationType::Bezier: return "Bezier";
            default: return "DontInterp";
        }
    }

    // ========================================================================
    // Track writing (generic)
    // ========================================================================

    // Format a value for track output
    static std::string fmtTrackValue(f32 v) { return fmtFloat(v); }
    static std::string fmtTrackValue(u32 v) { return std::to_string(v); }
    static std::string fmtTrackValue(const Vector3f& v) { return fmtVec3(v); }
    static std::string fmtTrackValue(const Vector4f& v) { return fmtVec4(v); }
    static std::string fmtTrackValue(const Quaternion& q) { return fmtQuat(q); }

    template <typename T>
    void writeTrack(const std::string& name, const Track<T>& track) {
        if (!track.isUsed || track.keyCount == 0) return;

        bool smooth = isSmoothInterpolation(track.interpolationType);

        openBlock(name + " " + std::to_string(track.keyCount));
        line(std::string(interpName(track.interpolationType)) + ",");
        if (track.globalSequenceId != 0xFFFFFFFF) {
            line("GlobalSeqId " + std::to_string(track.globalSequenceId) + ",");
        }

        if (smooth) {
            auto keys = const_cast<Track<T>&>(track).tangentKeys();
            for (size_t i = 0; i < keys.size(); ++i) {
                auto& k = keys[i];
                line(std::to_string(k.frame) + ": " + fmtTrackValue(k.value) + ",");
                indent();
                line("InTan " + fmtTrackValue(k.inTan) + ",");
                line("OutTan " + fmtTrackValue(k.outTan) + ",");
                dedent();
            }
        } else {
            auto keys = const_cast<Track<T>&>(track).keys();
            for (size_t i = 0; i < keys.size(); ++i) {
                auto& k = keys[i];
                line(std::to_string(k.frame) + ": " + fmtTrackValue(k.value) + ",");
            }
        }

        closeBlock();
    }

    // Write a track or a static value for properties that can be either.
    // If the track has exactly 1 key with frame 0 and None interpolation, write static.
    template <typename T>
    void writeTrackOrStatic(const std::string& name, const Track<T>& track, const T& /*staticVal*/) {
        if (!track.isUsed) return;

        // Check if it's effectively a static value
        if (track.keyCount == 1 && track.interpolationType == InterpolationType::None) {
            auto keys = const_cast<Track<T>&>(track).keys();
            if (keys.size() == 1 && keys[0].frame == 0) {
                line("static " + name + " " + fmtTrackValue(keys[0].value) + ",");
                return;
            }
        }

        writeTrack(name, track);
    }

    // ========================================================================
    // Node flags
    // ========================================================================

    void writeNodeFlags(const Node& node) {
        auto flags = node.flags;
        // Only write behavioral flags, not type flags
        if (mdx::hasFlag(flags, Node::NodeFlag::DontInheritTranslation))
            line("DontInheritTranslation,");
        if (mdx::hasFlag(flags, Node::NodeFlag::DontInheritRotation))
            line("DontInheritRotation,");
        if (mdx::hasFlag(flags, Node::NodeFlag::DontInheritScaling))
            line("DontInheritScaling,");
        if (mdx::hasFlag(flags, Node::NodeFlag::Billboarded)) line("Billboarded,");
        if (mdx::hasFlag(flags, Node::NodeFlag::BillboardedLockX))
            line("BillboardedLockX,");
        if (mdx::hasFlag(flags, Node::NodeFlag::BillboardedLockY))
            line("BillboardedLockY,");
        if (mdx::hasFlag(flags, Node::NodeFlag::BillboardedLockZ))
            line("BillboardedLockZ,");
        if (mdx::hasFlag(flags, Node::NodeFlag::CameraAnchored)) line("CameraAnchored,");
    }

    // ========================================================================
    // Node fields (common for Bone, Helper, Light, etc.)
    // ========================================================================

    void writeNodeFields(const Node& node) {
        line("ObjectId " + std::to_string(node.objectId) + ",");
        if (node.parentId != Node::NO_PARENT) {
            line("Parent " + std::to_string(node.parentId) + ",");
        }
        writeNodeFlags(node);
        writeTrack<Vector3f>("Translation", node.translationTracks);
        writeTrack<Quaternion>("Rotation", node.rotationTracks);
        writeTrack<Vector3f>("Scaling", node.scalingTracks);
    }

    // ========================================================================
    // Top-level block writers
    // ========================================================================

    void writeVersion() {
        openBlock("Version");
        line("FormatVersion " + std::to_string(m_model.version) + ",");
        closeBlock();
    }

    void writeModel() {
        openBlock("Model " + quoted(m_model.modelName));
        if (!m_model.animationFileName.empty()) {
            line("AnimationFileName " + quoted(m_model.animationFileName) + ",");
        }
        if (m_model.blendTime != 0) {
            line("BlendTime " + std::to_string(m_model.blendTime) + ",");
        }
        writeExtent(m_model.modelExtent);
        closeBlock();
    }

    void writeSequences() {
        if (m_model.sequences.empty()) return;
        openBlock("Sequences " + std::to_string(m_model.sequences.size()));
        for (auto& seq : m_model.sequences) {
            openBlock("Anim " + quoted(seq.name));
            line("Interval { " + std::to_string(seq.intervalStart) + ", " +
                 std::to_string(seq.intervalEnd) + " },");
            if (mdx::hasFlag(seq.flags, Sequence::Flag::NonLooping)) line("NonLooping,");
            if (seq.moveSpeed != 0.0f)
                line("MoveSpeed " + fmtFloat(seq.moveSpeed) + ",");
            if (seq.rarity != 0.0f)
                line("Rarity " + fmtFloat(seq.rarity) + ",");
            writeExtent(seq.extent);
            closeBlock();
        }
        closeBlock();
    }

    void writeGlobalSequences() {
        if (m_model.globalSequences.empty()) return;
        openBlock("GlobalSequences " + std::to_string(m_model.globalSequences.size()));
        for (auto dur : m_model.globalSequences) {
            line("Duration " + std::to_string(dur) + ",");
        }
        closeBlock();
    }

    void writeTextures() {
        if (m_model.textures.empty()) return;
        openBlock("Textures " + std::to_string(m_model.textures.size()));
        for (auto& tex : m_model.textures) {
            openBlock("Bitmap");
            line("Image " + quoted(tex.fileName) + ",");
            if (tex.replaceableId != 0)
                line("ReplaceableId " + std::to_string(tex.replaceableId) + ",");
            if (mdx::hasFlag(tex.flags, Texture::Flag::WrapWidth)) line("WrapWidth,");
            if (mdx::hasFlag(tex.flags, Texture::Flag::WrapHeight)) line("WrapHeight,");
            closeBlock();
        }
        closeBlock();
    }

    void writeMaterials() {
        if (m_model.materials.empty()) return;
        openBlock("Materials " + std::to_string(m_model.materials.size()));
        for (auto& mat : m_model.materials) {
            openBlock("Material");
            if (mat.priorityPlane != 0)
                line("PriorityPlane " + std::to_string(mat.priorityPlane) + ",");
            // Material flags as identifiers
            if (mdx::hasFlag(mat.flags, Material::Flag::ConstantColor)) line("ConstantColor,");
            if (mdx::hasFlag(mat.flags, Material::Flag::SortPrimitives)) line("SortPrimitives,");
            if (mdx::hasFlag(mat.flags, Material::Flag::FullResolution)) line("FullResolution,");
            if (mdx::hasFlag(mat.flags, Material::Flag::TwoSided)) line("TwoSided,");
            if (!mat.shader.empty())
                line("Shader " + quoted(mat.shader) + ",");

            for (auto& layer : mat.layers) {
                writeLayer(layer);
            }
            closeBlock();
        }
        closeBlock();
    }

    void writeLayer(const Layer& layer) {
        openBlock("Layer");
        // FilterMode
        const char* fmName = "None";
        switch (layer.filterMode) {
            case Layer::FilterMode::None: fmName = "None"; break;
            case Layer::FilterMode::Transparent: fmName = "Transparent"; break;
            case Layer::FilterMode::Blend: fmName = "Blend"; break;
            case Layer::FilterMode::Additive: fmName = "Additive"; break;
            case Layer::FilterMode::AddAlpha: fmName = "AddAlpha"; break;
            case Layer::FilterMode::Modulate: fmName = "Modulate"; break;
            case Layer::FilterMode::Modulate2x: fmName = "Modulate2x"; break;
            default: break;
        }
        line(std::string("FilterMode ") + fmName + ",");

        // Shading flags
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::Unshaded))
            line("Unshaded,");
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::SphereEnvMap))
            line("SphereEnvMap,");
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::TwoSided))
            line("TwoSided,");
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::Unfogged))
            line("Unfogged,");
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::NoDepthTest))
            line("NoDepthTest,");
        if (mdx::hasFlag(layer.shadingFlags, Layer::ShadingFlag::NoDepthSet))
            line("NoDepthSet,");

        // ShaderTypeId (Reforged HD/SD flag)
        if (m_model.version >= 1100) {
            line("ShaderTypeId " + std::to_string(layer.is_hd ? 1 : 0) + ",");
        }

        // TextureID / sub-texture slots
        if (layer.is_hd && !layer.subTextures.empty()) {
            for (const auto& subTex : layer.subTextures) {
                const char* slotName = "TextureID";
                switch (subTex.slot) {
                    case Layer::SlotType::DiffuseMap: slotName = "TextureID"; break;
                    case Layer::SlotType::NormalMap: slotName = "NormalTextureID"; break;
                    case Layer::SlotType::ORMMap: slotName = "ORMTextureID"; break;
                    case Layer::SlotType::EmissiveMap: slotName = "EmissiveTextureID"; break;
                    case Layer::SlotType::TeamColor: slotName = "TeamColorTextureID"; break;
                    case Layer::SlotType::EnvironmentMap: slotName = "ReflectionsTextureID"; break;
                    default: slotName = "TextureID"; break;
                }
                if (subTex.tracks.isUsed && subTex.tracks.keyCount > 0) {
                    writeTrackOrStatic<u32>(slotName, subTex.tracks, subTex.textureId);
                } else {
                    line("static " + std::string(slotName) + " " +
                         std::to_string(subTex.textureId) + ",");
                }
            }
        } else if (layer.textureIdTracks.isUsed && layer.textureIdTracks.keyCount > 0) {
            // Check for static case
            if (layer.textureIdTracks.keyCount == 1 &&
                layer.textureIdTracks.interpolationType == InterpolationType::None) {
                auto keys = const_cast<Track<u32>&>(layer.textureIdTracks).keys();
                if (keys.size() == 1 && keys[0].frame == 0) {
                    line("static TextureID " + std::to_string(keys[0].value) + ",");
                } else {
                    writeTrack<u32>("TextureID", layer.textureIdTracks);
                }
            } else {
                writeTrack<u32>("TextureID", layer.textureIdTracks);
            }
        } else {
            line("static TextureID " + std::to_string(layer.textureId) + ",");
        }

        if (layer.textureAnimationId != 0 && layer.textureAnimationId != 0xFFFFFFFF)
            line("TVertexAnimId " + std::to_string(layer.textureAnimationId) + ",");
        if (layer.coordId != 0)
            line("CoordId " + std::to_string(layer.coordId) + ",");

        // Alpha — track or static
        if (layer.alphaTracks.isUsed) {
            writeTrackOrStatic<f32>("Alpha", layer.alphaTracks, layer.alpha);
        } else if (layer.alpha != 1.0f) {
            line("static Alpha " + fmtFloat(layer.alpha) + ",");
        }

        // Reforged PBR properties
        if (layer.emissiveGain != 0.0f || layer.emissiveGainTracks.isUsed) {
            if (layer.emissiveGainTracks.isUsed) {
                writeTrackOrStatic<f32>("EmissiveGain", layer.emissiveGainTracks,
                                        layer.emissiveGain);
            } else {
                line("static EmissiveGain " + fmtFloat(layer.emissiveGain) + ",");
            }
        }
        if (layer.fresnelColorTracks.isUsed) {
            writeTrackOrStatic<Vector3f>("FresnelColor", layer.fresnelColorTracks,
                                          layer.fresnelColor);
        } else if (layer.fresnelColor.x != 1.0f || layer.fresnelColor.y != 1.0f ||
                   layer.fresnelColor.z != 1.0f) {
            line("static FresnelColor " + fmtVec3(layer.fresnelColor) + ",");
        }
        if (layer.fresnelAlphaTracks.isUsed) {
            writeTrackOrStatic<f32>("FresnelOpacity", layer.fresnelAlphaTracks,
                                     layer.fresnelOpacity);
        } else if (layer.fresnelOpacity != 0.0f) {
            line("static FresnelOpacity " + fmtFloat(layer.fresnelOpacity) + ",");
        }
        if (layer.fresnelTeamColorTracks.isUsed) {
            writeTrackOrStatic<f32>("FresnelTeamColor", layer.fresnelTeamColorTracks,
                                     layer.fresnelTeamColor);
        } else if (layer.fresnelTeamColor != 0.0f) {
            line("static FresnelTeamColor " + fmtFloat(layer.fresnelTeamColor) + ",");
        }

        closeBlock();
    }

    void writeTextureAnims() {
        if (m_model.textureAnimations.empty()) return;
        openBlock("TextureAnims " + std::to_string(m_model.textureAnimations.size()));
        for (auto& ta : m_model.textureAnimations) {
            openBlock("TVertexAnim");
            writeTrack<Vector3f>("Translation", ta.translationTracks);
            writeTrack<Quaternion>("Rotation", ta.rotationTracks);
            writeTrack<Vector3f>("Scaling", ta.scalingTracks);
            closeBlock();
        }
        closeBlock();
    }

    void writeGeosets() {
        if (m_model.geosets.empty()) return;
        for (auto& geo : m_model.geosets) {
            writeGeoset(geo);
        }
    }

    void writeGeoset(const Geoset& geo) {
        openBlock("Geoset");

        // Vertices
        openBlock("Vertices " + std::to_string(geo.vertexPositions.size()));
        for (auto& v : geo.vertexPositions) {
            line(fmtVec3(v) + ",");
        }
        closeBlock();

        // Normals
        if (!geo.vertexNormals.empty()) {
            openBlock("Normals " + std::to_string(geo.vertexNormals.size()));
            for (auto& n : geo.vertexNormals) {
                line(fmtVec3(n) + ",");
            }
            closeBlock();
        }

        // TVertices (UV sets)
        for (auto& uvSet : geo.textureCoordinateSets) {
            openBlock("TVertices " + std::to_string(uvSet.size()));
            for (auto& uv : uvSet) {
                line(fmtVec2(uv) + ",");
            }
            closeBlock();
        }

        // VertexGroup
        if (!geo.vertexGroups.empty()) {
            openBlock("VertexGroup");
            for (auto vg : geo.vertexGroups) {
                line(std::to_string(static_cast<u32>(vg)) + ",");
            }
            closeBlock();
        }

        // Faces
        if (!geo.faces.empty()) {
            openBlock("Faces 1 " + std::to_string(geo.faces.size()));
            openBlock("Triangles");
            writeIndent();
            m_out << "{ ";
            for (size_t i = 0; i < geo.faces.size(); ++i) {
                if (i > 0) m_out << ", ";
                m_out << geo.faces[i];
            }
            m_out << " },\n";
            closeBlock();
            closeBlock();
        }

        // Groups
        if (!geo.matrixGroups.empty()) {
            // Total matrix count
            u32 totalMatrices = 0;
            for (auto c : geo.matrixGroups)
                totalMatrices += c;
            openBlock("Groups " + std::to_string(geo.matrixGroups.size()) + " " +
                      std::to_string(totalMatrices));
            u32 offset = 0;
            for (auto count : geo.matrixGroups) {
                writeIndent();
                m_out << "Matrices { ";
                for (u32 j = 0; j < count; ++j) {
                    if (j > 0) m_out << ", ";
                    m_out << geo.matrixIndices[offset + j];
                }
                m_out << " },\n";
                offset += count;
            }
            closeBlock();
        }

        // Properties
        line("MaterialID " + std::to_string(geo.materialId) + ",");
        line("SelectionGroup " + std::to_string(geo.selectionGroup) + ",");
        if (geo.selectionFlags == 4) {
            line("Unselectable,");
        } else if (geo.selectionFlags != 0) {
            line("SelectionFlags " + std::to_string(geo.selectionFlags) + ",");
        }
        if (geo.lod != 0)
            line("LevelOfDetail " + std::to_string(geo.lod) + ",");
        if (!geo.lodName.empty())
            line("LevelOfDetailName " + quoted(geo.lodName) + ",");

        writeExtent(geo.extent);

        // Sequence extents
        for (auto& se : geo.sequenceExtents) {
            openBlock("Anim");
            writeExtent(se);
            closeBlock();
        }

        // Tangents (Reforged)
        if (!geo.tangents.empty()) {
            openBlock("Tangents " + std::to_string(geo.tangents.size()));
            for (auto& t : geo.tangents) {
                line(fmtVec4(t) + ",");
            }
            closeBlock();
        }

        // SkinWeights (Reforged)
        if (!geo.skinData.empty()) {
            size_t vertCount = geo.skinData.size() / 8;
            openBlock("SkinWeights " + std::to_string(vertCount));
            for (size_t i = 0; i < geo.skinData.size(); i += 8) {
                writeIndent();
                m_out << "{ ";
                for (size_t j = 0; j < 8 && (i + j) < geo.skinData.size(); ++j) {
                    if (j > 0) m_out << ", ";
                    m_out << static_cast<u32>(geo.skinData[i + j]);
                }
                m_out << " },\n";
            }
            closeBlock();
        }

        closeBlock();
    }

    void writeGeosetAnims() {
        if (m_model.geosetAnimations.empty()) return;
        for (auto& ga : m_model.geosetAnimations) {
            writeGeosetAnim(ga);
        }
    }

    void writeGeosetAnim(const GeosetAnimation& ga) {
        openBlock("GeosetAnim");

        // Alpha
        if (ga.alphaTracks.isUsed) {
            writeTrackOrStatic<f32>("Alpha", ga.alphaTracks, ga.alpha);
        } else {
            line("static Alpha " + fmtFloat(ga.alpha) + ",");
        }

        if (mdx::hasFlag(ga.flags, GeosetAnimation::Flag::DropShadow)) line("DropShadow,");

        line("GeosetId " + std::to_string(ga.geosetId) + ",");

        // Color
        if (ga.colorTracks.isUsed) {
            writeTrackOrStatic<Vector3f>("Color", ga.colorTracks, ga.color);
        } else if (mdx::hasFlag(ga.flags, GeosetAnimation::Flag::Color)) {
            line("static Color " + fmtVec3(ga.color) + ",");
        }

        closeBlock();
    }

    void writeBones() {
        for (auto& bone : m_model.bones) {
            openBlock("Bone " + quoted(bone.node.name));
            writeNodeFields(bone.node);

            // GeosetId
            if (bone.geosetId == Bone::MULTIPLE_GEOSETS) {
                line("GeosetId Multiple,");
            } else {
                line("GeosetId " + std::to_string(bone.geosetId) + ",");
            }

            // GeosetAnimId
            if (bone.geosetAnimationId == 0xFFFFFFFF) {
                line("GeosetAnimId None,");
            } else {
                line("GeosetAnimId " + std::to_string(bone.geosetAnimationId) + ",");
            }

            closeBlock();
        }
    }

    void writeLights() {
        for (auto& light : m_model.lights) {
            openBlock("Light " + quoted(light.node.name));
            writeNodeFields(light.node);

            // Light type
            switch (light.type) {
                case Light::LightType::Omni: line("Omnidirectional,"); break;
                case Light::LightType::Directional: line("Directional,"); break;
                case Light::LightType::Ambient: line("Ambient,"); break;
            }

            // Static properties or tracks
            if (light.attenuationStartTracks.isUsed) {
                writeTrackOrStatic<f32>("AttenuationStart", light.attenuationStartTracks,
                                         light.attenuationStart);
            } else {
                line("static AttenuationStart " + fmtFloat(light.attenuationStart) + ",");
            }

            if (light.attenuationEndTracks.isUsed) {
                writeTrackOrStatic<f32>("AttenuationEnd", light.attenuationEndTracks,
                                         light.attenuationEnd);
            } else {
                line("static AttenuationEnd " + fmtFloat(light.attenuationEnd) + ",");
            }

            if (light.intensityTracks.isUsed) {
                writeTrackOrStatic<f32>("Intensity", light.intensityTracks, light.intensity);
            } else {
                line("static Intensity " + fmtFloat(light.intensity) + ",");
            }

            if (light.colorTracks.isUsed) {
                writeTrackOrStatic<Vector3f>("Color", light.colorTracks, light.color);
            } else {
                line("static Color " + fmtVec3(light.color) + ",");
            }

            if (light.ambientIntensityTracks.isUsed) {
                writeTrackOrStatic<f32>("AmbIntensity", light.ambientIntensityTracks,
                                         light.ambientIntensity);
            } else {
                line("static AmbIntensity " + fmtFloat(light.ambientIntensity) + ",");
            }

            if (light.ambientColorTracks.isUsed) {
                writeTrackOrStatic<Vector3f>("AmbColor", light.ambientColorTracks,
                                              light.ambientColor);
            } else {
                line("static AmbColor " + fmtVec3(light.ambientColor) + ",");
            }

            writeTrack<f32>("Visibility", light.visibilityTracks);

            closeBlock();
        }
    }

    void writeHelpers() {
        for (auto& helper : m_model.helpers) {
            openBlock("Helper " + quoted(helper.node.name));
            writeNodeFields(helper.node);
            closeBlock();
        }
    }

    void writeAttachments() {
        for (auto& att : m_model.attachments) {
            openBlock("Attachment " + quoted(att.node.name));
            writeNodeFields(att.node);
            line("AttachmentID " + std::to_string(att.attachmentId) + ",");
            if (!att.path.empty()) line("Path " + quoted(att.path) + ",");
            writeTrack<f32>("Visibility", att.visibilityTracks);
            closeBlock();
        }
    }

    void writePivotPoints() {
        if (m_model.pivotPoints.empty()) return;
        openBlock("PivotPoints " + std::to_string(m_model.pivotPoints.size()));
        for (auto& p : m_model.pivotPoints) {
            line(fmtVec3(p) + ",");
        }
        closeBlock();
    }

    void writeParticleEmitters() {
        for (auto& pe : m_model.particleEmitters) {
            openBlock("ParticleEmitter " + quoted(pe.node.name));
            writeNodeFields(pe.node);

            // EmitterUsesMdl / EmitterUsesTga flags
            if (mdx::hasFlag(pe.node.flags, Node::NodeFlag::EmitterUsesMdl))
                line("EmitterUsesMdl,");
            if (mdx::hasFlag(pe.node.flags, Node::NodeFlag::EmitterUsesTga))
                line("EmitterUsesTga,");

            if (pe.emissionRateTracks.isUsed) {
                writeTrackOrStatic<f32>("EmissionRate", pe.emissionRateTracks, pe.emissionRate);
            } else {
                line("static EmissionRate " + fmtFloat(pe.emissionRate) + ",");
            }

            if (pe.gravityTracks.isUsed) {
                writeTrackOrStatic<f32>("Gravity", pe.gravityTracks, pe.gravity);
            } else {
                line("static Gravity " + fmtFloat(pe.gravity) + ",");
            }

            if (pe.longitudeTracks.isUsed) {
                writeTrackOrStatic<f32>("Longitude", pe.longitudeTracks, pe.longitude);
            } else {
                line("static Longitude " + fmtFloat(pe.longitude) + ",");
            }

            if (pe.latitudeTracks.isUsed) {
                writeTrackOrStatic<f32>("Latitude", pe.latitudeTracks, pe.latitude);
            } else {
                line("static Latitude " + fmtFloat(pe.latitude) + ",");
            }

            line("Path " + quoted(pe.spawnModelFileName) + ",");

            if (pe.lifespanTracks.isUsed) {
                writeTrackOrStatic<f32>("LifeSpan", pe.lifespanTracks, pe.lifespan);
            } else {
                line("static LifeSpan " + fmtFloat(pe.lifespan) + ",");
            }

            if (pe.speedTracks.isUsed) {
                writeTrackOrStatic<f32>("InitVelocity", pe.speedTracks, pe.initialVelocity);
            } else {
                line("static InitVelocity " + fmtFloat(pe.initialVelocity) + ",");
            }

            writeTrack<f32>("Visibility", pe.visibilityTracks);

            closeBlock();
        }
    }

    void writeParticleEmitters2() {
        for (auto& pe2 : m_model.particleEmitters2) {
            openBlock("ParticleEmitter2 " + quoted(pe2.node.name));
            writeNodeFields(pe2.node);

            // Node behaviour flags for PE2
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::SortPrimitives))
                line("SortPrimsFarZ,");
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::LineEmitter))
                line("LineEmitter,");
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::Unfogged)) line("Unfogged,");
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::ModelSpace))
                line("ModelSpace,");
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::Unshaded)) line("Unshaded,");
            if (mdx::hasFlag(pe2.node.flags, Node::NodeFlag::XYQuad)) line("XYQuad,");

            // Properties with potential tracks
            if (pe2.speedTracks.isUsed) {
                writeTrackOrStatic<f32>("Speed", pe2.speedTracks, pe2.speed);
            } else {
                line("static Speed " + fmtFloat(pe2.speed) + ",");
            }

            if (pe2.variationTracks.isUsed) {
                writeTrackOrStatic<f32>("Variation", pe2.variationTracks, pe2.variation);
            } else {
                line("static Variation " + fmtFloat(pe2.variation) + ",");
            }

            if (pe2.latitudeTracks.isUsed) {
                writeTrackOrStatic<f32>("Latitude", pe2.latitudeTracks, pe2.latitude);
            } else {
                line("static Latitude " + fmtFloat(pe2.latitude) + ",");
            }

            if (pe2.gravityTracks.isUsed) {
                writeTrackOrStatic<f32>("Gravity", pe2.gravityTracks, pe2.gravity);
            } else {
                line("static Gravity " + fmtFloat(pe2.gravity) + ",");
            }

            line("LifeSpan " + fmtFloat(pe2.lifespan) + ",");

            if (pe2.emissionRateTracks.isUsed) {
                writeTrackOrStatic<f32>("EmissionRate", pe2.emissionRateTracks,
                                         pe2.emissionRate);
            } else {
                line("static EmissionRate " + fmtFloat(pe2.emissionRate) + ",");
            }

            if (pe2.lengthTracks.isUsed) {
                writeTrackOrStatic<f32>("Length", pe2.lengthTracks, pe2.length);
            } else {
                line("static Length " + fmtFloat(pe2.length) + ",");
            }

            if (pe2.widthTracks.isUsed) {
                writeTrackOrStatic<f32>("Width", pe2.widthTracks, pe2.width);
            } else {
                line("static Width " + fmtFloat(pe2.width) + ",");
            }

            // FilterMode as bare identifier
            switch (pe2.filterMode) {
                case 0: line("Blend,"); break;
                case 1: line("Additive,"); break;
                case 2: line("Modulate,"); break;
                case 3: line("Modulate2x,"); break;
                case 4: line("AlphaKey,"); break;
                default: break;
            }

            line("Rows " + std::to_string(pe2.rows) + ",");
            line("Columns " + std::to_string(pe2.columns) + ",");

            // Head/Tail
            switch (pe2.headOrTail) {
                case 0: line("Head,"); break;
                case 1: line("Tail,"); break;
                case 2: line("Both,"); break;
                default: break;
            }

            line("TailLength " + fmtFloat(pe2.tailLength) + ",");
            line("Time " + fmtFloat(pe2.time) + ",");

            // SegmentColor
            openBlock("SegmentColor");
            for (int i = 0; i < 3; ++i) {
                line("Color " + fmtVec3(pe2.segmentColor[i]) + ",");
            }
            closeBlock();

            // Alpha { a, a, a }
            line("Alpha { " + std::to_string(static_cast<u32>(pe2.segmentAlpha[0])) + ", " +
                 std::to_string(static_cast<u32>(pe2.segmentAlpha[1])) + ", " +
                 std::to_string(static_cast<u32>(pe2.segmentAlpha[2])) + " },");

            // ParticleScaling { s, s, s }
            line("ParticleScaling { " + fmtFloat(pe2.segmentScaling[0]) + ", " +
                 fmtFloat(pe2.segmentScaling[1]) + ", " + fmtFloat(pe2.segmentScaling[2]) +
                 " },");

            // UV anim intervals
            auto fmtInterval = [](const std::array<u32, 3>& arr) {
                return "{ " + std::to_string(arr[0]) + ", " + std::to_string(arr[1]) + ", " +
                       std::to_string(arr[2]) + " }";
            };
            line("LifeSpanUVAnim " + fmtInterval(pe2.headInterval) + ",");
            line("DecayUVAnim " + fmtInterval(pe2.headDecayInterval) + ",");
            line("TailUVAnim " + fmtInterval(pe2.tailInterval) + ",");
            line("TailDecayUVAnim " + fmtInterval(pe2.tailDecayInterval) + ",");

            line("TextureID " + std::to_string(pe2.textureId) + ",");
            if (pe2.squirt != 0)
                line("Squirt " + std::to_string(pe2.squirt) + ",");
            if (pe2.priorityPlane != 0)
                line("PriorityPlane " + std::to_string(pe2.priorityPlane) + ",");
            if (pe2.replaceableId != 0)
                line("ReplaceableId " + std::to_string(pe2.replaceableId) + ",");

            writeTrack<f32>("Visibility", pe2.visibilityTracks);

            closeBlock();
        }
    }

    void writeRibbonEmitters() {
        for (auto& re : m_model.ribbonEmitters) {
            openBlock("RibbonEmitter " + quoted(re.node.name));
            writeNodeFields(re.node);

            if (re.heightAboveTracks.isUsed) {
                writeTrackOrStatic<f32>("HeightAbove", re.heightAboveTracks, re.heightAbove);
            } else {
                line("static HeightAbove " + fmtFloat(re.heightAbove) + ",");
            }

            if (re.heightBelowTracks.isUsed) {
                writeTrackOrStatic<f32>("HeightBelow", re.heightBelowTracks, re.heightBelow);
            } else {
                line("static HeightBelow " + fmtFloat(re.heightBelow) + ",");
            }

            if (re.alphaTracks.isUsed) {
                writeTrackOrStatic<f32>("Alpha", re.alphaTracks, re.alpha);
            } else {
                line("static Alpha " + fmtFloat(re.alpha) + ",");
            }

            if (re.colorTracks.isUsed) {
                writeTrackOrStatic<Vector3f>("Color", re.colorTracks, re.color);
            } else {
                line("static Color " + fmtVec3(re.color) + ",");
            }

            line("LifeSpan " + fmtFloat(re.lifespan) + ",");
            if (re.textureSlotTracks.isUsed) {
                writeTrackOrStatic<u32>("TextureSlot", re.textureSlotTracks, re.textureSlot);
            } else {
                line("TextureSlot " + std::to_string(re.textureSlot) + ",");
            }
            line("EmissionRate " + std::to_string(re.emissionRate) + ",");
            line("Rows " + std::to_string(re.rows) + ",");
            line("Columns " + std::to_string(re.columns) + ",");
            line("MaterialID " + std::to_string(re.materialId) + ",");
            if (re.gravity != 0.0f) line("Gravity " + fmtFloat(re.gravity) + ",");

            writeTrack<f32>("Visibility", re.visibilityTracks);

            closeBlock();
        }
    }

    void writeCornEmitters() {
        for (auto& ce : m_model.cornEmitters) {
            openBlock("ParticleEmitterPopcorn " + quoted(ce.node.name));
            writeNodeFields(ce.node);

            if (ce.lifeSpanTracks.isUsed) {
                writeTrackOrStatic<f32>("LifeSpan", ce.lifeSpanTracks, ce.lifeSpan);
            } else {
                line("static LifeSpan " + fmtFloat(ce.lifeSpan) + ",");
            }

            if (ce.emissionRateTracks.isUsed) {
                writeTrackOrStatic<f32>("EmissionRate", ce.emissionRateTracks, ce.emissionRate);
            } else {
                line("static EmissionRate " + fmtFloat(ce.emissionRate) + ",");
            }

            if (ce.speedTracks.isUsed) {
                writeTrackOrStatic<f32>("Speed", ce.speedTracks, ce.speed);
            } else {
                line("static Speed " + fmtFloat(ce.speed) + ",");
            }

            if (ce.replaceableId != 0)
                line("ReplaceableId " + std::to_string(ce.replaceableId) + ",");

            line("Path " + quoted(ce.path) + ",");
            if (!ce.animVisibilityGuide.empty())
                line("AnimVisibilityGuide " + quoted(ce.animVisibilityGuide) + ",");

            if (ce.colorTracks.isUsed) {
                writeTrack<Vector4f>("Color", ce.colorTracks);
            }

            writeTrack<f32>("Visibility", ce.visibilityTracks);

            closeBlock();
        }
    }

    void writeEventObjects() {
        for (auto& ev : m_model.eventObjects) {
            openBlock("EventObject " + quoted(ev.node.name));
            writeNodeFields(ev.node);

            if (!ev.eventTrackTimes.empty()) {
                openBlock("EventTrack " + std::to_string(ev.eventTrackTimes.size()));
                for (auto t : ev.eventTrackTimes) {
                    line(std::to_string(t) + ",");
                }
                closeBlock();
            }

            closeBlock();
        }
    }

    void writeCameras() {
        for (auto& cam : m_model.cameras) {
            openBlock("Camera " + quoted(cam.name));
            line("Position " + fmtVec3(cam.position) + ",");
            line("FieldOfView " + fmtFloat(cam.fieldOfView) + ",");
            line("FarClip " + fmtFloat(cam.farClippingPlane) + ",");
            line("NearClip " + fmtFloat(cam.nearClippingPlane) + ",");

            writeTrack<Vector3f>("Translation", cam.positionTracks);
            writeTrack<f32>("Rotation", cam.targetRotationTracks);

            openBlock("Target");
            line("Position " + fmtVec3(cam.targetPosition) + ",");
            writeTrack<Vector3f>("Translation", cam.targetPositionTracks);
            closeBlock();

            closeBlock();
        }
    }

    void writeCollisionShapes() {
        for (auto& cs : m_model.collisionShapes) {
            openBlock("CollisionShape " + quoted(cs.node.name));
            writeNodeFields(cs.node);

            switch (cs.type) {
                case CollisionShape::ShapeType::Box: line("Box,"); break;
                case CollisionShape::ShapeType::Plane: line("Plane,"); break;
                case CollisionShape::ShapeType::Sphere: line("Sphere,"); break;
                case CollisionShape::ShapeType::Cylinder: line("Cylinder,"); break;
            }

            if (!cs.vertices.empty()) {
                openBlock("Vertices " + std::to_string(cs.vertices.size()));
                for (auto& v : cs.vertices) {
                    line(fmtVec3(v) + ",");
                }
                closeBlock();
            }

            if (cs.type == CollisionShape::ShapeType::Sphere ||
                cs.type == CollisionShape::ShapeType::Cylinder) {
                line("BoundsRadius " + fmtFloat(cs.radius) + ",");
            }

            closeBlock();
        }
    }

    void writeFaceEffects() {
        for (auto& fe : m_model.faceEffects) {
            openBlock("FaceFX " + quoted(fe.target));
            line("Path " + quoted(fe.path) + ",");
            closeBlock();
        }
    }

    void writeBindPose() {
        if (m_model.bindPoses.empty()) return;
        openBlock("BindPose");
        openBlock("Matrices " + std::to_string(m_model.bindPoses.size()));
        for (auto& mat : m_model.bindPoses) {
            writeIndent();
            m_out << "{ ";
            for (size_t i = 0; i < 12; ++i) {
                if (i > 0) m_out << ", ";
                m_out << fmtFloat(mat[i]);
            }
            m_out << " },\n";
        }
        closeBlock();
        closeBlock();
    }
};

} // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

std::string writeModelToMdl(const Model& model) {
    MdlTextWriter writer(model);
    return writer.write();
}

} // namespace mdx
} // namespace whiteout
