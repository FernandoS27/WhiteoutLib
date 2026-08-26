
#include <whiteout/models/m3/m3.h>

#include "tags.h"

namespace whiteout {
namespace m3 {

template <typename T, typename = void>
struct ChunkTagTraits {
    // No specialization exists for T — add one below.
};

template <>
struct ChunkTagTraits<char> {
    static constexpr u32 value = TAG_CHAR;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u8> {
    static constexpr u32 value = TAG_U8;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u16> {
    static constexpr u32 value = TAG_U16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u32> {
    static constexpr u32 value = TAG_U32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<u64> {
    static constexpr u32 value = TAG_U64;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i16> {
    static constexpr u32 value = TAG_I16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<i32> {
    static constexpr u32 value = TAG_I32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<f32> {
    static constexpr u32 value = TAG_REAL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Flag> {
    static constexpr u32 value = TAG_FLAG;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector2f> {
    static constexpr u32 value = TAG_VEC2;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector3f> {
    static constexpr u32 value = TAG_VEC3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Vector4f> {
    static constexpr u32 value = TAG_VEC4;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Quaternion> {
    static constexpr u32 value = TAG_QUAT;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<ColorBGRA> {
    static constexpr u32 value = TAG_COL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<Extent> {
    static constexpr u32 value = TAG_BNDS;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimBlock<Event>> {
    static constexpr u32 value = TAG_SDEV;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Vector2f>> {
    static constexpr u32 value = TAG_SD2V;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Vector3f>> {
    static constexpr u32 value = TAG_SD3V;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<f32>> {
    static constexpr u32 value = TAG_SDR3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Quaternion>> {
    static constexpr u32 value = TAG_SD4Q;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<ColorBGRA>> {
    static constexpr u32 value = TAG_SDCC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u8>> {
    static constexpr u32 value = TAG_SDU8;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<i16>> {
    static constexpr u32 value = TAG_SDS6;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u16>> {
    static constexpr u32 value = TAG_SDU6;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<i32>> {
    static constexpr u32 value = TAG_SDS3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<u32>> {
    static constexpr u32 value = TAG_SDU3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Flag>> {
    static constexpr u32 value = TAG_SDFG;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimBlock<Extent>> {
    static constexpr u32 value = TAG_SDMB;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimRef<i32>> {
    static constexpr u32 value = TAG_SS32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<u32>> {
    static constexpr u32 value = TAG_SU32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<f32>> {
    static constexpr u32 value = TAG_SR32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<Vector3f>> {
    static constexpr u32 value = TAG_SVC3;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<AnimRef<Vector2f>> {
    static constexpr u32 value = TAG_SVC2;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u16, 7>> {
    static constexpr u32 value = TAG_MT16;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::array<u32, 7>> {
    static constexpr u32 value = TAG_MT32;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = true;
};

template <>
struct ChunkTagTraits<std::string> {
    static constexpr u32 value = TAG_SCHR;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

// ── Animation ────────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<Event> {
    static constexpr u32 value = TAG_EVNT;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Sequence> {
    static constexpr u32 value = TAG_SEQS;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SubTrackContainer> {
    static constexpr u32 value = TAG_STC;
    static constexpr u32 max_version = 4;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimationGroup> {
    static constexpr u32 value = TAG_STG;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AnimationState> {
    static constexpr u32 value = TAG_STS;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<BoneAnimationSet> {
    static constexpr u32 value = TAG_BSET;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

// ── Skeleton & Mesh ──────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<Bone> {
    static constexpr u32 value = TAG_BONE;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Region> {
    static constexpr u32 value = TAG_REGN;
    static constexpr u32 max_version = 5;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Batch> {
    static constexpr u32 value = TAG_BAT;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<MeshSection> {
    static constexpr u32 value = TAG_MSEC;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<MeshDivision> {
    static constexpr u32 value = TAG_DIV;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<InitialReference> {
    static constexpr u32 value = TAG_IREF;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AttachmentPoint> {
    static constexpr u32 value = TAG_ATT;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

// ── Materials ────────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<MaterialMap> {
    static constexpr u32 value = TAG_MATM;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TextureLayer> {
    static constexpr u32 value = TAG_LAYR;
    static constexpr u32 max_version = 26;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<StandardMaterial> {
    static constexpr u32 value = TAG_MAT;
    static constexpr u32 max_version = 20;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<DisplacementMaterial> {
    static constexpr u32 value = TAG_DIS;
    static constexpr u32 max_version = 4;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CompositeSection> {
    static constexpr u32 value = TAG_CMS;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CompositeMaterial> {
    static constexpr u32 value = TAG_CMP;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TerrainMaterial> {
    static constexpr u32 value = TAG_TER;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<VolumeMaterial> {
    static constexpr u32 value = TAG_VOL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<HairMaterial> {
    static constexpr u32 value = TAG_HAI;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<VolumeNoiseMaterial> {
    static constexpr u32 value = TAG_VON;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<CreepMaterial> {
    static constexpr u32 value = TAG_CREP;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<STBMaterial> {
    static constexpr u32 value = TAG_STBM;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ReflectionMaterial> {
    static constexpr u32 value = TAG_REF;
    static constexpr u32 max_version = 3;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SubFlare> {
    static constexpr u32 value = TAG_LFSB;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<LensFlare> {
    static constexpr u32 value = TAG_LFLR;
    static constexpr u32 max_version = 3;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<DataDrivenMaterial> {
    static constexpr u32 value = TAG_MADD;
    static constexpr u32 max_version = 3;
    static constexpr bool is_trivial = false;
};

// ── Effects ──────────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<ParticleEmitter> {
    static constexpr u32 value = TAG_PAR;
    static constexpr u32 max_version = 24;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ParticleEmitterCopy> {
    static constexpr u32 value = TAG_PARC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<SplineRibbon> {
    static constexpr u32 value = TAG_SRIB;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<RibbonEmitter> {
    static constexpr u32 value = TAG_RIB;
    static constexpr u32 max_version = 9;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Projector> {
    static constexpr u32 value = TAG_PROJ;
    static constexpr u32 max_version = 5;
    static constexpr bool is_trivial = false;
};

// ── Miscellaneous ────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<HitTestShape> {
    static constexpr u32 value = TAG_SSGS;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<AttachmentVolume> {
    static constexpr u32 value = TAG_ATVL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TriggerData> {
    static constexpr u32 value = TAG_TRGD;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TurretBehavior> {
    static constexpr u32 value = TAG_PATU;
    static constexpr u32 max_version = 4;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<BillboardBehavior> {
    static constexpr u32 value = TAG_BBSC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<IKJoint> {
    static constexpr u32 value = TAG_IKJT;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<IKTwoJoint> {
    static constexpr u32 value = TAG_IK2J;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<IKCCD> {
    static constexpr u32 value = TAG_IKCC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<OneBoneSolver> {
    static constexpr u32 value = TAG_PAOB;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ShadowBox> {
    static constexpr u32 value = TAG_SHBX;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ViewVolume> {
    static constexpr u32 value = TAG_VVOL;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<TrailingModel> {
    static constexpr u32 value = TAG_TMD;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

// ── Physics ──────────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<Force> {
    static constexpr u32 value = TAG_FOR;
    static constexpr u32 max_version = 2;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Warp> {
    static constexpr u32 value = TAG_WRP;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ConvexHullHalfEdge> {
    static constexpr u32 value = TAG_DMSE;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsMeshBvhNode> {
    static constexpr u32 value = TAG_DMMN;
    static constexpr u32 max_version = 1;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsMeshTriangle> {
    static constexpr u32 value = TAG_DMMT;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsMeshEdge> {
    static constexpr u32 value = TAG_DMME;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsShape> {
    static constexpr u32 value = TAG_PHSH;
    static constexpr u32 max_version = 3;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<RigidBody> {
    static constexpr u32 value = TAG_PHRB;
    static constexpr u32 max_version = 4;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsJoint> {
    static constexpr u32 value = TAG_PHYJ;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<PhysicsConstraint> {
    static constexpr u32 value = TAG_PHCT;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ClothCollider> {
    static constexpr u32 value = TAG_PHCC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ClothProxy> {
    static constexpr u32 value = TAG_PHAC;
    static constexpr u32 max_version = 0;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<ClothPhysics> {
    static constexpr u32 value = TAG_PHCL;
    static constexpr u32 max_version = 4;
    static constexpr bool is_trivial = false;
};

// ── Scene ────────────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<Light> {
    static constexpr u32 value = TAG_LITE;
    static constexpr u32 max_version = 7;
    static constexpr bool is_trivial = false;
};

template <>
struct ChunkTagTraits<Camera> {
    static constexpr u32 value = TAG_CAM;
    static constexpr u32 max_version = 5;
    static constexpr bool is_trivial = false;
};

// ── Model Root ───────────────────────────────────────────────────────────────

template <>
struct ChunkTagTraits<Model> {
    static constexpr u32 value = TAG_MODL;
    static constexpr u32 max_version = 30;
    static constexpr bool is_trivial = false;
};

} // namespace m3
} // namespace whiteout