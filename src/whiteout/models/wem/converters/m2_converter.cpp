// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/**
 * @file m2_converter.cpp
 * @brief `.m2` <-> `Document` (design §14, §10.7, §5.5).
 *
 * ### The vertex indirection is the import
 *
 * A `SkinProfile` does not own vertices. Its `vertices` array is an indirection
 * into the model's global vertex list, and its `indices` address *that* array,
 * not the global one. Import flattens both into a mesh-local space, which is
 * also what makes one `Mesh` per skin profile a self-contained thing an editor
 * can work on.
 *
 * ### A batch is a draw, a submesh is a section
 *
 * §5.5's convention is one `MeshSection` per `SkinSection`, and a batch names a
 * submesh plus everything about how it draws. Several batches can name the same
 * submesh — that is M2's multi-pass — and a face carries one section, so the
 * section binds the lowest-`materialLayer` batch and the later passes get slots
 * of their own that no section references. They are reported as
 * `MaterialSlotUnused` rather than dropped: the material is real, WEM's section
 * model just has nowhere to draw it from yet.
 *
 * ### Nodes hang off their bone
 *
 * §10.7: an attachment, light, event or camera record carries a `boneId` and a
 * position in **model space**. It becomes a child of that bone with the position
 * rebased through the bone's bind pose, so moving the bone moves the attachment,
 * which is what the game does and what a flat list of model-space points does
 * not.
 */

#include "whiteout/models/m2/writer.h"
#include "whiteout/models/wem/converters.h"
#include "whiteout/models/wem/geometry/builder.h"
#include "whiteout/models/wem/geometry/render_view.h"

#include "../materials/m2_core.h"
#include "m2_anim.h"

#include <algorithm>
#include <array>
#include <string>
#include <variant>

namespace whiteout {
namespace models {
namespace wem {

namespace {

constexpr ProfileId kM2Profiles[] = {ProfileId::Wow};

Extent ToExtent(const m2::Extent& source) {
    Extent out;
    out.minimum = source.minimum;
    out.maximum = source.maximum;
    out.sphereRadius = source.sphereRadius;
    return out;
}

m2::Extent FromExtent(const Extent& source) {
    m2::Extent out;
    out.minimum = source.minimum;
    out.maximum = source.maximum;
    out.sphereRadius = source.sphereRadius;
    return out;
}

/// A submesh's first index into `skin.indices`.
///
/// `SkinSection::indexStart` is a u16 and a skin profile routinely holds more
/// than 65535 indices, so the format carries the missing high word in the
/// neighbouring `level` field rather than widening the struct. Reading
/// `indexStart` alone is *in bounds* -- it silently draws another submesh's
/// triangles -- which is why the symptom was a 113% manifold-repair rate rather
/// than a crash. `vertexStart` gets no such treatment: it tiles on its own and a
/// profile stays under 65536 vertices.
std::size_t IndexStart(const m2::SkinSection& section) {
    return (static_cast<std::size_t>(section.level) << 16) | section.indexStart;
}

std::string BatchSlotName(std::size_t skin, std::size_t batch) {
    return "batch_" + std::to_string(skin) + "_" + std::to_string(batch);
}

NodeFlags ToNodeFlags(u32 boneFlags) {
    NodeFlags out = NodeFlags::None;
    if (boneFlags & static_cast<u32>(m2::BoneFlag::IgnoreParentTranslate)) {
        out |= NodeFlags::DontInheritTranslation;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::IgnoreParentScale)) {
        out |= NodeFlags::DontInheritScale;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::IgnoreParentRotation)) {
        out |= NodeFlags::DontInheritRotation;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::SphericalBillboard)) {
        out |= NodeFlags::Billboarded;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::CylindricalBillboardX)) {
        out |= NodeFlags::BillboardLockX;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::CylindricalBillboardY)) {
        out |= NodeFlags::BillboardLockY;
    }
    if (boneFlags & static_cast<u32>(m2::BoneFlag::CylindricalBillboardZ)) {
        out |= NodeFlags::BillboardLockZ;
    }
    return out;
}

