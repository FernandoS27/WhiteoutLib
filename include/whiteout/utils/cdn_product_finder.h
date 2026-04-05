// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace whiteout::interfaces { class HttpHandler; }

namespace whiteout::utils {

/// Version information for one region of a CDN product.
struct CdnProductVersion {
    std::string region;         ///< e.g. "us", "eu", "kr", "cn", "tw".
    std::string buildConfig;    ///< Build config key (hex MD5).
    std::string cdnConfig;      ///< CDN config key (hex MD5).
    uint32_t    buildId = 0;    ///< Numeric build ID (0 if not available).
    std::string versionName;    ///< Human-readable version string.
};

/// CDN host information for one region of a CDN product.
struct CdnProductCdn {
    std::string              region;     ///< e.g. "us", "eu", "kr".
    std::string              path;       ///< CDN data path, e.g. "tpr/wow".
    std::vector<std::string> hosts;      ///< CDN hostnames.
    std::string              configPath; ///< Config path, e.g. "tpr/configs".
};

/// A single product entry discovered on the Blizzard CDN.
struct CdnProductInfo {
    std::string product;     ///< Product code (e.g. "wow", "fenris", "hero").
    uint32_t    seqn = 0;   ///< Sequence number from the summary endpoint.
    std::string flags;       ///< Flags column from the summary (often empty).

    /// Populated only when CdnProductFinderOptions::queryVersions is true.
    std::vector<CdnProductVersion> versions;

    /// Populated only when CdnProductFinderOptions::queryCdns is true.
    std::vector<CdnProductCdn> cdns;
};

/// Options for findCdnProducts().
struct CdnProductFinderOptions {
    /// HTTP handler — REQUIRED. Must be thread-safe.
    interfaces::HttpHandler* http = nullptr;

    /// Region to query (e.g. "us", "eu"). Used for the summary URL and
    /// optional per-product version/cdn queries.
    std::string region = "us";

    /// When true, also query /v2/products/{product}/versions for each
    /// product discovered in the summary. Significantly increases the
    /// number of HTTP requests.
    bool queryVersions = false;

    /// When true, also query /v2/products/{product}/cdns for each
    /// product discovered in the summary.
    bool queryCdns = false;

    /// Optional progress callback. Called with (completed, total) counts
    /// during per-product detail fetches. Return false to cancel.
    std::function<bool(uint32_t completed, uint32_t total)> progressCallback;
};

/// Discovers all product repositories available on Blizzard's CDN by
/// querying the Ribbit v2 summary endpoint
/// (`https://{region}.version.battle.net/v2/summary`).
///
/// Each entry in the returned vector corresponds to a TACT product.
/// When `queryVersions` or `queryCdns` is set in the options, the
/// function additionally queries the per-product version/CDN endpoints
/// and populates the corresponding fields in each result.
///
/// Returns an empty vector on network failure or if no handler is provided.
///
/// Example:
///   CdnProductFinderOptions opts;
///   opts.http = &myHttpHandler;
///   opts.queryVersions = true;
///   auto products = utils::findCdnProducts(opts);
///   for (auto& p : products)
///       std::printf("%s  seqn=%u  ver=%s\n", p.product.c_str(), p.seqn,
///                   p.versions.empty() ? "?" : p.versions[0].versionName.c_str());
std::vector<CdnProductInfo> findCdnProducts(const CdnProductFinderOptions& opts);

} // namespace whiteout::utils
