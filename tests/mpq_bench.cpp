// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// MPQ module benchmarks. Not a test: nothing runs it, and it is built on
// Windows only (its clock, its memory counter and StormLib are Win32).
//
// Rows go to stdout, tab separated: section, archive, impl, metric, value.
// "ours" is this library. With --storm <StormLib.dll> the same work is given
// to StormLib, loaded from that file at run time: nothing of it is linked.
//
//   mpq_bench names   <mpq>                      open, listFiles, lookups
//   mpq_bench archive <mpq> [--quick]            every file read: one thread, a pool, many readers
//   mpq_bench readcmp <mpq> --storm <dll>        the two libraries reading in turn, pass by pass
//   mpq_bench codecs  <mpq>... [--cap MiB]       sectors of real archives, decoded
//   mpq_bench encode  <mpq> [--cap MiB]          4 KiB sectors of real files, compressed
//   mpq_bench micro   <mpq>                      hashes, checksums, the cipher
//   mpq_bench resave  <src> <dst>                open, save to another path
//   mpq_bench add1    <src> <work>               add one file, save
//   mpq_bench create  <src> <dst> <MiB> [--pool N] [--codec zlib|pkware|bzip2|none]
//   mpq_bench copy    <src> <dst>                what the file system takes to move an archive's bytes
//
// Common options: --reps N, --threads N, --storm <dll>.
//
// This machine's clock speed moves under load: take the best of several
// runs for a number, and compare two libraries only with readcmp.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <intrin.h>

#include <whiteout/storages/mpq/storage.h>
#include <whiteout/utils/simple_thread_pool.h>

#include "common/checksum.h"
#include "common/deflate.h"
#include "storages/common/inflate_fast.h"
#include "storages/common/mapped_file.h"
#include "storages/common/md5.h"
#include "storages/mpq/codecs/compression.h"
#include "storages/mpq/codecs/adpcm.h"
#include "storages/mpq/codecs/huffman.h"
#include "storages/mpq/codecs/pkware.h"
#include "storages/mpq/crypto.h"
#include "storages/mpq/file_data.h"
#include "storages/mpq/special_files.h"
#include "storages/mpq/tables/block_table.h"
#include "storages/mpq/tables/hash_table.h"
#include "storages/mpq/tables/header.h"


#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace whiteout;
namespace mpq = whiteout::storages::mpq;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- timing

static double g_ticksPerMs = 0;
static void calibrate() {
    auto const t0 = Clock::now();
    u64 const c0 = __rdtsc();
    Sleep(200);
    u64 const c1 = __rdtsc();
    double const ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    g_ticksPerMs = double(c1 - c0) / ms;
}
static double toMs(u64 ticks) {
    return double(ticks) / g_ticksPerMs;
}
template <class Fn>
static double timeMs(Fn fn) {
    u64 const c0 = __rdtsc();
    fn();
    return toMs(__rdtsc() - c0);
}
template <class Fn>
static double bestMs(int reps, Fn fn) {
    double best = 1e300;
    for (int i = 0; i < reps; ++i)
        best = std::min(best, timeMs(fn));
    return best;
}
static double peakMiB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return double(pmc.PeakPagefileUsage) / 1048576.0;
}
static double mib(u64 bytes) {
    return double(bytes) / 1048576.0;
}

static volatile u32 g_sink = 0;
static std::string g_archive;
static void row(const char* section, const char* impl, const std::string& metric, double value) {
    std::printf("%s\t%s\t%s\t%s\t%.4f\n", section, g_archive.c_str(), impl, metric.c_str(), value);
    std::fflush(stdout);
}
static std::string baseName(const std::string& path) {
    auto const at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}
static std::wstring wide(const std::string& s) {
    int const n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(size_t(n) - 1);
    return w;
}
static u64 fileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &d))
        return 0;
    return (u64{d.nFileSizeHigh} << 32) | d.nFileSizeLow;
}

// ---------------------------------------------------------------- StormLib

struct FindData {
    char cFileName[260];
    char* szPlainName;
    DWORD dwHashIndex, dwBlockIndex, dwFileSize, dwFileFlags, dwCompSize, dwFileTimeLo, dwFileTimeHi;
    LCID lcLocale;
};

struct Storm {
    HMODULE dll = nullptr;
    bool(WINAPI* OpenArchive)(const wchar_t*, DWORD, DWORD, HANDLE*) = nullptr;
    bool(WINAPI* CloseArchive)(HANDLE) = nullptr;
    HANDLE(WINAPI* FindFirst)(HANDLE, const char*, FindData*, const wchar_t*) = nullptr;
    bool(WINAPI* FindNext)(HANDLE, FindData*) = nullptr;
    bool(WINAPI* FindClose_)(HANDLE) = nullptr;
    bool(WINAPI* OpenFileEx)(HANDLE, const char*, DWORD, HANDLE*) = nullptr;
    DWORD(WINAPI* GetFileSize_)(HANDLE, DWORD*) = nullptr;
    bool(WINAPI* ReadFile_)(HANDLE, void*, DWORD, DWORD*, void*) = nullptr;
    bool(WINAPI* CloseFile)(HANDLE) = nullptr;
    bool(WINAPI* HasFile)(HANDLE, const char*) = nullptr;
    bool(WINAPI* CreateArchive)(const wchar_t*, DWORD, DWORD, HANDLE*) = nullptr;
    bool(WINAPI* CreateFile_)(HANDLE, const char*, ULONGLONG, DWORD, LCID, DWORD, HANDLE*) = nullptr;
    bool(WINAPI* WriteFile_)(HANDLE, const void*, DWORD, DWORD) = nullptr;
    bool(WINAPI* FinishFile)(HANDLE) = nullptr;
    bool(WINAPI* CompactArchive)(HANDLE, const wchar_t*, bool) = nullptr;
    int(WINAPI* CompDecompress)(void*, int*, void*, int) = nullptr;
    int(WINAPI* CompCompress)(void*, int*, void*, int, unsigned, int, int) = nullptr;
    int(WINAPI* CompExplode)(void*, int*, void*, int) = nullptr;
    int(WINAPI* CompImplode)(void*, int*, void*, int) = nullptr;

    /// Loads the DLL at @p path; the DLLs it needs are looked for beside it.
    bool load(std::wstring path) {
        std::replace(path.begin(), path.end(), L'/', L'\\'); // The loader takes backslashes only.
        dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll)
            return false;
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(dll, name));
            if (!fn) {
                std::fprintf(stderr, "StormLib lacks %s\n", name);
                ok = false;
            }
        };
        get(OpenArchive, "SFileOpenArchive");
        get(CloseArchive, "SFileCloseArchive");
        get(FindFirst, "SFileFindFirstFile");
        get(FindNext, "SFileFindNextFile");
        get(FindClose_, "SFileFindClose");
        get(OpenFileEx, "SFileOpenFileEx");
        get(GetFileSize_, "SFileGetFileSize");
        get(ReadFile_, "SFileReadFile");
        get(CloseFile, "SFileCloseFile");
        get(HasFile, "SFileHasFile");
        get(CreateArchive, "SFileCreateArchive");
        get(CreateFile_, "SFileCreateFile");
        get(WriteFile_, "SFileWriteFile");
        get(FinishFile, "SFileFinishFile");
        get(CompactArchive, "SFileCompactArchive");
        get(CompDecompress, "SCompDecompress");
        get(CompCompress, "SCompCompress");
        get(CompExplode, "SCompExplode");
        get(CompImplode, "SCompImplode");
        return ok;
    }
};

