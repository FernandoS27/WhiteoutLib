// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <algorithm>
#include <cmath>

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

NativeBag::Entry& NativeBag::slot(const std::string& name) {
    for (Entry& entry : entries) {
        if (entry.name == name) {
            return entry;
        }
    }
    entries.push_back(Entry{name, 0, {}});
    return entries.back();
}

void NativeBag::set(const std::string& name, i64 value) {
    slot(name).value = value;
}

void NativeBag::setText(const std::string& name, std::string text) {
    slot(name).text = std::move(text);
}

i64 NativeBag::value(const std::string& name, i64 fallback) const {
    const Entry* entry = find(name);
    return entry == nullptr ? fallback : entry->value;
}

const std::string& NativeBag::text(const std::string& name) const {
    static const std::string kEmpty;
    const Entry* entry = find(name);
    return entry == nullptr ? kEmpty : entry->text;
}

void SetMilli(NativeBag& bag, const std::string& name, f32 value) {
    // A float off a file, so neither finite nor small: `llround` of either is
    // undefined.
    const f64 milli = std::isfinite(value) ? static_cast<f64>(value) * 1000.0 : 0.0;
    bag.set(name, static_cast<i64>(std::llround(std::clamp(milli, -1e15, 1e15))));
}

f32 Milli(const NativeBag& bag, const std::string& name, f32 fallback) {
    const NativeBag::Entry* entry = bag.find(name);
    return entry == nullptr ? fallback : static_cast<f32>(entry->value) / 1000.0f;
}

} // namespace wem
} // namespace models
} // namespace whiteout
