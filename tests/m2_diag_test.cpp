// M2 diagnostic test — parses the file named by M2_DIAG_FILE and reports
// where an oversized allocation originates. Skips itself when the env var
// is absent so the suite stays green in CI.
#include <catch2/catch_all.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include <whiteout/models/m2/parser.h>
#include <whiteout/utils/os_file_system.h>

#ifdef _WIN32
// clang-format off — dbghelp.h needs windows.h first
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#pragma comment(lib, "dbghelp.lib")

static void printStackHere(const char* label) {
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    HANDLE process = GetCurrentProcess();

    char symbolBuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuf);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

    std::fprintf(stderr, "--- %s ---\n", label);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        DWORD64 disp64 = 0;
        DWORD disp32 = 0;
        std::fprintf(stderr, "  [%u] ", i);
        if (SymFromAddr(process, addr, &disp64, symbol)) {
            std::fprintf(stderr, "%s", symbol->Name);
            if (SymGetLineFromAddr64(process, addr, &disp32, &line)) {
                std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
            }
        } else {
            std::fprintf(stderr, "0x%llX", static_cast<unsigned long long>(addr));
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
}

static void onAllocFailure() {
    printStackHere("allocation failure");
    std::set_new_handler(nullptr); // let the retry throw bad_alloc normally
}
#endif

TEST_CASE("M2 diag single file", "[m2][diag]") {
    const char* path = std::getenv("M2_DIAG_FILE");
    if (!path) {
        SUCCEED("M2_DIAG_FILE not set");
        return;
    }

#ifdef _WIN32
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    std::set_new_handler(onAllocFailure);
#endif

    std::filesystem::path absPath = std::filesystem::absolute(path);
    REQUIRE(std::filesystem::exists(absPath));
    std::printf("file: %s (%llu bytes)\n", absPath.string().c_str(),
                static_cast<unsigned long long>(std::filesystem::file_size(absPath)));

    whiteout::utils::OsFileSystem vfs(absPath.parent_path().string());
    whiteout::m2::Parser parser;
    try {
        whiteout::m2::Model model = parser.parse(vfs, absPath.string());
        std::printf("parse OK: sequences=%zu bones=%zu vertices=%zu issues=%zu\n",
                    model.sequences.size(), model.bones.size(), model.vertices.size(),
                    parser.getIssues().size());
        for (const auto& issue : parser.getIssues()) {
            std::printf("  issue: %s\n", issue.c_str());
        }
    } catch (const std::exception& e) {
        FAIL("parse threw: " << e.what());
    }
}