static constexpr DWORD kStormReadOnly = 0x100;
static constexpr DWORD kStormFileCompress = 0x200, kStormReplace = 0x80000000;
static constexpr DWORD kStormCreateListfile = 0x100000, kStormCreateAttributes = 0x200000;

// ---------------------------------------------------------------- the archive, read with the module's own parts

struct View {
    storages::common::MappedFile map;
    mpq::MpqHeader header;
    size_t archiveOffset = 0;
    bool war3Map = false;
    mpq::HashTable hash;
    mpq::BlockTable blocks;

    bool open(const std::string& path) {
        auto mapped = storages::common::MappedFile::open(path);
        if (!mapped)
            return false;
        war3Map = mpq::isWar3Map(path, mapped->data());
        auto parsed = mpq::findAndParseHeader(mapped->data(), war3Map);
        if (!parsed)
            return false;
        header = parsed->header;
        archiveOffset = parsed->archiveOffset;
        auto const bytes = mapped->data();
        header.hashTableEntries &= mpq::kBlockIndexMask;
        header.blockTableEntries &= mpq::kBlockIndexMask;
        auto tableAt = [&](u64 offset) {
            u64 const at = archiveOffset + offset;
            return header.formatVersion == 0 ? at & 0xFFFFFFFF : at;
        };
        u64 const htAt = tableAt(header.hashTableByteOffset());
        u64 const htSize = u64{header.hashTableEntries} * 16;
        if (htAt + htSize > bytes.size() || !hash.parse(bytes.subspan(htAt, htSize), header.hashTableEntries))
            return false;
        u64 const btAt = tableAt(header.blockTableByteOffset());
        if (header.formatVersion == 0 && btAt <= bytes.size() && btAt + u64{header.blockTableEntries} * 16 > bytes.size())
            header.blockTableEntries = static_cast<u32>((bytes.size() - btAt) / 16);
        u64 const btSize = u64{header.blockTableEntries} * 16;
        if (btAt + btSize > bytes.size() || !blocks.parse(bytes.subspan(btAt, btSize), header.blockTableEntries))
            return false;
        if (war3Map)
            for (u32 i = 0; i < blocks.count(); ++i)
                blocks.entry(i).flags &= mpq::kWar3MapFileFlags;
        if (header.formatVersion >= 1 && header.hiBlockTableOffset != 0) {
            u64 const hiAt = archiveOffset + header.hiBlockTableOffset;
            u64 const hiSize = u64{header.blockTableEntries} * 2;
            if (hiAt + hiSize <= bytes.size())
                blocks.parseHiBlockTable(bytes.subspan(hiAt, hiSize), header.blockTableEntries);
        }
        map = std::move(*mapped);
        return true;
    }
};

static constexpr int kStored = -1, kImplode = 0x100;

static std::string kindName(int kind) {
    switch (kind) {
    case kStored:
        return "stored";
    case kImplode:
        return "implode";
    case 0x01:
        return "huffman";
    case 0x02:
        return "zlib";
    case 0x08:
        return "pkware";
    case 0x10:
        return "bzip2";
    case 0x12:
        return "lzma";
    case 0x20:
        return "sparse";
    case 0x22:
        return "sparse+zlib";
    case 0x30:
        return "sparse+bzip2";
    case 0x41:
        return "adpcm1+huff";
    case 0x81:
        return "adpcm2+huff";
    case 0x40:
        return "adpcm1";
    case 0x80:
        return "adpcm2";
    default: {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "mask%02X", kind);
        return buf;
    }
    }
}

/// @p fn(kind, stored bytes decrypted, expected size) for each unit of a file. False when it cannot be walked.
template <class Fn>
static bool walkUnits(const View& v, u32 blockIndex, u32 key, Fn fn) {
    const mpq::BlockEntry& be = v.blocks.entry(blockIndex);
    if (!be.exists() || be.uncompressedSize == 0 || mpq::hasFlag(be.flags, mpq::FileFlag::kPatchFile))
        return false;
    u64 const start = v.archiveOffset + v.blocks.fileOffset48(blockIndex);
    auto const bytes = v.map.data();
    if (start + be.compressedSize > bytes.size())
        return false;
    auto const file = bytes.subspan(start, be.compressedSize);
    u32 const sectorSize = v.header.sectorSize();
    bool const enc = be.isEncrypted();
    if (enc && key == 0)
        return false;
    std::vector<u8> scratch;
    auto unit = [&](std::span<const u8> stored, u32 expected, u32 unitKey) {
        if (enc) {
            scratch.assign(stored.begin(), stored.end());
            mpq::decryptBytes(scratch, unitKey);
            stored = scratch;
        }
        int kind = kStored;
        if (stored.size() < expected) {
            if (mpq::hasFlag(be.flags, mpq::FileFlag::kCompress))
                kind = stored.empty() ? kStored : stored[0];
            else
                kind = kImplode;
        }
        fn(kind, stored, expected);
    };
    if (be.isSingleUnit()) {
        unit(file, be.uncompressedSize, key);
        return true;
    }
    u32 const sectors = static_cast<u32>((u64{be.uncompressedSize} + sectorSize - 1) / sectorSize);
    if (!be.isCompressed()) {
        for (u32 i = 0; i < sectors; ++i) {
            u64 const at = u64{i} * sectorSize;
            if (at >= file.size())
                break;
            u32 const len = static_cast<u32>(std::min<u64>(sectorSize, std::min<u64>(file.size(), be.uncompressedSize) - at));
            unit(file.subspan(at, len), len, key + i);
        }
        return true;
    }
    u32 const entries = sectors + 1 + (be.hasSectorCrc() ? 1u : 0u);
    if (file.size() < size_t{entries} * 4)
        return false;
    std::vector<u32> table(entries);
    std::memcpy(table.data(), file.data(), size_t{entries} * 4);
    if (enc)
        mpq::decryptBlock(table.data(), table.size(), key - 1);
    if (table[0] != entries * 4)
        return false;
    for (u32 i = 0; i < sectors; ++i)
        if (table[i + 1] < table[i] || table[i + 1] > file.size())
            return false;
    for (u32 i = 0; i < sectors; ++i) {
        u32 const expected = std::min(sectorSize, be.uncompressedSize - i * sectorSize);
        unit(file.subspan(table[i], table[i + 1] - table[i]), expected, key + i);
    }
    return true;
}

struct FileRec {
    std::string name;
    u32 block = 0;
    u32 key = 0;
    u32 size = 0;
    std::string cls;
};

/// The files a (listfile) names, each with the codec most of its bytes are stored with.
static std::vector<FileRec> namedFiles(const View& v, const std::vector<std::string>& names) {
    std::vector<FileRec> out;
    for (const auto& name : names) {
        auto slot = v.hash.find(name, 0, v.blocks.count());
        if (!slot)
            continue;
        FileRec rec;
        rec.name = name;
        rec.block = v.hash.entry(*slot).blockIndex;
        const mpq::BlockEntry& be = v.blocks.entry(rec.block);
        if (!be.exists())
            continue;
        rec.key = be.isEncrypted() ? mpq::deriveFileKey(name, be) : 0;
        rec.size = be.uncompressedSize;
        std::map<int, u64> byKind;
        walkUnits(v, rec.block, rec.key, [&](int kind, std::span<const u8>, u32 expected) { byKind[kind] += expected; });
        int best = kStored;
        u64 bestBytes = 0;
        for (auto [kind, n] : byKind)
            if (n > bestBytes)
                best = kind, bestBytes = n;
        rec.cls = be.uncompressedSize == 0 ? "empty" : kindName(best);
        if (be.isEncrypted())
            rec.cls += "+enc";
        if (be.isSingleUnit())
            rec.cls += "+single";
        out.push_back(std::move(rec));
    }
    return out;
}

