// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// HAND-WRITTEN (not codegen). Factory for CDN-backed CASC storage —
// Storage::openOnline takes an OnlineOpenOptions whose HttpHandler* and
// std::function progress callback the codegen can't marshal, so it goes
// through the whiteout_casc_shim_openOnline shim.

using Whiteout.Casc.Internal;
using Whiteout.Host;

namespace Whiteout.Casc;

/// <summary>Opens CDN-backed (online) CASC storage. The returned
/// <see cref="Storage"/> exposes the same read API as a local one.</summary>
public static class OnlineStorage
{
    /// <summary>Open a CDN-backed storage.</summary>
    /// <param name="product">Product code (e.g. "wow", "w3", "d3", "fenris").</param>
    /// <param name="region">Region for version lookup ("us", "eu", "kr", ...); empty defaults to "us".</param>
    /// <param name="http">HTTP transport (e.g. <see cref="SimpleHttpHandler"/>); required.</param>
    /// <param name="buildKey">Optional hex build-config key; null selects the latest active build.</param>
    /// <param name="cacheDir">Optional on-disk cache directory; null keeps everything in memory.</param>
    /// <param name="localeMask">Locale filter (0 = accept all).</param>
    /// <param name="pool">Optional worker pool for parallel I/O.</param>
    /// <returns>A valid <see cref="Storage"/>, or null on failure.</returns>
    public static Storage? Open(
        string product,
        string region,
        HttpHandler http,
        string? buildKey = null,
        string? cacheDir = null,
        uint localeMask = 0,
        WorkerPool? pool = null)
    {
        ArgumentNullException.ThrowIfNull(http);
        IntPtr handle = NativeMethods.whiteout_casc_shim_openOnline(
            product,
            string.IsNullOrEmpty(region) ? "us" : region,
            buildKey ?? string.Empty,
            http.DangerousGet(),
            cacheDir ?? string.Empty,
            localeMask,
            pool?.DangerousGet() ?? IntPtr.Zero);
        return handle == IntPtr.Zero ? null : new Storage(handle);
    }
}
