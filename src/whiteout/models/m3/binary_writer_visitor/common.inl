// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

namespace detail {

template <typename T, typename = void>
struct has_get_version : std::false_type {};

template <typename T>
struct has_get_version<T, std::void_t<decltype(std::declval<T&>().getVersion())>>
    : std::true_type {};

/// The version to stamp on @p value's index entry.
///
/// A chunk a parser stamped keeps what it was read with — that is the whole of
/// "save it back the way it came". Only one a conversion invented arrives with
/// nothing to say, and it gets the newest layout this writer can spell, capped
/// at what both clients read: writing the -1 those used to carry made the file
/// unloadable in either game, because the version validator rejects the whole
/// model on any version above its descriptor table's.
///
/// `Engine::Both` rather than the export's own engine because the two differ on
/// four tags only, and a converter that means one of them says so on the chunk
/// itself (`m3_core::pushStandard`) — nothing reaches here still undecided.
template <typename T>
u32 getStructureVersion(const T& value) {
    if constexpr (has_get_version<T>::value) {
        const i32 stated = value.getVersion();
        if (stated >= 0) {
            return static_cast<u32>(stated);
        }
    }
    return CurrentChunkVersion(ChunkTagTraits<T>::value, ChunkTagTraits<T>::max_version,
                               Engine::Both);
}

} // namespace detail

template <typename T>
void BinaryWriterVisitor::visit(const AnimBlock<T>& block, u32 version) {
    (void)version;
    visit(block.timestamps);
    writer.write(block.flags);
    writer.write(block.endFrame);
    visit(block.keys);
}

template <typename T>
void BinaryWriterVisitor::visit(const std::vector<T>& container) {
    Reference ref = {};
    if (container.empty()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &container]() {
        auto new_entry_index = indexTable.size();
        auto version = detail::getStructureVersion(*container.begin());
        const auto currentOffset = writer.getPosition();
        indexTable.push_back({ChunkTagTraits<T>::value, currentOffset,
                              static_cast<u32>(container.size()), version});
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = static_cast<u32>(container.size());
        ref.index = static_cast<u32>(new_entry_index);
        writer.write(ref);
        writer.setPosition(entry.offset);
        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer.write(container);
        } else {
            for (auto& element : container) {
                visit(element, entry.version);
                transferDeferredWrites();
            }
        }
        writer.AlignTo(16, 0xAA);
    });
}

template <typename T>
void BinaryWriterVisitor::visit(const std::optional<T>& container) {
    Reference ref = {};
    if (!container.has_value()) {
        writer.write(ref);
        return;
    }

    auto ref_position = writer.getPosition();
    writer.write(ref);
    currentLevelWrites.push_back([this, ref_position, &container]() {
        auto new_entry_index = indexTable.size();
        auto version = detail::getStructureVersion(*container);
        const auto currentOffset = writer.getPosition();
        indexTable.push_back({ChunkTagTraits<T>::value, currentOffset, 1, version});
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = 1;
        ref.index = static_cast<u32>(new_entry_index);
        writer.write(ref);
        writer.setPosition(entry.offset);
        if constexpr (ChunkTagTraits<T>::is_trivial) {
            writer.write(*container);
        } else {
            visit(*container, entry.version);
            transferDeferredWrites();
        }
        writer.AlignTo(16, 0xAA);
    });
}
