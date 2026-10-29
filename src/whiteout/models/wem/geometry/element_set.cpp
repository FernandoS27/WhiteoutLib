// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/geometry/element_set.h>

#include <algorithm>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {

void ElementSet::normalise() {
    for (std::vector<u32>* list : {&vertices, &edges, &faces}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
}

} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