struct Acc {
    u64 files = 0, bytes = 0, ticks = 0;
};
static const char* sizeBucket(u32 size) {
    if (size < 4096)
        return "a_lt4K";
    if (size < 65536)
        return "b_4K-64K";
    if (size < 1048576)
        return "c_64K-1M";
    return "d_ge1M";
}
static void report(const char* section, const char* impl, const std::vector<FileRec>& files,
                   const std::vector<u64>& ticks, const std::vector<u8>& okFlags) {
    std::map<std::string, Acc> byClass, byBucket, byExt;
    Acc total;
    u64 failed = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (!okFlags[i]) {
            ++failed;
            continue;
        }
        auto const dot = files[i].name.find_last_of('.');
        std::string ext = dot == std::string::npos ? "(none)" : files[i].name.substr(dot + 1);
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (Acc* acc : {&byClass[files[i].cls], &byBucket[sizeBucket(files[i].size)], &byExt[ext + "/" + files[i].cls],
                         &total}) {
            ++acc->files;
            acc->bytes += files[i].size;
            acc->ticks += ticks[i];
        }
    }
    row(section, impl, "total_ms", toMs(total.ticks));
    row(section, impl, "total_files", double(total.files));
    row(section, impl, "total_MiB", mib(total.bytes));
    row(section, impl, "total_MiB_per_s", mib(total.bytes) / (toMs(total.ticks) / 1000.0));
    row(section, impl, "failed", double(failed));
    for (auto& [name, acc] : byClass) {
        row(section, impl, "class:" + name + ":files", double(acc.files));
        row(section, impl, "class:" + name + ":MiB", mib(acc.bytes));
        row(section, impl, "class:" + name + ":ms", toMs(acc.ticks));
        if (acc.bytes)
            row(section, impl, "class:" + name + ":MiB_per_s", mib(acc.bytes) / (toMs(acc.ticks) / 1000.0));
    }
    // The ten extensions that cost the most time, each with its codec.
    std::vector<std::pair<std::string, Acc>> exts(byExt.begin(), byExt.end());
    std::sort(exts.begin(), exts.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
    for (size_t i = 0; i < exts.size() && i < 10; ++i) {
        const auto& [name, acc] = exts[i];
        row(section, impl, "ext:" + name + ":files", double(acc.files));
        row(section, impl, "ext:" + name + ":MiB", mib(acc.bytes));
        row(section, impl, "ext:" + name + ":ms", toMs(acc.ticks));
        row(section, impl, "ext:" + name + ":us_per_file", toMs(acc.ticks) * 1000.0 / double(acc.files));
    }
    for (auto& [name, acc] : byBucket) {
        row(section, impl, "size:" + std::string(name) + ":files", double(acc.files));
        row(section, impl, "size:" + std::string(name) + ":us_per_file", toMs(acc.ticks) * 1000.0 / double(acc.files));
        if (acc.bytes)
            row(section, impl, "size:" + std::string(name) + ":MiB_per_s", mib(acc.bytes) / (toMs(acc.ticks) / 1000.0));
    }
}

// ---------------------------------------------------------------- archive: open, list, lookup, read

static bool g_quick = false;
static int benchArchive(const std::string& path, Storm* storm, int reps, int threads) {
    g_archive = baseName(path);
    row("archive", "-", "file_MiB", mib(fileSize(path)));

    // Open.
    {
        std::vector<double> times;
        for (int i = 0; i < reps + 2; ++i)
            times.push_back(timeMs([&] {
                auto s = mpq::Storage::open(path);
                if (!s)
                    std::exit(3);
            }));
        std::sort(times.begin(), times.end());
        row("open", "ours", "min_ms", times.front());
        row("open", "ours", "median_ms", times[times.size() / 2]);
    }
    auto storage = mpq::Storage::open(path);
    if (!storage)
        return 3;
    auto const info = storage->archiveInfo();
    row("archive", "-", "format_version", info.formatVersion);
    row("archive", "-", "hash_entries", info.hashTableEntries);
    row("archive", "-", "block_entries", info.blockTableEntries);
    row("archive", "-", "sector_size", info.sectorSize);

    // List.
    std::vector<std::string> names;
    row("list", "ours", "min_ms", bestMs(reps, [&] { names = storage->listFiles(); }));
    row("list", "ours", "names", double(names.size()));

    View view;
    if (!view.open(path))
        return 4;
    std::vector<FileRec> files = namedFiles(view, names);
    u64 nameChars = 0;
    for (const auto& f : files)
        nameChars += f.name.size();
    row("archive", "-", "named_files", double(files.size()));
    row("archive", "-", "avg_name_chars", files.empty() ? 0 : double(nameChars) / double(files.size()));

    // Lookup: every name, then every name with a tail no file has.
    std::vector<std::string> misses;
    for (const auto& f : files)
        misses.push_back(f.name + ".zz");
    int const rounds = std::max(1, int(2000000 / std::max<size_t>(files.size(), 1)));
    if (!files.empty() && !g_quick) {
        u64 hits = 0;
        double ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& f : files)
                    hits += storage->fileExists(f.name);
        });
        row("lookup", "ours", "hit_ns", ms * 1e6 / (double(rounds) * double(files.size())));
        ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& m : misses)
                    hits += storage->fileExists(m);
        });
        row("lookup", "ours", "miss_ns", ms * 1e6 / (double(rounds) * double(files.size())));
        ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& f : files)
                    hits += storage->fileInfo(f.name).has_value();
        });
        row("lookup", "ours", "fileInfo_ns", ms * 1e6 / (double(rounds) * double(files.size())));
        if (hits == 0)
            std::fprintf(stderr, "no lookup hit\n");
    }

    // Read every file, one thread, no pool.
    std::vector<u64> ticks(files.size(), ~0ull);
    std::vector<u8> ok(files.size(), 0);
    int const readReps = std::min(reps, 3);
    for (int rep = 0; rep < readReps; ++rep) {
        for (size_t i = 0; i < files.size(); ++i) {
            u64 const c0 = __rdtsc();
            auto data = storage->readFile(files[i].name);
            u64 const c1 = __rdtsc();
            ok[i] = data.has_value() && data->size() == files[i].size;
            ticks[i] = std::min(ticks[i], c1 - c0);
        }
    }
    report("read", "ours", files, ticks, ok);

    // The same reads through a worker pool (a large file's sectors in runs).
    if (!g_quick) {
        utils::SimpleThreadPool pool{size_t(threads)};
        auto pooled = mpq::Storage::open(path, &pool);
        double const ms = bestMs(readReps, [&] {
            for (const auto& f : files)
                (void)pooled->readFile(f.name);
        });
        row("read_pool", "ours", "threads", threads);
        row("read_pool", "ours", "total_ms", ms);
        // And asked for all at once: whole files shared between the threads.
        std::vector<std::string> wanted;
        for (const auto& f : files)
            wanted.push_back(f.name);
        row("read_pool", "ours", "readFiles_ms", bestMs(readReps, [&] {
                g_sink = g_sink + static_cast<u32>(pooled->readFiles(wanted).size());
            }));
        std::vector<u64> pticks(files.size(), ~0ull);
        for (int rep = 0; rep < readReps; ++rep)
            for (size_t i = 0; i < files.size(); ++i) {
                u64 const c0 = __rdtsc();
                (void)pooled->readFile(files[i].name);
                pticks[i] = std::min(pticks[i], __rdtsc() - c0);
            }
        report("read_pool", "ours", files, pticks, ok);
    }

    // Many readers on one Storage, no pool.
    for (int t : {1, 2, 4, 8, threads}) {
        if (t > threads || g_quick)
            continue;
        double const ms = bestMs(2, [&] {
            std::atomic<size_t> next{0};
            std::vector<std::thread> workers;
            for (int w = 0; w < t; ++w)
                workers.emplace_back([&] {
                    for (size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1))
                        (void)storage->readFile(files[i].name);
                });
            for (auto& w : workers)
                w.join();
        });
        row("read_mt", "ours", "threads_" + std::to_string(t) + "_ms", ms);
    }

    if (!storm)
        return 0;
    std::wstring const wpath = wide(path);
    {
        std::vector<double> times;
        for (int i = 0; i < reps + 2; ++i)
            times.push_back(timeMs([&] {
                HANDLE h = nullptr;
                if (!storm->OpenArchive(wpath.c_str(), 0, kStormReadOnly, &h))
                    std::exit(5);
                storm->CloseArchive(h);
            }));
        std::sort(times.begin(), times.end());
        row("open", "storm", "min_ms", times.front());
        row("open", "storm", "median_ms", times[times.size() / 2]);
    }
    HANDLE h = nullptr;
    if (!storm->OpenArchive(wpath.c_str(), 0, kStormReadOnly, &h))
        return 5;
    {
        size_t count = 0;
        double const ms = bestMs(reps, [&] {
            count = 0;
            FindData fd{};
            HANDLE find = storm->FindFirst(h, "*", &fd, nullptr);
            if (!find)
                return;
            do
                ++count;
            while (storm->FindNext(find, &fd));
            storm->FindClose_(find);
        });
        row("list", "storm", "min_ms", ms);
        row("list", "storm", "names", double(count));
    }
    if (!files.empty() && !g_quick) {
        u64 hits = 0;
        double ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& f : files)
                    hits += storm->HasFile(h, f.name.c_str());
        });
        row("lookup", "storm", "hit_ns", ms * 1e6 / (double(rounds) * double(files.size())));
        ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& m : misses)
                    hits += storm->HasFile(h, m.c_str());
        });
        row("lookup", "storm", "miss_ns", ms * 1e6 / (double(rounds) * double(files.size())));
    }
    std::fill(ticks.begin(), ticks.end(), ~0ull);
    std::fill(ok.begin(), ok.end(), u8{0});
    for (int rep = 0; rep < readReps; ++rep) {
        for (size_t i = 0; i < files.size(); ++i) {
            u64 const c0 = __rdtsc();
            HANDLE hf = nullptr;
            bool good = false;
            if (storm->OpenFileEx(h, files[i].name.c_str(), 0, &hf)) {
                DWORD hi = 0;
                DWORD const n = storm->GetFileSize_(hf, &hi);
                std::unique_ptr<u8[]> buf(new u8[n ? n : 1]);
                DWORD got = 0;
                storm->ReadFile_(hf, buf.get(), n, &got, nullptr);
                good = got == n && n == files[i].size;
                storm->CloseFile(hf);
            }
            u64 const c1 = __rdtsc();
            ok[i] = good;
            ticks[i] = std::min(ticks[i], c1 - c0);
        }
    }
    report("read", "storm", files, ticks, ok);
    storm->CloseArchive(h);
    return 0;
}

