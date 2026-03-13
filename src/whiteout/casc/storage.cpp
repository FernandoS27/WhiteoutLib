// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/casc/storage.h>

#include <CascLib.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace whiteout {
namespace casc {

namespace {

// ---------------------------------------------------------------------------
// Internal constants and conversions
// ---------------------------------------------------------------------------

constexpr u32 kOpenFlagsMask = 0xFFFFFFF0u;
constexpr u32 kSeekBegin = 0u;

using CascString = std::basic_string<TCHAR>;

size_t boundedStringLength(const char* text, size_t maxLength) {
    size_t count = 0;
    while (count < maxLength && text[count] != '\0') {
        ++count;
    }
    return count;
}

size_t boundedTStringLength(const TCHAR* text, size_t maxLength) {
    size_t count = 0;
    while (count < maxLength && text[count] != 0) {
        ++count;
    }
    return count;
}

CascString toCascString(const std::string& value) {
#if defined(_UNICODE) || defined(CASCLIB_UNICODE)
    CascString result;
    result.reserve(value.size());
    for (char ch : value) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }
    return result;
#else
    return value;
#endif
}

std::string fromCascString(const TCHAR* value) {
    if (value == nullptr) {
        return {};
    }

#if defined(_UNICODE) || defined(CASCLIB_UNICODE)
    std::string result;
    while (*value != 0) {
        const wchar_t wc = *value++;
        if (wc >= 0 && wc <= 0x7F) {
            result.push_back(static_cast<char>(wc));
        } else {
            result.push_back('?');
        }
    }
    return result;
#else
    return std::string(value);
#endif
}

// ---------------------------------------------------------------------------
// Internal metadata conversion helpers
// ---------------------------------------------------------------------------

u32 makeOpenFlags(u32 openFlags, u32 openType) {
    return (openFlags & kOpenFlagsMask) | openType;
}

void copyMd5(std::array<u8, 16>& target, const BYTE* source) {
    std::copy(source, source + target.size(), target.begin());
}

FileNameType toFileNameType(CASC_NAME_TYPE nameType) {
    switch (nameType) {
    case CascNameFull:
        return FileNameType::Full;
    case CascNameDataId:
        return FileNameType::DataId;
    case CascNameCKey:
        return FileNameType::CKey;
    case CascNameEKey:
        return FileNameType::EKey;
    default:
        return FileNameType::Unknown;
    }
}

FindEntry toFindEntry(const CASC_FIND_DATA& data) {
    FindEntry entry;
    entry.name = data.szFileName;
    if (data.szPlainName != nullptr) {
        entry.plainName = data.szPlainName;
    }
    copyMd5(entry.cKey, data.CKey);
    copyMd5(entry.eKey, data.EKey);
    entry.tagBitMask = static_cast<u64>(data.TagBitMask);
    entry.fileSize = static_cast<u64>(data.FileSize);
    entry.fileDataId = static_cast<u32>(data.dwFileDataId);
    entry.localeFlags = static_cast<u32>(data.dwLocaleFlags);
    entry.contentFlags = static_cast<u32>(data.dwContentFlags);
    entry.spanCount = static_cast<u32>(data.dwSpanCount);
    entry.fileAvailable = data.bFileAvailable != 0;
    entry.nameType = toFileNameType(data.NameType);
    return entry;
}

FileFullInfo toFileFullInfo(const CASC_FILE_FULL_INFO& data) {
    FileFullInfo info;
    copyMd5(info.cKey, data.CKey);
    copyMd5(info.eKey, data.EKey);
    info.dataFileName.assign(data.DataFileName,
                             boundedStringLength(data.DataFileName, sizeof(data.DataFileName)));
    info.storageOffset = static_cast<u64>(data.StorageOffset);
    info.segmentOffset = static_cast<u64>(data.SegmentOffset);
    info.tagBitMask = static_cast<u64>(data.TagBitMask);
    info.fileNameHash = static_cast<u64>(data.FileNameHash);
    info.contentSize = static_cast<u64>(data.ContentSize);
    info.encodedSize = static_cast<u64>(data.EncodedSize);
    info.segmentIndex = static_cast<u32>(data.SegmentIndex);
    info.spanCount = static_cast<u32>(data.SpanCount);
    info.fileDataId = static_cast<u32>(data.FileDataId);
    info.localeFlags = static_cast<u32>(data.LocaleFlags);
    info.contentFlags = static_cast<u32>(data.ContentFlags);
    return info;
}

FileSpanInfo toFileSpanInfo(const CASC_FILE_SPAN_INFO& data) {
    FileSpanInfo info;
    copyMd5(info.cKey, data.CKey);
    copyMd5(info.eKey, data.EKey);
    info.startOffset = static_cast<u64>(data.StartOffset);
    info.endOffset = static_cast<u64>(data.EndOffset);
    info.archiveIndex = static_cast<u32>(data.ArchiveIndex);
    info.archiveOffset = static_cast<u32>(data.ArchiveOffs);
    info.headerSize = static_cast<u32>(data.HeaderSize);
    info.frameCount = static_cast<u32>(data.FrameCount);
    return info;
}

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE handle = nullptr) : m_handle(handle) {}
    ~ScopedFileHandle() {
        if (m_handle != nullptr) {
            CascCloseFile(m_handle);
        }
    }

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    ScopedFileHandle(ScopedFileHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }

    ScopedFileHandle& operator=(ScopedFileHandle&& other) noexcept {
        if (this != &other) {
            if (m_handle != nullptr) {
                CascCloseFile(m_handle);
            }
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    HANDLE get() const {
        return m_handle;
    }

private:
    HANDLE m_handle;
};

class ScopedFindHandle {
public:
    explicit ScopedFindHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~ScopedFindHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CascFindClose(m_handle);
        }
    }

    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

    HANDLE get() const {
        return m_handle;
    }

