// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/m3/types.h>
#include "../../common/binary_writer.h"
#include "inner_structure_types.h"

#include <deque>
#include <functional>

namespace whiteout {
namespace m3 {

/// Visitor that writes an M3 Model to binary format.
/// Preserves original chunk layout for byte-identical round-trip.
class BinaryWriterVisitor {
public:
    explicit BinaryWriterVisitor(common::BinaryWriter& writer, bool ismd33 = false);

    /// Write the complete model.
    void write(const Model& model);

protected:
    void visit(const Model& model, u32 version);
    void visit(const VertexBuffer& value, u32 version);
    void visit(const Sequence& seq, u32 version);
    void visit(const SubTrackContainer& container, u32 version);
    void visit(const AnimationGroup& group, u32 version);
    void visit(const BoneAnimationSet& set, u32 version);
    void visit(const AnimationState& state, u32 version);
    void visit(const Bone& bone, u32 version);
    void visit(const MeshDivision& division, u32 version);
    void visit(const AttachmentPoint& point, u32 version);
    void visit(const Light& light, u32 version);
    void visit(const ShadowBox& box, u32 version);
    void visit(const Camera& camera, u32 version);
    void visit(const MaterialMap& map, u32 version);
    void visit(const StandardMaterial& material, u32 version);
    void visit(const DisplacementMaterial& material, u32 version);
    void visit(const CompositeSection& section, u32 version);
    void visit(const CompositeMaterial& material, u32 version);
    void visit(const TerrainMaterial& material, u32 version);
    void visit(const VolumeMaterial& material, u32 version);
    void visit(const HairMaterial& material, u32 version);
    void visit(const CreepMaterial& material, u32 version);
    void visit(const VolumeNoiseMaterial& material, u32 version);
    void visit(const STBMaterial& material, u32 version);
    void visit(const ReflectionMaterial& material, u32 version);
    void visit(const LensFlare& flare, u32 version);
    void visit(const DataDrivenMaterial& data, u32 version);
    void visit(const ParticleEmitter& emitter, u32 version);
    void visit(const ParticleEmitterCopy& copy, u32 version);
    void visit(const RibbonEmitter& emitter, u32 version);
    void visit(const Projector& projector, u32 version);
    void visit(const Force& force, u32 version);
    void visit(const Warp& warp, u32 version);
    void visit(const ViewVolume& volume, u32 version);
    void visit(const RigidBody& body, u32 version);
    void visit(const PhysicsConstraint& constraint, u32 version);
    void visit(const PhysicsJoint& joint, u32 version);
    void visit(const ClothPhysics& cloth, u32 version);
    void visit(const IKTwoJoint& joint, u32 version);
    void visit(const IKCCD& ccd, u32 version);
    void visit(const IKJoint& joint, u32 version);
    void visit(const OneBoneSolver& solver, u32 version);
    void visit(const TurretBehavior& behavior, u32 version);
    void visit(const TriggerData& trigger, u32 version);
    void visit(const InitialReference& ref, u32 version);
    void visit(const HitTestShape& shape, u32 version);
    void visit(const AttachmentVolume& volume, u32 version);
    void visit(const BillboardBehavior& behavior, u32 version);
    void visit(const TrailingModel& model, u32 version);
    void visit(const Event& event, u32 version);
    void visit(const Region& region, u32 version);
    void visit(const Batch& batch, u32 version);
    void visit(const MeshSection& section, u32 version);
    void visit(const TextureLayer& layer, u32 version);
    void visit(const SubFlare& flare, u32 version);
    void visit(const SplineRibbon& ribbon, u32 version);
    void visit(const PhysicsShape& shape, u32 version);
    void visit(const PhysicsMeshTriangle& triangle, u32 version);
    void visit(const PhysicsMeshEdge& edge, u32 version);
    void visit(const PhysicsMeshBvhNode& normal, u32 version);
    void visit(const ConvexHullHalfEdge& edge, u32 version);
    void visit(const ClothCollider& collider, u32 version);
    void visit(const ClothProxy& proxy, u32 version);

    template <typename T>
    void visit(const AnimBlock<T>& block, u32 version);

    template <typename T>
    void visit(const std::vector<T>& container);

    template <typename T>
    void visit(const std::optional<T>& container);

    void visit(const std::string& str, u32 version);
    void visit(const std::string& str);

    void visitCharBlob(const std::vector<u8>& blob);

    void transferDeferredWrites(bool pre_order = true);

    std::function<void(Reference&)> writeReferenceFunc;

    std::deque<std::function<void()>> currentLevelWrites;
    std::deque<std::function<void()>> deferredWrites;
    std::vector<IndexEntry> indexTable;

    // Legacy PHSH chunks are reconstructed from the canonical fields at write
    // time; the containers must outlive the deferred chunk writes, which
    // capture them by reference.
    std::deque<std::vector<Vector3f>> legacyVec3Scratch;
    std::deque<std::vector<PhysicsMeshTriangle>> legacyTriangleScratch;

    common::BinaryWriter& writer;
    bool ismd33;
};

} // namespace m3
} // namespace whiteout
