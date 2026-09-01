// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/// The extracted Diablo III SNO tree, as an `AssetProvider`.
///
/// Shared by every D3 test arm that needs real assets. It lives in a header
/// because the thing worth sharing is *why it works*: the corpus has **no
/// CoreTOC**, and it does not need one — every group these tests load starts its
/// payload with `dwSnoId`, so an index is a 4-byte read at file offset 16.
/// That is what makes group-37 shader resolution and `.ani` joins reachable in a
/// test at all.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <whiteout/common_types.h>
#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/types.h>

#include "test_helpers.h"

namespace whiteout::test::d3 {

namespace fs = std::filesystem;
namespace d3n = whiteout::sno::d3::native;

std::vector<u8> readWhole(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
}

fs::path corpusRoot() {
    const std::string base = test::findCorpusBase("Corpus");
    return base.empty() ? fs::path{} : fs::path(base) / "D3";
}

/// Indexed lazily per group: indexing all 19,177 actors to answer a question
/// about appearances would cost more than the test.
class CorpusProvider final : public d3n::AssetProvider {
public:
    explicit CorpusProvider(fs::path root) : root_(std::move(root)) {}

    std::vector<u8> load(d3n::Group group, i32 snoId) override {
        ++loads;
        const Index& index = indexOf(group);
        const auto found = index.byId.find(snoId);
        return found == index.byId.end() ? std::vector<u8>{} : readWhole(found->second);
    }

    /// The id a named file carries, so a test can name its fixture.
    i32 idOf(d3n::Group group, const std::string& stem) {
        const Index& index = indexOf(group);
        const auto found = index.byStem.find(stem);
        return found == index.byStem.end() ? -1 : found->second;
    }

    std::size_t count(d3n::Group group) {
        return indexOf(group).byId.size();
    }

    u32 loads = 0;

private:
    struct Index {
        std::map<i32, fs::path> byId;
        std::map<std::string, i32> byStem;
    };

    static const char* directoryOf(d3n::Group group) {
        switch (group) {
        case d3n::Group::Appearance:
            return "Appearances";
        case d3n::Group::Actor:
            return "Actor";
        case d3n::Group::ShaderMap:
            return "ShaderMap";
        case d3n::Group::Shaders:
            return "Shaders";
        case d3n::Group::Material:
            return "Material";
        case d3n::Group::Anim:
            return "Anim";
        case d3n::Group::AnimSet:
            return "AnimSet";
        default:
            return nullptr;
        }
    }

    const Index& indexOf(d3n::Group group) {
        const auto existing = indices_.find(static_cast<i32>(group));
        if (existing != indices_.end()) {
            return existing->second;
        }
        Index index;
        const char* directory = directoryOf(group);
        if (directory != nullptr && !root_.empty()) {
            std::error_code error;
            for (fs::directory_iterator it(root_ / directory, error);
                 it != fs::directory_iterator(); it.increment(error)) {
                if (error) {
                    break;
                }
                std::ifstream stream(it->path(), std::ios::binary);
                if (!stream) {
                    continue;
                }
                // magic, version, eight zero bytes, then the payload's own id.
                char header[20] = {};
                stream.read(header, sizeof(header));
                if (stream.gcount() < static_cast<std::streamsize>(sizeof(header))) {
                    continue;
                }
                i32 snoId = 0;
                std::memcpy(&snoId, header + 16, sizeof(snoId));
                index.byId.emplace(snoId, it->path());
                index.byStem.emplace(it->path().stem().string(), snoId);
            }
        }
        return indices_.emplace(static_cast<i32>(group), std::move(index)).first->second;
    }

    fs::path root_;
    std::map<i32, Index> indices_;
};

/// A provider that has nothing, for the arms that must work without one.
class EmptyProvider final : public d3n::AssetProvider {
public:
    std::vector<u8> load(d3n::Group, i32) override {
        return {};
    }
};

} // namespace whiteout::test::d3