/// Bones first, then every record that hangs off one.
///
/// Bone indices keep their source numbering because `Vertex::boneIndices` and
/// `boneCombos` both address that array; the attached records go after, so the
/// join survives.
NodeTree ImportNodes(const m2::Model& source) {
    NodeTree tree;
    tree.poseSchema.push_back(PoseSchema{});
    tree.authoritativePose = 0;
    tree.rig = RigConvention::PivotRelative;

    for (std::size_t b = 0; b < source.bones.size(); ++b) {
        const m2::Bone& bone = source.bones[b];
        Node node;
        node.name = "bone_" + std::to_string(b);
        node.kind = NodeKind::Bone;
        node.resetPayloadForKind();
        node.flags = ToNodeFlags(bone.flags);
        node.parent = bone.parentBoneId < 0 ? kInvalidNode : static_cast<u32>(bone.parentBoneId);
        node.native.set("keyBoneId", bone.keyBoneId);
        node.native.set("m2FlagBits", static_cast<i64>(bone.flags));
        node.native.set("submeshId", static_cast<i64>(bone.submeshId));
        node.native.set("boneNameCRC", static_cast<i64>(bone.boneNameCRC));

        // M2 pivots are absolute model space, like MDX's, so the local
        // translation is the difference from the parent's.
        Vector3f parentPivot{0, 0, 0};
        if (node.parent != kInvalidNode && node.parent < tree.size()) {
            parentPivot = tree.nodes[node.parent].pivot;
        }
        node.pivot = bone.pivot;
        node.local.translation =
            Vector3f{bone.pivot.x - parentPivot.x, bone.pivot.y - parentPivot.y,
                     bone.pivot.z - parentPivot.z};
        node.poses.push_back(node.local);
        tree.add(std::move(node));
    }

    const u32 boneCount = tree.size();
    const auto attach = [&](u32 boneId, const Vector3f& modelSpace, Node& node) {
        node.parent = boneId < boneCount ? boneId : kInvalidNode;
        Vector3f origin{0, 0, 0};
        if (node.parent != kInvalidNode) {
            origin = tree.nodes[node.parent].pivot;
        }
        // The pivot as well as the local, because in a pivot rig the pivot IS
        // the point the record names — that is what `.mdx` writes into `PIVT`
        // for the same node, and leaving it at zero put every `.m2` attachment
        // at the model origin on the way out to WC3.
        node.pivot = modelSpace;
        node.local.translation =
            Vector3f{modelSpace.x - origin.x, modelSpace.y - origin.y, modelSpace.z - origin.z};
        node.poses.push_back(node.local);
    };

    for (std::size_t a = 0; a < source.attachments.size(); ++a) {
        const m2::Attachment& attachment = source.attachments[a];
        Node node;
        node.name = "attachment_" + std::to_string(attachment.id);
        node.kind = NodeKind::Attachment;
        node.resetPayloadForKind();
        // The equip-slot key, which is the whole point of an M2 attachment.
        node.native.set("m2AttachmentId", static_cast<i64>(attachment.id));
        attach(attachment.boneId, attachment.position, node);
        tree.add(std::move(node));
    }

    for (std::size_t l = 0; l < source.lights.size(); ++l) {
        const m2::Light& light = source.lights[l];
        Node node;
        node.name = "light_" + std::to_string(l);
        node.kind = NodeKind::Light;
        node.resetPayloadForKind();
        auto& payload = std::get<LightPayload>(node.payload);
        payload.kind = light.type == 0 ? LightKind::Directional : LightKind::Omni;
        node.native.set("m2LightType", static_cast<i64>(light.type));
        attach(light.boneId < 0 ? 0xFFFFu : static_cast<u32>(light.boneId), light.position, node);
        tree.add(std::move(node));
    }

    for (std::size_t e = 0; e < source.events.size(); ++e) {
        const m2::Event& event = source.events[e];
        Node node;
        node.name = "event_" + std::to_string(e);
        node.kind = NodeKind::Event;
        node.resetPayloadForKind();
        std::get<EventPayload>(node.payload).id = event.identifier;
        node.native.set("eventData", static_cast<i64>(event.data));
        attach(event.boneId, event.position, node);
        tree.add(std::move(node));
    }

    for (std::size_t r = 0; r < source.ribbonEmitters.size(); ++r) {
        const m2::RibbonEmitter& ribbon = source.ribbonEmitters[r];
        Node node;
        node.name = "ribbon_" + std::to_string(r);
        node.kind = NodeKind::RibbonEmitter;
        node.resetPayloadForKind();
        std::get<RibbonPayload>(node.payload).system.id = ribbon.ribbonId;
        attach(ribbon.boneId, ribbon.position, node);
        tree.add(std::move(node));
    }

    for (std::size_t c = 0; c < source.cameras.size(); ++c) {
        const m2::Camera& camera = source.cameras[c];
        Node node;
        node.name = "camera_" + std::to_string(c);
        node.kind = NodeKind::Camera;
        node.resetPayloadForKind();
        auto& payload = std::get<CameraPayload>(node.payload);
        payload.fov = camera.fieldOfView;
        payload.nearClip = camera.nearClip;
        payload.farClip = camera.farClip;
        node.native.set("cameraType", static_cast<i64>(camera.type));
        node.local.translation = camera.positionBase;
        node.poses.push_back(node.local);
        tree.add(std::move(node));
    }

    return tree;
}

} // namespace