private:
    HANDLE m_handle;
};

// ---------------------------------------------------------------------------
// Internal CASC I/O helpers
// ---------------------------------------------------------------------------

bool openFileByPath(HANDLE storage, const std::string& cascPath, u32 localeFlags, u32 openFlags,
                    HANDLE* outFile) {
    const auto cascPathArg = toCascString(cascPath);

    return CascOpenFile(storage, cascPathArg.c_str(), static_cast<DWORD>(localeFlags),
                        static_cast<DWORD>(makeOpenFlags(openFlags, CASC_OPEN_BY_NAME)), outFile);
}

bool openFileById(HANDLE storage, i32 fileId, u32 localeFlags, u32 openFlags, HANDLE* outFile) {
    return CascOpenFile(storage, CASC_FILE_DATA_ID(fileId), static_cast<DWORD>(localeFlags),
                        static_cast<DWORD>(makeOpenFlags(openFlags, CASC_OPEN_BY_FILEID)), outFile);
}

std::optional<u64> queryFileSize(HANDLE fileHandle) {
    ULONGLONG size64 = 0;
    if (!CascGetFileSize64(fileHandle, &size64)) {
        return std::nullopt;
    }
    return static_cast<u64>(size64);
}

bool seekAbsolute(HANDLE fileHandle, u64 offset) {
    ULONGLONG newPos = 0;
    if (!CascSetFilePointer64(fileHandle, static_cast<LONGLONG>(offset), &newPos,
                              static_cast<DWORD>(kSeekBegin))) {
        return false;
    }
    return static_cast<u64>(newPos) == offset;
}

std::optional<std::vector<u8>> readFromCurrentPosition(HANDLE fileHandle, size_t bytesToRead) {
    std::vector<u8> buffer(bytesToRead);
    size_t totalRead = 0;

    while (totalRead < bytesToRead) {
        const size_t remaining = bytesToRead - totalRead;
        const DWORD chunkSize = static_cast<DWORD>(
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));

        DWORD bytesRead = 0;
        if (!CascReadFile(fileHandle, buffer.data() + totalRead, chunkSize, &bytesRead)) {
            return std::nullopt;
        }

        totalRead += static_cast<size_t>(bytesRead);
        if (bytesRead == 0) {
            break;
        }
    }

    buffer.resize(totalRead);
    return buffer;
}

