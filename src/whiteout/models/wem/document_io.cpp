// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/writer.h>

#include "../../common/binary_reader.h"
#include "../../common/binary_writer.h"
#include "../../common/streams.h"
#include "../../common/unicode_path.h"

#include "binary_read_visitor.h"
#include "binary_write_visitor.h"

#include <fstream>

namespace whiteout {
namespace models {
namespace wem {

namespace {

u32 TagOfNativeKind(NativeKind kind) {
    switch (kind) {
    case NativeKind::Mdx:
        return ChunkTagTraits<native::MdxMaterial>::value;
    case NativeKind::M2:
        return ChunkTagTraits<native::M2Material>::value;
    case NativeKind::M3:
        return ChunkTagTraits<native::M3Material>::value;
    case NativeKind::D3:
        return ChunkTagTraits<native::D3Material>::value;
    case NativeKind::None:
        break;
    }
    return 0;
}

std::vector<u8> ReadWholeFile(const std::string& filePath) {
    auto file = common::open_ifstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

} // namespace

// ============================================================================
// IsWemFile
// ============================================================================

bool IsWemFile(std::span<const u8> bytes, u32* versionOut) {
    if (bytes.size() < sizeof(WEMHeader)) {
        return false;
    }
    WEMHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kWoemMagic) {
        return false;
    }
    if (versionOut != nullptr) {
        *versionOut = header.version;
    }
    return header.version <= kCurrentVersion;
}

// ============================================================================
// Parser
// ============================================================================

class Parser::Impl {
public:
    Diagnostics diagnostics;
    std::vector<UnknownChunk> unknown;

    std::optional<Document> parse(std::span<const u8> bytes, const ReadOptions& options) {
        diagnostics.clear();
        unknown.clear();

        u32 version = 0;
        if (!IsWemFile(bytes, &version)) {
            diagnostics.error(DiagCode::UnsupportedVersion, "not a WEM file this build can read");
            return std::nullopt;
        }

        if (version != kCurrentVersion) {
            diagnostics.error(DiagCode::UnsupportedVersion,
                              "WEM version " + std::to_string(version) + "; this build reads " +
                                  std::to_string(kCurrentVersion));
            return std::nullopt;
        }

        common::span_streambuf streambuf(bytes);
        std::istream in(&streambuf);
        common::BinaryReader reader(in);

        BinaryReadVisitor visitor(reader);
        for (const NativeKind kind : options.skipNativeKinds) {
            const u32 tag = TagOfNativeKind(kind);
            if (tag != 0) {
                visitor.skipTag(tag);
            }
        }

        Document document;
        visitor.read(document, kCurrentVersion);
        unknown = visitor.unknownChunks();
        // §11.4 rides on the document, not on the parser: an edit-and-write
        // round trip preserves unknown chunks by doing nothing, and dropping
        // them takes clearing this vector.
        document.unknownChunks = unknown;

        for (const std::string& issue : visitor.issues()) {
            diagnostics.warn(DiagCode::ConnectivityCorrupt, issue);
        }
        for (const UnknownChunk& chunk : unknown) {
            diagnostics.info(DiagCode::UnknownChunkPreserved,
                             "preserved chunk at slot " + std::to_string(chunk.index) + " (" +
                                 std::to_string(chunk.data.size()) + " bytes)");
        }

        if (document.models.empty() && !visitor.issues().empty()) {
            return std::nullopt;
        }
        return document;
    }
};

Parser::Parser() : impl_(std::make_unique<Impl>()) {}
Parser::~Parser() = default;
Parser::Parser(Parser&&) noexcept = default;
Parser& Parser::operator=(Parser&&) noexcept = default;

std::optional<Document> Parser::parse(const std::string& filePath, const ReadOptions& options) {
    const std::vector<u8> bytes = ReadWholeFile(filePath);
    if (bytes.empty()) {
        impl_->diagnostics.clear();
        impl_->diagnostics.error(DiagCode::UnsupportedVersion, "cannot read " + filePath);
        return std::nullopt;
    }
    return impl_->parse(bytes, options);
}

std::optional<Document> Parser::parse(std::span<const u8> bytes, const ReadOptions& options) {
    return impl_->parse(bytes, options);
}

const std::vector<UnknownChunk>& Parser::unknownChunks() const {
    return impl_->unknown;
}

const Diagnostics& Parser::diagnostics() const {
    return impl_->diagnostics;
}

// ============================================================================
// Writer
// ============================================================================

class Writer::Impl {
public:
    Diagnostics diagnostics;

    std::vector<u8> write(const Document& document) {
        diagnostics.clear();

        std::vector<u8> buffer;
        buffer.reserve(256 * 1024);
        common::vector_streambuf streambuf(buffer);
        std::ostream out(&streambuf);
        common::BinaryWriter writer(out);

        BinaryWriteVisitor visitor(writer);
        visitor.write(document, kCurrentVersion, document.unknownChunks);

        // §11.4's stated limitation, reported rather than hidden: a preserved
        // chunk nothing this build wrote points at is still in the file, but it
        // is reachable only from another preserved chunk.
        for (const u32 slot : visitor.unreferencedUnknownSlots()) {
            diagnostics.warn(DiagCode::OrphanChunk,
                             "preserved chunk at slot " + std::to_string(slot) +
                                 " is not referenced by anything this build wrote");
        }

        buffer.shrink_to_fit();
        return buffer;
    }
};

Writer::Writer() : impl_(std::make_unique<Impl>()) {}
Writer::~Writer() = default;
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

std::vector<u8> Writer::write(const Document& document) {
    return impl_->write(document);
}

bool Writer::write(const std::string& filePath, const Document& document) {
    const std::vector<u8> bytes = impl_->write(document);
    auto file = common::open_ofstream(filePath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

const Diagnostics& Writer::diagnostics() const {
    return impl_->diagnostics;
}

} // namespace wem
} // namespace models
} // namespace whiteout