// ============================================================================
// fromM2
// ============================================================================

Result<Document> M2Converter::fromM2(const m2::Model& source, u32 sourceVersion) const {
    Result<Document> result;
    Document document;
    Diagnostics& diagnostics = result.diagnostics;

    document.name = source.modelName;
    document.bounds = ToExtent(source.bounding);
    document.space = CoordSpace::Blizzard;
    document.declare(ProfileId::Wow);
    document.defaultProfile = ProfileId::Wow;

    m2_core::Context context;
    context.sourceVersion = sourceVersion;
    document.textures.reserve(source.textures.size());
    for (std::size_t t = 0; t < source.textures.size(); ++t) {
        TextureRef ref;
        ref.path = source.textures[t].filename;
        ref.flags = source.textures[t].flags;
        // §7.4: WoW's texture *type* is the slot a skin or an item fills, and
        // a type-2 (or 11/12/13) reference is blank in the file on purpose.
        ref.slotType = static_cast<u8>(source.textures[t].type);
        ref.replaceableId = source.textures[t].type;
        if (t < source.texture_ids.size() && source.texture_ids[t] != 0) {
            ref.key = TextureFileDataId{source.texture_ids[t]};
        } else if (!ref.path.empty()) {
            ref.key = TexturePath{ref.path};
        }
        context.textureIndexMap.push_back(static_cast<u32>(document.textures.size()));
        document.textures.push_back(std::move(ref));
    }

    Model model;
    model.name = source.modelName;
    model.bounds = document.bounds;
    model.nodes = ImportNodes(source);

    m2_anim::Context animContext;
    animContext.bases = m2_anim::NodeBases::Of(source);
    animContext.slotOfBatch.resize(source.skinProfiles.size());

    ProfileMaterialSet set;
    set.profile = ProfileId::Wow;
    set.looks.looks.push_back(Look{});
    // The model header's own fields are profile-scoped (§6.3): they describe how
    // *this* game reads the model, and a second set would carry its own.
    set.native.set("globalFlags", static_cast<i64>(static_cast<u32>(source.globalFlags.value)));
    set.native.set("sourceVersion", static_cast<i64>(sourceVersion));

    // --- one mesh per skin profile -----------------------------------------
    for (std::size_t s = 0; s < source.skinProfiles.size(); ++s) {
        const m2::SkinProfile& skin = source.skinProfiles[s];

        // A submesh's section index in this mesh, so a batch can find it.
        std::vector<u32> sectionOfSubmesh(skin.submeshes.size(), kInvalidIndex);
        // The batch each submesh draws with first — the one whose material the
        // section binds. `kInvalidIndex` means nothing draws it.
        std::vector<u32> baseBatchOfSubmesh(skin.submeshes.size(), kInvalidIndex);
        for (std::size_t b = 0; b < skin.batches.size(); ++b) {
            const m2::Batch& batch = skin.batches[b];
            if (batch.skinSectionIndex >= skin.submeshes.size()) {
                diagnostics.warn(DiagCode::IndexOutOfRange,
                                 "batch names submesh " + std::to_string(batch.skinSectionIndex) +
                                     ", past the end",
                                 ElementRef(ElementKind::Mesh, s));
                continue;
            }
            const u32 current = baseBatchOfSubmesh[batch.skinSectionIndex];
            if (current == kInvalidIndex ||
                batch.materialLayer < skin.batches[current].materialLayer) {
                baseBatchOfSubmesh[batch.skinSectionIndex] = static_cast<u32>(b);
            }
        }

        // Every batch becomes a slot and a material, in batch order, so the
        // set's arrays and the source's line up whatever the sections bind.
        std::vector<u32> slotOfBatch(skin.batches.size(), kInvalidIndex);
        for (std::size_t b = 0; b < skin.batches.size(); ++b) {
            const u32 slot = model.addSlot(BatchSlotName(s, b));
            slotOfBatch[b] = slot;
        }
        animContext.slotOfBatch[s] = slotOfBatch;

        geom::MeshBuilder builder;
        for (std::size_t sub = 0; sub < skin.submeshes.size(); ++sub) {
            const m2::SkinSection& submesh = skin.submeshes[sub];
            MeshSection section;
            section.name = "submesh_" + std::to_string(submesh.skinSectionId);
            section.selectionGroup = submesh.skinSectionId;
            section.native.set("skinSectionId", static_cast<i64>(submesh.skinSectionId));
            // `level` is deliberately not carried: it is not independent data,
            // it is the high word of `indexStart`, and export recomputes both
            // from the ranges it actually writes.
            const u32 base = baseBatchOfSubmesh[sub];
            if (base != kInvalidIndex) {
                section.materialSlot = slotOfBatch[base];
                section.native.set("batchFlags", static_cast<i64>(skin.batches[base].flags));
            } else {
                section.profiles = kNoProfiles;
                diagnostics.info(DiagCode::SectionUndrawn,
                                 "submesh " + std::to_string(sub) + " has no batch",
                                 ElementRef(ElementKind::Section, sub), ProfileId::Wow);
            }
            sectionOfSubmesh[sub] = builder.addSection(std::move(section));
        }

        // The skin's vertex indirection, flattened.
        std::vector<u32> globalOf(skin.vertices.size(), 0);
        for (std::size_t v = 0; v < skin.vertices.size(); ++v) {
            const u32 global = skin.vertices[v];
            globalOf[v] = global;
            const m2::Vertex& vertex =
                global < source.vertices.size() ? source.vertices[global] : m2::Vertex{};
            builder.addVertex(vertex.position);
            for (std::size_t k = 0; k < 4; ++k) {
                const f32 weight = static_cast<f32>(vertex.boneWeights[k]) / 255.0f;
                if (weight <= 0.0f) {
                    continue;
                }
                // Global, unlike `.m3`'s: the retail client indexes its bone
                // matrix array with this byte directly. The section-local twin
                // resolved through `boneCombos` lives in the `.skin`'s own
                // `bones` array and reaches the same bone.
                builder.addInfluence(geom::VertexId(static_cast<u32>(v)), vertex.boneIndices[k],
                                     weight);
            }
        }

        for (std::size_t sub = 0; sub < skin.submeshes.size(); ++sub) {
            const m2::SkinSection& submesh = skin.submeshes[sub];
            const std::size_t first = IndexStart(submesh);
            const std::size_t last = first + submesh.indexCount;
            for (std::size_t i = first; i + 2 < last && i + 2 < skin.indices.size(); i += 3) {
                const std::array<u32, 3> corners = {skin.indices[i + 0], skin.indices[i + 1],
                                                    skin.indices[i + 2]};
                if (corners[0] >= skin.vertices.size() || corners[1] >= skin.vertices.size() ||
                    corners[2] >= skin.vertices.size()) {
                    diagnostics.warn(DiagCode::IndexOutOfRange,
                                     "submesh index past the skin's vertex list",
                                     ElementRef(ElementKind::Mesh, s));
                    continue;
                }
                const geom::FaceId face =
                    builder.addTriangle(geom::VertexId(corners[0]), geom::VertexId(corners[1]),
                                        geom::VertexId(corners[2]), sectionOfSubmesh[sub]);
                for (u32 c = 0; c < 3; ++c) {
                    const u32 global = globalOf[corners[c]];
                    if (global >= source.vertices.size()) {
                        continue;
                    }
                    const m2::Vertex& vertex = source.vertices[global];
                    builder.setCornerAttr(face, c, geom::names::kNormal, vertex.normal);
                    builder.setCornerAttr(face, c, geom::names::uv(0), vertex.texCoords[0]);
                    builder.setCornerAttr(face, c, geom::names::uv(1), vertex.texCoords[1]);
                }
            }
        }

        geom::MeshBuilder::BuildOutcome outcome = builder.build();
        outcome.mesh.name = "skin_" + std::to_string(s);
        outcome.mesh.lodLevel = static_cast<u32>(s);
        outcome.mesh.recomputeBounds();
        model.meshes.push_back(std::move(outcome.mesh));

        // The materials, after the mesh so a batch's diagnostics land with its
        // mesh in the report's order.
        for (std::size_t b = 0; b < skin.batches.size(); ++b) {
            Material material = m2_core::ImportBatch(source, skin.batches[b], context, diagnostics);
            material.name = BatchSlotName(s, b);
            set.resizeBindings(model.materialSlots.size());
            set.slotBindings[slotOfBatch[b]].byLook[0] = static_cast<u32>(set.materials.size());
            set.materials.push_back(std::move(material));
        }
        for (std::size_t b = 0; b < skin.batches.size(); ++b) {
            const u32 sub = skin.batches[b].skinSectionIndex;
            if (sub < baseBatchOfSubmesh.size() && baseBatchOfSubmesh[sub] != b) {
                diagnostics.info(DiagCode::MaterialSlotUnused,
                                 "batch " + std::to_string(b) + " is pass " +
                                     std::to_string(skin.batches[b].materialLayer) +
                                     " over submesh " + std::to_string(sub) +
                                     "; imported but no section draws it",
                                 ElementRef(ElementKind::Slot, slotOfBatch[b]), ProfileId::Wow);
            }
        }
    }

    set.resizeBindings(model.materialSlots.size());
    model.profileSets.push_back(std::move(set));

    const u32 modelIndex = static_cast<u32>(document.models.size());
    document.models.push_back(std::move(model));
    m2_anim::Import(source, animContext, document, modelIndex, diagnostics);

    result.value = std::move(document);
    return result;
}

