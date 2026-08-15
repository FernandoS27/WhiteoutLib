// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "db_internal.h"

#include <cstring>

namespace whiteout::database {

namespace {

const RowEntry* entryOf(const TableData* data, u32 index) {
    return data != nullptr ? data->rowAt(index) : nullptr;
}

} // namespace

u32 Row::id() const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? entry->id : 0;
}

u32 Row::section() const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? entry->section : 0;
}

bool Row::isCopy() const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr && entry->copy;
}

bool Row::isSparse() const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr && entry->sparse;
}

bool Row::isEncrypted() const {
    const RowEntry* entry = entryOf(data_, index_);
    if (entry == nullptr || entry->section >= data_->sections.size()) {
        return false;
    }
    return data_->sections[entry->section].encrypted;
}

std::optional<u32> Row::relationId() const {
    const RowEntry* entry = entryOf(data_, index_);
    if (entry == nullptr || entry->section >= data_->sections.size()) {
        return std::nullopt;
    }
    const SectionData& section = data_->sections[entry->section];
    if (section.relationKeyedById) {
        const auto found = section.relationById.find(entry->lookupId);
        return found != section.relationById.end() ? std::optional<u32>(found->second)
                                                   : std::nullopt;
    }
    if (entry->recordIndex < section.relationByRecord.size()) {
        const u32 value = section.relationByRecord[entry->recordIndex];
        return value != 0 ? std::optional<u32>(value) : std::nullopt;
    }
    return std::nullopt;
}

std::span<const u8> Row::raw() const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? data_->recordBytes(*entry) : std::span<const u8>{};
}

u64 Row::getUInt(u32 field, u32 arrayIndex) const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? data_->fieldValue(*entry, field, arrayIndex, false) : 0;
}

i64 Row::getInt(u32 field, u32 arrayIndex) const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? static_cast<i64>(data_->fieldValue(*entry, field, arrayIndex, true))
                            : 0;
}

f32 Row::getFloat(u32 field, u32 arrayIndex) const {
    const u32 raw = static_cast<u32>(getUInt(field, arrayIndex));
    f32 value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

std::string_view Row::getString(u32 field, u32 arrayIndex) const {
    const RowEntry* entry = entryOf(data_, index_);
    if (entry == nullptr) {
        return {};
    }
    const u32 raw = static_cast<u32>(data_->fieldValue(*entry, field, arrayIndex, false));
    return data_->resolveString(*entry, data_->fieldByteOffset(field, arrayIndex), raw);
}

Value Row::value(u32 column, u32 arrayIndex) const {
    const RowEntry* entry = entryOf(data_, index_);
    return entry != nullptr ? data_->columnValue(*entry, column, arrayIndex) : Value();
}

Value Row::value(std::string_view column, u32 arrayIndex) const {
    if (data_ == nullptr || !data_->schema.has_value()) {
        return Value();
    }
    const auto index = data_->schema->indexOf(column);
    return index.has_value() ? value(*index, arrayIndex) : Value();
}

std::string_view Row::locString(std::string_view column, Locale locale) const {
    return value(column, static_cast<u32>(locale)).asString();
}

} // namespace whiteout::database
