// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_writer_example.cpp
/// @brief Demonstrates CASC storage creation, modification, and persistence.

#include <whiteout/storages/casc/storage.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

static int s_pass = 0;
static int s_fail = 0;

static void check(bool cond, const char* msg) {
    if (cond) {
        std::printf("  PASS: %s\n", msg);
        ++s_pass;
    } else {
        std::printf("  FAIL: %s\n", msg);
        ++s_fail;
    }
}

static void cleanDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    std::printf("=== CASC Writer Example ===\n");

    const std::string testDir = "test_casc_writer_example";
    cleanDir(testDir);

    // -----------------------------------------------------------------------
    // Demo 1: Create a new CASC storage from scratch.
    // -----------------------------------------------------------------------

    std::printf("\n[Demo 1: Create new storage]\n");
    {
        CreateOptions opts;
        opts.product = "myproduct";
        opts.version = "1.0.0";
        auto storage = Storage::create(opts);
        check(static_cast<bool>(storage), "Storage::create() succeeds");

        // Write some files.
        std::string text = "Hello, CASC world!";
        std::vector<u8> textData(text.begin(), text.end());
        check(storage.writeFile("greeting.txt", textData), "write greeting.txt");

        std::vector<u8> binaryData(256);
        for (int i = 0; i < 256; ++i) binaryData[i] = static_cast<u8>(i);
        check(storage.writeFile("data/binary.bin", binaryData), "write binary.bin");

        // Read back from overlay (before save).
        auto readBack = storage.readFile("greeting.txt");
        check(readBack.has_value() && *readBack == textData, "read from overlay works");

        // Save.
        check(storage.save(testDir), "save to disk");
        std::printf("  Saved storage to: %s\n", testDir.c_str());
    }

    // -----------------------------------------------------------------------
    // Demo 2: Reopen and verify.
    // -----------------------------------------------------------------------

    std::printf("\n[Demo 2: Reopen and verify]\n");
    {
        auto storage = Storage::open(testDir);
        check(storage.has_value(), "reopen saved storage");

        if (storage) {
            auto files = storage->listFiles();
            std::printf("  Files in storage: %zu\n", files.size());
            for (auto& f : files)
                std::printf("    %s\n", f.c_str());

            auto greeting = storage->readFile("greeting.txt");
            check(greeting.has_value(), "read greeting.txt after reopen");
            if (greeting) {
                std::string text(greeting->begin(), greeting->end());
                std::printf("  greeting.txt content: \"%s\"\n", text.c_str());
                check(text == "Hello, CASC world!", "greeting.txt content matches");
            }

            auto binary = storage->readFile("data/binary.bin");
            check(binary.has_value() && binary->size() == 256, "read binary.bin after reopen");
        }
    }

    // -----------------------------------------------------------------------
    // Demo 3: Modify existing storage.
    // -----------------------------------------------------------------------

    std::printf("\n[Demo 3: Modify existing storage]\n");
    {
        auto storage = Storage::open(testDir);
        check(storage.has_value(), "open for modification");

        if (storage) {
            // Add a new file.
            std::string newText = "Added in demo 3";
            std::vector<u8> newData(newText.begin(), newText.end());
            check(storage->writeFile("added.txt", newData), "write added.txt");

            // Replace an existing file.
            std::string updatedText = "Updated greeting!";
            std::vector<u8> updatedData(updatedText.begin(), updatedText.end());
            check(storage->writeFile("greeting.txt", updatedData), "replace greeting.txt");

            // Delete a file.
            check(storage->deleteFile("data/binary.bin"), "delete binary.bin");

            // Save modifications.
            check(storage->save(testDir), "save modifications in-place");
        }
    }

    // -----------------------------------------------------------------------
    // Demo 4: Verify modifications.
    // -----------------------------------------------------------------------

    std::printf("\n[Demo 4: Verify modifications]\n");
    {
        auto storage = Storage::open(testDir);
        check(storage.has_value(), "reopen after modifications");

        if (storage) {
            auto files = storage->listFiles();
            std::printf("  Files after modification: %zu\n", files.size());
            for (auto& f : files)
                std::printf("    %s\n", f.c_str());

            // Greeting should be updated.
            auto greeting = storage->readFile("greeting.txt");
            check(greeting.has_value(), "read updated greeting.txt");
            if (greeting) {
                std::string text(greeting->begin(), greeting->end());
                check(text == "Updated greeting!", "greeting.txt updated correctly");
            }

            // binary.bin should be gone.
            check(!storage->fileExists("data/binary.bin"), "binary.bin deleted");

            // added.txt should exist.
            auto added = storage->readFile("added.txt");
            check(added.has_value(), "added.txt exists");
            if (added) {
                std::string text(added->begin(), added->end());
                check(text == "Added in demo 3", "added.txt content correct");
            }
        }
    }

    cleanDir(testDir);

    std::printf("\n=== Results: %d passed, %d failed ===\n", s_pass, s_fail);
    return s_fail > 0 ? 1 : 0;
}