std::optional<std::vector<u8>> readWholeFile(HANDLE fileHandle) {
    const auto size = queryFileSize(fileHandle);
    if (!size) {
        return std::nullopt;
    }

    if (*size > static_cast<u64>(std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }

    return readFromCurrentPosition(fileHandle, static_cast<size_t>(*size));
}

std::optional<u32> queryStorageU32(HANDLE storageHandle, CASC_STORAGE_INFO_CLASS infoClass) {
    u32 value = 0;
    if (!CascGetStorageInfo(storageHandle, infoClass, &value, sizeof(value), nullptr)) {
        return std::nullopt;
    }
    return value;
}

} // namespace

u32 Storage::lastError() noexcept {
    return static_cast<u32>(GetCascError());
}

// ---------------------------------------------------------------------------
// Construction / destruction / move
// ---------------------------------------------------------------------------

Storage::~Storage() {
    close();
}

Storage::Storage(Storage&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = nullptr;
}

Storage& Storage::operator=(Storage&& other) noexcept {
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

bool Storage::open(const std::string& path) {
    return open(path, LocaleMasks::None);
}

bool Storage::open(const std::string& path, u32 localeMask) {
    close();

    const auto cascPath = toCascString(path);

    HANDLE hStorage = nullptr;
    if (!CascOpenStorage(cascPath.c_str(), static_cast<DWORD>(localeMask), &hStorage)) {
        return false;
    }
    m_handle = hStorage;
    return true;
}

bool Storage::openOnline(const std::string& codeName) {
    return openOnline(codeName, LocaleMasks::None);
}

bool Storage::openOnline(const std::string& codeName, u32 localeMask) {
    close();

    const auto cascCodeName = toCascString(codeName);

    HANDLE hStorage = nullptr;
    if (!CascOpenOnlineStorage(cascCodeName.c_str(), static_cast<DWORD>(localeMask), &hStorage)) {
        return false;
    }
    m_handle = hStorage;
    return true;
}

bool Storage::openEx(const OpenStorageOptions& options) {
    close();

    if (options.path.empty() && options.codeName.empty()) {
        return false;
    }

    const auto pathArg = toCascString(options.path);
    const auto codeNameArg = toCascString(options.codeName);
    const auto regionArg = toCascString(options.region);
    const auto buildKeyArg = toCascString(options.buildKey);
    const auto cdnHostArg = toCascString(options.cdnHostUrl);

    const TCHAR* params = !options.path.empty() ? pathArg.c_str() : codeNameArg.c_str();

    CASC_OPEN_STORAGE_ARGS args{};
    args.Size = sizeof(args);
    args.szLocalPath = options.path.empty() ? nullptr : pathArg.c_str();
    args.szCodeName = options.codeName.empty() ? nullptr : codeNameArg.c_str();
    args.szRegion = options.region.empty() ? nullptr : regionArg.c_str();
    args.dwLocaleMask = static_cast<DWORD>(options.localeMask);
    args.dwFlags = static_cast<DWORD>(options.flags);
    args.szBuildKey = options.buildKey.empty() ? nullptr : buildKeyArg.c_str();
    args.szCdnHostUrl = options.cdnHostUrl.empty() ? nullptr : cdnHostArg.c_str();

    HANDLE hStorage = nullptr;
    if (!CascOpenStorageEx(params, &args, options.online, &hStorage)) {
        return false;
    }

    m_handle = hStorage;
    return true;
}

void Storage::close() {
    if (m_handle) {
        CascCloseStorage(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
}

// ---------------------------------------------------------------------------
// File reading
// ---------------------------------------------------------------------------

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath) const {
    return readFile(cascPath, LocaleMasks::None, FileOpenFlags::None);
}

std::optional<std::vector<u8>> Storage::readFile(const std::string& cascPath, u32 localeFlags,
                                                 u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    return readWholeFile(scopedFile.get());
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId) const {
    return readFile(fileId, LocaleMasks::None, FileOpenFlags::None);
}

std::optional<std::vector<u8>> Storage::readFile(i32 fileId, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    return readWholeFile(scopedFile.get());
}

std::optional<std::vector<u8>> Storage::readFileRange(const std::string& cascPath, u64 offset,
                                                      size_t byteCount, u32 localeFlags,
                                                      u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    const auto size = queryFileSize(scopedFile.get());
    if (!size || offset > *size) {
        return std::nullopt;
    }

    if (offset == *size || byteCount == 0) {
        return std::vector<u8>{};
    }

    if (!seekAbsolute(scopedFile.get(), offset)) {
        return std::nullopt;
    }

    const u64 available = *size - offset;
    const u64 requested = std::min<u64>(available, static_cast<u64>(byteCount));
    if (requested > static_cast<u64>(std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }

    return readFromCurrentPosition(scopedFile.get(), static_cast<size_t>(requested));
}

std::optional<std::vector<u8>> Storage::readFileRange(i32 fileId, u64 offset, size_t byteCount,
                                                      u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    const auto size = queryFileSize(scopedFile.get());
    if (!size || offset > *size) {
        return std::nullopt;
    }

    if (offset == *size || byteCount == 0) {
        return std::vector<u8>{};
    }

    if (!seekAbsolute(scopedFile.get(), offset)) {
        return std::nullopt;
    }

    const u64 available = *size - offset;
    const u64 requested = std::min<u64>(available, static_cast<u64>(byteCount));
    if (requested > static_cast<u64>(std::numeric_limits<size_t>::max())) {
        return std::nullopt;
    }

    return readFromCurrentPosition(scopedFile.get(), static_cast<size_t>(requested));
}

std::optional<u64> Storage::fileSize(const std::string& cascPath, u32 localeFlags,
                                     u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    return queryFileSize(scopedFile.get());
}

std::optional<u64> Storage::fileSize(i32 fileId, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    return queryFileSize(scopedFile.get());
}

// ---------------------------------------------------------------------------
// Existence check
// ---------------------------------------------------------------------------

bool Storage::fileExists(const std::string& cascPath) const {
    return fileExists(cascPath, LocaleMasks::None, FileOpenFlags::None);
}

bool Storage::fileExists(const std::string& cascPath, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return false;
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return false;
    }
    ScopedFileHandle scopedFile(hFile);
    return true;
}

bool Storage::fileExists(i32 fileId) const {
    return fileExists(fileId, LocaleMasks::None, FileOpenFlags::None);
}

bool Storage::fileExists(i32 fileId, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return false;
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return false;
    }
    ScopedFileHandle scopedFile(hFile);
    return true;
}

std::optional<FileFullInfo> Storage::fileInfo(const std::string& cascPath, u32 localeFlags,
                                              u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    CASC_FILE_FULL_INFO rawInfo{};
    if (!CascGetFileInfo(scopedFile.get(), CascFileFullInfo, &rawInfo, sizeof(rawInfo), nullptr)) {
        return std::nullopt;
    }

    return toFileFullInfo(rawInfo);
}

std::optional<FileFullInfo> Storage::fileInfo(i32 fileId, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return std::nullopt;
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return std::nullopt;
    }
    ScopedFileHandle scopedFile(hFile);

    CASC_FILE_FULL_INFO rawInfo{};
    if (!CascGetFileInfo(scopedFile.get(), CascFileFullInfo, &rawInfo, sizeof(rawInfo), nullptr)) {
        return std::nullopt;
    }

    return toFileFullInfo(rawInfo);
}

