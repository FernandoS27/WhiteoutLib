// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file binary_read_visitor.h
 * @brief The one generic reader — §11.2.
 *
 * The mirror of `BinaryWriteVisitor`, driven by the same `reflect()`. Reading is
 * the half where `since()` earns its keep: the writer always emits and stamps
 * the chunk version, so it is the *reader* that has to decide, per chunk, which
 * trailing fields are actually in the stream. `currentVersion_` is that decision,
 * and it is per chunk rather than per file because a v3.1 writer stamps each
 * chunk with its own `max_version` — an old chunk inside a new file stays old.
 */

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/reflect.h>
#include "../../common/binary_reader.h"
#include "chunk_tags.h"

#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

class BinaryReadVisitor {
public:
    explicit BinaryReadVisitor(common::BinaryReader& reader) : reader_(reader) {}
    /// True on the one visitor that fills the object rather than describing it.
    /// Almost nothing needs it; the exception is a struct with derived state,
    /// which has to rebuild or drop that state after its stored half arrives.
    static constexpr bool kReading = true;

    /// Reads the root chunk into @p root. @p maxVersion is the highest file
    /// version this build understands; a newer file is reported, not rejected.
    template <class Root>
    void read(Root& root, u32 maxVersion);

    const std::vector<std::string>& issues() const {
        return issues_;
    }

    /// Chunks this build did not recognise, plus any whose tag the caller asked
    /// to skip. Both are the same thing to the writer, and that is the point:
    /// §11.3's "a wasm build without D3 can still read the geometry" and §11.4's
    /// "a v3.0 tool opening a v3.2 file" are one mechanism, not two.
    const std::vector<UnknownChunk>& unknownChunks() const {
        return unknown_;
    }

    /// Skip every chunk carrying @p tag, as a build compiled without that format
    /// would. The chunk is preserved rather than parsed, and the record that
    /// pointed at it keeps its default value.
    void skipTag(u32 tag) {
        skipped_.push_back(tag);
    }

    bool isSkipped(u32 tag) const {
        for (const u32 entry : skipped_) {
            if (entry == tag) {
                return true;
            }
        }
        return false;
    }

    // ---- the visitor surface (§11.2) ----

    template <class T>
    void field(const char*, T& value) {
        constexpr FieldKind kind = ClassifyField<T>();
        if constexpr (kind == FieldKind::Enum) {
            value = static_cast<T>(reader_.read<std::underlying_type_t<T>>());
        } else if constexpr (kind == FieldKind::Bool) {
            value = reader_.read<u8>() != 0;
        } else if constexpr (kind == FieldKind::String) {
            readString(value);
        } else if constexpr (kind == FieldKind::Vector) {
            readVector(value);
        } else if constexpr (kind == FieldKind::Reflected) {
            value.reflect(*this);
        } else {
            value = reader_.read<T>();
        }
    }

    template <class T>
    void optional(const char* name, std::optional<T>& value) {
        if (reader_.read<u8>() == 0) {
            value.reset();
            return;
        }
        value.emplace();
        field(name, *value);
    }

    template <class T>
    void inlineList(const char* name, std::vector<T>& value) {
        const u32 count = reader_.read<u32>();
        value.clear();
        value.resize(count);
        for (u32 i = 0; i < count; ++i) {
            field(name, value[i]);
        }
    }

    /// The mirror of the writer's `chunk`. A tag mismatch is reported and the
    /// record left at its default rather than parsed as the wrong type.
    template <class T>
    void chunk(const char* name, T& value) {
        u32 slot = 0;
        if (openChunk<T>(name, slot)) {
            readChunkBody(index_[slot], value);
        }
    }

    /// One alternative of a variant, read as a chunk.
    ///
    /// The alternative is constructed **after** the decision to read it, never
    /// before. Constructing it first -- which is what a plain
    /// `chunk(name, VariantAs<Alt>(value))` does, since the argument is
    /// evaluated at the call -- turns a skipped block into a default-constructed
    /// one. That is not an absent block; it is a wrong block, and the next write
    /// puts it where the real one was.
    template <class Alt, class Variant>
    void chunkAlternative(const char* name, Variant& value, u32& slot) {
        if (!openChunk<Alt>(name, slot)) {
            return;
        }
        readChunkBody(index_[slot], VariantAs<Alt>(value));
    }

    /// Reads an inline element count and sizes @p container to it, ready for the
    /// caller's loop over the elements. The mirror of the writer's `count`.
    template <class C>
    void count(const char*, C& container) {
        const u32 size = reader_.read<u32>();
        container.clear();
        container.resize(size);
    }

    /// Gates the fields that follow on the chunk version being read. A field the
    /// chunk predates is left at its default and, crucially, not read — it is
    /// not in the stream to read.
    class Gate {
    public:
        Gate(BinaryReadVisitor& visitor, bool present) : visitor_(visitor), present_(present) {}

