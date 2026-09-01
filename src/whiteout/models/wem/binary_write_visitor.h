// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

/**
 * @file binary_write_visitor.h
 * @brief The one generic writer — §11.2.
 *
 * Everything about the *container* is the hand-written writer's, unchanged: the
 * same header, the same reserved index slots 0 and 1, the same deferred-write
 * queue with breadth-first level transfer, the same 16-byte `0xAA` alignment.
 * What is generic is only the per-record body, which now comes from `reflect()`
 * instead of from a `visit()` overload per struct.
 *
 * The queue discipline is the subtle part and is reproduced exactly: a nested
 * reference appends to `currentLevel_`, and a level is transferred to the *front*
 * of `deferred_` in reverse, which is what keeps same-level chunks contiguous.
 * Get that wrong and every offset in the file moves while every value stays
 * right — precisely the failure the byte-level parity test exists to catch,
 * because no structural round trip would notice it.
 */

#include <whiteout/models/wem/document.h>
#include <whiteout/models/wem/reflect.h>
#include "../../common/binary_writer.h"
#include "chunk_tags.h"

#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace whiteout {
namespace models {
namespace wem {

class BinaryWriteVisitor {
public:
    explicit BinaryWriteVisitor(common::BinaryWriter& writer) : writer_(writer) {}
    static constexpr bool kReading = false;

    /// Writes @p root as the file's root chunk, stamping @p formatVersion into
    /// the header. The root's own chunk version is its `ChunkTagTraits`.
    ///
    /// @p unknown is re-emitted verbatim (§11.4), each chunk back into the
    /// index-table slot it came from.
    template <class Root>
    void write(const Root& root, u32 formatVersion, std::span<const UnknownChunk> unknown = {});

    /// Preserved chunks nothing this build wrote points at. Not an error and not
    /// a loss -- they are still in the file -- but a chunk reachable only from
    /// another preserved chunk is worth saying out loud, because that is the
    /// shape data goes missing in one version later.
    const std::vector<u32>& unreferencedUnknownSlots() const {
        return unreferenced_;
    }

    // ---- the visitor surface (§11.2) ----