std::vector<FileSpanInfo> Storage::fileSpans(const std::string& cascPath, u32 localeFlags,
                                             u32 openFlags) const {
    if (!m_handle) {
        return {};
    }

    HANDLE hFile = nullptr;
    if (!openFileByPath(static_cast<HANDLE>(m_handle), cascPath, localeFlags, openFlags, &hFile)) {
        return {};
    }
    ScopedFileHandle scopedFile(hFile);

    CASC_FILE_FULL_INFO rawInfo{};
    if (!CascGetFileInfo(scopedFile.get(), CascFileFullInfo, &rawInfo, sizeof(rawInfo), nullptr)) {
        return {};
    }

    if (rawInfo.SpanCount == 0) {
        return {};
    }

    std::vector<CASC_FILE_SPAN_INFO> rawSpans(static_cast<size_t>(rawInfo.SpanCount));
    if (!CascGetFileInfo(scopedFile.get(), CascFileSpanInfo, rawSpans.data(),
                         rawSpans.size() * sizeof(CASC_FILE_SPAN_INFO), nullptr)) {
        return {};
    }

    std::vector<FileSpanInfo> spans;
    spans.reserve(rawSpans.size());
    for (const auto& rawSpan : rawSpans) {
        spans.push_back(toFileSpanInfo(rawSpan));
    }
    return spans;
}

