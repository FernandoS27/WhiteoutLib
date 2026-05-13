// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file bc7/encode_block.inl
/// @brief BC7 encode: partition search, encode_block dispatch, and encodeTexture entry point.

// ============================================================================
// Partition search helper (for 2-subset modes)
// ============================================================================

/// Quickly estimate partition error by computing colour bounding-box volume
/// per subset.  Smaller total volume ≈ better partition.
u64 estimate_partition_cost(const u8* rgba, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_2[partition].data();
    std::array<RGBA, 2> min_color = {{{255, 255, 255, 255}, {255, 255, 255, 255}}};
    std::array<RGBA, 2> max_color = {{{0, 0, 0, 0}, {0, 0, 0, 0}}};
    for (u32 i = 0; i < 16; ++i) {
        u8 s = part[i];
        auto p = pixel_at(rgba, i);
        min_color[s].r = std::min(min_color[s].r, p.r);
        min_color[s].g = std::min(min_color[s].g, p.g);
        min_color[s].b = std::min(min_color[s].b, p.b);
        max_color[s].r = std::max(max_color[s].r, p.r);
        max_color[s].g = std::max(max_color[s].g, p.g);
        max_color[s].b = std::max(max_color[s].b, p.b);
    }
    u64 cost = 0;
    for (u32 s = 0; s < 2; ++s) {
        u64 dr = max_color[s].r - min_color[s].r;
        u64 dg = max_color[s].g - min_color[s].g;
        u64 db = max_color[s].b - min_color[s].b;
        cost += dr * dr + dg * dg + db * db;
    }
    return cost;
}

/// Find the best partition index (out of 64) by quick cost estimate, then
/// return the top-N indices.
std::array<u32, 4> best_partitions(const u8* rgba, u32 top_n) {
    struct PC {
        u32 idx = 0;
        u64 cost = 0;
    };
    std::array<PC, 64> costs;
    for (u32 i = 0; i < 64; ++i)
        costs[i] = {i, estimate_partition_cost(rgba, i)};
    std::sort(costs.begin(), costs.end(), [](const PC& a, const PC& b) { return a.cost < b.cost; });
    std::array<u32, 4> result{};
    for (u32 i = 0; i < std::min(top_n, 4u); ++i)
        result[i] = costs[i].idx;
    return result;
}

// ============================================================================
// Partition search helper (for 3-subset modes)
// ============================================================================

/// Quickly estimate partition error for a 3-subset partition.
u64 estimate_partition_cost_3(const u8* rgba, u32 partition) {
    const u8* part = BC7_PARTITION_TABLE_3[partition].data();
    std::array<RGBA, 3> min_color = {{{255, 255, 255, 255}, {255, 255, 255, 255}, {255, 255, 255, 255}}};
    std::array<RGBA, 3> max_color = {{{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}};
    for (u32 i = 0; i < 16; ++i) {
        u8 s = part[i];
        auto p = pixel_at(rgba, i);
        min_color[s].r = std::min(min_color[s].r, p.r);
        min_color[s].g = std::min(min_color[s].g, p.g);
        min_color[s].b = std::min(min_color[s].b, p.b);
        max_color[s].r = std::max(max_color[s].r, p.r);
        max_color[s].g = std::max(max_color[s].g, p.g);
        max_color[s].b = std::max(max_color[s].b, p.b);
    }
    u64 cost = 0;
    for (u32 s = 0; s < 3; ++s) {
        u64 dr = max_color[s].r - min_color[s].r;
        u64 dg = max_color[s].g - min_color[s].g;
        u64 db = max_color[s].b - min_color[s].b;
        cost += dr * dr + dg * dg + db * db;
    }
    return cost;
}

/// Find the best 3-subset partition indices by quick cost estimate.
/// @param num_partitions  16 for mode 0, 64 for mode 2.
std::array<u32, 4> best_partitions_3(const u8* rgba, u32 num_partitions, u32 top_n) {
    struct PC {
        u32 idx = 0;
        u64 cost = 0;
    };
    std::array<PC, 64> costs;
    for (u32 i = 0; i < num_partitions; ++i)
        costs[i] = {i, estimate_partition_cost_3(rgba, i)};
    std::sort(costs.begin(), costs.begin() + num_partitions,
              [](const PC& a, const PC& b) { return a.cost < b.cost; });
    std::array<u32, 4> result{};
    for (u32 i = 0; i < std::min(top_n, 4u); ++i)
        result[i] = costs[i].idx;
    return result;
}

// ============================================================================
// Internal helpers
// ============================================================================

void encode_block(const u8* rgba, u8* out, Quality quality) {
    // Always try mode 6 (best all-rounder for RGBA)
    std::array<u8, 16> buf{};
    bool use_pca = quality >= Quality::Normal;
    u64 best_err = encode_mode6(rgba, buf.data(), use_pca);
    std::memcpy(out, buf.data(), 16);

    auto try_update = [&](u64 err) {
        if (err < best_err) {
            best_err = err;
            std::memcpy(out, buf.data(), 16);
        }
    };

    if (quality >= Quality::Normal) {
        // Try other 1-subset modes
        try_update(encode_mode5(rgba, buf.data(), use_pca));
        try_update(encode_mode4(rgba, buf.data(), use_pca));
    }

    if (quality >= Quality::High) {
        // Try 2-subset modes (1, 3, 7) with best partitions
        auto parts2 = best_partitions(rgba, 4);
        for (u32 t = 0; t < 4; ++t) {
            try_update(encode_mode1_partition(rgba, buf.data(), parts2[t]));
            try_update(encode_mode3_partition(rgba, buf.data(), parts2[t]));
            try_update(encode_mode7_partition(rgba, buf.data(), parts2[t]));
        }

        // Try 3-subset modes (0, 2)
        // Mode 0: 4-bit partition → 16 entries; mode 2: 6-bit → 64 entries
        auto parts3_m0 = best_partitions_3(rgba, 16, 4);
        for (u32 t = 0; t < 4; ++t)
            try_update(encode_mode0_partition(rgba, buf.data(), parts3_m0[t]));

        auto parts3_m2 = best_partitions_3(rgba, 64, 4);
        for (u32 t = 0; t < 4; ++t)
            try_update(encode_mode2_partition(rgba, buf.data(), parts3_m2[t]));
    }
}

} // anonymous namespace

// ============================================================================
// encodeTexture
// ============================================================================

std::optional<Texture> encodeTexture(const Texture& src, Quality quality, std::string* out_error,
                                     interfaces::WorkerPool* pool) {
    return transform_texture_impl(
        src, PixelFormat::RGBA8, PixelFormat::BC7, "bc7::encodeTexture",
        [quality, pool](std::span<const u8> data, u32 w, u32 h) {
            return encode_image_rgba8<16>(data, w, h,
                [quality](const u8* b, u8* o) { encode_block(b, o, quality); }, pool);
        },
        out_error);
}