    template <class T>
    void field(const char*, const T& value) {
        constexpr FieldKind kind = ClassifyField<T>();
        if constexpr (kind == FieldKind::Enum) {
            writer_.write(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (kind == FieldKind::Bool) {
            writer_.write<u8>(value ? 1 : 0);
        } else if constexpr (kind == FieldKind::String) {
            writeString(value);
        } else if constexpr (kind == FieldKind::Vector) {
            writeVector(value);
        } else if constexpr (kind == FieldKind::Reflected) {
            // `reflect()` is non-const because the reader writes through it. The
            // writer only reads, so this is a signature detail, not a mutation.
            const_cast<T&>(value).reflect(*this);
        } else {
            writer_.write(value);
        }
    }

    template <class T>
    void optional(const char* name, const std::optional<T>& value) {
        writer_.write<u8>(value.has_value() ? 1 : 0);
        if (value.has_value()) {
            field(name, *value);
        }
    }

    /// One record written as a chunk of its own, referenced inline.
    ///
    /// This is what makes a material kind or a feature payload *skippable*
    /// (§11.3): the record carries a fixed 12-byte `Reference` whether or not the
    /// reader understands what is on the other end, so a reader that has never
    /// heard of `PBRDeferred` still parses the material header and the features
    /// around it. Written inline, an unknown body would have no length and the
    /// whole record would be unreadable.
    template <class T>
    void chunk(const char* name, const T& value) {
        u32 ignored = 0;
        chunkInto(name, value, ignored);
    }

    /// One alternative of a variant, written as a chunk -- and the one case
    /// where the *reader* may decline. If @p value does not hold @p Alt, this
    /// build could not read the block on the way in: the chunk itself is
    /// preserved at the container level, and what has to be put back here is
    /// the reference to it, pointing at the slot it still occupies.
    template <class Alt, class Variant>
    void chunkAlternative(const char* name, Variant& value, u32& slot) {
        if (std::holds_alternative<Alt>(value)) {
            chunkInto(name, std::get<Alt>(value), slot);
            return;
        }
        Reference ref{};
        ref.entries = 1;
        ref.index = slot;
        writer_.write(ref);
        noteReference(slot);
    }

    template <class T>
    void chunkInto(const char*, const T& value, u32& slot) {
        const auto refPos = writer_.getPosition();
        writer_.write(Reference{});

        currentLevel_.push_back([this, refPos, &value, &slot]() {
            const auto entryIndex = static_cast<u32>(index_.size());
            const auto offset = writer_.getPosition();
            index_.emplace_back(
                IndexEntry{ChunkTagTraits<T>::value, offset, 1, ChunkTagTraits<T>::max_version});
            slot = entryIndex;

            writer_.setPosition(refPos);
            Reference ref{};
            ref.entries = 1;
            ref.index = entryIndex;
            writer_.write(ref);
            noteReference(entryIndex);
            writer_.setPosition(offset);

            const_cast<T&>(value).reflect(*this);
            transferLevel();
            writer_.AlignTo(16, 0xAA);
        });
    }

    /// An inline `u32` count followed by one `Reference` per element — the shape
    /// `Mesh::uvSets` has, and the one case a plain `field()` cannot express
    /// because a vector of vectors has no chunk tag of its own.
    template <class T>
    void inlineList(const char* name, const std::vector<T>& value) {
        writer_.write<u32>(static_cast<u32>(value.size()));
        for (const T& element : value) {
            field(name, element);
        }
    }

    /// Emits a container's element count inline, for a run whose elements are
    /// then visited in place rather than referenced as a chunk. `std::pair`
    /// members are the reason it exists: a pair has no `reflect()` and no chunk
    /// tag, so the slot tables spell their two halves out themselves.
    template <class C>
    void count(const char*, const C& container) {
        writer_.write<u32>(static_cast<u32>(container.size()));
    }

    /// The writer always emits; the chunk version it stamps is what tells a
    /// reader whether to expect the field.
    BinaryWriteVisitor& since(u32) {
        return *this;
    }

private:
    template <class T>
    void writeVector(const std::vector<T>& container);
    void writeString(const std::string& text);
    void transferLevel();

    /// Records every index slot an emitted `Reference` names, so the orphan
    /// report above can be computed without a second pass over the file.
    void noteReference(u32 slot) {
        referenced_.push_back(slot);
    }

    std::deque<std::function<void()>> currentLevel_;
    std::deque<std::function<void()>> deferred_;
    std::vector<IndexEntry> index_;
    std::vector<u32> referenced_;
    std::vector<u32> unreferenced_;
    common::BinaryWriter& writer_;
};

template <class Root>
void BinaryWriteVisitor::write(const Root& root, u32 formatVersion,
                               std::span<const UnknownChunk> unknown) {
    index_.clear();
    deferred_.clear();
    currentLevel_.clear();
    referenced_.clear();
    unreferenced_.clear();

    WEMHeader header{};
    header.magic = kWoemMagic;
    header.version = formatVersion;
    writer_.write(header);

    index_.emplace_back(IndexEntry{kWoemMagic, 0, 1, 0});

    const u32 rootIndex = static_cast<u32>(index_.size());
    index_.emplace_back(IndexEntry{chunkTag<Root>, 0, 1, ChunkTagTraits<Root>::max_version});

    header.documentRef.entries = 1;
    header.documentRef.index = rootIndex;
    noteReference(rootIndex);

    // Hold every preserved slot open *before* anything else allocates one. A
    // `Reference` inside a preserved chunk names a slot number, and those bytes
    // cannot be rewritten -- this build has no idea where the references inside
    // them are -- so the numbering has to be the one they were written against.
    // Slots in the reserved range that no preserved chunk claims stay as holes;
    // an index entry nothing points at costs 16 bytes and confuses nobody.
    for (const UnknownChunk& chunk : unknown) {
        while (index_.size() <= chunk.index) {
            index_.emplace_back(IndexEntry{kHoleTag, 0, 0, 0});
        }
    }

    deferred_.push_back([this, &root, rootIndex]() {
        index_[rootIndex].offset = writer_.getPosition();
        const_cast<Root&>(root).reflect(*this);
        writer_.AlignTo(16, 0xAA);
        transferLevel();
    });

    while (!deferred_.empty()) {
        auto emit = std::move(deferred_.front());
        deferred_.pop_front();
        emit();
    }

    for (const UnknownChunk& chunk : unknown) {
        const u32 offset = writer_.getPosition();
        index_[chunk.index] = IndexEntry{chunk.tag, offset, chunk.count, chunk.version};
        writer_.writeBytes(reinterpret_cast<const char*>(chunk.data.data()),
                           static_cast<u32>(chunk.data.size()));
        // The captured bytes already carry their own alignment fill, so aligning
        // again here would insert a second run of it and move every chunk after.
    }

    for (const UnknownChunk& chunk : unknown) {
        bool seen = false;
        for (const u32 slot : referenced_) {
            if (slot == chunk.index) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unreferenced_.push_back(chunk.index);
        }
    }

    const u32 indexOffset = writer_.getPosition();
    writer_.write(index_);

    header.indexOffset = indexOffset;
    header.indexCount = static_cast<u32>(index_.size());
    writer_.setPosition(0);
    writer_.write(header);

    writer_.setPosition(indexOffset + static_cast<u32>(index_.size()) * sizeof(IndexEntry));
}

template <class T>
void BinaryWriteVisitor::writeVector(const std::vector<T>& container) {
    Reference const nullRef{};
    if (container.empty()) {
        writer_.write(nullRef);
        return;
    }

    const auto refPos = writer_.getPosition();
    writer_.write(nullRef); // placeholder, backpatched below

    currentLevel_.push_back([this, refPos, &container]() {
        const auto entryIndex = static_cast<u32>(index_.size());
        const auto offset = writer_.getPosition();
        index_.emplace_back(IndexEntry{ChunkTagTraits<T>::value, offset,
                                       static_cast<u32>(container.size()),
                                       ChunkTagTraits<T>::max_version});

        writer_.setPosition(refPos);
        Reference ref{};
        ref.entries = static_cast<u32>(container.size());
        ref.index = entryIndex;
        writer_.write(ref);
        noteReference(entryIndex);
        writer_.setPosition(offset);

        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer_.write(container);
        } else {
            for (const T& element : container) {
                const_cast<T&>(element).reflect(*this);
                transferLevel();
            }
        }
        writer_.AlignTo(16, 0xAA);
    });
}

} // namespace wem
} // namespace models
} // namespace whiteout