// ---------------------------------------------------------------- readcmp: the two libraries, pass by pass

static int benchReadCompare(const std::string& path, Storm& storm, int reps) {
    g_archive = baseName(path);
    auto storage = mpq::Storage::open(path);
    View view;
    if (!storage || !view.open(path))
        return 3;
    std::vector<FileRec> files = namedFiles(view, storage->listFiles());
    HANDLE h = nullptr;
    if (!storm.OpenArchive(wide(path).c_str(), 0, kStormReadOnly, &h))
        return 5;
    size_t const n = files.size();
    std::vector<u64> ticks[2];
    std::vector<u8> ok[2];
    for (int i = 0; i < 2; ++i) {
        ticks[i].assign(n, ~0ull);
        ok[i].assign(n, 0);
    }
    for (int rep = 0; rep < reps; ++rep) {
        // The order turns each pass, so neither reader always runs first.
        for (int turn = 0; turn < 2; ++turn) {
            int const impl = (rep + turn) % 2;
            for (size_t i = 0; i < n; ++i) {
                bool good = false;
                u64 const c0 = __rdtsc();
                if (impl == 0) {
                    auto data = storage->readFile(files[i].name);
                    good = data && data->size() == files[i].size;
                } else {
                    HANDLE hf = nullptr;
                    if (storm.OpenFileEx(h, files[i].name.c_str(), 0, &hf)) {
                        DWORD hi = 0;
                        DWORD const size = storm.GetFileSize_(hf, &hi);
                        std::unique_ptr<u8[]> buf(new u8[size ? size : 1]);
                        DWORD got = 0;
                        storm.ReadFile_(hf, buf.get(), size, &got, nullptr);
                        good = got == size && size == files[i].size;
                        storm.CloseFile(hf);
                    }
                }
                u64 const c1 = __rdtsc();
                ok[impl][i] = good;
                ticks[impl][i] = std::min(ticks[impl][i], c1 - c0);
            }
        }
    }
    storm.CloseArchive(h);
    report("readcmp", "ours", files, ticks[0], ok[0]);
    report("readcmp", "storm", files, ticks[1], ok[1]);
    return 0;
}

// ---------------------------------------------------------------- codecs: sectors of real archives, decoded

struct SectorSet {
    struct Item {
        size_t at;
        u32 len;
        u32 expected;
    };
    std::vector<u8> arena;
    std::vector<Item> items;
    u64 outBytes = 0;
};

/// Sectors of @p path by codec, up to @p capBytes of output each, in a shuffled file order.
static void harvest(const std::string& path, std::map<int, SectorSet>& sets, u64 capBytes) {
    View view;
    if (!view.open(path)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return;
    }
    std::vector<u32> order;
    // Keys come from names where there is a listfile.
    std::map<u32, u32> keys;
    if (auto storage = mpq::Storage::open(path))
        for (const auto& name : storage->listFiles())
            if (auto slot = view.hash.find(name, 0, view.blocks.count())) {
                u32 const block = view.hash.entry(*slot).blockIndex;
                const auto& be = view.blocks.entry(block);
                if (be.isEncrypted())
                    keys[block] = mpq::deriveFileKey(name, be);
            }
    for (u32 b = 0; b < view.blocks.count(); ++b)
        order.push_back(b);
    std::mt19937 rng(1);
    std::shuffle(order.begin(), order.end(), rng);
    for (u32 const block : order) {
        const auto& be = view.blocks.entry(block);
        u32 key = 0;
        if (be.isEncrypted()) {
            if (auto it = keys.find(block); it != keys.end())
                key = it->second;
            else {
                u64 const start = view.archiveOffset + view.blocks.fileOffset48(block);
                if (start + be.compressedSize > view.map.data().size())
                    continue;
                auto detected = mpq::detectStoredFileKey(view.map.data().subspan(start, be.compressedSize), be,
                                                         view.header.sectorSize());
                if (!detected)
                    continue;
                key = *detected;
            }
        }
        walkUnits(view, block, key, [&](int kind, std::span<const u8> stored, u32 expected) {
            if (kind == kStored)
                return;
            SectorSet& set = sets[kind];
            if (set.outBytes >= capBytes)
                return;
            set.items.push_back({set.arena.size(), static_cast<u32>(stored.size()), expected});
            set.arena.insert(set.arena.end(), stored.begin(), stored.end());
            set.outBytes += expected;
        });
    }
}

