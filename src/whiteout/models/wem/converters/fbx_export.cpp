// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file fbx_export.cpp
 * @brief `Document` → FBX 7.4/7.5 (FBX_OBJ_DESIGN §§3–8).
 *
 * The records follow the SDK's own layout — header extension, file id and
 * creation time (one consistent set, measured from an SDK-written file, so the
 * footer code matches it), global settings, definitions, objects, connections,
 * takes — and its conventions where its reader depends on them (an empty
 * `AnimationLayer` is closed with a null record; fbx.h).
 *
 * WEM's row-vector matrices are FBX's stored layout already: the stored
 * sixteen doubles are the column-vector matrix transposed, so a WEM matrix in
 * the file's basis is written as it is. Animation is baked through `ClipPose` —
 * the evaluator the editor plays — at the frame rate, per channel component,
 * Euler angles unwrapped against the previous sample and keys a line through
 * their neighbours reproduces dropped.
 */

#include "whiteout/models/wem/fbx_converter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "whiteout/models/fbx/eval.h"
#include "whiteout/models/wem/anim/pose.h"
#include "whiteout/models/wem/geometry/triangulation.h"
#include "whiteout/models/wem/materials/surface_flatten.h"
#include "whiteout/textures/pbr_bake.h"

#include "export_sections.h"
#include "interchange_mesh.h"

