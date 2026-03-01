#pragma once

#include "types.h"
#include "structures/m2_base.h"
#include "structures/m2_chunks.h"
#include "structures/m2_skin.h"
#include "structures/m2_skeleton.h"
#include "structures/m2_phys.h"
#include "structures/m2_bone.h"
#include "structures/m2_anim.h"

#include <optional>
#include <limits>

namespace whiteout {
namespace m2 {

enum class M2Format : u32 {
    ClassicMD20 = 0,
    LegionMD21 = 1,

    Invalid = std::numeric_limits<u32>::max(),
};

struct M2BaseFile {
    M2Format format = M2Format::Invalid;
    MD20Header header;
    std::optional<M2PFIDChunk> pfid_chunk = std::nullopt;
    std::optional<M2SFIDChunk> sfid_chunk = std::nullopt;
    std::optional<M2AFIDChunk> afid_chunk = std::nullopt;
    std::optional<M2BFIDChunk> bfid_chunk = std::nullopt;
    std::optional<M2TXACChunk> txac_chunk = std::nullopt;
    std::optional<M2EXPTChunk> expt_chunk = std::nullopt;
    std::optional<M2EXP2Chunk> exp2_chunk = std::nullopt;
    std::optional<M2PABCChunk> pabc_chunk = std::nullopt;
    std::optional<M2PADCChunk> padc_chunk = std::nullopt;
    std::optional<M2PSBCChunk> psbc_chunk = std::nullopt;
    std::optional<M2PEDCChunk> pedc_chunk = std::nullopt;
    std::optional<M2SKIDChunk> skid_chunk = std::nullopt;
    std::optional<M2TXIDChunk> txid_chunk = std::nullopt;
    std::optional<M2LDV1Chunk> ldv1_chunk = std::nullopt;
    std::optional<M2RPIDChunk> rpid_chunk = std::nullopt;
    std::optional<M2GPIDChunk> gpid_chunk = std::nullopt;
    std::optional<M2WFV1Chunk> wfv1_chunk = std::nullopt;
    std::optional<M2WFV2Chunk> wfv2_chunk = std::nullopt;
    std::optional<M2PGD1Chunk> pgd1_chunk = std::nullopt;
    std::optional<M2WFV3Chunk> wfv3_chunk = std::nullopt;
    std::optional<M2PFDCChunk> pfdc_chunk = std::nullopt;
    std::optional<M2EDGFChunk> edgf_chunk = std::nullopt;
    std::optional<M2NERFChunk> nerf_chunk = std::nullopt;
    std::optional<M2DETLChunk> detl_chunk = std::nullopt;
    std::optional<M2DBOCChunk> dboc_chunk = std::nullopt;
    std::optional<M2AFRAChunk> afra_chunk = std::nullopt;
    std::optional<M2PCOLChunk> pcol_chunk = std::nullopt;
    std::optional<M2DPIVChunk> dpiv_chunk = std::nullopt;
    std::optional<M2TEXLChunk> texl_chunk = std::nullopt;
};

struct M2SkinFile {
    M2SkinProfile profile;
    bool isLodSkin = false;
    int lodLevel = 0;
    int index = 0;
};

struct M2AnimFile {
    M2AnimProfile profile;
    u32 animId = 0;
    u32 variant = 0;
};

struct M2BoneFile {
    BONEHeader header;
    std::optional<BIDAChunk> bida_chunk = std::nullopt;  // Bone IDs
    std::optional<BOMTChunk> bomt_chunk = std::nullopt;  // Bone offset matrices
    u32 boneId = 0;
};

struct M2SkeletonFile {
    std::optional<M2SKL1Chunk> skl1_chunk = std::nullopt;  // Skeleton label/header
    std::optional<M2SKA1Chunk> ska1_chunk = std::nullopt;  // Skeleton attachments
    std::optional<M2SKB1Chunk> skb1_chunk = std::nullopt;  // Skeleton bones
    std::optional<M2SKS1Chunk> sks1_chunk = std::nullopt;  // Skeleton sequences
    std::optional<M2SKPDChunk> skpd_chunk = std::nullopt;  // Skeleton parent data
    std::optional<M2AFIDChunk> afid_chunk = std::nullopt;  // Animation file IDs
    std::optional<M2BFIDChunk> bfid_chunk = std::nullopt;  // Bone file IDs
};

struct M2PhysicsFile {
    // placeholder;
};

enum class M2FileType {
    Base,
    Skin,
    Anim,
    Skeleton,
    Physics,
};

struct M2FileSystem {
    std::string baseName;
    M2BaseFile base;
    std::vector<M2SkinFile> skins;
    std::vector<M2AnimFile> anims;
    std::vector<M2BoneFile> bones;
    std::optional<M2SkeletonFile> skeleton = std::nullopt;
    std::optional<M2PhysicsFile> physics = std::nullopt;
};

} // namespace m2
} // namespace whiteout
