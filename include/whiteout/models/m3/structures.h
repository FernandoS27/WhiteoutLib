// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file structures.h
 * @brief All M3 data structures and the Model root
 *
 * This file includes all sub-headers for the M3 structure definitions
 * (animation, mesh, materials, effects, physics, scene, miscellaneous)
 * and defines the top-level Model struct — the MODL root chunk that
 * contains Ref<T> fields pointing to every other chunk in the file.
 *
 * MODL versions: v23 (784 bytes) through v30 (868 bytes). Later versions
 * add material types, cloth physics, or IK solver references. The preamble
 * (bytes 0x000–0x0E3, 228 bytes) is identical across all versions.
 */

#include "structures/anim.h"
#include "structures/base.h"
#include "structures/effect.h"
#include "structures/material.h"
#include "structures/mesh.h"
#include "structures/misc.h"
#include "structures/physics.h"
#include "structures/scene.h"
#include "types.h"

namespace whiteout {
namespace m3 {

// ============================================================================
// Model Root
// ============================================================================

/**
 * @brief MODL — Model root chunk (v23–v30, 784–868 bytes)
 *
 * The root of all model data. Contains Ref<T> fields pointing to every
 * sub-chunk in the file: skeleton, mesh, materials, particles, physics, etc.
 * The preamble (bytes 0x000–0x0E3) is identical across all versions;
 * version-dependent material and physics references follow at 0x0E4+.
 *
 * Version history:
 * - v23 (784 bytes): Base release layout
 * - v24 (+ikCCD): 796 bytes
 * - v25 (+volumeNoiseMaterials): 808 bytes
 * - v26 (+stbMaterials): 820 bytes
 * - v28 (+reflectionMaterials, +clothPhysics): 844 bytes
 * - v29 (+lensFlareMaterials): 856 bytes
 * - v30 (+dataDrivenMaterials): 868 bytes
 */
struct Model {
    // ─── Preamble (0x000–0x0E3, 228 bytes) ────────────────────────
    std::string name;                  ///< Model file path (Ref<CHAR>)
    ModelFlag flags = ModelFlag::None; ///< Model flags (tangents, FOW, instancing, etc.)
    std::vector<Sequence> sequences;   ///< Animation sequences (SEQS)
    std::vector<SubTrackContainer>
        subTrackCollections;                     ///< Sub-track containers (STC_) with keyframe refs
    std::vector<AnimationGroup> animationGroups; ///< Animation groups (STG_)
    std::vector<BoneAnimationSet> boneAnimationSets; ///< Bone animation sets (BSET, always null)
    u32 animationSplitCount;                         ///< Always 0
    std::vector<AnimationState> animationStates;     ///< Animation states (STS_)
    std::vector<Bone> bones;                         ///< Skeleton bones (BONE)
    u32 skinBoneCount;                               ///< Number of bones affecting skin
    VertexBuffer vertices;                           ///< Vertex data blob (U8__) with format flags
    std::vector<MeshDivision> divisions;    ///< Mesh divisions (DIV_: faces, regions, batches)
    std::vector<u16> boneLookup;            ///< Bone index remap table (U16_)
    Extent bounds;                          ///< Model bounding volume
    Extent collisionBounds;                 ///< Collision bounding volume
    std::vector<u16> collisionFaces;        ///< Collision triangle indices (U16_)
    std::vector<Vector3f> collisionVerts;   ///< Collision vertex positions (VEC3)
    std::vector<Vector3f> collisionNormals; ///< Collision face normals (VEC3)

    // ─── Scene objects (0x0E4+) ────────────────────────────────────
    std::vector<AttachmentPoint> attachmentPoints; ///< Named bone locations (ATT_)
    std::vector<u16> attachmentPointAddons;        ///< Attachment point addon indices (U16_)
    std::vector<Light> lights;                     ///< Lights (LITE)
    std::vector<ShadowBox> shadowBoxes;            ///< Shadow boxes (SHBX)
    std::vector<Camera> cameras;                   ///< Cameras (CAM_)
    std::vector<u16> camerasAddons;                ///< Camera addon indices (U16_)