namespace whiteout {
namespace models {
namespace wem {

namespace {

using fbx::Node;
using fbx::Property;

constexpr f64 kRadiansToDegrees = 180.0 / 3.14159265358979323846;
constexpr i64 kFirstId = 1000000000LL;

/// One consistent header set, as the SDK wrote it into a 3ds Max file: the
/// footer code is derived from the timestamp, so the four travel together.
constexpr i32 kStamp[8] = {1000, 2014, 9, 25, 13, 42, 7, 0};
constexpr const char* kCreationTime = "2014-09-25 13:42:07:000";
constexpr u8 kFileId[16] = {0x28, 0xb4, 0x2d, 0xe4, 0xb9, 0x28, 0xc2, 0xc8,
                            0xb5, 0xc2, 0xb9, 0x2a, 0xaf, 0x29, 0xfa, 0xf5};
constexpr u8 kFooterCode[16] = {0xfa, 0xbc, 0xab, 0x0e, 0xd7, 0xc0, 0xdc, 0x62,
                                0xb7, 0x78, 0xf7, 0x87, 0x19, 0xf3, 0x25, 0x78};

std::string ObjectNameOf(const std::string& name, const char* kind) {
    std::string out = name;
    out.push_back('\0');
    out.push_back('\1');
    out += kind;
    return out;
}

/// One `P:` row.
Node& Row(Node& properties, const char* name, const char* type, const char* label,
          const char* flags) {
    return properties.add("P", {Property::Str(name), Property::Str(type), Property::Str(label),
                                Property::Str(flags)});
}

void RowInt(Node& p, const char* name, const char* type, i32 value) {
    Row(p, name, type, type == std::string_view("int") ? "Integer" : "", "")
        .properties.push_back(Property::Int(value));
}

void RowDouble(Node& p, const char* name, f64 value, const char* flags = "") {
    Row(p, name, "double", "Number", flags).properties.push_back(Property::Double(value));
}

void RowVec(Node& p, const char* name, const char* type, const char* label, const char* flags,
            const std::array<f64, 3>& v) {
    Node& row = Row(p, name, type, label, flags);
    for (const f64 x : v) {
        row.properties.push_back(Property::Double(x));
    }
}

void RowString(Node& p, const char* name, const std::string& value, const char* flags = "") {
    Row(p, name, "KString", "", flags).properties.push_back(Property::Str(value));
}

void RowTime(Node& p, const char* name, i64 value) {
    Row(p, name, "KTime", "Time", "").properties.push_back(Property::Long(value));
}

// ============================================================================
// Rotations: quaternion ⇄ FBX's XYZ Euler, column vectors, degrees
// ============================================================================

using Mat3 = std::array<std::array<f64, 3>, 3>;

/// `R = Rz · Ry · Rx` (X turns first), angles in degrees.
std::array<f64, 3> EulerXyz(const Mat3& r) {
    const f64 sy = std::clamp(-r[2][0], -1.0, 1.0);
    const f64 b = std::asin(sy);
    f64 a = 0.0;
    f64 c = 0.0;
    if (std::fabs(sy) < 0.9999999) {
        a = std::atan2(r[2][1], r[2][2]);
        c = std::atan2(r[1][0], r[0][0]);
    } else {
        // Gimbal: X and Z turn about one axis; Z takes it all.
        c = std::atan2(-r[0][1], r[1][1]);
    }
    return {a * kRadiansToDegrees, b * kRadiansToDegrees, c * kRadiansToDegrees};
}

/// The equivalent of @p e nearest @p previous: each angle wrapped by whole
/// turns, and the (x+180, 180-y, z+180) twin tried too.
std::array<f64, 3> NearestEuler(const std::array<f64, 3>& e, const std::array<f64, 3>& previous) {
    const auto wrap = [](f64 angle, f64 near) {
        return angle + 360.0 * std::round((near - angle) / 360.0);
    };
    const auto settle = [&](std::array<f64, 3> v) {
        for (std::size_t i = 0; i < 3; ++i) {
            v[i] = wrap(v[i], previous[i]);
        }
        return v;
    };
    const std::array<f64, 3> a = settle(e);
    const std::array<f64, 3> b = settle({e[0] + 180.0, 180.0 - e[1], e[2] + 180.0});
    const auto distance = [&](const std::array<f64, 3>& v) {
        return std::fabs(v[0] - previous[0]) + std::fabs(v[1] - previous[1]) +
               std::fabs(v[2] - previous[2]);
    };
    return distance(b) < distance(a) ? b : a;
}

/// A WEM local (row-vector) in the file's basis as FBX's T / Euler R / S.
struct FileTrs {
    std::array<f64, 3> t{0, 0, 0};
    std::array<f64, 3> r{0, 0, 0};
    std::array<f64, 3> s{1, 1, 1};
};

FileTrs TrsOf(const Matrix44f& wemRow, const AxisBasis& basis) {
    const Matrix44f m = basis.toFile(wemRow);
    FileTrs out;
    out.t = {m.data[3][0], m.data[3][1], m.data[3][2]};
    // Column-vector matrix M = mᵀ: its columns are m's rows.
    Mat3 r;
    for (std::size_t c = 0; c < 3; ++c) {
        const f64 length = std::sqrt(static_cast<f64>(m.data[c][0]) * m.data[c][0] +
                                     static_cast<f64>(m.data[c][1]) * m.data[c][1] +
                                     static_cast<f64>(m.data[c][2]) * m.data[c][2]);
        out.s[c] = length;
        for (std::size_t row = 0; row < 3; ++row) {
            r[row][c] = length > 1e-12 ? m.data[c][row] / length : (row == c ? 1.0 : 0.0);
        }
    }
    const f64 det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) -
                    r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
                    r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.0) {
        // A mirror: the X scale carries it, the rotation stays proper.
        out.s[0] = -out.s[0];
        for (std::size_t row = 0; row < 3; ++row) {
            r[row][0] = -r[row][0];
        }
    }
    out.r = EulerXyz(r);
    return out;
}

// ============================================================================
// Curve keys
// ============================================================================

/// Keys a straight line between their kept neighbours reproduces within
/// @p tolerance are dropped; the first and the last always stay.
std::vector<std::size_t> ReduceLinear(const std::vector<f64>& times, const std::vector<f64>& values,
                                      f64 tolerance) {
    std::vector<std::size_t> kept;
    if (values.empty()) {
        return kept;
    }
    kept.push_back(0);
    std::size_t anchor = 0;
    for (std::size_t end = 2; end < values.size(); ++end) {
        bool fits = true;
        const f64 t0 = times[anchor];
        const f64 span = times[end] - t0;
        for (std::size_t i = anchor + 1; i < end && fits; ++i) {
            const f64 u = span > 0.0 ? (times[i] - t0) / span : 0.0;
            const f64 line = values[anchor] + (values[end] - values[anchor]) * u;
            fits = std::fabs(line - values[i]) <= tolerance;
        }
        if (!fits) {
            anchor = end - 1;
            kept.push_back(anchor);
        }
    }
    if (values.size() > 1) {
        kept.push_back(values.size() - 1);
    }
    return kept;
}

bool Constant(const std::vector<f64>& values, f64 tolerance) {
    for (const f64 v : values) {
        if (std::fabs(v - values.front()) > tolerance) {
            return false;
        }
    }
    return true;
}

/// The packed default weights (a third each, as 3333) an SDK key attribute
/// carries when its tangents are not weighted.
f32 DefaultWeights() {
    const u32 bits = 3333u | (3333u << 16);
    f32 out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// ============================================================================
// The writer
// ============================================================================

class Exporter {
public:
    Exporter(const Document& document, ProfileId profile, const FbxWriteOptions& options,
             Diagnostics& diagnostics)
        : document_(document), profile_(profile), options_(options),
          basis_(AxisBasis::FromPreset(options.axes)), diagnostics_(diagnostics),
          names_(document) {}

    FbxExport run() {
        FbxExport out;
        fbx::File& file = out.file;
        file.version = options_.version;
        file.footerCode.assign(kFooterCode, kFooterCode + 16);
        writeHeader(file);
        writeGlobals(file);
        Node& documents = file.nodes.emplace_back("Documents");
        documents.add("Count", {Property::Int(1)});
        Node& doc = documents.add("Document", {Property::Long(nextId()),
                                               Property::Str(ObjectNameOf("", "Scene")),
                                               Property::Str("Scene")});
        Node& docProps = doc.add("Properties70");
        Row(docProps, "SourceObject", "object", "", "");
        RowString(docProps, "ActiveAnimStackName", activeStackName());
        doc.add("RootNode", {Property::Long(0)});
        file.nodes.emplace_back("References").block = true;

        claimed_ = ClaimedChildModels(document_);
        for (u32 m = 0; m < document_.models.size(); ++m) {
            exportModel(m);
        }
        if (options_.bakeChildModels) {
            attachChildModels();
        }
        exportBindPose();
        exportAnimation();

        writeDefinitions(file);
        file.nodes.push_back(std::move(objects_));
        file.nodes.push_back(std::move(connections_));
        writeTakes(file);
        out.images = names_.take();
        return out;
    }

private:
    i64 nextId() {
        return kFirstId + nextId_++;
    }

    void connect(i64 child, i64 parent) {
        connections_.add("C", {Property::Str("OO"), Property::Long(child), Property::Long(parent)});
    }

    void connectProperty(i64 child, i64 parent, const char* property) {
        connections_.add("C", {Property::Str("OP"), Property::Long(child), Property::Long(parent),
                               Property::Str(property)});
    }

    Node& object(const char* kind, i64 id, const std::string& name, const char* nameKind,
                 const char* subclass) {
        ++counts_[kind];
        return objects_.add(kind, {Property::Long(id), Property::Str(ObjectNameOf(name, nameKind)),
                                   Property::Str(subclass)});
    }

    // ---- header ----------------------------------------------------------------------

    void writeHeader(fbx::File& file) {
        Node& header = file.nodes.emplace_back("FBXHeaderExtension");
        header.add("FBXHeaderVersion", {Property::Int(1003)});
        header.add("FBXVersion", {Property::Int(static_cast<i32>(options_.version))});
        header.add("EncryptionType", {Property::Int(0)});
        Node& stamp = header.add("CreationTimeStamp");
        const char* fields[8] = {"Version", "Year", "Month", "Day", "Hour", "Minute", "Second",
                                 "Millisecond"};
        for (int i = 0; i < 8; ++i) {
            stamp.add(fields[i], {Property::Int(kStamp[i])});
        }
        header.add("Creator", {Property::Str(options_.creator)});
        Node& info = header.add("SceneInfo", {Property::Str(ObjectNameOf("GlobalInfo", "SceneInfo")),
                                              Property::Str("UserData")});
        info.add("Type", {Property::Str("UserData")});
        info.add("Version", {Property::Int(100)});
        Node& meta = info.add("MetaData");
        meta.add("Version", {Property::Int(100)});
        for (const char* field : {"Title", "Subject", "Author", "Keywords", "Revision", "Comment"}) {
            meta.add(field, {Property::Str("")});
        }
        Node& props = info.add("Properties70");
        RowString(props, "Original|ApplicationVendor", "WhiteoutFlakes");
        RowString(props, "Original|ApplicationName", options_.creator);
        file.nodes.emplace_back("FileId").properties.push_back(
            Property::RawBytes(std::string(reinterpret_cast<const char*>(kFileId), 16)));
        file.nodes.emplace_back("CreationTime").properties.push_back(Property::Str(kCreationTime));
        file.nodes.emplace_back("Creator").properties.push_back(Property::Str(options_.creator));
    }

    void writeGlobals(fbx::File& file) {
        Node& global = file.nodes.emplace_back("GlobalSettings");
        global.add("Version", {Property::Int(1000)});
        Node& p = global.add("Properties70");
        // The declaration: where WEM's up (+Z), forward (+X) and left (+Y) land.
        RowInt(p, "UpAxis", "int", static_cast<i32>(basis_.fileAxisOf(2)));
        RowInt(p, "UpAxisSign", "int", static_cast<i32>(basis_.fileSignOf(2)));
        RowInt(p, "FrontAxis", "int", static_cast<i32>(basis_.fileAxisOf(0)));
        RowInt(p, "FrontAxisSign", "int", static_cast<i32>(basis_.fileSignOf(0)));
        RowInt(p, "CoordAxis", "int", static_cast<i32>(basis_.fileAxisOf(1)));
        RowInt(p, "CoordAxisSign", "int", static_cast<i32>(basis_.fileSignOf(1)));
        RowInt(p, "OriginalUpAxis", "int", static_cast<i32>(basis_.fileAxisOf(2)));
        RowInt(p, "OriginalUpAxisSign", "int", static_cast<i32>(basis_.fileSignOf(2)));
        // One Generic unit is a centimetre, FBX's own unit (§3); a caller that
        // restated the numbers in another unit says which (§16).
        RowDouble(p, "UnitScaleFactor", options_.unitScaleFactor);
        RowDouble(p, "OriginalUnitScaleFactor", options_.unitScaleFactor);
        RowVec(p, "AmbientColor", "ColorRGB", "Color", "", {0, 0, 0});
        RowString(p, "DefaultCamera", "Producer Perspective");
        const bool thirty = std::fabs(options_.frameRate - 30.0f) < 1e-4f;
        RowInt(p, "TimeMode", "enum", thirty ? 6 : 14);
        RowInt(p, "TimeProtocol", "enum", 2);
        RowInt(p, "SnapOnFrameMode", "enum", 0);
        i64 stop = 0;
        for (u32 c = 0; c < document_.clips.size(); ++c) {
            if (clipWritten(c)) {
                stop = std::max(stop, Ticks(document_.clips[c].duration));
            }
        }
        RowTime(p, "TimeSpanStart", 0);
        RowTime(p, "TimeSpanStop", stop > 0 ? stop : fbx::kTicksPerSecond);
        RowDouble(p, "CustomFrameRate", options_.frameRate);
        Row(p, "TimeMarker", "Compound", "", "");
        RowInt(p, "CurrentTimeMarker", "int", -1);
    }

    void writeDefinitions(fbx::File& file) {
        Node& definitions = file.nodes.emplace_back("Definitions");
        definitions.add("Version", {Property::Int(100)});
        i32 total = 1;
        for (const auto& [kind, count] : counts_) {
            total += static_cast<i32>(count);
        }
        definitions.add("Count", {Property::Int(total)});
        Node& globals = definitions.add("ObjectType", {Property::Str("GlobalSettings")});
        globals.add("Count", {Property::Int(1)});
        for (const auto& [kind, count] : counts_) {
            Node& type = definitions.add("ObjectType", {Property::Str(kind)});
            type.add("Count", {Property::Int(static_cast<i32>(count))});
        }
    }

    void writeTakes(fbx::File& file) {
        Node& takes = file.nodes.emplace_back("Takes");
        takes.add("Current", {Property::Str(takes_.empty() ? std::string() : takes_.front().first)});
        for (const auto& [name, stop] : takes_) {
            Node& take = takes.add("Take", {Property::Str(name)});
            take.add("FileName", {Property::Str(name + ".tak")});
            take.add("LocalTime", {Property::Long(0), Property::Long(stop)});
            take.add("ReferenceTime", {Property::Long(0), Property::Long(stop)});
        }
    }

    std::string activeStackName() const {
        for (u32 c = 0; c < document_.clips.size(); ++c) {
            if (clipWritten(c)) {
                return document_.clips[c].name;
            }
        }
        return {};
    }

    bool clipWritten(u32 clip) const {
        return !options_.onlyClip.has_value() || *options_.onlyClip == clip;
    }

    static i64 Ticks(f32 seconds) {
        return static_cast<i64>(std::llround(static_cast<f64>(seconds) * fbx::kTicksPerSecond));
    }

    // ---- nodes -----------------------------------------------------------------------

    struct ModelIds {
        i64 root = 0;
        std::vector<i64> nodes;
        std::vector<FileTrs> rest; ///< Each node's static `Lcl` values, as written.
    };

    void exportModel(u32 m) {
        const Model& model = document_.models[m];
        ModelIds ids;
        // One synthetic root per model: a WEM tree is routinely a forest, and
        // a DCC armature wants one root. Marked so a re-import strips it.
        ids.root = nextId();
        {
            const std::string rootName = model.name.empty() ? "model" + std::to_string(m) : model.name;
            Node& root = object("Model", ids.root, rootName, "Model", "Null");
            writeModelBody(root, FileTrs{}, true, nullptr);
            nullAttribute(ids.root, rootName);
            if (!(options_.bakeChildModels && claimed_[m])) {
                connect(ids.root, 0);
            }
        }
        const NodeTree& tree = model.nodes;
        ids.nodes.resize(tree.size());
        for (u32 i = 0; i < tree.size(); ++i) {
            const wem::Node& node = tree.nodes[i];
            ids.nodes[i] = nextId();
            const bool bone = node.kind == NodeKind::Bone;
            Node& out = object("Model", ids.nodes[i], node.name.empty() ? "node" + std::to_string(i) : node.name,
                               "Model", bone ? "LimbNode" : "Null");
            Transform local = node.local;
            const bool flagged = hasFlag(node.flags, NodeFlags::DontInheritTranslation) ||
                                 hasFlag(node.flags, NodeFlags::DontInheritRotation) ||
                                 hasFlag(node.flags, NodeFlags::DontInheritScale) ||
                                 hasFlag(node.flags, NodeFlags::ModelSpace);
            if (flagged && node.parent != kInvalidNode) {
                // FBX nodes inherit; the plain chain reproduces the bind world.
                local = Compose(Inverse(tree.worldBind(node.parent)), tree.worldBind(i));
                ++recomposed_;
            }
            const char* kind = bone || node.kind == NodeKind::Helper ? nullptr : ToString(node.kind);
            ids.rest.push_back(TrsOf(ToMatrix(local), basis_));
            writeModelBody(out, ids.rest.back(), false, kind);
            if (bone) {
                Node& attribute = object("NodeAttribute", nextId(), node.name, "NodeAttribute", "LimbNode");
                RowDouble(attribute.add("Properties70"), "Size", 33.333333);
                attribute.add("TypeFlags", {Property::Str("Skeleton")});
                connect(attribute.properties[0].integer, ids.nodes[i]);
            } else {
                nullAttribute(ids.nodes[i], node.name);
            }
        }
        for (u32 i = 0; i < tree.size(); ++i) {
            const u32 parent = tree.nodes[i].parent;
            connect(ids.nodes[i], parent != kInvalidNode && parent < tree.size() ? ids.nodes[parent] : ids.root);
        }
        if (recomposed_ != 0) {
            diagnostics_.info(DiagCode::Unspecified,
                              "model '" + model.name + "': " + std::to_string(recomposed_) +
                                  " node(s) with inherit/model-space flags recomposed to plain "
                                  "parenting; their clips are baked to the motion the flags made");
            recomposed_ = 0;
        }
        modelIds_.push_back(ids);
        exportMeshes(m);
    }

    void nullAttribute(i64 model, std::string name) {
        Node& attribute = object("NodeAttribute", nextId(), name, "NodeAttribute", "Null");
        Node& p = attribute.add("Properties70");
        RowDouble(p, "Size", 100.0);
        attribute.add("TypeFlags", {Property::Str("Null")});
        connect(attribute.properties[0].integer, model);
    }

    void writeModelBody(Node& model, const FileTrs& trs, bool synthetic, const char* kind) {
        model.add("Version", {Property::Int(232)});
        Node& p = model.add("Properties70");
        RowInt(p, "InheritType", "enum", 1);
        RowInt(p, "DefaultAttributeIndex", "int", 0);
        RowVec(p, "Lcl Translation", "Lcl Translation", "", "A", trs.t);
        RowVec(p, "Lcl Rotation", "Lcl Rotation", "", "A", trs.r);
        RowVec(p, "Lcl Scaling", "Lcl Scaling", "", "A", trs.s);
        if (synthetic) {
            Row(p, "wem:synthetic", "int", "Integer", "U").properties.push_back(Property::Int(1));
        }
        if (kind != nullptr) {
            RowString(p, "wem:kind", kind, "U");
        }
        model.add("Shading", {Property::Char('T')});
        model.add("Culling", {Property::Str("CullingOff")});
    }

    void attachChildModels() {
        for (u32 m = 0; m < document_.models.size(); ++m) {
            const Model& model = document_.models[m];
            for (u32 i = 0; i < model.nodes.size(); ++i) {
                const AttachmentPayload* attachment =
                    std::get_if<AttachmentPayload>(&model.nodes.nodes[i].payload);
                if (attachment == nullptr || attachment->model == kInvalidIndex ||
                    attachment->model >= document_.models.size() || attachment->model == m ||
                    attached_.count(attachment->model) != 0) {
                    continue;
                }
                attached_.insert(attachment->model);
                connect(modelIds_[attachment->model].root, modelIds_[m].nodes[i]);
            }
        }
        // A claimed model nothing attached after all stays at the scene root.
        for (u32 m = 0; m < document_.models.size(); ++m) {
            if (claimed_[m] && attached_.count(m) == 0) {
                connect(modelIds_[m].root, 0);
            }
        }
    }

    // ---- meshes ----------------------------------------------------------------------

    void exportMeshes(u32 m) {
        const Model& model = document_.models[m];
        const ProfileMaterialSet* set = model.setFor(profile_);
        const u32 look = set != nullptr ? set->defaultLook : 0;
        std::vector<std::optional<FlatSurface>> surfaces(model.materialSlots.size());
        std::vector<SlotDrawState> drawStates(model.materialSlots.size());
        for (u32 slot = 0; slot < model.materialSlots.size(); ++slot) {
            if (const Material* resolved = Resolve(model, slot, profile_, look)) {
                surfaces[slot] = FlattenSurface(document_, *resolved);
                drawStates[slot] = SlotDrawState{surfaces[slot]->invisible, surfaces[slot]->gameComposited};
            }
        }
        const DefaultLookAlpha defaultLook(document_, model, m, profile_, look);
        const bool flipWinding = basis_.determinant() < 0.0f;

        for (u32 meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
            const Mesh& mesh = model.meshes[meshIndex];
            const PolygonWalk walk(mesh);
            if (!walk.ok() || walk.faceCount() == 0) {
                continue;
            }
            const std::span<const Vector3f> positions =
                mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex);
            const std::span<const Vector3f> normals =
                mesh.attributes.get<Vector3f>(geom::names::kNormal, geom::Domain::Halfedge);
            const std::span<const std::array<u8, 4>> colors =
                mesh.attributes.get<std::array<u8, 4>>(geom::names::color(0), geom::Domain::Halfedge);
            std::vector<std::pair<u32, std::span<const Vector2f>>> uvSets;
            for (u32 uvSet = 0; uvSet < 8; ++uvSet) {
                const std::span<const Vector2f> uv =
                    mesh.attributes.get<Vector2f>(geom::names::uv(uvSet), geom::Domain::Halfedge);
                if (!uv.empty()) {
                    uvSets.emplace_back(uvSet, uv);
                }
            }
            const std::span<const u32> sectionOf = mesh.faceSections();

            // The triangles the model draws, when asked for them.
            std::vector<std::vector<u32>> trianglesOfFace;
            std::vector<u32> triangles;
            if (options_.triangulate) {
                std::vector<u32> faceOf;
                geom::TriangulateMesh(mesh, triangles, &faceOf);
                trianglesOfFace.resize(walk.faceCount());
                for (std::size_t t = 0; t < faceOf.size(); ++t) {
                    const u32 face = walk.faceOfSlot(faceOf[t]);
                    if (face != kInvalidIndex) {
                        trianglesOfFace[face].push_back(static_cast<u32>(t));
                    }
                }
            }

            std::vector<i32> polygonIndex;
            std::vector<f64> normalValues;
            std::vector<f64> colorValues;
            std::vector<i32> colorIndex;
            std::unordered_map<u32, i32> colorPool;
            std::vector<std::vector<f64>> uvValues(uvSets.size());
            std::vector<std::vector<i32>> uvIndex(uvSets.size());
            std::vector<std::unordered_map<u64, i32>> uvPool(uvSets.size());
            std::vector<i32> polygonMaterial;
            std::vector<u32> materialOfSlot(model.materialSlots.size(), kInvalidIndex);
            std::vector<u32> meshMaterials;
            SectionSkipCounts skipped;
            std::vector<u32> scratch;

            const auto emit = [&](const std::span<const u32> hs, const std::span<const u32> vs,
                                  const std::vector<u32>& corners, bool reverse, i32 material) {
                for (u32 i = 0; i < corners.size(); ++i) {
                    const u32 k = corners[reverse ? corners.size() - 1 - i : i];
                    const i32 vertex = static_cast<i32>(vs[k]);
                    polygonIndex.push_back(i + 1 == corners.size() ? ~vertex : vertex);
                    const u32 h = hs[k];
                    if (!normals.empty()) {
                        const Vector3f n = h < normals.size() ? basis_.toFile(normals[h]) : Vector3f{0, 0, 1};
                        normalValues.insert(normalValues.end(), {n.x, n.y, n.z});
                    }
                    for (std::size_t s = 0; s < uvSets.size(); ++s) {
                        const Vector2f uv = h < uvSets[s].second.size() ? uvSets[s].second[h] : Vector2f{0, 0};
                        const Vector2f flipped{uv.x, 1.0f - uv.y};
                        u64 key = 0;
                        std::memcpy(&key, &flipped, sizeof(key));
                        auto found = uvPool[s].find(key);
                        if (found == uvPool[s].end()) {
                            found = uvPool[s].emplace(key, static_cast<i32>(uvValues[s].size() / 2)).first;
                            uvValues[s].insert(uvValues[s].end(), {flipped.x, flipped.y});
                        }
                        uvIndex[s].push_back(found->second);
                    }
                    if (!colors.empty()) {
                        const std::array<u8, 4> c = h < colors.size() ? colors[h] : std::array<u8, 4>{255, 255, 255, 255};
                        u32 key = 0;
                        std::memcpy(&key, c.data(), 4);
                        auto found = colorPool.find(key);
                        if (found == colorPool.end()) {
                            found = colorPool.emplace(key, static_cast<i32>(colorValues.size() / 4)).first;
                            colorValues.insert(colorValues.end(), {c[0] / 255.0, c[1] / 255.0, c[2] / 255.0,
                                                                   c[3] / 255.0});
                        }
                        colorIndex.push_back(found->second);
                    }
                }
                polygonMaterial.push_back(material);
            };

            for (u32 f = 0; f < walk.faceCount(); ++f) {
                const u32 slot = walk.slot(f);
                const u32 section = slot < sectionOf.size() ? sectionOf[slot] : 0;
                const u32 materialSlot =
                    section < mesh.sections.size() ? mesh.sections[section].materialSlot : kInvalidIndex;
                const bool bound = materialSlot < surfaces.size() && surfaces[materialSlot].has_value();
                const SectionSkip skip =
                    SkipSection(mesh, meshIndex, section, materialSlot, profile_,
                                bound ? &drawStates[materialSlot] : nullptr, &defaultLook);
                if (skip != SectionSkip::None) {
                    skipped.count(skip);
                    continue;
                }
                i32 material = 0;
                bool reverse = flipWinding;
                if (bound) {
                    if (materialOfSlot[materialSlot] == kInvalidIndex) {
                        materialOfSlot[materialSlot] = static_cast<u32>(meshMaterials.size());
                        meshMaterials.push_back(materialSlot);
                    }
                    material = static_cast<i32>(materialOfSlot[materialSlot]);
                    reverse = reverse != (surfaces[materialSlot]->cull == CullMode::Front);
                } else {
                    material = -1;
                }
                const std::span<const u32> hs = walk.halfedges(f);
                const std::span<const u32> vs = walk.vertices(f);
                if (options_.triangulate && hs.size() > 3) {
                    for (const u32 t : trianglesOfFace[f]) {
                        scratch.clear();
                        for (u32 c = 0; c < 3; ++c) {
                            const u32 k = walk.cornerOf(f, triangles[t * 3 + c]);
                            if (k != kInvalidIndex) {
                                scratch.push_back(k);
                            }
                        }
                        if (scratch.size() == 3) {
                            emit(hs, vs, scratch, reverse, material);
                        }
                    }
                } else {
                    scratch.resize(hs.size());
                    for (u32 k = 0; k < hs.size(); ++k) {
                        scratch[k] = k;
                    }
                    emit(hs, vs, scratch, reverse, material);
                }
            }
            skipped.report(diagnostics_, mesh.name, meshIndex, profile_);
            if (polygonIndex.empty()) {
                continue;
            }
            // Faces with no material get the default one, after the others.
            bool anyUnbound = false;
            for (i32& material : polygonMaterial) {
                if (material < 0) {
                    material = static_cast<i32>(meshMaterials.size());
                    anyUnbound = true;
                }
            }

            // The mesh model, at the model's identity: geometry is model space.
            const i64 meshModel = nextId();
            const std::string meshName = !mesh.name.empty() ? mesh.name : "mesh" + std::to_string(meshIndex);
            Node& modelNode = object("Model", meshModel, meshName, "Model", "Mesh");
            writeModelBody(modelNode, FileTrs{}, true, nullptr);
            connect(meshModel, modelIds_[m].root);

            const i64 geometry = nextId();
            Node& g = object("Geometry", geometry, meshName, "Geometry", "Mesh");
            std::vector<f64> vertices;
            vertices.reserve(positions.size() * 3);
            for (const Vector3f& p : positions) {
                const Vector3f f = basis_.toFile(p);
                vertices.insert(vertices.end(), {f.x, f.y, f.z});
            }
            g.add("Vertices", {Property::Doubles(std::move(vertices))});
            g.add("PolygonVertexIndex", {Property::Ints(polygonIndex)});
            g.add("GeometryVersion", {Property::Int(124)});
            std::vector<std::pair<const char*, i32>> layer0;
            if (!normalValues.empty()) {
                Node& e = g.add("LayerElementNormal", {Property::Int(0)});
                e.add("Version", {Property::Int(102)});
                e.add("Name", {Property::Str("")});
                e.add("MappingInformationType", {Property::Str("ByPolygonVertex")});
                e.add("ReferenceInformationType", {Property::Str("Direct")});
                e.add("Normals", {Property::Doubles(std::move(normalValues))});
                layer0.emplace_back("LayerElementNormal", 0);
            }
            if (!colorValues.empty()) {
                Node& e = g.add("LayerElementColor", {Property::Int(0)});
                e.add("Version", {Property::Int(101)});
                e.add("Name", {Property::Str("Col")});
                e.add("MappingInformationType", {Property::Str("ByPolygonVertex")});
                e.add("ReferenceInformationType", {Property::Str("IndexToDirect")});
                e.add("Colors", {Property::Doubles(std::move(colorValues))});
                e.add("ColorIndex", {Property::Ints(std::move(colorIndex))});
                layer0.emplace_back("LayerElementColor", 0);
            }
            for (std::size_t s = 0; s < uvSets.size(); ++s) {
                Node& e = g.add("LayerElementUV", {Property::Int(static_cast<i32>(s))});
                e.add("Version", {Property::Int(101)});
                e.add("Name", {Property::Str("UVSet" + std::to_string(uvSets[s].first))});
                e.add("MappingInformationType", {Property::Str("ByPolygonVertex")});
                e.add("ReferenceInformationType", {Property::Str("IndexToDirect")});
                e.add("UV", {Property::Doubles(std::move(uvValues[s]))});
                e.add("UVIndex", {Property::Ints(std::move(uvIndex[s]))});
            }
            {
                Node& e = g.add("LayerElementMaterial", {Property::Int(0)});
                e.add("Version", {Property::Int(101)});
                e.add("Name", {Property::Str("")});
                const bool same = std::all_of(polygonMaterial.begin(), polygonMaterial.end(),
                                              [&](i32 x) { return x == polygonMaterial.front(); });
                e.add("MappingInformationType", {Property::Str(same ? "AllSame" : "ByPolygon")});
                e.add("ReferenceInformationType", {Property::Str("IndexToDirect")});
                e.add("Materials", {Property::Ints(same ? std::vector<i32>{polygonMaterial.front()}
                                                        : polygonMaterial)});
                layer0.emplace_back("LayerElementMaterial", 0);
            }
            // The SDK reads elements through their layers.
            for (std::size_t s = 0; s < std::max<std::size_t>(uvSets.size(), 1); ++s) {
                Node& layer = g.add("Layer", {Property::Int(static_cast<i32>(s))});
                layer.add("Version", {Property::Int(100)});
                if (s == 0) {
                    for (const auto& [type, index] : layer0) {
                        Node& element = layer.add("LayerElement");
                        element.add("Type", {Property::Str(type)});
                        element.add("TypedIndex", {Property::Int(index)});
                    }
                }
                if (s < uvSets.size()) {
                    Node& element = layer.add("LayerElement");
                    element.add("Type", {Property::Str("LayerElementUV")});
                    element.add("TypedIndex", {Property::Int(static_cast<i32>(s))});
                }
            }
            connect(geometry, meshModel);

            // Materials, in the order the polygons index them.
            for (const u32 slot : meshMaterials) {
                connect(materialFor(model, slot, look, *surfaces[slot], uvSets), meshModel);
            }
            if (anyUnbound) {
                connect(defaultMaterial(), meshModel);
            }
            exportSkin(m, mesh, geometry, meshModel);
        }
    }

    // ---- materials ---------------------------------------------------------------------

    i64 materialFor(const Model& model, u32 slot, u32 look, const FlatSurface& surface,
                    const std::vector<std::pair<u32, std::span<const Vector2f>>>& uvSets) {
        const Material* source = Resolve(model, slot, profile_, look);
        const auto found = materialIds_.find(source);
        if (found != materialIds_.end()) {
            return found->second;
        }
        const i64 id = nextId();
        const std::string name = source != nullptr && !source->name.empty() ? source->name
                                                                            : model.materialSlots[slot];
        Node& material = object("Material", id, name, "Material", "");
        material.add("Version", {Property::Int(102)});
        material.add("ShadingModel", {Property::Str("phong")});
        material.add("MultiLayer", {Property::Int(0)});
        Node& p = material.add("Properties70");
        RowString(p, "ShadingModel", "phong");
        const Vector4f& base = surface.baseColorFactor;
        RowVec(p, "DiffuseColor", "Color", "", "A", {base.x, base.y, base.z});
        RowDouble(p, "DiffuseFactor", 1.0, "A");
        std::array<f64, 3> specular;
        f64 exponent = 20.0;
        if (surface.hasSpecular) {
            specular = {surface.specularFactor.x, surface.specularFactor.y, surface.specularFactor.z};
            exponent = surface.specularExponent;
        } else {
            // A metal's highlight takes its albedo; a dielectric's is the 4%.
            const f64 metal = surface.metallicFactor;
            specular = {0.04 + (base.x - 0.04) * metal, 0.04 + (base.y - 0.04) * metal,
                        0.04 + (base.z - 0.04) * metal};
            exponent = textures::pbr::ExponentFromRoughness(surface.roughnessFactor);
        }
        RowVec(p, "SpecularColor", "Color", "", "A", specular);
        RowDouble(p, "SpecularFactor", 1.0, "A");
        RowDouble(p, "ShininessExponent", exponent, "A");
        RowDouble(p, "Shininess", exponent, "A");
        bool emissiveMap = false;
        for (const SurfaceBinding& binding : surface.bindings) {
            emissiveMap = emissiveMap || (binding.role == SurfaceRole::Emissive &&
                                          TextureExportable(document_, binding.input));
        }
        Vector3f emissive = surface.emissiveFactor;
        if (surface.emissiveIsGain() && !emissiveMap) {
            emissive = Vector3f{0, 0, 0};
        }
        RowVec(p, "EmissiveColor", "Color", "", "A", {emissive.x, emissive.y, emissive.z});
        RowDouble(p, "EmissiveFactor", 1.0, "A");
        RowDouble(p, "Opacity", base.w, "A");
        RowDouble(p, "TransparencyFactor", 1.0 - base.w, "A");
        RowVec(p, "TransparentColor", "Color", "", "A", {1, 1, 1});
        switch (surface.blend) {
        case BlendMode::Opaque:
        case BlendMode::AlphaKey:
        case BlendMode::Transparent:
        case BlendMode::AlphaBlend:
            break;
        default:
            diagnostics_.warn(DiagCode::LossyBlendMode,
                              name + ": " + std::string(ToString(surface.blend)) +
                                  " has no Phong equivalent; exported as plain transparency");
            break;
        }

        bool normalBound = false;
        for (const SurfaceBinding& binding : surface.bindings) {
            const char* property = nullptr;
            bool normal = false;
            switch (binding.role) {
            case SurfaceRole::BaseColor:
                property = "DiffuseColor";
                break;
            case SurfaceRole::Normal:
                property = "NormalMap";
                normal = true;
                normalBound = true;
                break;
            case SurfaceRole::Emissive:
                property = "EmissiveColor";
                break;
            case SurfaceRole::Specular:
                property = "SpecularColor";
                break;
            case SurfaceRole::Gloss:
                property = "ShininessExponent";
                break;
            case SurfaceRole::Height:
                property = normalBound ? nullptr : "Bump";
                break;
            case SurfaceRole::Dropped:
                diagnostics_.warn(DiagCode::LayerDropped,
                                  name + ": " + binding.what + " does not cross to Phong");
                continue;
            default:
                diagnostics_.info(DiagCode::LayerDropped,
                                  name + ": a metal/rough map has no Phong property; its factor "
                                         "lowers into the specular colour and exponent");
                continue;
            }
            if (property == nullptr) {
                continue;
            }
            const i64 texture = textureFor(binding.input, normal, name, uvSets);
            if (texture == 0) {
                continue;
            }
            connectProperty(texture, id, property);
            if (binding.role == SurfaceRole::BaseColor &&
                (surface.blend == BlendMode::AlphaKey || surface.blend == BlendMode::Transparent)) {
                connectProperty(texture, id, "TransparentColor");
            }
        }
        materialIds_.emplace(source, id);
        return id;
    }

    i64 defaultMaterial() {
        if (defaultMaterial_ == 0) {
            defaultMaterial_ = nextId();
            Node& material = object("Material", defaultMaterial_, "default", "Material", "");
            material.add("Version", {Property::Int(102)});
            material.add("ShadingModel", {Property::Str("lambert")});
            material.add("MultiLayer", {Property::Int(0)});
            material.add("Properties70");
        }
        return defaultMaterial_;
    }

    /// The file texture for @p input; 0 when it has no file to name.
    i64 textureFor(const TextureInput& input, bool normal, const std::string& where,
                   const std::vector<std::pair<u32, std::span<const Vector2f>>>& uvSets) {
        if (!TextureExportable(document_, input)) {
            if (input.hasTexture()) {
                diagnostics_.info(DiagCode::TextureUnresolved,
                                  where + ": a replaceable or generated-UV texture has no file to "
                                          "name");
            }
            return 0;
        }
        const std::string file = names_.nameFor(input.texture, ImageChannel::All, normal);
        const u64 key = (static_cast<u64>(input.texture) << 8) ^ (static_cast<u64>(input.uvSet) << 4) ^
                        (input.wrapU == WrapMode::Clamp ? 1u : 0u) ^ (input.wrapV == WrapMode::Clamp ? 2u : 0u);
        const auto found = textureIds_.find(key);
        if (found != textureIds_.end() && input.uvTransform.isIdentity()) {
            return found->second;
        }
        const std::string stem = file.substr(0, file.find_last_of('.'));
        const i64 video = videoFor(file, stem);
        const i64 id = nextId();
        Node& texture = object("Texture", id, stem, "Texture", "");
        texture.add("Type", {Property::Str("TextureVideoClip")});
        texture.add("Version", {Property::Int(202)});
        texture.add("TextureName", {Property::Str(ObjectNameOf(stem, "Texture"))});
        Node& p = texture.add("Properties70");
        std::string uvSet;
        for (const auto& [set, data] : uvSets) {
            if (set == input.uvSet) {
                uvSet = "UVSet" + std::to_string(set);
            }
        }
        RowString(p, "UVSet", uvSet);
        Row(p, "UseMaterial", "bool", "", "").properties.push_back(Property::Int(1));
        RowInt(p, "WrapModeU", "enum", input.wrapU == WrapMode::Clamp ? 1 : 0);
        RowInt(p, "WrapModeV", "enum", input.wrapV == WrapMode::Clamp ? 1 : 0);
        if (!input.uvTransform.isIdentity()) {
            const auto& m = input.uvTransform.m;
            RowVec(p, "Translation", "Vector", "", "A", {m[0][2], 1.0 - m[1][1] - m[1][2], 0});
            RowVec(p, "Scaling", "Vector", "", "A", {m[0][0], m[1][1], 1});
        }
        texture.add("Media", {Property::Str(ObjectNameOf(stem, "Video"))});
        texture.add("FileName", {Property::Str(file)});
        texture.add("RelativeFilename", {Property::Str(file)});
        texture.add("ModelUVTranslation", {Property::Double(0), Property::Double(0)});
        texture.add("ModelUVScaling", {Property::Double(1), Property::Double(1)});
        texture.add("Texture_Alpha_Source", {Property::Str("None")});
        texture.add("Cropping", {Property::Int(0), Property::Int(0), Property::Int(0), Property::Int(0)});
        connect(video, id);
        textureIds_.emplace(key, id);
        return id;
    }

    i64 videoFor(const std::string& file, const std::string& stem) {
        const auto found = videoIds_.find(file);
        if (found != videoIds_.end()) {
            return found->second;
        }
        const i64 id = nextId();
        Node& video = object("Video", id, stem, "Video", "Clip");
        video.add("Type", {Property::Str("Clip")});
        RowString(video.add("Properties70"), "Path", file);
        video.add("UseMipMap", {Property::Int(0)});
        video.add("Filename", {Property::Str(file)});
        video.add("RelativeFilename", {Property::Str(file)});
        videoIds_.emplace(file, id);
        return id;
    }

    // ---- skins -----------------------------------------------------------------------

    void exportSkin(u32 m, const Mesh& mesh, i64 geometry, i64 meshModel) {
        const Model& model = document_.models[m];
        const u32 vertexCount = static_cast<u32>(
            mesh.attributes.get<Vector3f>(geom::names::kPosition, geom::Domain::Vertex).size());
        // Influences per vertex: the mesh's own, normalised, and a rigid
        // section's whole vertex onto its node (glTF's arrangement, §7).
        std::vector<std::vector<std::pair<u32, f32>>> influences(vertexCount);
        bool any = false;
        if (!mesh.skin.empty()) {
            for (u32 v = 0; v < vertexCount && v < mesh.skin.vertexCount(); ++v) {
                f32 total = 0.0f;
                for (const geom::Influence& influence : mesh.skin.forVertex(v)) {
                    if (influence.weight > 0.0f && influence.bone < model.nodes.size()) {
                        influences[v].emplace_back(influence.bone, influence.weight);
                        total += influence.weight;
                    }
                }
                if (total > 0.0f) {
                    for (auto& entry : influences[v]) {
                        entry.second /= total;
                    }
                    any = true;
                }
            }
        }
        const geom::FaceSet& faces = mesh.faceSet();
        const std::span<const u32> sectionOf = mesh.faceSections();
        std::size_t corner = 0;
        for (std::size_t f = 0; f < faces.faceCount(); ++f) {
            const u32 valence = faces.faceValence[f];
            const u32 section = f < sectionOf.size() ? sectionOf[f] : 0;
            if (section < mesh.sections.size() && mesh.sections[section].rigidNode.has_value()) {
                const u32 node = *mesh.sections[section].rigidNode;
                for (u32 k = 0; k < valence; ++k) {
                    const u32 v = faces.cornerVertex[corner + k];
                    if (v < vertexCount && influences[v].empty() && node < model.nodes.size()) {
                        influences[v].emplace_back(node, 1.0f);
                        any = true;
                    }
                }
            }
            corner += valence;
        }
        if (!any) {
            return;
        }
        std::vector<std::vector<i32>> indexes(model.nodes.size());
        std::vector<std::vector<f64>> weights(model.nodes.size());
        for (u32 v = 0; v < vertexCount; ++v) {
            for (const auto& [bone, weight] : influences[v]) {
                indexes[bone].push_back(static_cast<i32>(v));
                weights[bone].push_back(weight);
            }
        }
        const i64 skin = nextId();
        Node& skinNode = object("Deformer", skin, mesh.name, "Deformer", "Skin");
        skinNode.add("Version", {Property::Int(101)});
        skinNode.add("Link_DeformAcuracy", {Property::Double(50.0)});
        connect(skin, geometry);
        for (u32 bone = 0; bone < model.nodes.size(); ++bone) {
            if (indexes[bone].empty()) {
                continue;
            }
            const i64 cluster = nextId();
            Node& c = object("Deformer", cluster, model.nodes.nodes[bone].name, "SubDeformer", "Cluster");
            c.add("Version", {Property::Int(100)});
            c.add("UserData", {Property::Str(""), Property::Str("")});
            c.add("Indexes", {Property::Ints(std::move(indexes[bone]))});
            c.add("Weights", {Property::Doubles(std::move(weights[bone]))});
            // `Transform` is the mesh's bind relative to the link's, and the
            // mesh sits at the origin: the link's inverse.
            c.add("Transform",
                  {Property::Doubles(StoredOf(basis_.toFile(model.nodes.inverseBindMatrix(bone))))});
            const Matrix44f bindWorld = Matrix44f::inverse(model.nodes.inverseBindMatrix(bone));
            c.add("TransformLink", {Property::Doubles(StoredOf(basis_.toFile(bindWorld)))});
            connect(cluster, skin);
            connect(modelIds_[m].nodes[bone], cluster);
            boneBinds_.emplace_back(modelIds_[m].nodes[bone], basis_.toFile(bindWorld));
        }
        meshBinds_.push_back(meshModel);
    }

    /// The sixteen doubles FBX stores for a matrix: WEM's row-vector layout
    /// is exactly that order (fbx.h, `Matrix4d::FromStored`).
    static std::vector<f64> StoredOf(const Matrix44f& m) {
        std::vector<f64> out(16);
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t c = 0; c < 4; ++c) {
                out[r * 4 + c] = m.data[r][c];
            }
        }
        return out;
    }

