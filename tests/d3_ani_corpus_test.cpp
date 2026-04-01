// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// Corpus test for D3 Animation (.ani) files: loads every .ani file from a
/// directory, parses it with the SNO type system, and validates key fields
/// against the ANI_FILE_FORMAT_SPECIFICATION.md.

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
    if (off + 4 <= buf.size())
        std::memcpy(&v, buf.data() + off, 4);
    return v;
}

static float readF32(const std::vector<whiteout::u8>& buf, size_t off) {
    float v = 0;
    if (off + 4 <= buf.size())
        std::memcpy(&v, buf.data() + off, 4);
    return v;
}

static whiteout::i16 readI16(const std::vector<whiteout::u8>& buf, size_t off) {
    whiteout::i16 v = 0;
    if (off + 2 <= buf.size())
        std::memcpy(&v, buf.data() + off, 2);
    return v;
}

TEST_CASE("D3 ANI corpus", "[d3][ani][corpus]") {
    using namespace whiteout;
    using namespace whiteout::sno;

    // Auto-discover corpus directory
    fs::path corpusDir;
    for (auto candidate : {"Corpus/D3/Anim", "../Corpus/D3/Anim", "../../Corpus/D3/Anim"}) {
        if (fs::is_directory(candidate)) { corpusDir = candidate; break; }
    }
    if (corpusDir.empty()) SKIP("D3 Anim corpus not found");

    SnoReader reader;

    size_t totalFiles     = 0;
    size_t parseOk        = 0;
    size_t parseFailed    = 0;
    size_t headerOk       = 0;
    size_t headerBad      = 0;
    size_t boneNameOk     = 0;
    size_t boneNameBad    = 0;
    size_t translDescOk   = 0;
    size_t translDescBad  = 0;
    size_t rotDescOk      = 0;
    size_t rotDescBad     = 0;
    size_t scaleDescOk    = 0;
    size_t scaleDescBad   = 0;
    size_t translKfOk     = 0;
    size_t translKfBad    = 0;
    size_t rotKfOk        = 0;
    size_t rotKfBad       = 0;
    size_t scaleKfOk      = 0;
    size_t scaleKfBad     = 0;
    size_t lookOk         = 0;
    size_t lookBad        = 0;
    size_t perFrameOk     = 0;
    size_t perFrameBad    = 0;

    for (auto& entry : fs::directory_iterator(corpusDir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".ani") continue;

        ++totalFiles;
        auto data = readFile(entry.path());
        if (data.size() < 56) {
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

        // Parse with SNO reader (D3 Animation = group 6)
        auto file = reader.parse(data, SnoGroup::Animation);
        if (!file) {
            ++parseFailed;
            std::cerr << "[FAIL] " << entry.path().filename() << ": parse failed\n";
            continue;
        }
        ++parseOk;

        const auto& root = file->root;

        // ── Validate header fields ──────────────────────────────────────
        // D3::Anim is a flat 448-byte struct:
        //   - 40 bytes preamble ANI-specific (file offset 0x010–0x037, type offset 0–39)
        //   - 408 bytes SubAnimBlock (file offset 0x038–0x1CF, type offset 40–447)
        // All fields are directly on root (no nested block array).
        u32 rawVersion = readU32(data, 0x004);
        bool hdrGood = true;
        if (rawVersion != 118) {
            hdrGood = false;
            std::cerr << "[HDR] " << entry.path().filename()
                      << ": unexpected version " << rawVersion << "\n";
        }

        // dwBlockCount at file offset 0x030
        u32 rawBlockCount = readU32(data, 0x030);
        auto* parsedBlockCount = root.field("dwBlockCount");
        if (parsedBlockCount && parsedBlockCount->isInt()) {
            if (static_cast<u32>(parsedBlockCount->asInt()) != rawBlockCount) {
                hdrGood = false;
                std::cerr << "[HDR] " << entry.path().filename()
                          << ": dwBlockCount mismatch " << parsedBlockCount->asInt()
                          << " != " << rawBlockCount << "\n";
            }
        }

        if (hdrGood) ++headerOk; else ++headerBad;

        // Raw offsets from the SubAnimBlock (starts at file offset 0x038)
        const size_t blkBase = 0x038;

        // boneCount at blkBase+0x088
        u32 rawBoneCount = readU32(data, blkBase + 0x088);

        // ── Bone names ──────────────────────────────────────────────────
        auto* boneNames = root.field("sdBoneNames");
        if (boneNames && boneNames->isArray()) {
            if (boneNames->size() == rawBoneCount) {
                // Spot-check first bone name against raw data
                u32 rawBoneNameOff = readU32(data, blkBase + 0x08C);
                if (rawBoneCount > 0 && rawBoneNameOff > 0) {
                    size_t fileOff = rawBoneNameOff + 16;
                    if (fileOff + 64 <= data.size()) {
                        auto* firstBone = boneNames->at(0);
                        if (firstBone && firstBone->isObject()) {
                            auto* nameField = firstBone->field("szBoneNameStr");
                            if (nameField && nameField->isString()) {
                                // DT_FIXEDARRAY<DT_BYTE> is parsed as string
                                const char* rawName = reinterpret_cast<const char*>(data.data() + fileOff);
                                size_t rawLen = 0;
                                while (rawLen < 64 && rawName[rawLen] != '\0') ++rawLen;
                                std::string rawStr(rawName, rawLen);
                                if (nameField->asString() == rawStr) ++boneNameOk;
                                else {
                                    ++boneNameBad;
                                    std::cerr << "[BONE] " << entry.path().filename()
                                              << ": bone name mismatch: '"
                                              << nameField->asString() << "' != '" << rawStr << "'\n";
                                }
                            } else if (nameField && nameField->isArray()) {
                                // Alternative: check first byte
                                u8 rawByte = data[fileOff];
                                auto* firstByte = nameField->at(0);
                                bool match = false;
                                if (firstByte) {
                                    if (firstByte->isInt()) match = static_cast<u8>(firstByte->asInt()) == rawByte;
                                    else if (firstByte->isUint()) match = static_cast<u8>(firstByte->asUint()) == rawByte;
                                }
                                if (match) ++boneNameOk; else {
                                    ++boneNameBad;
                                    std::cerr << "[BONE] " << entry.path().filename()
                                              << ": first bone byte mismatch\n";
                                }
                            } else {
                                ++boneNameBad;
                                std::cerr << "[BONE] " << entry.path().filename()
                                          << ": szBoneNameStr unexpected type\n";
                            }
                        } else {
                            ++boneNameBad;
                            std::cerr << "[BONE] " << entry.path().filename()
                                      << ": first bone entry not an object\n";
                        }
                    } else {
                        ++boneNameBad;
                        std::cerr << "[BONE] " << entry.path().filename()
                                  << ": bone name offset out of range\n";
                    }
                } else {
                    ++boneNameOk;
                }
            } else {
                ++boneNameBad;
                std::cerr << "[BONE] " << entry.path().filename()
                          << ": bone array size " << boneNames->size()
                          << " != rawBoneCount " << rawBoneCount << "\n";
            }
        } else if (rawBoneCount == 0) {
            ++boneNameOk;
        } else {
            ++boneNameBad;
            std::cerr << "[BONE] " << entry.path().filename()
                      << ": sdBoneNames not found or not array (rawBoneCount=" << rawBoneCount << ")\n";
        }

        // ── Translation descriptors ─────────────────────────────────────
        auto* translDescs = root.field("sdTranslDescs");
        if (translDescs && translDescs->isArray()) {
            if (translDescs->size() == rawBoneCount) {
                u32 rawTranslDescOff = readU32(data, blkBase + 0x0A4);
                if (rawBoneCount > 0 && rawTranslDescOff > 0) {
                    size_t descFileOff = rawTranslDescOff + 16;
                    if (descFileOff + 24 <= data.size()) {
                        u32 rawKfCount = readU32(data, descFileOff);
                        auto* firstDesc = translDescs->at(0);
                        if (firstDesc) {
                            auto* kfField = firstDesc->field("dwKeyframeCount");
                            if (kfField && kfField->isInt()) {
                                if (static_cast<u32>(kfField->asInt()) == rawKfCount)
                                    ++translDescOk;
                                else {
                                    ++translDescBad;
                                    std::cerr << "[TRDESC] " << entry.path().filename()
                                              << ": kfCount mismatch " << kfField->asInt()
                                              << " != " << rawKfCount << "\n";
                                }
                            } else {
                                ++translDescBad;
                                std::cerr << "[TRDESC] " << entry.path().filename()
                                          << ": dwKeyframeCount not found or not int\n";
                            }
                        } else { ++translDescOk; }
                    } else { ++translDescOk; }
                } else { ++translDescOk; }
            } else {
                ++translDescBad;
                std::cerr << "[TRDESC] " << entry.path().filename()
                          << ": array size " << translDescs->size()
                          << " != rawBoneCount " << rawBoneCount << "\n";
            }
        } else if (rawBoneCount == 0) {
            ++translDescOk;
        } else {
            ++translDescBad;
            std::cerr << "[TRDESC] " << entry.path().filename()
                      << ": sdTranslDescs not found or not array\n";
        }

        // ── Validate translation keyframes (spot-check first bone) ──────
        if (translDescs && translDescs->isArray() && translDescs->size() > 0) {
            auto* firstDesc = translDescs->at(0);
            if (firstDesc) {
                auto* kfs = firstDesc->field("sdKeyframes");
                u32 rawTranslDescOff = readU32(data, blkBase + 0x0A4);
                if (rawTranslDescOff > 0 && rawBoneCount > 0) {
                    size_t descFileOff = rawTranslDescOff + 16;
                    u32 rawKfCount = readU32(data, descFileOff);
                    // D3 var array: offset(4), size(4), pad(8)
                    u32 rawKfOffset = readU32(data, descFileOff + 4);

                    if (kfs && kfs->isArray() && rawKfCount > 0) {
                        if (kfs->size() == rawKfCount) {
                            size_t kfFileOff = rawKfOffset + 16;
                            if (kfFileOff + 16 <= data.size()) {
                                u32 rawFrame = readU32(data, kfFileOff);
                                auto* firstKf = kfs->at(0);
                                if (firstKf) {
                                    auto* frameField = firstKf->field("dwFrame");
                                    if (frameField && frameField->isInt() &&
                                        static_cast<u32>(frameField->asInt()) == rawFrame) {
                                        ++translKfOk;
                                    } else {
                                        ++translKfBad;
                                        std::cerr << "[TRKF] " << entry.path().filename()
                                                  << ": frame mismatch\n";
                                    }
                                } else { ++translKfOk; }
                            } else { ++translKfOk; }
                        } else {
                            ++translKfBad;
                            std::cerr << "[TRKF] " << entry.path().filename()
                                      << ": kf array size " << kfs->size()
                                      << " != rawKfCount " << rawKfCount << "\n";
                        }
                    } else if (rawKfCount == 0) {
                        ++translKfOk;
                    } else {
                        ++translKfBad;
                        std::cerr << "[TRKF] " << entry.path().filename()
                                  << ": sdKeyframes not found/not array (rawKfCount=" << rawKfCount << ")\n";
                    }
                } else { ++translKfOk; }
            } else { ++translKfOk; }
        }

        // ── Rotation descriptors ────────────────────────────────────────
        auto* rotDescs = root.field("sdRotDescs");
        if (rotDescs && rotDescs->isArray()) {
            if (rotDescs->size() == rawBoneCount) {
                u32 rawRotDescOff = readU32(data, blkBase + 0x0B8);
                if (rawBoneCount > 0 && rawRotDescOff > 0) {
                    size_t descFileOff = rawRotDescOff + 16;
                    if (descFileOff + 24 <= data.size()) {
                        u32 rawKfCount = readU32(data, descFileOff);
                        auto* firstDesc = rotDescs->at(0);
                        if (firstDesc) {
                            auto* kfField = firstDesc->field("dwKeyframeCount");
                            if (kfField && kfField->isInt()) {
                                if (static_cast<u32>(kfField->asInt()) == rawKfCount)
                                    ++rotDescOk;
                                else {
                                    ++rotDescBad;
                                    std::cerr << "[RTDESC] " << entry.path().filename()
                                              << ": kfCount mismatch " << kfField->asInt()
                                              << " != " << rawKfCount << "\n";
                                }
                            } else {
                                ++rotDescBad;
                                std::cerr << "[RTDESC] " << entry.path().filename()
                                          << ": dwKeyframeCount not found or not int\n";
                            }
                        } else { ++rotDescOk; }
                    } else { ++rotDescOk; }
                } else { ++rotDescOk; }
            } else {
                ++rotDescBad;
                std::cerr << "[RTDESC] " << entry.path().filename()
                          << ": array size " << rotDescs->size()
                          << " != rawBoneCount " << rawBoneCount << "\n";
            }
        } else if (rawBoneCount == 0) {
            ++rotDescOk;
        } else {
            ++rotDescBad;
            std::cerr << "[RTDESC] " << entry.path().filename()
                      << ": sdRotDescs not found or not array\n";
        }

        // ── Validate rotation keyframes (spot-check first bone) ─────────
        if (rotDescs && rotDescs->isArray() && rotDescs->size() > 0) {
            auto* firstDesc = rotDescs->at(0);
            if (firstDesc) {
                auto* kfs = firstDesc->field("sdKeyframes");
                u32 rawRotDescOff = readU32(data, blkBase + 0x0B8);
                if (rawRotDescOff > 0 && rawBoneCount > 0) {
                    size_t descFileOff = rawRotDescOff + 16;
                    u32 rawKfCount = readU32(data, descFileOff);
                    u32 rawKfOffset = readU32(data, descFileOff + 4);

                    if (kfs && kfs->isArray() && rawKfCount > 0) {
                        if (kfs->size() == rawKfCount) {
                            size_t kfFileOff = rawKfOffset + 16;
                            if (kfFileOff + 12 <= data.size()) {
                                u32 rawFrame = readU32(data, kfFileOff);
                                i16 rawQx = readI16(data, kfFileOff + 4);
                                auto* firstKf = kfs->at(0);
                                if (firstKf) {
                                    auto* frameField = firstKf->field("dwFrame");
                                    if (frameField && frameField->isInt() &&
                                        static_cast<u32>(frameField->asInt()) == rawFrame) {
                                        auto* qxField = firstKf->field("wQx");
                                        if (qxField && (qxField->isUint() || qxField->isInt())) {
                                            u16 parsedQx = qxField->isUint()
                                                ? static_cast<u16>(qxField->asUint())
                                                : static_cast<u16>(qxField->asInt());
                                            u16 rawQxU = static_cast<u16>(rawQx);
                                            if (parsedQx == rawQxU)
                                                ++rotKfOk;
                                            else {
                                                ++rotKfBad;
                                                std::cerr << "[RTKF] " << entry.path().filename()
                                                          << ": qx mismatch " << parsedQx
                                                          << " != " << rawQxU << "\n";
                                            }
                                        } else { ++rotKfOk; }
                                    } else {
                                        ++rotKfBad;
                                        std::cerr << "[RTKF] " << entry.path().filename()
                                                  << ": frame mismatch\n";
                                    }
                                } else { ++rotKfOk; }
                            } else { ++rotKfOk; }
                        } else {
                            ++rotKfBad;
                            std::cerr << "[RTKF] " << entry.path().filename()
                                      << ": kf array size " << kfs->size()
                                      << " != rawKfCount " << rawKfCount << "\n";
                        }
                    } else if (rawKfCount == 0) {
                        ++rotKfOk;
                    } else {
                        ++rotKfBad;
                        std::cerr << "[RTKF] " << entry.path().filename()
                                  << ": sdKeyframes not found/not array\n";
                    }
                } else { ++rotKfOk; }
            } else { ++rotKfOk; }
        }

        // ── Scale descriptors ───────────────────────────────────────────
        auto* scaleDescs = root.field("sdScaleDescs");
        if (scaleDescs && scaleDescs->isArray()) {
            if (scaleDescs->size() == rawBoneCount) {
                u32 rawScaleDescOff = readU32(data, blkBase + 0x0C8);
                if (rawBoneCount > 0 && rawScaleDescOff > 0) {
                    size_t descFileOff = rawScaleDescOff + 16;
                    if (descFileOff + 24 <= data.size()) {
                        u32 rawKfCount = readU32(data, descFileOff);
                        auto* firstDesc = scaleDescs->at(0);
                        if (firstDesc) {
                            auto* kfField = firstDesc->field("dwKeyframeCount");
                            if (kfField && kfField->isInt()) {
                                if (static_cast<u32>(kfField->asInt()) == rawKfCount)
                                    ++scaleDescOk;
                                else {
                                    ++scaleDescBad;
                                    std::cerr << "[SCDESC] " << entry.path().filename()
                                              << ": kfCount mismatch " << kfField->asInt()
                                              << " != " << rawKfCount << "\n";
                                }
                            } else {
                                ++scaleDescBad;
                                std::cerr << "[SCDESC] " << entry.path().filename()
                                          << ": dwKeyframeCount not found or not int\n";
                            }
                        } else { ++scaleDescOk; }
                    } else { ++scaleDescOk; }
                } else { ++scaleDescOk; }
            } else {
                ++scaleDescBad;
                std::cerr << "[SCDESC] " << entry.path().filename()
                          << ": array size " << scaleDescs->size()
                          << " != rawBoneCount " << rawBoneCount << "\n";
            }
        } else if (rawBoneCount == 0) {
            ++scaleDescOk;
        } else {
            ++scaleDescBad;
            std::cerr << "[SCDESC] " << entry.path().filename()
                      << ": sdScaleDescs not found or not array\n";
        }

        // ── Validate scale keyframes (spot-check first bone) ────────────
        if (scaleDescs && scaleDescs->isArray() && scaleDescs->size() > 0) {
            auto* firstDesc = scaleDescs->at(0);
            if (firstDesc) {
                auto* kfs = firstDesc->field("sdKeyframes");
                u32 rawScaleDescOff = readU32(data, blkBase + 0x0C8);
                if (rawScaleDescOff > 0 && rawBoneCount > 0) {
                    size_t descFileOff = rawScaleDescOff + 16;
                    u32 rawKfCount = readU32(data, descFileOff);
                    u32 rawKfOffset = readU32(data, descFileOff + 4);

                    if (kfs && kfs->isArray() && rawKfCount > 0) {
                        if (kfs->size() == rawKfCount) {
                            size_t kfFileOff = rawKfOffset + 16;
                            if (kfFileOff + 8 <= data.size()) {
                                u32 rawFrame = readU32(data, kfFileOff);
                                auto* firstKf = kfs->at(0);
                                if (firstKf) {
                                    auto* frameField = firstKf->field("dwFrame");
                                    if (frameField && frameField->isInt() &&
                                        static_cast<u32>(frameField->asInt()) == rawFrame) {
                                        ++scaleKfOk;
                                    } else {
                                        ++scaleKfBad;
                                        std::cerr << "[SCKF] " << entry.path().filename()
                                                  << ": frame mismatch\n";
                                    }
                                } else { ++scaleKfOk; }
                            } else { ++scaleKfOk; }
                        } else {
                            ++scaleKfBad;
                            std::cerr << "[SCKF] " << entry.path().filename()
                                      << ": kf array size " << kfs->size()
                                      << " != rawKfCount " << rawKfCount << "\n";
                        }
                    } else if (rawKfCount == 0) {
                        ++scaleKfOk;
                    } else {
                        ++scaleKfBad;
                        std::cerr << "[SCKF] " << entry.path().filename()
                                  << ": sdKeyframes not found/not array\n";
                    }
                } else { ++scaleKfOk; }
            } else { ++scaleKfOk; }
        }

        // ── Look entries ────────────────────────────────────────────────
        u32 rawLookCount = readU32(data, blkBase + 0x124);
        auto* lookEntries = root.field("sdLookEntries");
        if (lookEntries && lookEntries->isArray()) {
            if (lookEntries->size() == rawLookCount)
                ++lookOk;
            else {
                ++lookBad;
                std::cerr << "[LOOK] " << entry.path().filename()
                          << ": array size " << lookEntries->size()
                          << " != rawLookCount " << rawLookCount << "\n";
            }
        } else if (rawLookCount == 0) {
            ++lookOk;
        } else {
            ++lookBad;
            std::cerr << "[LOOK] " << entry.path().filename()
                      << ": sdLookEntries not found or not array\n";
        }

        // ── Per-frame validation ────────────────────────────────────────
        u32 rawFrameCount = readU32(data, blkBase + 0x0A0);
        auto* perFrame1 = root.field("sdPerFrame1");
        auto* perFrame2 = root.field("sdPerFrame2");
        bool pfGood = true;
        if (perFrame1 && perFrame1->isArray()) {
            if (perFrame1->size() != rawFrameCount && rawFrameCount > 0) {
                pfGood = false;
                std::cerr << "[PF1] " << entry.path().filename()
                          << ": array size " << perFrame1->size()
                          << " != rawFrameCount " << rawFrameCount << "\n";
            }
        }
        if (perFrame2 && perFrame2->isArray()) {
            if (perFrame2->size() != rawFrameCount && rawFrameCount > 0) {
                pfGood = false;
                std::cerr << "[PF2] " << entry.path().filename()
                          << ": array size " << perFrame2->size()
                          << " != rawFrameCount " << rawFrameCount << "\n";
            }
        }
        if (pfGood) ++perFrameOk; else ++perFrameBad;
    }

    // ── Summary ─────────────────────────────────────────────────────────
    std::cout << "\n=== D3 ANI Corpus Test Summary ===\n";
    std::cout << "Total files:          " << totalFiles << "\n";
    std::cout << "Parse OK:             " << parseOk << "\n";
    std::cout << "Parse Failed:         " << parseFailed << "\n";
    std::cout << "Header OK:            " << headerOk << "\n";
    std::cout << "Header Bad:           " << headerBad << "\n";
    std::cout << "Bone Names OK:        " << boneNameOk << "\n";
    std::cout << "Bone Names Bad:       " << boneNameBad << "\n";
    std::cout << "Transl Desc OK:       " << translDescOk << "\n";
    std::cout << "Transl Desc Bad:      " << translDescBad << "\n";
    std::cout << "Rot Desc OK:          " << rotDescOk << "\n";
    std::cout << "Rot Desc Bad:         " << rotDescBad << "\n";
    std::cout << "Scale Desc OK:        " << scaleDescOk << "\n";
    std::cout << "Scale Desc Bad:       " << scaleDescBad << "\n";
    std::cout << "Transl Keyframes OK:  " << translKfOk << "\n";
    std::cout << "Transl Keyframes Bad: " << translKfBad << "\n";
    std::cout << "Rot Keyframes OK:     " << rotKfOk << "\n";
    std::cout << "Rot Keyframes Bad:    " << rotKfBad << "\n";
    std::cout << "Scale Keyframes OK:   " << scaleKfOk << "\n";
    std::cout << "Scale Keyframes Bad:  " << scaleKfBad << "\n";
    std::cout << "Look Entries OK:      " << lookOk << "\n";
    std::cout << "Look Entries Bad:     " << lookBad << "\n";
    std::cout << "Per-Frame OK:         " << perFrameOk << "\n";
    std::cout << "Per-Frame Bad:        " << perFrameBad << "\n";

    size_t totalBad = parseFailed + headerBad + boneNameBad +
                      translDescBad + rotDescBad + scaleDescBad +
                      translKfBad + rotKfBad + scaleKfBad +
                      lookBad + perFrameBad;
    std::cout << "\nTotal issues: " << totalBad << "\n";

    CHECK(totalBad == 0);
}
