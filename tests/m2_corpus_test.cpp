// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// M2 corpus validation test: iterates all .m2 files in a directory,
// parses each, and reports success/failure/issue statistics.
// Optionally performs round-trip (parse → write → re-parse) validation.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <vector>
#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off — dbghelp.h needs windows.h first
#include <windows.h>
#include <dbghelp.h>
#include <eh.h>
// clang-format on
#pragma comment(lib, "dbghelp.lib")

static std::string captureStackTrace(EXCEPTION_POINTERS* ep) {
    std::string result;
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame = {};
#ifdef _M_X64
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#else
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    char symbolBuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    for (int i = 0; i < 30; ++i) {
        if (!StackWalk64(machineType, process, thread, &frame, &ctx, NULL, SymFunctionTableAccess64,
                         SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0)
            break;

        DWORD64 displacement64 = 0;
        DWORD displacement32 = 0;
        result += "  [" + std::to_string(i) + "] ";
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement64, symbol)) {
            result += symbol->Name;
            if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement32, &line)) {
                result += " (";
                result += line.FileName;
                result += ":";
                result += std::to_string(line.LineNumber);
                result += ")";
            }
        } else {
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "0x%llX", (unsigned long long)frame.AddrPC.Offset);
            result += addrBuf;
        }
        result += "\n";
    }
    return result;
}

// Custom exception class wrapping a Windows SEH exception with stack trace
class SehException : public std::exception {
public:
    unsigned int sehCode;
    std::string trace;
    std::string msg;

    SehException(unsigned int code, EXCEPTION_POINTERS* ep) : sehCode(code) {
        trace = captureStackTrace(ep);
        char buf[64];
        snprintf(buf, sizeof(buf), "SEH exception 0x%08X", code);
        msg = buf;
        if (!trace.empty()) {
            msg += "\nStack trace:\n" + trace;
        }
    }

    const char* what() const noexcept override {
        return msg.c_str();
    }
};

// Translator function: converts SEH to throwable C++ exception.
// Requires /EHa compiler flag.
static void sehTranslator(unsigned int code, EXCEPTION_POINTERS* ep) {
    throw SehException(code, ep);
}
#endif

namespace fs = std::filesystem;

struct FileResult {
    std::string filename;
    bool parsed = false;
    bool roundTripped = false;
    std::vector<std::string> parseIssues;
    std::vector<std::string> roundTripIssues;
    std::string error;

    // Model stats
    size_t bones = 0;
    size_t sequences = 0;
    size_t vertices = 0;
    size_t textures = 0;
    size_t materials = 0;
    size_t lights = 0;
    size_t cameras = 0;
    size_t attachments = 0;
    size_t events = 0;
    size_t particles = 0;
    size_t ribbons = 0;
    size_t skinProfiles = 0;
};

