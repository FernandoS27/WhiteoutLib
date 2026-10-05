// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include <whiteout/common_types.h>

namespace whiteout {
namespace models {
namespace wem {
namespace geom {
namespace fracture {

/// Runs @p work(i) for i in [0, count) on @p threads workers (0: one per
/// hardware thread). Each item writes only its own slot, so the result does
/// not depend on the schedule.
template <class F>
void Parallel(std::size_t count, u32 threads, F&& work) {
    const u32 wanted = threads != 0 ? threads : std::max(1u, std::thread::hardware_concurrency());
    const u32 used = static_cast<u32>(std::min<std::size_t>(wanted, count));
    std::atomic<std::size_t> nextItem{0};
    const auto run = [&] {
        for (std::size_t i = nextItem++; i < count; i = nextItem++) {
            work(i);
        }
    };
    if (used <= 1) {
        run();
        return;
    }
    std::vector<std::thread> pool;
    for (u32 t = 0; t < used; ++t) {
        pool.emplace_back(run);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

} // namespace fracture
} // namespace geom
} // namespace wem
} // namespace models
} // namespace whiteout
