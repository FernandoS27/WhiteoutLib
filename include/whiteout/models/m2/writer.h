
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include "structures.h"

namespace whiteout {

namespace interfaces {
class VirtualPathFileSystem;
class CascFileSystem;
} // namespace interfaces

namespace m2 {

struct WriteOptions {
    Format format = Format::LegionMD21;
    u32 m2Version = M2_VERSION_LEGION;
    bool emitSkeleton = false;
    std::string baseStem;
};

struct M2SerializeResult {
    struct SkinFileEntry {
        u32 fileDataId = 0;
        std::vector<u8> data;
        size_t pathOffset = 0;
    };

    struct SkeletonFileEntry {
        u32 fileDataId = 0;
        std::vector<u8> data;
        size_t pathOffset = 0;
    };

    struct AnimDataEntry {
        u16 animId = 0;
        u16 subAnimId = 0;
        u32 fileDataId = 0;
        std::vector<u8> data;
        size_t pathOffset = 0;
    };

    std::vector<u8> m2Data;
    std::vector<SkinFileEntry> skinData;
    std::vector<SkinFileEntry> skinlodData;
    std::optional<SkeletonFileEntry> skeletonData;

    std::vector<AnimDataEntry> animData;

    void patchSkinFileId(u32 skinId, u32 newId) {
        skinData[skinId].fileDataId = newId;
        applyPatch(m2Data, skinData[skinId].pathOffset, newId);
    }

    void patchSkinLodFileId(u32 skinLodId, u32 newId) {
        skinlodData[skinLodId].fileDataId = newId;
        applyPatch(m2Data, skinlodData[skinLodId].pathOffset, newId);
    }

    void patchSkeletonFileId(u32 newId) {
        if (skeletonData.has_value()) {
            skeletonData->fileDataId = newId;
            applyPatch(m2Data, skeletonData->pathOffset, newId);
        }
    }

    void patchAnimFileId(u16 animId, u16 subAnimId, u32 newId) {
        std::vector<u8>* data = skeletonData.has_value() ? &skeletonData->data : &m2Data;
        for (auto& entry : animData) {
            if (entry.animId == animId && entry.subAnimId == subAnimId) {
                entry.fileDataId = newId;
                applyPatch(*data, entry.pathOffset, newId);
                break;
            }
        }
    }

private:
    void applyPatch(std::vector<u8>& buffer, size_t offset, u32 newId) {
        std::memcpy(buffer.data() + offset, &newId, sizeof(newId));
    }
};

class Writer {
public:
    explicit Writer(WriteOptions options = {});

    ~Writer();

    void write(interfaces::VirtualPathFileSystem& fs, const std::string& filePath,
               const Model& model);

    void write(interfaces::CascFileSystem& cascFs, const Model& model);

    M2SerializeResult write(const Model& model);

    bool hasIssues() const;

    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace m2
} // namespace whiteout
