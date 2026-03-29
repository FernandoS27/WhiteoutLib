#pragma once

#include <whiteout/models/m2/structures.h>

#include "structures/anim.h"
#include "structures/bone.h"
#include "structures/chunks.h"
#include "structures/phys.h"
#include "structures/skeleton.h"

namespace whiteout {
namespace m2 {

struct MD20Header {
    u32 magic = MD20_TAG;
    u32 version = M2_VERSION_LEGION;

    Model model;
};

struct BaseFile {
    Format format = Format::Invalid;
    MD20Header header;

    std::optional<PFIDChunk> pfid_chunk = std::nullopt;
    std::optional<SFIDChunk> sfid_chunk = std::nullopt;
    std::optional<AFIDChunk> afid_chunk = std::nullopt;
    std::optional<BFIDChunk> bfid_chunk = std::nullopt;
    std::optional<SKIDChunk> skid_chunk = std::nullopt;
    std::optional<TXIDChunk> txid_chunk = std::nullopt;

    std::optional<TXACChunk> txac_chunk = std::nullopt;
    std::optional<EXPTChunk> expt_chunk = std::nullopt;
    std::optional<EXP2Chunk> exp2_chunk = std::nullopt;

    std::optional<PABCChunk> pabc_chunk = std::nullopt;
    std::optional<PADCChunk> padc_chunk = std::nullopt;
    std::optional<PSBCChunk> psbc_chunk = std::nullopt;
    std::optional<PEDCChunk> pedc_chunk = std::nullopt;

    std::optional<LodProfile> ldv1_chunk = std::nullopt;
    std::optional<M2RPIDChunk> rpid_chunk = std::nullopt;
    std::optional<GPIDChunk> gpid_chunk = std::nullopt;

    std::optional<WFV3Chunk> wfv3_chunk = std::nullopt;

    std::optional<PGD1Chunk> pgd1_chunk = std::nullopt;
    std::optional<PFDCChunk> pfdc_chunk = std::nullopt;
    std::optional<EDGFChunk> edgf_chunk = std::nullopt;
    std::optional<NERFChunk> nerf_chunk = std::nullopt;
    std::optional<DETLChunk> detl_chunk = std::nullopt;
    std::optional<DBOCChunk> dboc_chunk = std::nullopt;
    std::optional<AFRAChunk> afra_chunk = std::nullopt;
    std::optional<PCOLChunk> pcol_chunk = std::nullopt;
    std::optional<DPIVChunk> dpiv_chunk = std::nullopt;
    std::optional<TEXLChunk> texl_chunk = std::nullopt;
};

struct SkinFile {
    u32 version = 0;
    SkinProfile profile;
    bool isLodSkin = false;
    int lodLevel = 0;
    int index = 0;
};

struct AnimFile {
    AnimProfile profile;
    u32 animId = 0;
    u32 variant = 0;
};

struct BoneFile {
    BONEHeader header;
    std::optional<BIDAChunk> bida_chunk = std::nullopt;
    std::optional<BOMTChunk> bomt_chunk = std::nullopt;
    u32 boneId = 0;
};

struct SkeletonFile {
    std::optional<SKL1Chunk> skl1_chunk = std::nullopt;
    std::optional<SKA1Chunk> ska1_chunk = std::nullopt;
    std::optional<SKB1Chunk> skb1_chunk = std::nullopt;
    std::optional<SKS1Chunk> sks1_chunk = std::nullopt;
    std::optional<SKPDChunk> skpd_chunk = std::nullopt;
    std::optional<AFIDChunk> afid_chunk = std::nullopt;
    std::optional<BFIDChunk> bfid_chunk = std::nullopt;
};

struct PhysicsFile {};

enum class FileType {
    Base,
    Skin,
    Anim,
    Skeleton,
    Physics,
};

struct FileSystem {
    std::string baseName;
    BaseFile base;
    std::vector<SkinFile> skins;
    std::vector<AnimFile> anims;
    std::vector<BoneFile> bones;
    std::optional<SkeletonFile> skeleton = std::nullopt;
    std::optional<PhysicsFile> physics = std::nullopt;
};

} // namespace m2
} // namespace whiteout
