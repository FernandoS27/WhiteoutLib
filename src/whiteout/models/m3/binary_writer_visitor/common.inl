// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

namespace detail {

template <typename T, typename = void>
struct has_get_version : std::false_type {};

template <typename T>
struct has_get_version<T, std::void_t<decltype(std::declval<T&>().getVersion())>>
    : std::true_type {};

template <typename T>
u32 getStructureVersion(const T& value) {
    if constexpr (has_get_version<T>::value) {
        return value.getVersion();
    }
    return ChunkTagTraits<T>::max_version;
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
        indexTable.emplace_back(ChunkTagTraits<T>::value, currentOffset, container.size(), version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = container.size();
        ref.index = new_entry_index;
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
        indexTable.emplace_back(ChunkTagTraits<T>::value, currentOffset, 1, version);
        auto& entry = indexTable[new_entry_index];
        writer.setPosition(ref_position);
        Reference ref = {};
        ref.entries = 1;
        ref.index = new_entry_index;
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