    void exportBindPose() {
        if (boneBinds_.empty()) {
            return;
        }
        const i64 id = nextId();
        Node& pose = object("Pose", id, "BindPose", "Pose", "BindPose");
        pose.add("Type", {Property::Str("BindPose")});
        pose.add("Version", {Property::Int(100)});
        std::vector<std::pair<i64, Matrix44f>> nodes;
        for (const i64 mesh : meshBinds_) {
            nodes.emplace_back(mesh, Matrix44f::identity());
        }
        for (const auto& entry : boneBinds_) {
            if (std::none_of(nodes.begin(), nodes.end(),
                             [&](const auto& n) { return n.first == entry.first; })) {
                nodes.push_back(entry);
            }
        }
        pose.add("NbPoseNodes", {Property::Int(static_cast<i32>(nodes.size()))});
        for (const auto& [node, matrix] : nodes) {
            Node& p = pose.add("PoseNode");
            p.add("Node", {Property::Long(node)});
            p.add("Matrix", {Property::Doubles(StoredOf(matrix))});
        }
    }

    // ---- animation -------------------------------------------------------------------

    void exportAnimation() {
        u32 dropped = 0;
        for (u32 c = 0; c < document_.clips.size(); ++c) {
            if (!clipWritten(c)) {
                continue;
            }
            const Clip& clip = document_.clips[c];
            if (clip.model >= document_.models.size()) {
                continue;
            }
            const Model& model = document_.models[clip.model];
            for (const SubTrackContainer& container : clip.containers) {
                for (const SubTrack& track : container.subTracks) {
                    const AnimChannel* channel = model.animChannels.find(track.channel);
                    if (channel != nullptr &&
                        !(channel->target.kind == TrackTarget::Kind::Node &&
                          (channel->target.channel == Channel::Translation ||
                           channel->target.channel == Channel::Rotation ||
                           channel->target.channel == Channel::Scale))) {
                        ++dropped;
                    }
                }
            }
            const ClipPose pose(document_, clip.model, c);
            if (pose.empty()) {
                continue;
            }
            const i64 stop = Ticks(clip.duration);
            const i64 stack = nextId();
            Node& stackNode = object("AnimationStack", stack, clip.name, "AnimStack", "");
            Node& sp = stackNode.add("Properties70");
            RowTime(sp, "LocalStart", 0);
            RowTime(sp, "LocalStop", stop);
            RowTime(sp, "ReferenceStart", 0);
            RowTime(sp, "ReferenceStop", stop);
            const i64 layer = nextId();
            object("AnimationLayer", layer, "BaseLayer", "AnimLayer", "").block = true;
            connect(layer, stack);
            takes_.emplace_back(clip.name, stop);

            // Samples: the frame grid over the clip, its end, and every key.
            std::vector<f32> times;
            const f32 step = 1.0f / std::max(options_.frameRate, 1.0f);
            for (f32 t = 0.0f; t < clip.duration; t += step) {
                times.push_back(t);
            }
            times.push_back(clip.duration);
            for (const f32 key : pose.keyTimes()) {
                if (key > 0.0f && key < clip.duration) {
                    times.push_back(key);
                }
            }
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end(),
                                    [](f32 a, f32 b) { return std::fabs(a - b) < 1e-6f; }),
                        times.end());
            std::vector<f64> seconds(times.begin(), times.end());

