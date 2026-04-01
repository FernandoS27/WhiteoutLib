// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Corpus test for D3 Appearance (.app) files: loads every .app file from a
/// directory, parses it with the SNO type system, and validates key header
/// fields against the APP_FILE_FORMAT_SPECIFICATION.md.

#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<whiteout::u8> readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = static_cast<size_t>(f.tellg());
    std::vector<whiteout::u8> buf(sz);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
    return buf;
}

static whiteout::u32 readU32(const std::vector<whiteout::u8>& buf, size_t off) {
    whiteout::u32 v = 0;
    std::memcpy(&v, buf.data() + off, 4);
    return v;
}

static float readF32(const std::vector<whiteout::u8>& buf, size_t off) {
    float v = 0;
    std::memcpy(&v, buf.data() + off, 4);
    return v;
}

TEST_CASE("D3 APP corpus", "[d3][app][corpus]") {
    using namespace whiteout;
    using namespace whiteout::sno;

    // Auto-discover corpus directory
    fs::path corpusDir;
    for (auto candidate : {"Corpus/D3/Appearances", "../Corpus/D3/Appearances", "../../Corpus/D3/Appearances"}) {
        if (fs::is_directory(candidate)) { corpusDir = candidate; break; }
    }
    if (corpusDir.empty()) SKIP("D3 Appearances corpus not found");

    SnoReader reader;

    size_t totalFiles    = 0;
    size_t parseOk       = 0;
    size_t parseFailed   = 0;
    size_t boneDataOk    = 0;
    size_t boneDataBad   = 0;
    size_t boneStructOk  = 0;
    size_t boneStructBad = 0;
    size_t capsuleDataOk = 0;
    size_t capsuleDataBad= 0;
    size_t submeshDataOk = 0;
    size_t submeshDataBad= 0;
    size_t matDataOk     = 0;
    size_t matDataBad    = 0;
    size_t matStructOk   = 0;
    size_t matStructBad  = 0;
    size_t submeshStrOk  = 0;
    size_t submeshStrBad = 0;
    size_t lookStructOk  = 0;
    size_t lookStructBad = 0;
    size_t refPtStructOk = 0;
    size_t refPtStructBad= 0;
    size_t capsStrOk     = 0;
    size_t capsStrBad    = 0;
    size_t vertexOk      = 0;
    size_t vertexBad     = 0;

    for (auto& entry : fs::directory_iterator(corpusDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".app") continue;

        ++totalFiles;
        auto data = readFile(entry.path());
        if (data.size() < 552) { // Minimum APP header size
            ++parseFailed;
            std::cerr << "[FAIL] " << entry.path().filename() << ": too small ("
                      << data.size() << " bytes)\n";
            continue;
        }

        // Verify magic
        u32 magic = readU32(data, 0);
        if (magic != 0xDEADBEEF) {
            ++parseFailed;
            std::cerr << "[FAIL] " << entry.path().filename() << ": bad magic 0x"
                      << std::hex << magic << std::dec << "\n";
            continue;
        }

        // Parse with the SNO reader (this exercises the D3 type registry)
        auto file = reader.parse(data, SnoGroup::Appearance);
        if (!file) {
            ++parseFailed;
            std::cerr << "[FAIL] " << entry.path().filename() << ": parse failed\n";
            continue;
        }
        ++parseOk;

        // Validate key fields from the parsed result AND raw binary
        const auto& root = file->root;

        // --- Bone data validation ---
        // Raw: boneCount at 0x024, boneOffset at 0x028, boneDataSize at 0x02C
        u32 rawBoneCount  = readU32(data, 0x024);
        u32 rawBoneOffset = readU32(data, 0x028);
        u32 rawBoneSize   = readU32(data, 0x02C);

        // Check parsed dwBoneCount matches raw
        auto* parsedBoneCount = root.field("dwBoneCount");
        if (parsedBoneCount && parsedBoneCount->isInt()) {
            i32 pbc = parsedBoneCount->asInt();
            if (static_cast<u32>(pbc) == rawBoneCount) {
                ++boneDataOk;
            } else {
                ++boneDataBad;
                std::cerr << "[BONE] " << entry.path().filename()
                          << ": parsed boneCount=" << pbc
                          << " raw=" << rawBoneCount << "\n";
            }
        } else {
            ++boneDataBad;
            std::cerr << "[BONE] " << entry.path().filename()
                      << ": dwBoneCount field not found\n";
        }

        // Validate sdBoneData: check structured bone entries match raw data
        auto* boneArr = root.field("sdBoneData");
        if (rawBoneCount > 0 && boneArr && boneArr->isArray()) {
            if (boneArr->size() == rawBoneCount) {
                bool allGoodBones = true;
                size_t actualDataBase = rawBoneOffset + 16; // +16 data access convention
                for (size_t bi = 0; bi < boneArr->size() && allGoodBones; ++bi) {
                    auto* bone = boneArr->at(bi);
                    if (!bone) { allGoodBones = false; break; }
                    // Validate bone name vs raw
                    auto* nameField = bone->field("szBoneName");
                    if (!nameField || !nameField->isString()) {
                        allGoodBones = false;
                        std::cerr << "[BSTR] " << entry.path().filename()
                                  << ": bone " << bi << " szBoneName missing\n";
                        break;
                    }
                    size_t boneFileOff = actualDataBase + bi * 312;
                    const char* rawName = reinterpret_cast<const char*>(data.data() + boneFileOff);
                    size_t rawLen = 0;
                    while (rawLen < 64 && rawName[rawLen] != '\0') ++rawLen;
                    if (nameField->asString() != std::string(rawName, rawLen)) {
                        allGoodBones = false;
                        std::cerr << "[BSTR] " << entry.path().filename()
                                  << ": bone " << bi << " name mismatch\n";
                    }
                    // Validate parentBoneId vs raw
                    auto* parentField = bone->field("dwParentBoneId");
                    if (!parentField || !parentField->isInt()) {
                        allGoodBones = false;
                        break;
                    }
                    u32 rawParent = readU32(data, boneFileOff + 64);
                    if (static_cast<u32>(parentField->asInt()) != rawParent) {
                        allGoodBones = false;
                        std::cerr << "[BSTR] " << entry.path().filename()
                                  << ": bone " << bi << " parentId mismatch\n";
                    }
                }
                if (allGoodBones) ++boneStructOk; else ++boneStructBad;
            } else {
                ++boneStructBad;
                std::cerr << "[BSTR] " << entry.path().filename()
                          << ": bone array size " << boneArr->size()
                          << " != rawBoneCount " << rawBoneCount << "\n";
            }
        } else if (rawBoneCount == 0) {
            ++boneStructOk; // No bones is fine
        } else {
            ++boneStructBad;
            std::cerr << "[BSTR] " << entry.path().filename()
                      << ": sdBoneData not an array\n";
        }

        // --- Collision capsule data validation ---
        u32 rawCapsuleCount  = readU32(data, 0x0E8);
        u32 rawCapsuleOffset = readU32(data, 0x0EC);
        u32 rawCapsuleSize   = readU32(data, 0x0F0);

        auto* parsedCapsuleCount = root.field("dwCollCapsuleCount");
        if (parsedCapsuleCount && parsedCapsuleCount->isInt()) {
            i32 pcc = parsedCapsuleCount->asInt();
            if (static_cast<u32>(pcc) == rawCapsuleCount) {
                ++capsuleDataOk;
            } else {
                ++capsuleDataBad;
                std::cerr << "[CAPS] " << entry.path().filename()
                          << ": parsed capsuleCount=" << pcc
                          << " raw=" << rawCapsuleCount << "\n";
            }
        } else {
            ++capsuleDataBad;
            std::cerr << "[CAPS] " << entry.path().filename()
                      << ": dwCollCapsuleCount field not found\n";
        }

        // --- Submesh data validation ---
        u32 rawSubmeshCount = readU32(data, 0x0A8);
        auto* parsedSubmeshCount = root.field("dwSubmeshCount");
        if (parsedSubmeshCount && parsedSubmeshCount->isInt()) {
            i32 psc = parsedSubmeshCount->asInt();
            if (static_cast<u32>(psc) == rawSubmeshCount) {
                ++submeshDataOk;
            } else {
                ++submeshDataBad;
            }
        } else {
            ++submeshDataBad;
        }

        // --- Material data validation ---
        u32 rawMatCount = readU32(data, 0x1B4);
        auto* parsedMatCount = root.field("dwMaterialCount");
        if (parsedMatCount && parsedMatCount->isInt()) {
            i32 pmc = parsedMatCount->asInt();
            if (static_cast<u32>(pmc) == rawMatCount) {
                ++matDataOk;
            } else {
                ++matDataBad;
            }
        } else {
            ++matDataBad;
        }

        // --- Material struct validation (name + shader data vs raw) ---
        u32 rawMatOffset = readU32(data, 0x1B8);
        auto* matArr = root.field("sdMaterialData");
        if (rawMatCount > 0 && matArr && matArr->isArray()) {
            if (matArr->size() == rawMatCount) {
                bool allGoodMat = true;
                size_t matBase = rawMatOffset + 16;
                for (size_t mi = 0; mi < matArr->size() && allGoodMat; ++mi) {
                    auto* mat = matArr->at(mi);
                    if (!mat) { allGoodMat = false; break; }
                    auto* nameField = mat->field("szMaterialName");
                    if (!nameField || !nameField->isString()) {
                        allGoodMat = false;
                        std::cerr << "[MSTR] " << entry.path().filename()
                                  << ": mat " << mi << " szMaterialName missing\n";
                        break;
                    }
                    size_t matFileOff = matBase + mi * 144;
                    const char* rawName = reinterpret_cast<const char*>(data.data() + matFileOff);
                    size_t rawLen = 0;
                    while (rawLen < 128 && rawName[rawLen] != '\0') ++rawLen;
                    if (nameField->asString() != std::string(rawName, rawLen)) {
                        allGoodMat = false;
                        std::cerr << "[MSTR] " << entry.path().filename()
                                  << ": mat " << mi << " name mismatch: \""
                                  << nameField->asString() << "\" vs raw\n";
                    }
                    // Validate shader data block (tShaderData pointer)
                    u32 rawSDOff = readU32(data, matFileOff + 128);
                    u32 rawSDSize = readU32(data, matFileOff + 132);
                    auto* sdb = mat->field("tShaderData");
                    if (rawSDOff > 0 && rawSDSize >= 120 && sdb && sdb->isObject()) {
                        // Validate shaderMapSno against raw at shaderDataBlock + 0x18
                        size_t blkFile = rawSDOff + 16; // +16 data access convention
                        auto* snoField = sdb->field("snoShaderMap");
                        if (snoField && snoField->isObject()) {
                            auto* snoId = snoField->field("snoId");
                            u32 rawSnoVal = readU32(data, blkFile + 0x18);
                            if (!snoId || !snoId->isInt() ||
                                static_cast<u32>(snoId->asInt()) != rawSnoVal) {
                                allGoodMat = false;
                                std::cerr << "[MSHD] " << entry.path().filename()
                                          << ": mat " << mi << " shaderMapSno mismatch\n";
                            }
                        }
                        // Validate diffuseR against raw
                        auto* dr = sdb->field("flDiffuseR");
                        float rawDR = readF32(data, blkFile + 0x1C);
                        if (!dr || !dr->isFloat() || dr->asFloat() != rawDR) {
                            allGoodMat = false;
                            std::cerr << "[MSHD] " << entry.path().filename()
                                      << ": mat " << mi << " diffuseR mismatch\n";
                        }
                        // Validate texture array
                        u32 rawTexOff = readU32(data, blkFile + 0x64);
                        u32 rawTexSize = readU32(data, blkFile + 0x68);
                        auto* texArr = sdb->field("sdTexData");
                        if (rawTexOff > 0 && rawTexSize >= 160) {
                            u32 texCount = rawTexSize / 160;
                            if (!texArr || !texArr->isArray() ||
                                texArr->size() != texCount) {
                                allGoodMat = false;
                                std::cerr << "[MTEX] " << entry.path().filename()
                                          << ": mat " << mi << " texArray size "
                                          << (texArr && texArr->isArray() ? texArr->size() : 0)
                                          << " != " << texCount << "\n";
                            } else if (texCount > 0) {
                                // Spot-check first texture's SNO ref
                                size_t texBase = rawTexOff + 16;
                                u32 rawTexSno = readU32(data, texBase + 0x08);
                                auto* t0 = texArr->at(0);
                                auto* snoTex = t0 ? t0->field("snoTexture") : nullptr;
                                if (snoTex && snoTex->isObject()) {
                                    auto* tid = snoTex->field("snoId");
                                    if (!tid || !tid->isInt() ||
                                        static_cast<u32>(tid->asInt()) != rawTexSno) {
                                        allGoodMat = false;
                                        std::cerr << "[MTEX] " << entry.path().filename()
                                                  << ": mat " << mi << " tex[0] sno mismatch\n";
                                    }
                                }
                            }
                        }
                    } else if (rawSDOff > 0 && rawSDSize >= 120) {
                        allGoodMat = false;
                        std::cerr << "[MSHD] " << entry.path().filename()
                                  << ": mat " << mi << " tShaderData missing\n";
                    }
                }
                if (allGoodMat) ++matStructOk; else ++matStructBad;
            } else {
                ++matStructBad;
                std::cerr << "[MSTR] " << entry.path().filename()
                          << ": mat array size " << matArr->size()
                          << " != rawMatCount " << rawMatCount << "\n";
            }
        } else if (rawMatCount == 0) {
            ++matStructOk;
        } else {
            ++matStructBad;
        }

        // --- Submesh struct validation (meshName, vertexCount vs raw) ---
        u32 rawSubmeshOffset = readU32(data, 0x0AC);
        auto* submeshArr = root.field("sdSubmeshData");
        if (rawSubmeshCount > 0 && submeshArr && submeshArr->isArray()) {
            if (submeshArr->size() == rawSubmeshCount) {
                bool allGoodSub = true;
                size_t subBase = rawSubmeshOffset + 16;
                for (size_t si = 0; si < submeshArr->size() && allGoodSub; ++si) {
                    auto* sub = submeshArr->at(si);
                    if (!sub) { allGoodSub = false; break; }
                    size_t subFileOff = subBase + si * 400;
                    // Validate meshName
                    auto* mnf = sub->field("szMeshName");
                    if (!mnf || !mnf->isString()) {
                        allGoodSub = false;
                        std::cerr << "[SSTR] " << entry.path().filename()
                                  << ": submesh " << si << " szMeshName missing\n";
                        break;
                    }
                    const char* rawMN = reinterpret_cast<const char*>(data.data() + subFileOff + 0x5C);
                    size_t rl = 0;
                    while (rl < 128 && rawMN[rl] != '\0') ++rl;
                    if (mnf->asString() != std::string(rawMN, rl)) {
                        allGoodSub = false;
                        std::cerr << "[SSTR] " << entry.path().filename()
                                  << ": submesh " << si << " meshName mismatch\n";
                    }
                    // Validate vertexCount
                    auto* vcf = sub->field("dwVertexCount");
                    u32 rawVC = readU32(data, subFileOff + 4);
                    if (!vcf || !vcf->isInt() || static_cast<u32>(vcf->asInt()) != rawVC) {
                        allGoodSub = false;
                        std::cerr << "[SSTR] " << entry.path().filename()
                                  << ": submesh " << si << " vertexCount mismatch\n";
                    }
                    // Validate vertex array count matches vertexCount
                    auto* verts = sub->field("sdVertexData");
                    u32 rawVertOff = readU32(data, subFileOff + 8);
                    if (rawVC > 0 && rawVertOff > 0) {
                        if (!verts || !verts->isArray() || verts->size() != rawVC) {
                            allGoodSub = false;
                            std::cerr << "[SSTR] " << entry.path().filename()
                                      << ": submesh " << si << " vertex array size mismatch ("
                                      << (verts && verts->isArray() ? verts->size() : 0)
                                      << " vs " << rawVC << ")\n";
                        } else {
                            // Spot-check first vertex position vs raw
                            size_t vertBase = rawVertOff + 16;
                            float rawVX = readF32(data, vertBase);
                            auto* v0 = verts->at(0);
                            auto* vp = v0 ? v0->field("vVertPos") : nullptr;
                            if (!vp || !vp->isVec3() || vp->asVec3().x != rawVX) {
                                allGoodSub = false;
                                std::cerr << "[SSTR] " << entry.path().filename()
                                          << ": submesh " << si << " vertex[0].x mismatch\n";
                            }
                        }
                    }
                }
                if (allGoodSub) ++submeshStrOk; else ++submeshStrBad;
            } else {
                ++submeshStrBad;
                std::cerr << "[SSTR] " << entry.path().filename()
                          << ": submesh array size " << submeshArr->size()
                          << " != rawSubmeshCount " << rawSubmeshCount << "\n";
            }
        } else if (rawSubmeshCount == 0) {
            ++submeshStrOk;
        } else {
            ++submeshStrBad;
        }

        // --- Look table validation ---
        u32 rawLookCount  = readU32(data, 0x1B0);
        u32 rawLookOffset = readU32(data, 0x1C8);
        auto* lookArr = root.field("sdLookTableData");
        if (rawLookCount > 0 && lookArr && lookArr->isArray()) {
            if (lookArr->size() == rawLookCount) {
                bool allGoodLk = true;
                size_t lkBase = rawLookOffset + 16;
                for (size_t li = 0; li < lookArr->size() && allGoodLk; ++li) {
                    auto* look = lookArr->at(li);
                    if (!look) { allGoodLk = false; break; }
                    auto* lnf = look->field("szLookName");
                    if (!lnf || !lnf->isString()) { allGoodLk = false; break; }
                    const char* rawLN = reinterpret_cast<const char*>(data.data() + lkBase + li * 64);
                    size_t rl = 0;
                    while (rl < 64 && rawLN[rl] != '\0') ++rl;
                    if (lnf->asString() != std::string(rawLN, rl)) {
                        allGoodLk = false;
                        std::cerr << "[LOOK] " << entry.path().filename()
                                  << ": look " << li << " name mismatch\n";
                    }
                }
                if (allGoodLk) ++lookStructOk; else ++lookStructBad;
            } else {
                ++lookStructBad;
            }
        } else if (rawLookCount == 0) {
            ++lookStructOk;
        } else {
            ++lookStructBad;
        }

        // --- Reference point validation ---
        u32 rawRefCount  = readU32(data, 0x100);
        u32 rawRefOffset = readU32(data, 0x104);
        auto* refArr = root.field("sdRefPointData");
        if (rawRefCount > 0 && refArr && refArr->isArray()) {
            if (refArr->size() == rawRefCount) {
                bool allGoodRef = true;
                size_t refBase = rawRefOffset + 16;
                for (size_t ri = 0; ri < refArr->size() && allGoodRef; ++ri) {
                    auto* rp = refArr->at(ri);
                    if (!rp) { allGoodRef = false; break; }
                    auto* rnf = rp->field("szRefPointName");
                    if (!rnf || !rnf->isString()) { allGoodRef = false; break; }
                    const char* rawRN = reinterpret_cast<const char*>(data.data() + refBase + ri * 96);
                    size_t rl = 0;
                    while (rl < 64 && rawRN[rl] != '\0') ++rl;
                    if (rnf->asString() != std::string(rawRN, rl)) {
                        allGoodRef = false;
                        std::cerr << "[REFP] " << entry.path().filename()
                                  << ": refpoint " << ri << " name mismatch\n";
                    }
                    // Validate parentId
                    auto* rpid = rp->field("dwRefParentId");
                    u32 rawPID = readU32(data, refBase + ri * 96 + 64);
                    if (!rpid || !rpid->isInt() || static_cast<u32>(rpid->asInt()) != rawPID) {
                        allGoodRef = false;
                        std::cerr << "[REFP] " << entry.path().filename()
                                  << ": refpoint " << ri << " parentId mismatch\n";
                    }
                }
                if (allGoodRef) ++refPtStructOk; else ++refPtStructBad;
            } else {
                ++refPtStructBad;
            }
        } else if (rawRefCount == 0) {
            ++refPtStructOk;
        } else {
            ++refPtStructBad;
        }

        // --- Collision capsule struct validation ---
        auto* capsArr = root.field("sdCollCapsuleData");
        if (rawCapsuleCount > 0 && capsArr && capsArr->isArray()) {
            if (capsArr->size() == rawCapsuleCount) {
                bool allGoodCap = true;
                size_t capBase = rawCapsuleOffset + 16;
                for (size_t ci = 0; ci < capsArr->size() && allGoodCap; ++ci) {
                    auto* cap = capsArr->at(ci);
                    if (!cap) { allGoodCap = false; break; }
                    auto* cnf = cap->field("szCapsuleName");
                    if (!cnf || !cnf->isString()) { allGoodCap = false; break; }
                    const char* rawCN = reinterpret_cast<const char*>(data.data() + capBase + ci * 104);
                    size_t rl = 0;
                    while (rl < 64 && rawCN[rl] != '\0') ++rl;
                    if (cnf->asString() != std::string(rawCN, rl)) {
                        allGoodCap = false;
                        std::cerr << "[CAPN] " << entry.path().filename()
                                  << ": capsule " << ci << " name mismatch\n";
                    }
                }
                if (allGoodCap) ++capsStrOk; else ++capsStrBad;
            } else {
                ++capsStrBad;
                std::cerr << "[CAPN] " << entry.path().filename()
                          << ": capsule array size " << capsArr->size()
                          << " != raw " << rawCapsuleCount << "\n";
            }
        } else if (rawCapsuleCount == 0) {
            ++capsStrOk;
        } else {
            ++capsStrBad;
        }

        // Print progress every 1000 files
        if (totalFiles % 1000 == 0) {
            std::cout << "  ... processed " << totalFiles << " files\n";
        }
    }

    // Summary
    std::cout << "\n=== D3 APP Corpus Test Results ===\n"
              << "Total files:          " << totalFiles << "\n"
              << "Parse OK:             " << parseOk << "\n"
              << "Parse FAILED:         " << parseFailed << "\n"
              << "Bone count OK:        " << boneDataOk << "/" << parseOk << "\n"
              << "Bone count BAD:       " << boneDataBad << "\n"
              << "Bone struct OK:       " << boneStructOk << "/" << parseOk << "\n"
              << "Bone struct BAD:      " << boneStructBad << "\n"
              << "Capsule count OK:     " << capsuleDataOk << "/" << parseOk << "\n"
              << "Capsule count BAD:    " << capsuleDataBad << "\n"
              << "Capsule struct OK:    " << capsStrOk << "/" << parseOk << "\n"
              << "Capsule struct BAD:   " << capsStrBad << "\n"
              << "Submesh count OK:     " << submeshDataOk << "/" << parseOk << "\n"
              << "Submesh count BAD:    " << submeshDataBad << "\n"
              << "Submesh struct OK:    " << submeshStrOk << "/" << parseOk << "\n"
              << "Submesh struct BAD:   " << submeshStrBad << "\n"
              << "Material count OK:    " << matDataOk << "/" << parseOk << "\n"
              << "Material count BAD:   " << matDataBad << "\n"
              << "Material struct OK:   " << matStructOk << "/" << parseOk << "\n"
              << "Material struct BAD:  " << matStructBad << "\n"
              << "Look struct OK:       " << lookStructOk << "/" << parseOk << "\n"
              << "Look struct BAD:      " << lookStructBad << "\n"
              << "RefPoint struct OK:   " << refPtStructOk << "/" << parseOk << "\n"
              << "RefPoint struct BAD:  " << refPtStructBad << "\n";

    bool allGood = (parseFailed == 0 && boneDataBad == 0 && boneStructBad == 0
                    && capsuleDataBad == 0 && capsStrBad == 0
                    && submeshDataBad == 0 && submeshStrBad == 0
                    && matDataBad == 0 && matStructBad == 0
                    && lookStructBad == 0 && refPtStructBad == 0);

    if (allGood) {
        std::cout << "\n*** ALL TESTS PASSED ***\n";
        // done
    } else {
        std::cout << "\n*** SOME TESTS FAILED ***\n";
        FAIL("corpus test failed");
    }
}
