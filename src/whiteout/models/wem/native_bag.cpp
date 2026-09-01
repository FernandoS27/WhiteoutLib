// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/wem/native_bag.h"

namespace whiteout {
namespace models {
namespace wem {

const NativeBag::Entry* NativeBag::find(const std::string& name) const {
    for (const Entry& entry : entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

void NativeBag::set(const std::string& name, i64 value) {
    for (Entry& entry : entries) {
        if (entry.name == name) {
            entry.value = value;
            return;
        }
    }
    entries.push_back(Entry{name, value});
}

i64 NativeBag::value(const std::string& name, i64 fallback) const {
    const Entry* entry = find(name);
    return entry == nullptr ? fallback : entry->value;
}

} // namespace wem
} // namespace models
} // namespace whiteout