static int benchCodecs(const std::vector<std::string>& paths, Storm* storm, u64 capBytes) {
    std::map<int, SectorSet> sets;
    for (const auto& path : paths)
        harvest(path, sets, capBytes);
    g_archive = paths.size() == 1 ? baseName(paths[0]) : "corpus";
    for (auto& [kind, set] : sets) {
        std::string const name = kindName(kind);
        u64 inBytes = set.arena.size();
        row("decode", "-", name + ":sectors", double(set.items.size()));
        row("decode", "-", name + ":in_MiB", mib(inBytes));
        row("decode", "-", name + ":out_MiB", mib(set.outBytes));
        double const seconds = [&] {
            // Reps sized so each codec runs for about a second at 100 MiB/s.
            return 0.0;
        }();
        (void)seconds;
        int const reps = set.outBytes < (u64{8} << 20) ? 20 : 5;
        u64 bad = 0;
        std::vector<u8> into(size_t{1} << 22);
        mpq::DecodeScratch scratch;
        double ms = bestMs(reps, [&] {
            bad = 0;
            for (const auto& item : set.items) {
                std::span<const u8> const stored(set.arena.data() + item.at, item.len);
                if (item.expected > into.size())
                    into.resize(item.expected);
                std::span<u8> const place(into.data(), item.expected);
                bad += kind == kImplode ? mpq::pkwareExplodeInto(stored, place) != item.expected
                                        : !mpq::mpqDecompressInto(stored, place, scratch);
            }
        });
        row("decode", "ours", name + ":MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
        row("decode", "ours", name + ":us_per_sector", ms * 1000.0 / double(set.items.size()));
        row("decode", "ours", name + ":failed", double(bad));

        if (kind == 0x02) {
            // The same streams through the inflater alone, then through the one CASC reads with.
            ms = bestMs(reps, [&] {
                for (const auto& item : set.items) {
                    std::span<const u8> const stream(set.arena.data() + item.at + 1, item.len - 1);
                    auto out = zlib_decompress(stream, nullptr, item.expected);
                    bad += out.size() != item.expected;
                }
            });
            row("decode", "ours", "zlib(inflater alone):MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
            ms = bestMs(reps, [&] {
                for (const auto& item : set.items) {
                    std::span<const u8> const stream(set.arena.data() + item.at + 1, item.len - 1);
                    auto out = storages::common::zlibInflateFast(stream, item.expected);
                    bad += out.size() != item.expected;
                }
            });
            row("decode", "ours", "zlib(casc inflater):MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
            std::vector<u8> dst(size_t{1} << 22);
            ms = bestMs(reps, [&] {
                for (const auto& item : set.items) {
                    std::span<const u8> const stream(set.arena.data() + item.at + 1, item.len - 1);
                    if (item.expected > dst.size())
                        dst.resize(item.expected);
                    auto n = storages::common::zlibInflateFastInto(std::span<u8>(dst.data(), item.expected), stream);
                    bad += !n || *n != item.expected;
                }
            });
            row("decode", "ours", "zlib(casc inflater, into buffer):MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
        }
        if (kind == 0x41 || kind == 0x81) {
            int const channels = kind == 0x41 ? 1 : 2;
            std::vector<std::vector<u8>> mids(set.items.size());
            ms = bestMs(reps, [&] {
                for (size_t i = 0; i < set.items.size(); ++i) {
                    const auto& item = set.items[i];
                    mids[i] = mpq::huffmanDecompress(std::span<const u8>(set.arena.data() + item.at + 1, item.len - 1),
                                                     item.expected);
                }
            });
            u64 midBytes = 0;
            for (const auto& mid : mids)
                midBytes += mid.size();
            row("decode", "ours", name + ":huffman_stage_ms", ms);
            {
                // The tree alone: one byte decoded per sector.
                u8 one[1];
                double const setupMs = bestMs(reps, [&] {
                    for (size_t i = 0; i < set.items.size(); ++i) {
                        const auto& item = set.items[i];
                        g_sink = g_sink + static_cast<u32>(mpq::huffmanDecompressInto(
                                              std::span<const u8>(set.arena.data() + item.at + 1, item.len - 1), std::span<u8>(one, 1)));
                    }
                });
                row("decode", "ours", name + ":huffman_setup_ms", setupMs);
                row("decode", "ours", name + ":huffman_setup_us_per_sector", setupMs * 1000.0 / double(set.items.size()));
                row("decode", "ours", name + ":huffman_stage_us_per_sector", ms * 1000.0 / double(set.items.size()));
            }
            row("decode", "ours", name + ":huffman_stage_out_MiB_per_s", mib(midBytes) / (ms / 1000.0));
            ms = bestMs(reps, [&] {
                for (size_t i = 0; i < set.items.size(); ++i)
                    bad += mpq::adpcmDecompress(mids[i], set.items[i].expected, channels).size() != set.items[i].expected;
            });
            row("decode", "ours", name + ":adpcm_stage_ms", ms);
            row("decode", "ours", name + ":adpcm_stage_out_MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
        }
        if (storm) {
            std::vector<u8> out(size_t{1} << 22);
            ms = bestMs(reps, [&] {
                bad = 0;
                for (const auto& item : set.items) {
                    if (size_t{item.expected} + 64 > out.size())
                        out.resize(size_t{item.expected} + 64);
                    int n = static_cast<int>(item.expected);
                    void* in = set.arena.data() + item.at;
                    int const good = kind == kImplode
                                         ? (n = static_cast<int>(out.size()),
                                            storm->CompExplode(out.data(), &n, in, static_cast<int>(item.len)))
                                         : storm->CompDecompress(out.data(), &n, in, static_cast<int>(item.len));
                    bad += !good || static_cast<u32>(n) != item.expected;
                }
            });
            row("decode", "storm", name + ":MiB_per_s", mib(set.outBytes) / (ms / 1000.0));
            row("decode", "storm", name + ":us_per_sector", ms * 1000.0 / double(set.items.size()));
            row("decode", "storm", name + ":failed", double(bad));
        }
    }
    return 0;
}

// ---------------------------------------------------------------- encode: plain sectors of real files, compressed

static int benchEncode(const std::string& path, Storm* storm, u64 capBytes) {
    g_archive = baseName(path);
    auto storage = mpq::Storage::open(path);
    if (!storage)
        return 3;
    auto names = storage->listFiles();
    std::mt19937 rng(1);
    std::shuffle(names.begin(), names.end(), rng);
    u32 const sectorSize = storage->archiveInfo().sectorSize;
    // Plain data by kind of file: what compresses differs a great deal between them.
    std::map<std::string, std::vector<u8>> groups;
    auto groupOf = [](const std::string& name) -> std::string {
        auto const dot = name.find_last_of('.');
        std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == "blp" || ext == "tga" || ext == "dds")
            return "textures";
        if (ext == "mdx" || ext == "m3" || ext == "mdl")
            return "models";
        if (ext == "wav" || ext == "mp3" || ext == "ogg")
            return "audio";
        if (ext == "txt" || ext == "slk" || ext == "j" || ext == "fdf" || ext == "ai" || ext == "xml" || ext == "galaxy" ||
            ext == "toc" || ext == "ini" || ext == "pld")
            return "text";
        return "other";
    };
    for (const auto& name : names) {
        std::string const group = groupOf(name);
        auto& bytes = groups[group];
        if (bytes.size() >= capBytes)
            continue;
        auto data = storage->readFile(name);
        if (data)
            bytes.insert(bytes.end(), data->begin(), data->end());
    }
    struct Codec {
        const char* name;
        mpq::CompressionFlag flag;
        unsigned stormMask;
    };
    const Codec codecs[] = {{"zlib", mpq::CompressionFlag::kZlib, 0x02},
                            {"pkware", mpq::CompressionFlag::kPKware, 0x08},
                            {"bzip2", mpq::CompressionFlag::kBZip2, 0x10},
                            {"huffman", mpq::CompressionFlag::kHuffman, 0x01},
                            {"sparse", mpq::CompressionFlag::kSparse, 0x20}};
    for (auto& [group, bytes] : groups) {
        if (bytes.empty())
            continue;
        size_t const sectors = (bytes.size() + sectorSize - 1) / sectorSize;
        row("encode", "-", group + ":MiB", mib(bytes.size()));
        for (const Codec& codec : codecs) {
            std::string const key = group + ":" + codec.name;
            u64 outBytes = 0;
            double ms = bestMs(3, [&] {
                outBytes = 0;
                for (size_t s = 0; s < sectors; ++s) {
                    size_t const at = s * sectorSize;
                    std::span<const u8> const plain(bytes.data() + at, std::min<size_t>(sectorSize, bytes.size() - at));
                    auto out = mpq::mpqCompress(plain, codec.flag);
                    outBytes += out.empty() ? plain.size() : out.size();
                }
            });
            row("encode", "ours", key + ":MiB_per_s", mib(bytes.size()) / (ms / 1000.0));
            row("encode", "ours", key + ":ratio", double(outBytes) / double(bytes.size()));
            row("encode", "ours", key + ":us_per_sector", ms * 1000.0 / double(sectors));
            if (!storm)
                continue;
            std::vector<u8> out(size_t{sectorSize} * 2 + 1024);
            ms = bestMs(3, [&] {
                outBytes = 0;
                for (size_t s = 0; s < sectors; ++s) {
                    size_t const at = s * sectorSize;
                    int const len = static_cast<int>(std::min<size_t>(sectorSize, bytes.size() - at));
                    int n = len; // As SFileWriteFile calls it: no room to grow.
                    int const good = storm->CompCompress(out.data(), &n, bytes.data() + at, len, codec.stormMask, 0, -1);
                    outBytes += (good && n < len) ? static_cast<u64>(n) : static_cast<u64>(len);
                }
            });
            row("encode", "storm", key + ":MiB_per_s", mib(bytes.size()) / (ms / 1000.0));
            row("encode", "storm", key + ":ratio", double(outBytes) / double(bytes.size()));
            row("encode", "storm", key + ":us_per_sector", ms * 1000.0 / double(sectors));
        }
    }
    return 0;
}

// ---------------------------------------------------------------- micro: the parts under every call

/// CRC-32 eight bytes a step (slicing by 8): what the attribute checksum could cost without SIMD.
static int benchMicro(const std::string& path) {
    g_archive = baseName(path);
    auto storage = mpq::Storage::open(path);
    if (!storage)
        return 3;
    auto const names = storage->listFiles();
    u64 chars = 0;
    for (const auto& n : names)
        chars += n.size();
    int const rounds = std::max(1, int(3000000 / std::max<size_t>(names.size(), 1)));
    double const calls = double(rounds) * double(names.size());

    u32 sink = 0;
    double ms = bestMs(5, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names) {
                sink += mpq::hashString(n, mpq::HashType::TableOffset);
                sink += mpq::hashString(n, mpq::HashType::NameA);
                sink += mpq::hashString(n, mpq::HashType::NameB);
            }
    });
    row("micro", "ours", "hash3_ns_per_name", ms * 1e6 / calls);
    row("micro", "ours", "hash3_ns_per_char", ms * 1e6 / (double(rounds) * double(chars)));
    u32 sink2 = 0;
    ms = bestMs(5, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names) {
                mpq::NameHash const h = mpq::hashName(n);
                sink2 += h.offset + h.a + h.b;
            }
    });
    row("micro", "ours", "hashName_ns_per_name", ms * 1e6 / calls);
    row("micro", "-", "names", double(names.size()));
    row("micro", "-", "avg_name_chars", names.empty() ? 0 : double(chars) / double(names.size()));

    // Cipher.
    size_t const big = size_t{64} << 20;
    std::vector<u8> buffer(big);
    std::mt19937 rng(7);
    for (size_t i = 0; i + 4 <= big; i += 4) {
        u32 const v = rng();
        std::memcpy(buffer.data() + i, &v, 4);
    }
    ms = bestMs(3, [&] { mpq::decryptBytes(buffer, 0x12345678); });
    row("micro", "ours", "decryptBytes_MiB_per_s", mib(big) / (ms / 1000.0));
    ms = bestMs(3, [&] { mpq::encryptBytes(buffer, 0x12345678); });
    row("micro", "ours", "encryptBytes_MiB_per_s", mib(big) / (ms / 1000.0));
    // The same cipher over 4 KiB sectors, each with its own key, as a file is read.
    ms = bestMs(3, [&] {
        u32 key = 0x1000;
        for (size_t at = 0; at < big; at += 4096)
            mpq::decryptBytes(std::span<u8>(buffer.data() + at, 4096), key++);
    });
    row("micro", "ours", "decryptBytes_4K_sectors_MiB_per_s", mib(big) / (ms / 1000.0));

    // Checksums a save computes over every new file.
    ms = bestMs(3, [&] { g_sink = g_sink + crc32(buffer.data(), buffer.size()); });
    row("micro", "ours", "crc32_MiB_per_s", mib(big) / (ms / 1000.0));
    ms = bestMs(3, [&] { g_sink = g_sink + adler32(buffer); });
    row("micro", "ours", "adler32_MiB_per_s", mib(big) / (ms / 1000.0));
    ms = bestMs(3, [&] { g_sink = g_sink + storages::common::md5Hash(buffer)[0]; });
    row("micro", "ours", "md5_MiB_per_s", mib(big) / (ms / 1000.0));

    // Tables.
    for (u32 capacity : {u32{1} << 12, u32{1} << 16, u32{1} << 20}) {
        mpq::HashTable table;
        table.createEmpty(capacity);
        for (u32 i = 0; i < capacity / 2; ++i)
            (void)table.insert("file" + std::to_string(i) + ".bin", 0, i);
        auto const bytes = table.serialize();
        mpq::HashTable parsed;
        ms = bestMs(5, [&] { parsed.parse(bytes, capacity); });
        row("micro", "ours", "hash_table_parse_" + std::to_string(capacity) + "_ms", ms);
    }

    // (listfile).
    if (auto listfile = storage->readFile("(listfile)")) {
        size_t count = 0;
        ms = bestMs(5, [&] { count = mpq::parseListfile(*listfile).size(); });
        row("micro", "ours", "parse_listfile_ms", ms);
        row("micro", "-", "listfile_KiB", double(listfile->size()) / 1024.0);
        row("micro", "-", "listfile_lines", double(count));
        ms = bestMs(3, [&] { (void)storage->readFile("(listfile)"); });
        row("micro", "ours", "read_listfile_ms", ms);
    }

    if (sink == sink2)
        std::fprintf(stderr, " ");
    return 0;
}