        template <class T>
        void field(const char* name, T& value) {
            if (present_) {
                visitor_.field(name, value);
            }
        }

        template <class T>
        void optional(const char* name, std::optional<T>& value) {
            if (present_) {
                visitor_.optional(name, value);
            }
        }

        template <class T>
        void inlineList(const char* name, std::vector<T>& value) {
            if (present_) {
                visitor_.inlineList(name, value);
            }
        }

    private:
        BinaryReadVisitor& visitor_;
        bool present_;
    };

    Gate since(u32 version) {
        return Gate(*this, currentVersion_ >= version);
    }

private:
    /// Resolves a chunk reference, reporting the ways it can fail to resolve,
    /// and always leaving @p slot as the referenced index so a caller that
    /// declines can still put the reference back.
    template <class T>
    bool openChunk(const char* name, u32& slot) {
        const Reference ref = reader_.read<Reference>();
        slot = ref.index;
        if (ref.entries == 0) {
            return false;
        }
        if (ref.index >= index_.size()) {
            issues_.push_back(std::string("Chunk reference out of bounds for ") + name);
            return false;
        }
        const IndexEntry& entry = index_[ref.index];
        if (entry.tag != ChunkTagTraits<T>::value) {
            issues_.push_back(std::string("Tag mismatch for chunk ") + name);
            return false;
        }
        // Not an error and not a loss: the chunk is already in `unknown_`, and
        // the reference to it is re-emitted pointing at the same slot.
        return !isSkipped(entry.tag);
    }

    template <class T>
    void readChunkBody(const IndexEntry& entry, T& value) {
        const auto savedPos = reader_.getPosition();
        const u32 savedVersion = currentVersion_;
        currentVersion_ = entry.version;
        reader_.setPosition(entry.offset);
        value.reflect(*this);
        currentVersion_ = savedVersion;
        reader_.setPosition(savedPos);
    }

    template <class T>
    void readVector(std::vector<T>& container);
    void readString(std::string& text);

    void collectUnknownChunks(u32 indexOffset);

    std::vector<IndexEntry> index_;
    common::BinaryReader& reader_;
    std::vector<std::string> issues_;
    std::vector<UnknownChunk> unknown_;
    std::vector<u32> skipped_;
    u32 currentVersion_ = 0;
};

template <class Root>
void BinaryReadVisitor::read(Root& root, u32 maxVersion) {
    issues_.clear();

    const WEMHeader header = reader_.read<WEMHeader>();
    if (header.magic != kWoemMagic) {
        issues_.push_back("Invalid WEM file: expected magic 'WOEM'");
        return;
    }
    if (header.version > maxVersion) {
        issues_.push_back("WEM version " + std::to_string(header.version) +
                          " is newer than supported version " + std::to_string(maxVersion));
    }

    reader_.setPosition(header.indexOffset);
    index_ = reader_.read<std::vector<IndexEntry>>(header.indexCount);

    // Before any record is parsed, because a record that fails to parse must not
    // take the chunks around it down with it.
    collectUnknownChunks(header.indexOffset);

    if (header.documentRef.entries == 0) {
        issues_.push_back("WEM file has no model data");
        return;
    }
    if (header.documentRef.index >= index_.size()) {
        issues_.push_back("WEM model reference index out of bounds");
        return;
    }

    const IndexEntry& entry = index_[header.documentRef.index];
    currentVersion_ = entry.version;
    reader_.setPosition(entry.offset);
    root.reflect(*this);
}

template <class T>
void BinaryReadVisitor::readVector(std::vector<T>& container) {
    const Reference ref = reader_.read<Reference>();
    if (ref.entries == 0) {
        container.clear();
        return;
    }

    if (ref.index >= index_.size()) {
        issues_.push_back("Reference index out of bounds: " + std::to_string(ref.index));
        container.clear();
        return;
    }

    const IndexEntry& entry = index_[ref.index];
    if (entry.tag != ChunkTagTraits<T>::value) {
        issues_.push_back("Tag mismatch in reference: expected " +
                          std::to_string(ChunkTagTraits<T>::value) + " got " +
                          std::to_string(entry.tag));
    }

    const auto savedPos = reader_.getPosition();
    const u32 savedVersion = currentVersion_;
    currentVersion_ = entry.version;
    reader_.setPosition(entry.offset);

    if constexpr (ChunkTagTraits<T>::is_trivial) {
        container = reader_.read<std::vector<T>>(ref.entries);
    } else {
        container.clear();
        container.resize(ref.entries);
        for (u32 i = 0; i < ref.entries; ++i) {
            container[i].reflect(*this);
        }
    }

    currentVersion_ = savedVersion;
    reader_.setPosition(savedPos);
}

} // namespace wem
} // namespace models
} // namespace whiteout