std::vector<FileSpanInfo> Storage::fileSpans(i32 fileId, u32 localeFlags, u32 openFlags) const {
    if (!m_handle) {
        return {};
    }

    HANDLE hFile = nullptr;
    if (!openFileById(static_cast<HANDLE>(m_handle), fileId, localeFlags, openFlags, &hFile)) {
        return {};
    }
    ScopedFileHandle scopedFile(hFile);

    CASC_FILE_FULL_INFO rawInfo{};
    if (!CascGetFileInfo(scopedFile.get(), CascFileFullInfo, &rawInfo, sizeof(rawInfo), nullptr)) {
        return {};
    }

    if (rawInfo.SpanCount == 0) {
        return {};
    }

    std::vector<CASC_FILE_SPAN_INFO> rawSpans(static_cast<size_t>(rawInfo.SpanCount));
    if (!CascGetFileInfo(scopedFile.get(), CascFileSpanInfo, rawSpans.data(),
                         rawSpans.size() * sizeof(CASC_FILE_SPAN_INFO), nullptr)) {
        return {};
    }

    std::vector<FileSpanInfo> spans;
    spans.reserve(rawSpans.size());
    for (const auto& rawSpan : rawSpans) {
        spans.push_back(toFileSpanInfo(rawSpan));
    }
    return spans;
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

void Storage::enumerate(const std::string& listFilePath,
                        std::function<bool(const std::string& name)> callback) const {
    enumerate("*", listFilePath, std::move(callback));
}

void Storage::enumerate(const std::string& mask, const std::string& listFilePath,
                        std::function<bool(const std::string& name)> callback) const {
    if (!callback) {
        return;
    }

    enumerateEntries(mask, listFilePath,
                     [&callback](const FindEntry& entry) { return callback(entry.name); });
}

void Storage::enumerateEntries(const std::string& listFilePath,
                               std::function<bool(const FindEntry& entry)> callback) const {
    enumerateEntries("*", listFilePath, std::move(callback));
}

void Storage::enumerateEntries(const std::string& mask, const std::string& listFilePath,
                               std::function<bool(const FindEntry& entry)> callback) const {
    if (!m_handle || !callback) {
        return;
    }

    const auto listFileArg = toCascString(listFilePath);
    const auto maskArg = toCascString(mask);
    const TCHAR* listFile = listFilePath.empty() ? nullptr : listFileArg.c_str();

    CASC_FIND_DATA findData{};
    HANDLE rawFind =
        CascFindFirstFile(static_cast<HANDLE>(m_handle), maskArg.c_str(), &findData, listFile);
    if (rawFind == INVALID_HANDLE_VALUE) {
        return;
    }

    ScopedFindHandle hFind(rawFind);

    do {
        if (!callback(toFindEntry(findData))) {
            break;
        }
    } while (CascFindNextFile(hFind.get(), &findData));
}

std::vector<std::string> Storage::listFiles(const std::string& mask,
                                            const std::string& listFilePath) const {
    std::vector<std::string> names;
    enumerate(mask, listFilePath, [&names](const std::string& name) {
        names.push_back(name);
        return true;
    });
    return names;
}

std::vector<FindEntry> Storage::listEntries(const std::string& mask,
                                            const std::string& listFilePath) const {
    std::vector<FindEntry> entries;
    enumerateEntries(mask, listFilePath, [&entries](const FindEntry& entry) {
        entries.push_back(entry);
        return true;
    });
    return entries;
}

// ---------------------------------------------------------------------------
// Storage information
// ---------------------------------------------------------------------------

std::optional<u32> Storage::localFileCount() const {
    if (!m_handle) {
        return std::nullopt;
    }
    return queryStorageU32(static_cast<HANDLE>(m_handle), CascStorageLocalFileCount);
}

std::optional<u32> Storage::totalFileCount() const {
    if (!m_handle) {
        return std::nullopt;
    }
    return queryStorageU32(static_cast<HANDLE>(m_handle), CascStorageTotalFileCount);
}

std::optional<u32> Storage::features() const {
    if (!m_handle) {
        return std::nullopt;
    }
    return queryStorageU32(static_cast<HANDLE>(m_handle), CascStorageFeatures);
}

std::optional<StorageProduct> Storage::product() const {
    if (!m_handle) {
        return std::nullopt;
    }

    CASC_STORAGE_PRODUCT productInfo{};
    if (!CascGetStorageInfo(static_cast<HANDLE>(m_handle), CascStorageProduct, &productInfo,
                            sizeof(productInfo), nullptr)) {
        return std::nullopt;
    }

    StorageProduct product;
    product.codeName.assign(
        productInfo.szCodeName,
        boundedStringLength(productInfo.szCodeName, sizeof(productInfo.szCodeName)));
    product.buildNumber = static_cast<u32>(productInfo.BuildNumber);
    return product;
}

std::optional<std::string> Storage::pathProduct() const {
    if (!m_handle) {
        return std::nullopt;
    }

    size_t needed = 0;
    CascGetStorageInfo(static_cast<HANDLE>(m_handle), CascStoragePathProduct, nullptr, 0, &needed);

    if (needed == 0) {
        needed = 1024;
    }

    std::vector<TCHAR> buffer(needed + 1, static_cast<TCHAR>(0));
    if (!CascGetStorageInfo(static_cast<HANDLE>(m_handle), CascStoragePathProduct, buffer.data(),
                            buffer.size() * sizeof(TCHAR), &needed)) {
        return std::nullopt;
    }

    const size_t length = boundedTStringLength(buffer.data(), buffer.size());
    buffer[length] = 0;
    return fromCascString(buffer.data());
}

std::vector<StorageTag> Storage::tags() const {
    if (!m_handle) {
        return {};
    }

    size_t needed = 0;
    CascGetStorageInfo(static_cast<HANDLE>(m_handle), CascStorageTags, nullptr, 0, &needed);
    if (needed == 0) {
        return {};
    }

    std::vector<u8> rawBuffer(needed);
    if (!CascGetStorageInfo(static_cast<HANDLE>(m_handle), CascStorageTags, rawBuffer.data(),
                            rawBuffer.size(), &needed)) {
        return {};
    }

    if (rawBuffer.size() < offsetof(CASC_STORAGE_TAGS, Tags)) {
        return {};
    }

    const auto* rawTags = reinterpret_cast<const CASC_STORAGE_TAGS*>(rawBuffer.data());
    const size_t bytesForTags = rawBuffer.size() - offsetof(CASC_STORAGE_TAGS, Tags);
    const size_t maxTagCount = bytesForTags / sizeof(CASC_STORAGE_TAG);
    const size_t tagCount = std::min(rawTags->TagCount, maxTagCount);

    std::vector<StorageTag> results;
    results.reserve(tagCount);
    for (size_t i = 0; i < tagCount; ++i) {
        const CASC_STORAGE_TAG& rawTag = rawTags->Tags[i];
        StorageTag tag;
        if (rawTag.szTagName != nullptr) {
            if (rawTag.TagNameLength > 0) {
                tag.name.assign(rawTag.szTagName, rawTag.TagNameLength);
            } else {
                tag.name.assign(rawTag.szTagName);
            }
        }
        tag.value = static_cast<u32>(rawTag.TagValue);
        results.push_back(std::move(tag));
    }

    return results;
}

// ---------------------------------------------------------------------------
// Encryption key helpers
// ---------------------------------------------------------------------------

bool Storage::addEncryptionKey(u64 keyName, const std::array<u8, 16>& key) {
    if (!m_handle) {
        return false;
    }

    return CascAddEncryptionKey(static_cast<HANDLE>(m_handle), static_cast<ULONGLONG>(keyName),
                                const_cast<LPBYTE>(key.data()));
}

bool Storage::addEncryptionKey(u64 keyName, const std::string& keyHex) {
    if (!m_handle) {
        return false;
    }

    const auto keyHexArg = toCascString(keyHex);

    return CascAddStringEncryptionKey(static_cast<HANDLE>(m_handle),
                                      static_cast<ULONGLONG>(keyName), keyHexArg.c_str());
}

bool Storage::importKeysFromString(const std::string& keyList) {
    if (!m_handle) {
        return false;
    }

    const auto keyListArg = toCascString(keyList);

    return CascImportKeysFromString(static_cast<HANDLE>(m_handle), keyListArg.c_str());
}

bool Storage::importKeysFromFile(const std::string& keyFilePath) {
    if (!m_handle) {
        return false;
    }

    const auto cascPath = toCascString(keyFilePath);
    return CascImportKeysFromFile(static_cast<HANDLE>(m_handle), cascPath.c_str());
}

std::optional<std::array<u8, 16>> Storage::findEncryptionKey(u64 keyName) const {
    if (!m_handle) {
        return std::nullopt;
    }

    LPBYTE key =
        CascFindEncryptionKey(static_cast<HANDLE>(m_handle), static_cast<ULONGLONG>(keyName));
    if (key == nullptr) {
        return std::nullopt;
    }

    std::array<u8, 16> keyCopy{};
    std::copy(key, key + keyCopy.size(), keyCopy.begin());
    return keyCopy;
}

std::optional<u64> Storage::notFoundEncryptionKey() const {
    if (!m_handle) {
        return std::nullopt;
    }

    ULONGLONG keyName = 0;
    if (!CascGetNotFoundEncryptionKey(static_cast<HANDLE>(m_handle), &keyName)) {
        return std::nullopt;
    }

    return static_cast<u64>(keyName);
}

// ---------------------------------------------------------------------------
// CDN helpers
// ---------------------------------------------------------------------------

std::string Storage::defaultCdnHost() {
    const TCHAR* host = CascCdnGetDefault();
    return fromCascString(host);
}

std::optional<std::vector<u8>> Storage::downloadFromCdn(const std::string& cdnHostUrl,
                                                        const std::string& product,
                                                        const std::string& fileName) {
    const auto cascCdnHost = toCascString(cdnHostUrl);
    const auto cascProduct = toCascString(product);
    const auto cascFileName = toCascString(fileName);

    DWORD dataSize = 0;
    LPBYTE data =
        CascCdnDownload(cascCdnHost.c_str(), cascProduct.c_str(), cascFileName.c_str(), &dataSize);
    if (data == nullptr) {
        return std::nullopt;
    }

    std::vector<u8> result(static_cast<size_t>(dataSize));
    std::copy(data, data + result.size(), result.begin());
    CascCdnFree(data);
    return result;
}

} // namespace casc
} // namespace whiteout