// ---------------------------------------------------------------- saves

static std::vector<u8> textLike(size_t size) {
    std::vector<u8> data(size);
    std::mt19937 rng(11);
    static const char* words[] = {"unit", "ability", "texture", "model", "sound", "trigger", "region", "camera"};
    size_t at = 0;
    while (at < size) {
        const char* w = words[rng() % 8];
        for (; *w && at < size; ++w)
            data[at++] = static_cast<u8>(*w);
        if (at < size)
            data[at++] = static_cast<u8>(rng() % 5 == 0 ? '\n' : ' ');
    }
    return data;
}

static int benchResave(const std::string& src, const std::string& dst, Storm* storm) {
    g_archive = baseName(src);
    DeleteFileW(wide(dst).c_str());
    if (!storm) {
        auto storage = mpq::Storage::open(src);
        if (!storage)
            return 3;
        double const before = peakMiB();
        bool ok = false;
        double const ms = timeMs([&] { ok = storage->save(dst); });
        row("resave", "ours", "ms", ms);
        row("resave", "ours", "ok", ok);
        row("resave", "ours", "peak_added_MiB", peakMiB() - before);
        row("resave", "ours", "out_MiB", mib(fileSize(dst)));
        row("resave", "ours", "MiB_per_s", mib(fileSize(dst)) / (ms / 1000.0));
        return ok ? 0 : 1;
    }
    if (!CopyFileW(wide(src).c_str(), wide(dst).c_str(), FALSE))
        return 6;
    HANDLE h = nullptr;
    if (!storm->OpenArchive(wide(dst).c_str(), 0, 0, &h))
        return 5;
    bool ok = false;
    double const ms = timeMs([&] {
        ok = storm->CompactArchive(h, nullptr, false);
        storm->CloseArchive(h);
    });
    row("resave", "storm", "ms", ms);
    row("resave", "storm", "ok", ok);
    row("resave", "storm", "out_MiB", mib(fileSize(dst)));
    row("resave", "storm", "MiB_per_s", mib(fileSize(dst)) / (ms / 1000.0));
    return 0;
}

