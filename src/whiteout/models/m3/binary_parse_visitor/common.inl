// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

namespace detail {

template <typename T, typename = void>
struct has_set_version : std::false_type {};

template <typename T>
struct has_set_version<T, std::void_t<decltype(std::declval<T&>().setVersion(std::declval<i32>()))>>
    : std::true_type {};

template <typename T>
void setStructureVersion(T& value, u32 version) {
    if constexpr (has_set_version<T>::value) {
        value.setVersion(static_cast<i32>(version));
    }
}

} // namespace detail

template <typename T>
void BinaryParseVisitor::visit(AnimBlock<T>& block, u32 version) {
    (void)version;
    visit(block.timestamps);
    block.flags = reader.read<u32>();
    block.endFrame = reader.read<u32>();
    visit(block.keys);
}

template <typename T>
void BinaryParseVisitor::visit(std::vector<T>& container) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        container.clear();
        return;
    }

    indexUsed[ref.index] = true;
    assert(indexTable[ref.index].tag == ChunkTagTraits<T>::value);
    const auto currentPos = reader.getPosition();
    reader.setPosition(indexTable[ref.index].offset);
    const u32 version = indexTable[ref.index].version;
    if constexpr (ChunkTagTraits<T>::is_trivial) {
        container = reader.read<std::vector<T>>(ref.entries);
        if constexpr (detail::has_set_version<T>::value) {
            for (auto& element : container) {
                detail::setStructureVersion(element, version);
            }
        }
    } else {
        container.clear();
        container.reserve(ref.entries);
        for (size_t i = 0; i < ref.entries; ++i) {
            container.emplace_back();
            T& element = container.back();
            detail::setStructureVersion(element, version);
            this->visit(element, version);
        }
    }
    reader.setPosition(currentPos);
}

template <typename T>
void BinaryParseVisitor::visit(std::optional<T>& container) {
    Reference ref = readReferenceFunc();
    if (ref.entries == 0) {
        container = std::nullopt;
        return;
    }

    assert(indexTable[ref.index].tag == ChunkTagTraits<T>::value);
    indexUsed[ref.index] = true;
    const auto currentPos = reader.getPosition();
    reader.setPosition(indexTable[ref.index].offset);
    if constexpr (std::is_trivially_copyable_v<T>) {
        container = reader.read<T>();
        if constexpr (detail::has_set_version<T>::value) {
            detail::setStructureVersion(*container, indexTable[ref.index].version);
        }
    } else {
        container.emplace();
        const u32 version = indexTable[ref.index].version;
        detail::setStructureVersion(*container, version);
        this->visit(*container, version);
    }
    reader.setPosition(currentPos);
}