            // A node's FBX local is its frame against its FBX parent's, whatever
            // the rig: a pivot rig's track values are offsets, not locals.
            std::vector<std::vector<Matrix44f>> frames(times.size());
            for (std::size_t k = 0; k < times.size(); ++k) {
                pose.framesAt(times[k], frames[k]);
            }
            const ModelIds& ids = modelIds_[clip.model];
            for (u32 node = 0; node < model.nodes.size(); ++node) {
                const u32 parent = model.nodes.nodes[node].parent;
                const bool rooted = parent != kInvalidNode && parent < model.nodes.size();
                std::array<std::vector<f64>, 9> channels; // T xyz, R xyz, S xyz
                std::array<f64, 3> previous = ids.rest[node].r;
                for (std::size_t k = 0; k < times.size(); ++k) {
                    const Matrix44f local =
                        rooted ? frames[k][node] * Matrix44f::inverse(frames[k][parent]) : frames[k][node];
                    const FileTrs trs = TrsOf(local, basis_);
                    const std::array<f64, 3> euler = NearestEuler(trs.r, previous);
                    previous = euler;
                    for (std::size_t a = 0; a < 3; ++a) {
                        channels[a].push_back(trs.t[a]);
                        channels[3 + a].push_back(euler[a]);
                        channels[6 + a].push_back(trs.s[a]);
                    }
                }
                const FileTrs& rest = ids.rest[node];
                writeCurveNode(layer, ids.nodes[node], "T", "Lcl Translation", seconds,
                               {&channels[0], &channels[1], &channels[2]}, rest.t, 1e-4);
                writeCurveNode(layer, ids.nodes[node], "R", "Lcl Rotation", seconds,
                               {&channels[3], &channels[4], &channels[5]}, rest.r, 1e-3);
                writeCurveNode(layer, ids.nodes[node], "S", "Lcl Scaling", seconds,
                               {&channels[6], &channels[7], &channels[8]}, rest.s, 1e-5);
            }
        }
        if (dropped != 0) {
            diagnostics_.info(DiagCode::AnimTrackDropped,
                              std::to_string(dropped) +
                                  " material, UV, visibility or feature track(s) have no FBX node "
                                  "channel; dropped");
        }
    }

    void writeCurveNode(i64 layer, i64 model, const char* name, const char* property,
                        const std::vector<f64>& seconds, std::array<const std::vector<f64>*, 3> values,
                        const std::array<f64, 3>& rest, f64 tolerance) {
        // A channel that holds the model's static value through the clip is left to it.
        bool still = true;
        for (std::size_t a = 0; a < 3; ++a) {
            still = still && Constant(*values[a], tolerance) &&
                    std::fabs(values[a]->front() - rest[a]) <= tolerance;
        }
        if (still) {
            return;
        }
        const i64 id = nextId();
        Node& curveNode = object("AnimationCurveNode", id, name, "AnimCurveNode", "");
        Node& p = curveNode.add("Properties70");
        const char* axes[3] = {"d|X", "d|Y", "d|Z"};
        for (std::size_t a = 0; a < 3; ++a) {
            RowDouble(p, axes[a], values[a]->front(), "A");
        }
        connect(id, layer);
        connectProperty(id, model, property);
        for (std::size_t a = 0; a < 3; ++a) {
            const std::vector<std::size_t> kept = ReduceLinear(seconds, *values[a], tolerance);
            std::vector<i64> keyTimes;
            std::vector<f32> keyValues;
            for (const std::size_t k : kept) {
                keyTimes.push_back(static_cast<i64>(std::llround(seconds[k] * fbx::kTicksPerSecond)));
                keyValues.push_back(static_cast<f32>((*values[a])[k]));
            }
            const i64 curve = nextId();
            Node& c = object("AnimationCurve", curve, "", "AnimCurve", "");
            c.add("Default", {Property::Double((*values[a])[0])});
            c.add("KeyVer", {Property::Int(4009)});
            c.add("KeyTime", {Property::Longs(std::move(keyTimes))});
            const i32 count = static_cast<i32>(keyValues.size());
            c.add("KeyValueFloat", {Property::Floats(std::move(keyValues))});
            c.add("KeyAttrFlags", {Property::Ints({fbx::keyflags::kLinear})});
            c.add("KeyAttrDataFloat", {Property::Floats({0.0f, 0.0f, DefaultWeights(), 0.0f})});
            c.add("KeyAttrRefCount", {Property::Ints({count})});
            connectProperty(curve, id, axes[a]);
        }
    }

    const Document& document_;
    ProfileId profile_;
    const FbxWriteOptions& options_;
    AxisBasis basis_;
    Diagnostics& diagnostics_;
    InterchangeImageNames names_;
    Node objects_{"Objects"};
    Node connections_{"Connections"};
    std::map<std::string, u32> counts_;
    i64 nextId_ = 0;
    std::vector<bool> claimed_;
    std::vector<ModelIds> modelIds_;
    std::set<u32> attached_;
    std::unordered_map<const Material*, i64> materialIds_;
    std::unordered_map<u64, i64> textureIds_;
    std::unordered_map<std::string, i64> videoIds_;
    i64 defaultMaterial_ = 0;
    std::vector<std::pair<i64, Matrix44f>> boneBinds_;
    std::vector<i64> meshBinds_;
    std::vector<std::pair<std::string, i64>> takes_;
    u32 recomposed_ = 0;
};

} // namespace

