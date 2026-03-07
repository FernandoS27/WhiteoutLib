// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <functional>

#include <whiteout/m3/structures.h>
#include <whiteout/m3/types.h>
#include "../common/binary_reader.h"

namespace whiteout {
namespace m3 {

/// Visitor that reads M3 binary data into the Model structure.
/// Preserves all raw chunk data for perfect round-trip fidelity.
class BinaryParseVisitor {
public:
    explicit BinaryParseVisitor(common::BinaryReader& reader, bool ismd33 = false);

    /// Read the complete model from the current reader position.
    void read(Model& model);

    /// Issues encountered during parsing (non-fatal in Lenient mode).
    const std::vector<std::string>& getIssues() const {
        return issues;
    }

protected:
    void visit(Model& model, u32 version);
    void visit(VertexBuffer& value, u32 version);
    void visit(Sequence& value, u32 version);
    void visit(SubTrackContainer& value, u32 version);
    void visit(AnimationGroup& value, u32 version);
    void visit(AnimationState& value, u32 version);
    void visit(BoneAnimationSet& value, u32 version);
    void visit(Bone& value, u32 version);
    void visit(Region& value, u32 version);
    void visit(Batch& value, u32 version);
    void visit(MeshSection& value, u32 version);
    void visit(MeshDivision& value, u32 version);
    void visit(InitialReference& value, u32 version);
    void visit(AttachmentPoint& value, u32 version);
    void visit(MaterialMap& value, u32 version);
    void visit(TextureLayer& value, u32 version);
    void visit(StandardMaterial& value, u32 version);
    void visit(DisplacementMaterial& value, u32 version);
    void visit(CompositeSection& value, u32 version);
    void visit(CompositeMaterial& value, u32 version);
    void visit(TerrainMaterial& value, u32 version);
    void visit(VolumeMaterial& value, u32 version);
    void visit(HairMaterial& value, u32 version);
    void visit(VolumeNoiseMaterial& value, u32 version);
    void visit(CreepMaterial& value, u32 version);
    void visit(STBMaterial& value, u32 version);
    void visit(ReflectionMaterial& value, u32 version);
    void visit(SubFlare& value, u32 version);
    void visit(LensFlare& value, u32 version);
    void visit(MaterialAddData& value, u32 version);
    void visit(Light& value, u32 version);
    void visit(Camera& value, u32 version);
    void visit(Event& value, u32 version);
    void visit(ParticleEmitter& value, u32 version);
    void visit(ParticleEmitterCopy& value, u32 version);
    void visit(SplineRibbon& value, u32 version);
    void visit(RibbonEmitter& value, u32 version);
    void visit(Projector& value, u32 version);
    void visit(Force& value, u32 version);
    void visit(Warp& value, u32 version);
    void visit(ConvexHullHalfEdge& value, u32 version);
    void visit(PhysicsMeshNormal& value, u32 version);
    void visit(PhysicsMeshTriangle& value, u32 version);
    void visit(PhysicsMeshEdge& value, u32 version);
    void visit(PhysicsShape& value, u32 version);
    void visit(RigidBody& value, u32 version);
    void visit(PhysicsJoint& value, u32 version);
    void visit(PhysicsConstraint& value, u32 version);
    void visit(ClothCollider& value, u32 version);
    void visit(ClothProxy& value, u32 version);
    void visit(ClothPhysics& value, u32 version);
    void visit(HitTestShape& value, u32 version);
    void visit(AttachmentVolume& value, u32 version);
    void visit(TriggerData& value, u32 version);
    void visit(TurretBehavior& value, u32 version);
    void visit(BillboardBehavior& value, u32 version);
    void visit(IKJoint& value, u32 version);
    void visit(IKTwoJoint& value, u32 version);
    void visit(IKCCD& value, u32 version);
    void visit(OneBoneSolver& value, u32 version);
    void visit(ShadowBox& value, u32 version);
    void visit(ViewVolume& value, u32 version);
    void visit(TrailingModel& value, u32 version);

    template <typename T>
    void visit(AnimBlock<T>& block, u32 version);

    template <typename T>
    void visit(std::vector<T>& container);

    template <typename T>
    void visit(std::optional<T>& container);

    void visit(std::string& str, u32 version);
    void visit(std::string& str);

    std::function<Reference(void)> readReferenceFunc;

    std::vector<IndexEntry> indexTable;
    std::vector<bool> indexUsed; // tracks which index entries have been used for validation

    common::BinaryReader& reader;
    std::vector<std::string> issues;
};

} // namespace m3
} // namespace whiteout