// ============================================================================
// toM2
// ============================================================================

Result<m2::Model> M2Converter::toM2(const Document& document, ProfileId profile,
                                    u32 targetVersion) const {
    Result<m2::Model> result;
    if (!checkExportProfile(document, profile, result.diagnostics)) {
        return result;
    }
    checkRigConvention(document, profile, result.diagnostics);

    Diagnostics& diagnostics = result.diagnostics;
    m2::Model out;
    if (document.models.empty()) {
        result.value = std::move(out);
        return result;
    }

    const Model& model = document.models.front();
    out.modelName = document.name.empty() ? model.name : document.name;
    out.bounding = FromExtent(model.bounds);
    out.collision = out.bounding;
    const ProfileMaterialSet* set = model.setFor(profile);
    if (set != nullptr) {
        out.globalFlags.value =
            static_cast<m2::GlobalFlag>(static_cast<u32>(set->native.value("globalFlags")));
    }

    // --- textures -----------------------------------------------------------
    m2_core::Context context;
    context.sourceVersion = targetVersion;
    for (const TextureRef& ref : document.textures) {
        m2::Texture texture;
        texture.filename = ref.path;
        texture.flags = ref.flags;
        texture.type = ref.slotType;
        context.textureIndexMap.push_back(static_cast<u32>(out.textures.size()));
        out.textures.push_back(std::move(texture));
        const auto* fileId = std::get_if<TextureFileDataId>(&ref.key);
        out.texture_ids.push_back(fileId == nullptr ? 0u : fileId->value);
    }

    // --- bones --------------------------------------------------------------
    //
    // Only Bone nodes become bones, and they keep their WEM order, so a vertex's
    // influence index is the bone index. The attached kinds are written from the
    // same tree below, their local translation composed back into model space.
    // Where each node lands, for the animation export below: an `.m2` keeps a
    // record's tracks ON the record, and only this walk knows which array a
    // node went into.
    m2_anim::ExportContext animContext;
    animContext.nodeSlots.assign(model.nodes.size(), m2_anim::ExportContext::NodeSlot{});

    std::vector<u32> boneOf(model.nodes.size(), 0xFFFFu);
    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        if (model.nodes.nodes[n].kind != NodeKind::Bone) {
            continue;
        }
        const Node& node = model.nodes.nodes[n];
        boneOf[n] = static_cast<u32>(out.bones.size());
        animContext.nodeSlots[n] = {m2_anim::ExportContext::Slot::Bone,
                                    static_cast<u32>(out.bones.size())};
        m2::Bone bone;
        bone.keyBoneId = static_cast<i32>(node.native.value("keyBoneId", -1));
        bone.flags = static_cast<u32>(node.native.value("m2FlagBits"));
        bone.submeshId = static_cast<u16>(node.native.value("submeshId"));
        bone.boneNameCRC = static_cast<u32>(node.native.value("boneNameCRC"));
        bone.parentBoneId = -1;
        if (node.parent != kInvalidNode && node.parent < boneOf.size() &&
            boneOf[node.parent] != 0xFFFFu) {
            bone.parentBoneId = static_cast<i16>(boneOf[node.parent]);
        }
        bone.pivot = model.nodes.worldBind(static_cast<u32>(n)).translation;
        out.bones.push_back(std::move(bone));
    }

    for (std::size_t n = 0; n < model.nodes.size(); ++n) {
        const Node& node = model.nodes.nodes[n];
        const Vector3f world = model.nodes.worldBind(static_cast<u32>(n)).translation;
        const u16 parentBone = node.parent != kInvalidNode && node.parent < boneOf.size() &&
                                       boneOf[node.parent] != 0xFFFFu
                                   ? static_cast<u16>(boneOf[node.parent])
                                   : 0u;
        switch (node.kind) {
        case NodeKind::Attachment: {
            m2::Attachment attachment;
            attachment.id = static_cast<u32>(node.native.value("m2AttachmentId"));
            attachment.boneId = parentBone;
            attachment.position = world;
            animContext.nodeSlots[n] = {m2_anim::ExportContext::Slot::Attachment,
                                        static_cast<u32>(out.attachments.size())};
            out.attachments.push_back(std::move(attachment));
            break;
        }
        case NodeKind::Light: {
            m2::Light light;
            light.type = static_cast<u16>(node.native.value("m2LightType"));
            light.boneId = static_cast<i16>(parentBone);
            light.position = world;
            animContext.nodeSlots[n] = {m2_anim::ExportContext::Slot::Light,
                                        static_cast<u32>(out.lights.size())};
            out.lights.push_back(std::move(light));
            break;
        }
        case NodeKind::Event: {
            m2::Event event;
            if (const auto* payload = std::get_if<EventPayload>(&node.payload)) {
                event.identifier = payload->id;
            }
            event.data = static_cast<u32>(node.native.value("eventData"));
            event.boneId = parentBone;
            event.position = world;
            animContext.nodeSlots[n] = {m2_anim::ExportContext::Slot::Event,
                                        static_cast<u32>(out.events.size())};
            out.events.push_back(std::move(event));
            break;
        }
        case NodeKind::Camera: {
            m2::Camera camera;
            camera.type = static_cast<u32>(node.native.value("cameraType"));
            if (const auto* payload = std::get_if<CameraPayload>(&node.payload)) {
                camera.fieldOfView = payload->fov;
                camera.nearClip = payload->nearClip;
                camera.farClip = payload->farClip;
            }
            camera.positionBase = world;
            animContext.nodeSlots[n] = {m2_anim::ExportContext::Slot::Camera,
                                        static_cast<u32>(out.cameras.size())};
            out.cameras.push_back(std::move(camera));
            break;
        }
        default:
            break;
        }
    }

    // --- geometry + batches -------------------------------------------------
    //
    // Export writes one skin profile per mesh, which is the inverse of import's
    // one mesh per skin. The global vertex list is the concatenation of the
    // meshes' and each skin's indirection is the identity over its own slice —
    // WEM has no shared vertex pool to preserve, and building one would mean
    // welding across meshes that the import deliberately kept apart.
    geom::RenderMeshDesc desc;
    desc.attributes = {
        {geom::names::kPosition, utils::AttributeClass::Position, utils::AttributeEncoding::Float32,
         3, 0},
        {geom::names::kNormal, utils::AttributeClass::Normal, utils::AttributeEncoding::Float32, 3,
         0},
        {geom::names::uv(0), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
        {geom::names::uv(1), utils::AttributeClass::UV, utils::AttributeEncoding::Float32, 2, 0},
    };
    desc.includeSkin = true;
    desc.maxInfluences = Profile(profile).maxBoneInfluences;
    desc.wantU16Indices = true;

    for (std::size_t m = 0; m < model.meshes.size(); ++m) {
        const Mesh& mesh = model.meshes[m];
        const geom::RenderMesh render = geom::BuildRenderMesh(mesh, desc);
        diagnostics.append(render.diagnostics);

        m2::SkinProfile skin;
        const u32 vertexBase = static_cast<u32>(out.vertices.size());
        const std::vector<Vector3f> positions = render.vertices.getPositions();
        const std::vector<Vector3f> normals = render.vertices.getNormals();
        const std::vector<Vector2f> uv0 = render.vertices.getUVs(0);
        const std::vector<Vector2f> uv1 = render.vertices.getUVs(1);
        const std::vector<std::array<u32, 4>> boneIndices = render.vertices.getBoneIndices();
        const std::vector<std::array<f32, 4>> boneWeights = render.vertices.getBoneWeights();

        for (std::size_t v = 0; v < positions.size(); ++v) {
            m2::Vertex vertex;
            vertex.position = positions[v];
            vertex.normal = v < normals.size() ? normals[v] : Vector3f{0, 0, 1};
            vertex.texCoords[0] = v < uv0.size() ? uv0[v] : Vector2f{0, 0};
            vertex.texCoords[1] = v < uv1.size() ? uv1[v] : Vector2f{0, 0};
            for (std::size_t k = 0; k < 4; ++k) {
                if (v < boneIndices.size() && v < boneWeights.size()) {
                    const u32 node = boneIndices[v][k];
                    const u32 bone = node < boneOf.size() ? boneOf[node] : 0xFFFFu;
                    vertex.boneIndices[k] = bone == 0xFFFFu ? 0u : static_cast<u8>(bone);
                    vertex.boneWeights[k] =
                        static_cast<u8>(std::clamp(boneWeights[v][k], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            out.vertices.push_back(vertex);
            if (vertexBase + v > 0xFFFFu) {
                diagnostics.warn(DiagCode::IndexWidthExceeded,
                                 "more than 65535 vertices across all meshes",
                                 ElementRef(ElementKind::Mesh, m));
            }
            skin.vertices.push_back(static_cast<u16>(vertexBase + v));
        }

        for (const geom::RenderRange& range : render.ranges) {
            m2::SkinSection submesh;
            // The split form: low word in `indexStart`, high word in `level`.
            const std::size_t start = skin.indices.size();
            submesh.indexStart = static_cast<u16>(start & 0xFFFFu);
            submesh.level = static_cast<u16>(start >> 16);
            submesh.indexCount = static_cast<u16>(range.indexCount);
            submesh.vertexStart = 0;
            submesh.vertexCount = static_cast<u16>(positions.size());
            if (range.section < mesh.sections.size()) {
                const MeshSection& section = mesh.sections[range.section];
                submesh.skinSectionId = static_cast<u16>(section.native.value(
                    "skinSectionId", static_cast<i64>(section.selectionGroup)));
            }
            for (u32 i = 0; i < range.indexCount; ++i) {
                const u32 index = render.indices[range.firstIndex + i];
                skin.indices.push_back(static_cast<u16>(index));
            }

            const Material* material = range.materialSlot < model.materialSlots.size()
                                           ? Resolve(model, range.materialSlot, profile)
                                           : nullptr;
            m2::Batch batch;
            if (material != nullptr) {
                batch = m2_core::ExportMaterial(*material, context, out, diagnostics);
            }
            batch.skinSectionIndex = static_cast<u16>(skin.submeshes.size());
            batch.geosetIndex = batch.skinSectionIndex;
            skin.submeshes.push_back(std::move(submesh));
            skin.batches.push_back(batch);
        }

        out.skinProfiles.push_back(std::move(skin));
    }
    out.numSkinProfiles = static_cast<u32>(out.skinProfiles.size());

    // Last, because a material track is placed through the native block's
    // resolved indices and the batches that carry them are written above.
    m2_anim::Export(document, 0, animContext, out, diagnostics);

    result.value = std::move(out);
    return result;
}

// ============================================================================
// FormatConverter
// ============================================================================

std::string M2Converter::formatId() const {
    return "m2";
}

std::string M2Converter::formatName() const {
    return "World of Warcraft M2";
}

std::span<const ProfileId> M2Converter::profiles() const {
    return kM2Profiles;
}

bool M2Converter::supportsImport() const {
    return false;
}

bool M2Converter::supportsExport() const {
    return true;
}

u32 M2Converter::defaultExportVersion() const {
    return 274;
}

Result<std::vector<u8>> M2Converter::exportToBytes(const Document& document, ProfileId profile,
                                                   u32 version) const {
    Result<m2::Model> converted =
        toM2(document, profile, version == 0 ? defaultExportVersion() : version);
    Result<std::vector<u8>> result;
    result.diagnostics = std::move(converted.diagnostics);
    if (!converted.ok()) {
        return result;
    }
    // The base `.m2` only: the skins and the animation sidecars are separate
    // files, and a byte-level export has one return value.
    m2::Writer writer;
    m2::M2SerializeResult written = writer.write(*converted);
    for (const std::string& issue : writer.getIssues()) {
        result.diagnostics.warn(DiagCode::Unspecified, issue);
    }
    result.value = std::move(written.m2Data);
    return result;
}

} // namespace wem
} // namespace models
} // namespace whiteout