bool EmbedFbxMedia(fbx::File& file, std::string_view name, std::span<const u8> bytes) {
    bool embedded = false;
    for (fbx::Node& top : file.nodes) {
        if (top.name != "Objects") {
            continue;
        }
        for (fbx::Node& video : top.children) {
            const fbx::Node* relative = video.child("RelativeFilename");
            if (video.name != "Video" || relative == nullptr || relative->properties.empty() ||
                relative->properties[0].text != name) {
                continue;
            }
            fbx::Node* content = nullptr;
            for (fbx::Node& child : video.children) {
                content = child.name == "Content" ? &child : content;
            }
            if (content == nullptr) {
                content = &video.add("Content");
            }
            content->properties = {
                fbx::Property::RawBytes(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()))};
            embedded = true;
        }
    }
    return embedded;
}

Result<FbxExport> FbxConverter::toFbx(const Document& document, ProfileId profile,
                                      const FbxWriteOptions& options) const {
    Result<FbxExport> result;
    // Any carried profile, as glTF: FBX serves `Generic` only on the way in.
    if (!document.carries(profile)) {
        result.diagnostics.error(DiagCode::ProfileNotCarried,
                                 "the document does not carry profile " +
                                     std::string(ToString(profile)),
                                 {}, profile);
        return result;
    }
    if (options.version < 7100 || options.version > 7700) {
        result.diagnostics.error(DiagCode::UnsupportedVersion,
                                 "FBX " + std::to_string(options.version) +
                                     " is not a version this writer produces (7100-7700)");
        return result;
    }
    Exporter exporter(document, profile, options, result.diagnostics);
    result.value = exporter.run();
    return result;
}

Result<std::vector<u8>> FbxConverter::exportToBytes(const Document& document, ProfileId profile,
                                                    u32 version) const {
    FbxWriteOptions options;
    if (version != 0) {
        options.version = version;
    }
    Result<FbxExport> converted = toFbx(document, profile, options);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    result.value = fbx::WriteBinary(converted->file);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