    // ─── Materials ─────────────────────────────────────────────────
    std::vector<MaterialMap> materialMaps;                   ///< Material type+index maps (MATM)
    std::vector<StandardMaterial> standardMaterials;         ///< Standard materials (MAT_)
    std::vector<DisplacementMaterial> displacementMaterials; ///< Displacement materials (DIS_)
    std::vector<CompositeMaterial> compositeMaterials;       ///< Composite materials (CMP_)
    std::vector<TerrainMaterial> terrainMaterials;           ///< Terrain materials (TER_)
    std::vector<VolumeMaterial> volumeMaterials;             ///< Volume materials (VOL_)
    std::vector<HairMaterial> hairMaterials;   ///< Hair materials (HAI_, defunct — always null)
    std::vector<CreepMaterial> creepMaterials; ///< Creep materials (CREP)
    std::vector<VolumeNoiseMaterial> volumeNoiseMaterials; ///< Volume noise materials (VON_, v25+)
    std::vector<STBMaterial> stbMaterials; ///< Splat terrain bake materials (STBM, v26+)
    std::vector<ReflectionMaterial> reflectionMaterials; ///< Reflection materials (REF_, v28+)
    std::vector<LensFlare> lensFlareMaterials;           ///< Lens flare materials (LFLR, v29+)
    std::vector<DataDrivenMaterial> dataDrivenMaterials; ///< Data-driven materials (MADD, v30+)

    // ─── Effects ───────────────────────────────────────────────────
    std::vector<ParticleEmitter> particleEmitters;          ///< Particle emitters (PAR_)
    std::vector<ParticleEmitterCopy> particleEmitterCopies; ///< Particle emitter copies (PARC)
    std::vector<RibbonEmitter> ribbonEmitters;              ///< Ribbon emitters (RIB_)
    std::vector<Projector> projections;                     ///< Projectors / decals (PROJ)
    std::vector<Force> forces;                              ///< Forces (FOR_)
    std::vector<Warp> warps;                                ///< Warps (WRP_)
    std::vector<ViewVolume> viewVolumes;                    ///< View volumes (VVOL)

    // ─── Physics ───────────────────────────────────────────────────
    std::vector<RigidBody> rigidBodies;                ///< Rigid bodies (PHRB)
    std::vector<PhysicsConstraint> physicsConstraints; ///< Physics constraints (PHCT)
    std::vector<PhysicsJoint> physicsJoints;           ///< Physics joints (PHYJ)
    std::vector<ClothPhysics> clothPhysics;            ///< Cloth physics (PHCL, v28+)
    std::vector<IKTwoJoint> ikTwoJoints;               ///< Two-joint IK solvers (IK2J)
    std::vector<IKCCD> ikCCD;                          ///< CCD IK solvers (IKCC, v24+)
    std::vector<IKJoint> ikJoints;                     ///< IK joints (IKJT)
    std::vector<OneBoneSolver> oneBoneSolvers;         ///< One-bone IK solvers (PAOB)

    // ─── Behaviors & data ──────────────────────────────────────────
    std::vector<TurretBehavior> turretBehaviors;    ///< Turret behaviors (PATU)
    std::vector<TriggerData> triggerData;           ///< Trigger data (TRGD)
    std::vector<InitialReference> initialReference; ///< Inverse bind-pose matrices (IREF)

    // ─── Inline hit-test (108 bytes) ──────────────────────────────
    HitTestShape tightHitTestObject; ///< Tight hit-test shape (SSGS, inline)

    // ─── Trailing references ──────────────────────────────────────
    std::vector<HitTestShape> fuzzyHitTestObjects;     ///< Fuzzy hit-test shapes (SSGS)
    std::vector<AttachmentVolume> attachmentVolumes;   ///< Attachment volumes (ATVL)
    std::vector<u16> attachmentVolumesAddon0;          ///< Attachment volume addon 0 (U16_)
    std::vector<u16> attachmentVolumesAddon1;          ///< Attachment volume addon 1 (U16_)
    std::vector<BillboardBehavior> billboardBehaviors; ///< Billboard behaviors (BBSC)
    std::vector<TrailingModel> trailingModels;         ///< Trailing models (TMD_, defunct)
    u32 m3aAnimHash;                                   ///< Hash for .m3a animation file binding
    std::vector<u32> m3aAnimHashes;                    ///< Additional .m3a hashes (U32_)
    M3_DEFINE_VERSION_ACCESSORS()
};

#undef M3_DEFINE_VERSION_ACCESSORS

} // namespace m3
} // namespace whiteout
