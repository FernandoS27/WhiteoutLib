// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Global flags the 12.1 client applies while loading (M2_FLAGS_RE.md §3.15, §3.21): 0x8 is never
/// tested on the versions it loads, the SKID skeleton loads only with ExternalSkeleton, and an SKPD
/// parent adds a second sequence table without ever replacing the child.
///
/// Corpus-driven: a model whose globalFlags word is rewritten in a temporary copy is the one
/// honest way to see a flag's effect on a real file. The parent skeleton is named by FileDataID,
/// so a map-backed CascFileSystem stands in for the install.

#include <catch2/catch_all.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <whiteout/models/m2/m2.h>
#include <whiteout/models/m2/sequence_loader.h>
#include <whiteout/utils/os_file_system.h>

namespace fs = std::filesystem;
using namespace whiteout;

namespace {

std::vector<u8> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string corpusDir() {
    if (const char* env = std::getenv("M2_CORPUS_DIR"); env && fs::is_directory(env)) {
        return env;
    }
    for (auto candidate : {"Corpus/WoW", "../Corpus/WoW", "../../Corpus/WoW"}) {
        if (fs::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

/// Copies @p model and every sibling sharing its stem into a fresh directory, with the MD20
/// globalFlags word rewritten to `(flags & ~clear) | set`. Returns the copy's path.
fs::path copyWithFlags(const fs::path& model, const std::string& tag, u32 clear, u32 set) {
    const fs::path dir = fs::temp_directory_path() / ("m2_load_flags_test_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string stem = model.stem().string();
    for (const auto& entry : fs::directory_iterator(model.parent_path())) {
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind(stem, 0) == 0) {
            fs::copy_file(entry.path(), dir / name);
        }
    }
    const fs::path copy = dir / model.filename();
    std::vector<u8> bytes = readFile(copy);
    // MD21 wraps the MD20 header in an 8-byte chunk header; flags sit at MD20 +0x10.
    const size_t at = (bytes.size() >= 4 && std::memcmp(bytes.data(), "MD21", 4) == 0) ? 0x18 : 0x10;
    REQUIRE(bytes.size() >= at + 4);
    u32 flags = 0;
    std::memcpy(&flags, bytes.data() + at, 4);
    flags = (flags & ~clear) | set;
    std::memcpy(bytes.data() + at, &flags, 4);
    writeFile(copy, bytes);
    return copy;
}

m2::Model parseByPath(const fs::path& model) {
    const fs::path abs = fs::absolute(model);
    utils::OsFileSystem vfs(abs.parent_path().string());
    m2::Parser parser;
    return parser.parse(vfs, abs.string());
}

/// FileDataIDs served from files on disk.
class MapCascFs final : public interfaces::CascFileSystem {
public:
    std::map<u32, fs::path> files;

    std::vector<u8> readFile(u32 fileId) const override {
        const auto it = files.find(fileId);
        return it == files.end() ? std::vector<u8>{} : ::readFile(it->second);
    }
    std::optional<u32> reserveFileId(const std::string&) override {
        return std::nullopt;
    }
    bool writeFile(u32, const std::vector<u8>&) override {
        return false;
    }
    bool fileExists(u32 fileId) const override {
        return files.count(fileId) != 0;
    }
};

/// Maps @p skel's AFID entries to the `<stem>NNNN-NN.anim` files beside it.
void mapAnims(MapCascFs& casc, const fs::path& skel, const std::string& animStem) {
    const std::vector<u8> bytes = readFile(skel);
    for (size_t pos = 0; pos + 8 <= bytes.size();) {
        u32 size = 0;
        std::memcpy(&size, bytes.data() + pos + 4, 4);
        if (std::memcmp(bytes.data() + pos, "AFID", 4) == 0) {
            for (size_t e = 0; e + 8 <= size; e += 8) {
                u16 anim = 0, sub = 0;
                u32 id = 0;
                std::memcpy(&anim, bytes.data() + pos + 8 + e, 2);
                std::memcpy(&sub, bytes.data() + pos + 8 + e + 2, 2);
                std::memcpy(&id, bytes.data() + pos + 8 + e + 4, 4);
                char name[32];
                std::snprintf(name, sizeof(name), "%04u-%02u.anim", anim, sub);
                const fs::path path = skel.parent_path() / (animStem + name);
                if (id != 0 && fs::exists(path)) {
                    casc.files[id] = path;
                }
            }
        }
        pos += 8 + size;
    }
}

constexpr u32 kExternalSkeleton = static_cast<u32>(m2::GlobalFlag::ExternalSkeleton);

} // namespace

TEST_CASE("M2 reads its .skel only with ExternalSkeleton", "[m2][skel][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }
    const fs::path model = fs::path(dir) / "creature/bloodtick/bloodtick.m2";
    REQUIRE(fs::exists(model));

    const m2::Model withFlag = parseByPath(copyWithFlags(model, "with", 0, 0));
    REQUIRE(hasFlag(withFlag.globalFlags.value, m2::GlobalFlag::ExternalSkeleton));
    CHECK_FALSE(withFlag.bones.empty());
    CHECK_FALSE(withFlag.sequences.empty());

    // The same file with the bit cleared keeps only what its own header holds, which for a
    // SKID model is nothing: 12.1 never opens the `.skel`.
    const m2::Model without = parseByPath(copyWithFlags(model, "without", kExternalSkeleton, 0));
    CHECK(without.bones.empty());
    CHECK(without.sequences.empty());
}

TEST_CASE("M2 keeps an SKPD child's own skeleton", "[m2][skel][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }
    const m2::Model model =
        parseByPath(fs::path(dir) / "character/darkirondwarf/female/darkirondwarffemale.m2");

    // The child's 231 bones and 25 sequences; the library used to swap in the parent's, and by
    // path (where the parent cannot be reached) ended up with none at all.
    CHECK(model.bones.size() == 231);
    CHECK(model.sequences.size() == 25);
    REQUIRE(model.parentSkeleton.has_value());
    CHECK(model.parentSkeleton->fileId == 0x1CE141);
    CHECK_FALSE(model.parentSkeleton->loaded);
}

TEST_CASE("M2 an SKPD parent adds its table and ORs its flags", "[m2][skel][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }
    const fs::path parentSkel = fs::path(dir) / "character/dwarf/female/dwarffemale_hd.skel";
    REQUIRE(fs::exists(parentSkel));
    MapCascFs casc;
    casc.files[0x1CE141] = parentSkel;
    mapAnims(casc, parentSkel, "dwarffemale");

    m2::Model model =
        parseByPath(fs::path(dir) / "character/darkirondwarf/female/darkirondwarffemale.m2");
    REQUIRE(model.parentSkeleton.has_value());
    std::vector<u32> before;
    for (const auto& bone : model.bones) {
        before.push_back(bone.flags);
    }

    REQUIRE(m2::loadParentSkeleton(model, casc, true));
    const m2::ParentSkeleton& parent = *model.parentSkeleton;
    CHECK(parent.loaded);
    CHECK(parent.sequences.size() > model.sequences.size());
    REQUIRE(parent.bones.size() == model.bones.size());

    // child |= parent & 0xFFF8FFFF: every parent bit but the LOD tiers.
    size_t gained = 0;
    for (size_t i = 0; i < model.bones.size(); ++i) {
        INFO("bone " << i);
        CHECK(model.bones[i].flags == (before[i] | (parent.bones[i].flags & 0xFFF8FFFFu)));
        CHECK(((model.bones[i].flags & ~before[i]) & 0x70000u) == 0);
        gained += model.bones[i].flags != before[i] ? 1 : 0;
    }
    CHECK(gained > 0);
    // The root is 0 in the child and animated in the parent.
    CHECK((before[0] & 0x200u) == 0);
    CHECK((model.bones[0].flags & 0x200u) != 0);

    // A lazy parent streams its keys per sequence, from its own `.anim`s.
    u32 streamed = 0;
    for (u32 s = 0; s < parent.sequences.size() && streamed == 0; ++s) {
        if (!m2::parentSequenceKeysPending(model, s)) {
            continue;
        }
        if (!m2::loadParentSequence(model, s)) {
            continue;
        }
        for (const auto& bone : model.parentSkeleton->bones) {
            if (s < bone.rotation.values.size() && !bone.rotation.values[s].empty()) {
                ++streamed;
                break;
            }
        }
    }
    CHECK(streamed == 1);
}

TEST_CASE("M2 an SKPD parent of another size ORs nothing", "[m2][skel][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }
    MapCascFs casc;
    casc.files[0x1CE141] = fs::path(dir) / "character/dwarf/female/dwarffemale_hd.skel";

    m2::Model model =
        parseByPath(fs::path(dir) / "character/darkirondwarf/female/darkirondwarffemale.m2");
    model.bones.emplace_back();
    std::vector<u32> before;
    for (const auto& bone : model.bones) {
        before.push_back(bone.flags);
    }
    REQUIRE(m2::loadParentSkeleton(model, casc, true));
    for (size_t i = 0; i < model.bones.size(); ++i) {
        CHECK(model.bones[i].flags == before[i]);
    }
}

TEST_CASE("M2 version 274 ignores UseTextureCombinerCombos", "[m2][flags][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }
    // 12.1 loads versions 272-274 only, and its header is a fixed 0x130 bytes: a set 0x8 names no
    // array there. Read as one, the 8 bytes past the header would become a bogus combo table.
    const fs::path model = fs::path(dir) / "creature/cow/cow.m2";
    REQUIRE(fs::exists(model));
    const m2::Model plain = parseByPath(copyWithFlags(model, "plain", 0, 0));
    REQUIRE(plain.fileVersion >= 272);
    const m2::Model flagged = parseByPath(
        copyWithFlags(model, "combos", 0, static_cast<u32>(m2::GlobalFlag::UseTextureCombinerCombos)));
    CHECK(flagged.textureCombinerCombos.empty());
    CHECK(flagged.bones.size() == plain.bones.size());
    CHECK(flagged.particleEmitters.size() == plain.particleEmitters.size());
}
