// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ functionality test suite.

#include <whiteout/storages/mpq/storage.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace mpq = whiteout::storages::mpq;

using whiteout::u8;

namespace {

std::vector<u8> makePatternData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>((i * 13 + (i / 17) * 7 + seed) & 0xFF);
    }
    return data;
}

std::vector<u8> makeCompressibleData(size_t size) {
    static constexpr char kPhrase[] = "whiteout-mpq-test-block-";
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<u8>(kPhrase[i % (sizeof(kPhrase) - 1)]);
    }
    return data;
}

bool fail(int testNumber, const char* fmt, ...) {
    std::printf("TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

bool pass(int testNumber, const char* fmt, ...) {
    std::printf("TEST %d PASS: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return true;
}

bool expect(bool condition, int testNumber, const char* fmt, ...) {
    if (condition) {
        return true;
    }

    std::printf("TEST %d FAIL: ", testNumber);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
    return false;
}

bool expectContents(mpq::Storage& storage, const std::string& name,
                    std::span<const u8> expected, int testNumber) {
    auto data = storage.readFile(name);
    if (!expect(data.has_value(), testNumber, "missing file: %s", name.c_str())) {
        return false;
    }
    if (!expect(data->size() == expected.size(), testNumber,
                "size mismatch for %s (expected %zu, got %zu)",
                name.c_str(), expected.size(), data->size())) {
        return false;
    }
    if (!expect(std::equal(data->begin(), data->end(), expected.begin(), expected.end()),
                testNumber, "content mismatch for %s", name.c_str())) {
        return false;
    }
    return true;
}

bool expectContents(mpq::Storage& storage, const std::string& name, whiteout::u16 locale,
                    std::span<const u8> expected, int testNumber) {
    auto data = storage.readFile(name, locale);
    if (!expect(data.has_value(), testNumber, "missing localized file: %s", name.c_str())) {
        return false;
    }
    if (!expect(data->size() == expected.size(), testNumber,
                "localized size mismatch for %s", name.c_str())) {
        return false;
    }
    if (!expect(std::equal(data->begin(), data->end(), expected.begin(), expected.end()),
                testNumber, "localized content mismatch for %s", name.c_str())) {
        return false;
    }
    return true;
}

void cleanup(const fs::path& tempDir) {
    std::error_code ec;
    fs::remove_all(tempDir, ec);
}

} // anonymous namespace

int main() {
    const std::vector<u8> helloV1{'h', 'e', 'l', 'l', 'o', '\n'};
    const std::vector<u8> helloV2{'g', 'o', 'o', 'd', 'b', 'y', 'e', '\n'};
    const std::vector<u8> singleUnitData = makePatternData(777, 0x31);
    const std::vector<u8> largeCompressed = makeCompressibleData(20000);
    const std::vector<u8> pkwareData = makeCompressibleData(900);
    const std::vector<u8> encryptedData = makePatternData(512, 0x77);
    const std::vector<u8> localizedData{'g', 'u', 't', 'e', 'n', ' ', 't', 'a', 'g', '\n'};

    fs::path tempDir = fs::temp_directory_path() / "whiteout_mpq_test";
    fs::path archivePath = tempDir / "functionality.mpq";
    cleanup(tempDir);
    fs::create_directories(tempDir);

    // Test 1: create() is valid, but save() without a path fails as expected.
    {
        auto storage = mpq::Storage::create();
        if (!expect(static_cast<bool>(storage), 1, "create() returned invalid storage")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(!storage.save(), 1, "save() without source path should fail")) {
            cleanup(tempDir);
            return 1;
        }
        pass(1, "create() valid, save() without path rejected");
    }

    // Test 2: overlay read/write, listFiles, fileInfo, missing-file diagnostics.
    {
        auto storage = mpq::Storage::create();

        mpq::WriteOptions singleUnitOpts;
        singleUnitOpts.compression = mpq::Compression::Zlib;
        singleUnitOpts.singleUnit = true;

        mpq::WriteOptions largeOpts;
        largeOpts.compression = mpq::Compression::Zlib;

        mpq::WriteOptions pkwareOpts;
        pkwareOpts.compression = mpq::Compression::PKware;

        mpq::WriteOptions localizedOpts;
        localizedOpts.locale = mpq::Locale::German;

        if (!expect(storage.writeFile("docs\\hello.txt", helloV1), 2, "failed to write hello.txt")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage.writeFile("data\\single.bin", singleUnitData, singleUnitOpts), 2,
                    "failed to write single-unit file")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage.writeFile("data\\big.bin", largeCompressed, largeOpts), 2,
                    "failed to write multi-sector file")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage.writeFile("data\\implode.bin", pkwareData, pkwareOpts), 2,
                    "failed to write PKware file")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage.writeFile("locale\\greeting.txt", localizedData, localizedOpts), 2,
                    "failed to write localized file")) {
            cleanup(tempDir);
            return 1;
        }

        if (!expectContents(storage, "docs\\hello.txt", helloV1, 2) ||
            !expectContents(storage, "data\\single.bin", singleUnitData, 2) ||
            !expectContents(storage, "data\\big.bin", largeCompressed, 2) ||
            !expectContents(storage, "data\\implode.bin", pkwareData, 2) ||
            !expectContents(storage, "locale\\greeting.txt", mpq::Locale::German, localizedData, 2)) {
            cleanup(tempDir);
            return 1;
        }

        std::string missingError;
        auto missing = storage.readFile("missing\\file.txt", &missingError);
        if (!expect(!missing.has_value(), 2, "missing file unexpectedly read")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(!missingError.empty(), 2, "missing file should populate an error message")) {
            cleanup(tempDir);
            return 1;
        }

        auto files = storage.listFiles();
        if (!expect(files.size() == 5, 2, "expected 5 overlay files, got %zu", files.size())) {
            cleanup(tempDir);
            return 1;
        }

        auto info = storage.fileInfo("docs\\hello.txt");
        if (!expect(info.has_value(), 2, "fileInfo missing for hello.txt") ||
            !expect(info->uncompressedSize == helloV1.size(), 2, "unexpected hello.txt size in fileInfo")) {
            cleanup(tempDir);
            return 1;
        }

        pass(2, "overlay read/write/list/fileInfo/missing-file checks passed");
    }

    // Test 3: save(path) and reopen preserve plain, compressed, PKware, single-unit, and localized files.
    {
        auto storage = mpq::Storage::create();

        mpq::WriteOptions singleUnitOpts;
        singleUnitOpts.compression = mpq::Compression::Zlib;
        singleUnitOpts.singleUnit = true;

        mpq::WriteOptions largeOpts;
        largeOpts.compression = mpq::Compression::Zlib;

        mpq::WriteOptions pkwareOpts;
        pkwareOpts.compression = mpq::Compression::PKware;

        mpq::WriteOptions localizedOpts;
        localizedOpts.locale = mpq::Locale::German;

        storage.writeFile("docs\\hello.txt", helloV1);
        storage.writeFile("data\\single.bin", singleUnitData, singleUnitOpts);
        storage.writeFile("data\\big.bin", largeCompressed, largeOpts);
        storage.writeFile("data\\implode.bin", pkwareData, pkwareOpts);
        storage.writeFile("locale\\greeting.txt", localizedData, localizedOpts);

        if (!expect(storage.save(archivePath.string()), 3, "save(path) failed")) {
            cleanup(tempDir);
            return 1;
        }

        std::string openError;
        auto reopened = mpq::Storage::open(archivePath.string(), &openError);
        if (!expect(reopened.has_value(), 3, "reopen failed: %s", openError.c_str())) {
            cleanup(tempDir);
            return 1;
        }

        if (!expectContents(*reopened, "docs\\hello.txt", helloV1, 3) ||
            !expectContents(*reopened, "data\\single.bin", singleUnitData, 3) ||
            !expectContents(*reopened, "data\\big.bin", largeCompressed, 3) ||
            !expectContents(*reopened, "data\\implode.bin", pkwareData, 3) ||
            !expectContents(*reopened, "locale\\greeting.txt", mpq::Locale::German, localizedData, 3)) {
            cleanup(tempDir);
            return 1;
        }

        auto archiveInfo = reopened->archiveInfo();
        if (!expect(archiveInfo.sectorSize == 4096, 3, "unexpected sector size: %u", archiveInfo.sectorSize)) {
            cleanup(tempDir);
            return 1;
        }

        pass(3, "save(path)+reopen preserved multiple file modes");
    }

    // Test 4: overwrite save() on same path persists overwrite, delete, and encrypted writes.
    {
        std::string openError;
        auto storage = mpq::Storage::open(archivePath.string(), &openError);
        if (!expect(storage.has_value(), 4, "open before overwrite failed: %s", openError.c_str())) {
            cleanup(tempDir);
            return 1;
        }

        mpq::WriteOptions encryptedOpts;
        encryptedOpts.encrypt = true;
        encryptedOpts.singleUnit = true;

        if (!expect(storage->writeFile("docs\\hello.txt", helloV2), 4, "overwrite hello.txt failed")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage->deleteFile("data\\implode.bin"), 4, "delete implode.bin failed")) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(storage->writeFile("secure\\secret.bin", encryptedData, encryptedOpts), 4,
                    "encrypted write failed")) {
            cleanup(tempDir);
            return 1;
        }

        if (!expectContents(*storage, "secure\\secret.bin", encryptedData, 4)) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(!storage->fileExists("data\\implode.bin"), 4, "deleted file still visible before save")) {
            cleanup(tempDir);
            return 1;
        }

        if (!expect(storage->save(), 4, "overwrite save() failed")) {
            cleanup(tempDir);
            return 1;
        }

        std::string reopenError;
        auto reopened = mpq::Storage::open(archivePath.string(), &reopenError);
        if (!expect(reopened.has_value(), 4, "reopen after overwrite failed: %s", reopenError.c_str())) {
            cleanup(tempDir);
            return 1;
        }

        if (!expectContents(*reopened, "docs\\hello.txt", helloV2, 4) ||
            !expectContents(*reopened, "secure\\secret.bin", encryptedData, 4)) {
            cleanup(tempDir);
            return 1;
        }
        if (!expect(!reopened->fileExists("data\\implode.bin"), 4, "deleted file reappeared after reopen")) {
            cleanup(tempDir);
            return 1;
        }

        pass(4, "same-path overwrite save preserved overwrite/delete/encrypted data");
    }

    // Test 5: close() invalidates storage.
    {
        auto storage = mpq::Storage::create();
        storage.close();
        if (!expect(!static_cast<bool>(storage), 5, "close() should invalidate storage")) {
            cleanup(tempDir);
            return 1;
        }
        pass(5, "close() invalidates storage");
    }

    cleanup(tempDir);
    std::printf("\n=== All MPQ tests passed ===\n");
    return 0;
}