/// The floor for a save that rewrites the archive: its bytes copied by the system, then from the mapping
/// in 4 MiB writes, then the way a save moves them today (mapping -> a 128 MiB vector -> ofstream).
static int benchCopy(const std::string& src, const std::string& dst) {
    g_archive = baseName(src);
    std::wstring const wsrc = wide(src), wdst = wide(dst);
    u64 const size = fileSize(src);
    for (int rep = 0; rep < 3; ++rep) {
        DeleteFileW(wdst.c_str());
        double ms = timeMs([&] { CopyFileW(wsrc.c_str(), wdst.c_str(), FALSE); });
        row("copy", "system", "CopyFile_ms", ms);
        row("copy", "system", "CopyFile_MiB_per_s", mib(size) / (ms / 1000.0));

        DeleteFileW(wdst.c_str());
        ms = timeMs([&] {
            auto map = storages::common::MappedFile::open(src);
            HANDLE out = CreateFileW(wdst.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            auto bytes = map->data();
            for (size_t at = 0; at < bytes.size();) {
                DWORD const n = static_cast<DWORD>(std::min<size_t>(size_t{4} << 20, bytes.size() - at));
                DWORD wrote = 0;
                WriteFile(out, bytes.data() + at, n, &wrote, nullptr);
                at += n;
            }
            CloseHandle(out);
        });
        row("copy", "direct", "map_to_WriteFile_ms", ms);
        row("copy", "direct", "map_to_WriteFile_MiB_per_s", mib(size) / (ms / 1000.0));

        DeleteFileW(wdst.c_str());
        ms = timeMs([&] {
            auto map = storages::common::MappedFile::open(src);
            std::ofstream out(std::filesystem::path(wdst), std::ios::binary | std::ios::trunc);
            auto bytes = map->data();
            std::vector<u8> buffer;
            size_t const spill = size_t{128} << 20;
            // Appended a file at a time, as a save appends them.
            for (size_t at = 0; at < bytes.size();) {
                size_t const n = std::min<size_t>(40000, bytes.size() - at);
                if (buffer.size() + n >= spill) {
                    out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
                    buffer.clear();
                }
                buffer.insert(buffer.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at),
                              bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
                at += n;
            }
            out.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        });
        row("copy", "ours", "map_to_vector_to_ofstream_ms", ms);
        row("copy", "ours", "map_to_vector_to_ofstream_MiB_per_s", mib(size) / (ms / 1000.0));
    }
    DeleteFileW(wdst.c_str());
    return 0;
}

static int benchAdd1(const std::string& src, const std::string& work, Storm* storm) {
    g_archive = baseName(src);
    if (!CopyFileW(wide(src).c_str(), wide(work).c_str(), FALSE))
        return 6;
    auto const data = textLike(256 * 1024);
    const char* name = "bench\\added.txt";
    if (!storm) {
        double const before = peakMiB();
        bool ok = false;
        double openMs = 0, saveMs = 0;
        double const ms = timeMs([&] {
            std::optional<mpq::Storage> storage;
            openMs = timeMs([&] { storage = mpq::Storage::open(work); });
            if (!storage)
                return;
            storage->writeFile(name, data);
            saveMs = timeMs([&] { ok = storage->save(); });
        });
        row("add1", "ours", "total_ms", ms);
        row("add1", "ours", "open_ms", openMs);
        row("add1", "ours", "save_ms", saveMs);
        row("add1", "ours", "ok", ok);
        row("add1", "ours", "peak_added_MiB", peakMiB() - before);
        row("add1", "ours", "out_MiB", mib(fileSize(work)));
        return ok ? 0 : 1;
    }
    bool ok = false;
    double const ms = timeMs([&] {
        HANDLE h = nullptr;
        if (!storm->OpenArchive(wide(work).c_str(), 0, 0, &h))
            return;
        HANDLE hf = nullptr;
        if (storm->CreateFile_(h, name, 0, static_cast<DWORD>(data.size()), 0, kStormFileCompress | kStormReplace, &hf)) {
            ok = storm->WriteFile_(hf, data.data(), static_cast<DWORD>(data.size()), 0x02);
            ok = storm->FinishFile(hf) && ok;
        }
        storm->CloseArchive(h);
    });
    row("add1", "storm", "total_ms", ms);
    row("add1", "storm", "ok", ok);
    row("add1", "storm", "out_MiB", mib(fileSize(work)));
    return 0;
}

static int benchCreate(const std::string& src, const std::string& dst, u64 capBytes, Storm* storm, int poolThreads,
                       const std::string& codecName) {
    g_archive = baseName(src);
    auto source = mpq::Storage::open(src);
    if (!source)
        return 3;
    auto names = source->listFiles();
    std::mt19937 rng(1);
    std::shuffle(names.begin(), names.end(), rng);
    std::vector<std::pair<std::string, std::vector<u8>>> files;
    u64 total = 0;
    for (const auto& name : names) {
        if (total >= capBytes)
            break;
        auto data = source->readFile(name);
        if (!data || data->empty())
            continue;
        total += data->size();
        files.emplace_back(name, std::move(*data));
    }
    source.reset();
    DeleteFileW(wide(dst).c_str());
    mpq::Compression compression = mpq::Compression::Zlib;
    unsigned stormMask = 0x02;
    if (codecName == "pkware")
        compression = mpq::Compression::PKware, stormMask = 0x08;
    else if (codecName == "bzip2")
        compression = mpq::Compression::BZip2, stormMask = 0x10;
    else if (codecName == "none")
        compression = mpq::Compression::None, stormMask = 0;
    std::string const tag = codecName + (poolThreads > 0 ? ":pool" + std::to_string(poolThreads) : ":serial");
    row("create", "-", "files", double(files.size()));
    row("create", "-", "in_MiB", mib(total));
    u32 hashSize = 16;
    while (hashSize < files.size() * 4 / 3 + 8)
        hashSize *= 2;
    if (!storm) {
        std::unique_ptr<utils::SimpleThreadPool> pool;
        if (poolThreads > 0)
            pool = std::make_unique<utils::SimpleThreadPool>(size_t(poolThreads));
        double const before = peakMiB();
        bool ok = false;
        double writeMs = 0, saveMs = 0;
        double const ms = timeMs([&] {
            mpq::CreateOptions create;
            create.hashTableSize = hashSize;
            auto storage = mpq::Storage::create(create, pool.get());
            mpq::WriteOptions opts;
            opts.compression = compression;
            writeMs = timeMs([&] {
                for (const auto& [name, data] : files)
                    storage.writeFile(name, data, opts);
            });
            saveMs = timeMs([&] { ok = storage.save(dst); });
        });
        row("create", "ours", tag + ":total_ms", ms);
        row("create", "ours", tag + ":writeFile_ms", writeMs);
        row("create", "ours", tag + ":save_ms", saveMs);
        row("create", "ours", tag + ":ok", ok);
        row("create", "ours", tag + ":out_MiB", mib(fileSize(dst)));
        row("create", "ours", tag + ":in_MiB_per_s", mib(total) / (ms / 1000.0));
        row("create", "ours", tag + ":peak_added_MiB", peakMiB() - before);
        return ok ? 0 : 1;
    }
    bool ok = true;
    double const ms = timeMs([&] {
        HANDLE h = nullptr;
        if (!storm->CreateArchive(wide(dst).c_str(), kStormCreateListfile | kStormCreateAttributes, hashSize, &h)) {
            ok = false;
            return;
        }
        for (const auto& [name, data] : files) {
            HANDLE hf = nullptr;
            if (!storm->CreateFile_(h, name.c_str(), 0, static_cast<DWORD>(data.size()), 0,
                                    stormMask ? kStormFileCompress : 0, &hf)) {
                ok = false;
                continue;
            }
            ok = storm->WriteFile_(hf, data.data(), static_cast<DWORD>(data.size()), stormMask) && ok;
            ok = storm->FinishFile(hf) && ok;
        }
        storm->CloseArchive(h);
    });
    row("create", "storm", tag + ":total_ms", ms);
    row("create", "storm", tag + ":ok", ok);
    row("create", "storm", tag + ":out_MiB", mib(fileSize(dst)));
    row("create", "storm", tag + ":in_MiB_per_s", mib(total) / (ms / 1000.0));
    return 0;
}

// ---------------------------------------------------------------- names: open, list, look up

static int benchNames(const std::string& path, Storm* storm, int reps) {
    g_archive = baseName(path);
    {
        std::vector<double> times;
        for (int i = 0; i < reps * 4 + 2; ++i)
            times.push_back(timeMs([&] {
                auto s = mpq::Storage::open(path);
                if (!s)
                    std::exit(3);
            }));
        std::sort(times.begin(), times.end());
        row("open", "ours", "min_ms", times.front());
        row("open", "ours", "median_ms", times[times.size() / 2]);
    }
    // The first listing of a fresh Storage reads the (listfile); later ones do not.
    std::vector<std::string> names;
    double first = 1e300, later = 1e300;
    for (int i = 0; i < reps * 2; ++i) {
        auto fresh = mpq::Storage::open(path);
        first = std::min(first, timeMs([&] { names = fresh->listFiles(); }));
        later = std::min(later, timeMs([&] { names = fresh->listFiles(); }));
    }
    row("list", "ours", "first_ms", first);
    row("list", "ours", "later_ms", later);
    row("list", "ours", "names", double(names.size()));
    auto storage = mpq::Storage::open(path);
    if (names.empty())
        return 0;
    std::vector<std::string> misses;
    for (const auto& n : names)
        misses.push_back(n + ".zz");
    int const rounds = std::max(1, int(2000000 / names.size()));
    double const per = 1e6 / (double(rounds) * double(names.size()));
    u64 hits = 0;
    double ms = bestMs(reps, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names)
                hits += storage->fileExists(n);
    });
    row("lookup", "ours", "hit_ns", ms * per);
    ms = bestMs(reps, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& m : misses)
                hits += storage->fileExists(m);
    });
    row("lookup", "ours", "miss_ns", ms * per);
    ms = bestMs(reps, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names)
                hits += storage->fileInfo(n).has_value();
    });
    row("lookup", "ours", "fileInfo_ns", ms * per);
    // The same with an overlay pending: every lookup asks it first.
    {
        auto edited = mpq::Storage::open(path);
        u8 const byte = 1;
        edited->writeFile("bench\\pending.bin", std::span<const u8>(&byte, 1));
        ms = bestMs(reps, [&] {
            for (int r = 0; r < rounds; ++r)
                for (const auto& n : names)
                    hits += edited->fileExists(n);
        });
        row("lookup", "ours", "hit_overlay_ns", ms * per);
        row("list", "ours", "overlay_ms", bestMs(reps, [&] { names = edited->listFiles(); }));
    }
    g_sink = static_cast<u32>(hits);
    if (!storm)
        return 0;
    HANDLE h = nullptr;
    if (!storm->OpenArchive(wide(path).c_str(), 0, kStormReadOnly, &h))
        return 5;
    ms = bestMs(reps, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& n : names)
                hits += storm->HasFile(h, n.c_str());
    });
    row("lookup", "storm", "hit_ns", ms * per);
    ms = bestMs(reps, [&] {
        for (int r = 0; r < rounds; ++r)
            for (const auto& m : misses)
                hits += storm->HasFile(h, m.c_str());
    });
    row("lookup", "storm", "miss_ns", ms * per);
    storm->CloseArchive(h);
    g_sink = static_cast<u32>(hits);
    return 0;
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    if (argc < 3)
        return 2;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    calibrate();
    std::string const mode = argv[1];
    std::vector<std::string> args;
    std::string stormDll;
    int reps = 5, threads = int(std::thread::hardware_concurrency()), pool = 0;
    u64 capMiB = 48;
    std::string codec = "zlib";
    for (int i = 2; i < argc; ++i) {
        std::string const a = argv[i];
        if (a == "--storm" && i + 1 < argc)
            stormDll = argv[++i];
        else if (a == "--quick")
            g_quick = true;
        else if (a == "--reps" && i + 1 < argc)
            reps = std::atoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc)
            threads = std::atoi(argv[++i]);
        else if (a == "--pool" && i + 1 < argc)
            pool = std::atoi(argv[++i]);
        else if (a == "--cap" && i + 1 < argc)
            capMiB = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--codec" && i + 1 < argc)
            codec = argv[++i];
        else
            args.push_back(a);
    }
    Storm storm;
    if (!stormDll.empty() && !storm.load(wide(stormDll))) {
        std::fprintf(stderr, "%s did not load\n", stormDll.c_str());
        return 7;
    }
    Storm* s = stormDll.empty() ? nullptr : &storm;
    if (mode == "archive")
        return benchArchive(args[0], s, reps, threads);
    if (mode == "names")
        return benchNames(args[0], s, reps);
    if (mode == "readcmp")
        return s ? benchReadCompare(args[0], storm, reps) : 7;
    if (mode == "codecs")
        return benchCodecs(args, s, capMiB << 20);
    if (mode == "encode")
        return benchEncode(args[0], s, capMiB << 20);
    if (mode == "micro")
        return benchMicro(args[0]);
    if (mode == "resave" && args.size() >= 2)
        return benchResave(args[0], args[1], s);
    if (mode == "copy" && args.size() >= 2)
        return benchCopy(args[0], args[1]);
    if (mode == "add1" && args.size() >= 2)
        return benchAdd1(args[0], args[1], s);
    if (mode == "create" && args.size() >= 3)
        return benchCreate(args[0], args[1], std::strtoull(args[2].c_str(), nullptr, 10) << 20, s, pool, codec);
    return 2;
}