TEST_CASE("M2 corpus parse", "[m2][corpus]") {
#ifdef _WIN32
    _set_se_translator(sehTranslator);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#endif

    // Corpus directory: M2_CORPUS_DIR env var, else auto-discover
    std::string corpusDir;
    if (const char* env = std::getenv("M2_CORPUS_DIR"); env && fs::is_directory(env)) {
        corpusDir = env;
    }
    if (corpusDir.empty()) {
        for (auto candidate : {"Corpus/M2", "../Corpus/M2", "../../Corpus/M2"}) {
            if (fs::is_directory(candidate)) {
                corpusDir = candidate;
                break;
            }
        }
    }
    if (corpusDir.empty())
        SKIP("M2 corpus not found");

    bool doRoundTrip = std::getenv("M2_CORPUS_ROUNDTRIP") != nullptr;
    bool verbose = std::getenv("M2_CORPUS_VERBOSE") != nullptr;
    bool stopOnError = false;

    // Collect all .m2 files (extension match is case-insensitive: old
    // installs mix ".m2" and ".M2")
    std::vector<fs::path> m2Files;
    for (const auto& entry : fs::recursive_directory_iterator(corpusDir)) {
        if (!entry.is_regular_file())
            continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".m2") {
            m2Files.push_back(entry.path());
        }
    }
    std::sort(m2Files.begin(), m2Files.end());

    std::cout << "=== M2 Corpus Validation ===" << std::endl;
    std::cout << "Directory: " << corpusDir << std::endl;
    std::cout << "M2 files found: " << m2Files.size() << std::endl;
    std::cout << "Round-trip: " << (doRoundTrip ? "yes" : "no") << std::endl;
    std::cout << std::endl;

    std::vector<FileResult> results;
    results.reserve(m2Files.size());

    size_t parseOk = 0, parseFail = 0, issueFiles = 0;
    size_t rtOk = 0, rtFail = 0;
    std::map<std::string, int> issueCounts;

    auto startTime = std::chrono::steady_clock::now();

    for (size_t idx = 0; idx < m2Files.size(); ++idx) {
        const auto& m2Path = m2Files[idx];
        FileResult result;
        result.filename = m2Path.filename().string();

        if (verbose || (idx % 500 == 0 && idx > 0)) {
            std::cout << "\r[" << idx << "/" << m2Files.size() << "] " << std::flush;
        }

        try {
            fs::path absPath = fs::absolute(m2Path);
            whiteout::utils::OsFileSystem vfs(absPath.parent_path().string());
            whiteout::m2::Parser parser;

            whiteout::m2::Model model = parser.parse(vfs, absPath.string());

            const auto& issues = parser.getIssues();
            result.parsed = true;
            result.parseIssues.assign(issues.begin(), issues.end());
            result.bones = model.bones.size();
            result.sequences = model.sequences.size();
            result.vertices = model.vertices.size();
            result.textures = model.textures.size();
            result.materials = model.materials.size();
            result.lights = model.lights.size();
            result.cameras = model.cameras.size();
            result.attachments = model.attachments.size();
            result.events = model.events.size();
            result.particles = model.particleEmitters.size();
            result.ribbons = model.ribbonEmitters.size();
            result.skinProfiles = model.numSkinProfiles;

            if (issues.empty()) {
                parseOk++;
            } else {
                issueFiles++;
                parseOk++; // Still parsed, just with warnings
                for (const auto& issue : issues) {
                    // Bucket by first 60 chars to group similar issues
                    std::string key = issue.substr(0, std::min(issue.size(), size_t(80)));
                    issueCounts[key]++;
                }
            }

            if (verbose && !issues.empty()) {
                std::cout << "\n  [WARN] " << result.filename << ": " << issues.size()
                          << " issue(s)" << std::endl;
                for (const auto& issue : issues) {
                    std::cout << "    - " << issue << std::endl;
                }
            }

            // Round-trip test
            if (doRoundTrip) {
                try {
                    fs::path tempDir = fs::temp_directory_path() / "m2_corpus_test";
                    fs::create_directories(tempDir);
                    fs::path outPath = tempDir / m2Path.filename();

                    whiteout::utils::OsFileSystem outVfs(tempDir.string());
                    whiteout::m2::Writer writer;
                    writer.write(outVfs, outPath.string(), model);

                    // Re-parse the written file
                    whiteout::utils::OsFileSystem reVfs(tempDir.string());
                    whiteout::m2::Parser reParser;
                    whiteout::m2::Model reModel = reParser.parse(reVfs, outPath.string());

                    const auto& reIssues = reParser.getIssues();
                    result.roundTripped = true;
                    result.roundTripIssues.assign(reIssues.begin(), reIssues.end());

                    // Basic comparison
                    bool match = (model.bones.size() == reModel.bones.size() &&
                                  model.sequences.size() == reModel.sequences.size() &&
                                  model.vertices.size() == reModel.vertices.size() &&
                                  model.textures.size() == reModel.textures.size());

                    if (match && reIssues.empty()) {
                        rtOk++;
                    } else {
                        rtFail++;
                        if (verbose) {
                            std::cout << "\n  [RT-FAIL] " << result.filename << std::endl;
                            if (!match) {
                                std::cout << "    Mismatch: bones " << model.bones.size() << "/"
                                          << reModel.bones.size() << " seqs "
                                          << model.sequences.size() << "/"
                                          << reModel.sequences.size() << " verts "
                                          << model.vertices.size() << "/" << reModel.vertices.size()
                                          << std::endl;
                            }
                        }
                    }

                    fs::remove(outPath);
                } catch (const std::exception& e) {
                    rtFail++;
                    result.roundTripIssues.push_back(std::string("Exception: ") + e.what());
                    if (verbose) {
                        std::cout << "\n  [RT-ERR] " << result.filename << ": " << e.what()
                                  << std::endl;
                    }
                }
            }

        } catch (const std::exception& e) {
            parseFail++;
            result.error = e.what();
            std::cerr << "\n[ERROR] " << result.filename << ": " << e.what() << std::endl;
            if (stopOnError) {
                std::cerr << "Stopping on first error (--stop-on-error)." << std::endl;
                break;
            }
        }

        results.push_back(std::move(result));
    }

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    // Print summary
    std::cout << "\r                                        " << std::endl;
    std::cout << "\n=== Summary ===" << std::endl;
    std::cout << "Total files:     " << m2Files.size() << std::endl;
    std::cout << "Parse OK:        " << parseOk << " (" << std::fixed << std::setprecision(1)
              << (100.0 * parseOk / m2Files.size()) << "%)" << std::endl;
    std::cout << "Parse FAIL:      " << parseFail << std::endl;
    std::cout << "Files w/ issues: " << issueFiles << std::endl;
    std::cout << "Time:            " << elapsed.count() << " ms (" << std::fixed
              << std::setprecision(1) << (1000.0 * m2Files.size() / elapsed.count())
              << " files/sec)" << std::endl;

    if (doRoundTrip) {
        std::cout << "\n--- Round-Trip ---" << std::endl;
        std::cout << "RT OK:    " << rtOk << std::endl;
        std::cout << "RT FAIL:  " << rtFail << std::endl;
    }

    if (!issueCounts.empty()) {
        std::cout << "\n--- Top Parse Issues ---" << std::endl;
        // Sort by frequency
        std::vector<std::pair<std::string, int>> sorted(issueCounts.begin(), issueCounts.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < std::min(sorted.size(), size_t(20)); ++i) {
            std::cout << "  [" << sorted[i].second << "x] " << sorted[i].first << std::endl;
        }
    }

    // Print failed files
    if (parseFail > 0) {
        std::cout << "\n--- Failed Files ---" << std::endl;
        for (const auto& r : results) {
            if (!r.parsed && !r.error.empty()) {
                std::cout << "  " << r.filename << ": " << r.error << std::endl;
            }
        }
    }

    // Statistics
    size_t totalBones = 0, totalSeqs = 0, totalVerts = 0, totalTex = 0, totalMats = 0;
    size_t totalLights = 0, totalCams = 0, totalAttach = 0, totalEvents = 0;
    size_t totalParticles = 0, totalRibbons = 0;
    for (const auto& r : results) {
        if (!r.parsed)
            continue;
        totalBones += r.bones;
        totalSeqs += r.sequences;
        totalVerts += r.vertices;
        totalTex += r.textures;
        totalMats += r.materials;
        totalLights += r.lights;
        totalCams += r.cameras;
        totalAttach += r.attachments;
        totalEvents += r.events;
        totalParticles += r.particles;
        totalRibbons += r.ribbons;
    }

    std::cout << "\n--- Aggregate Statistics ---" << std::endl;
    std::cout << "  Total bones:       " << totalBones << std::endl;
    std::cout << "  Total sequences:   " << totalSeqs << std::endl;
    std::cout << "  Total vertices:    " << totalVerts << std::endl;
    std::cout << "  Total textures:    " << totalTex << std::endl;
    std::cout << "  Total materials:   " << totalMats << std::endl;
    std::cout << "  Total lights:      " << totalLights << std::endl;
    std::cout << "  Total cameras:     " << totalCams << std::endl;
    std::cout << "  Total attachments: " << totalAttach << std::endl;
    std::cout << "  Total events:      " << totalEvents << std::endl;
    std::cout << "  Total particles:   " << totalParticles << std::endl;
    std::cout << "  Total ribbons:     " << totalRibbons << std::endl;

#ifdef _WIN32
    SymCleanup(GetCurrentProcess());
#endif

    CHECK(parseFail == 0);
}
