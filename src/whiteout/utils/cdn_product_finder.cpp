// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/interfaces.h>
#include <whiteout/utils/cdn_product_finder.h>

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace whiteout::utils {

// ============================================================================
// Helpers
// ============================================================================

static std::string_view trim(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return sv;
}

static std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        auto nl = text.find('\n');
        if (nl == std::string_view::npos) {
            if (!text.empty())
                lines.push_back(text);
            break;
        }
        lines.push_back(text.substr(0, nl));
        text.remove_prefix(nl + 1);
    }
    return lines;
}

static std::vector<std::string_view> split(std::string_view sv, char delim) {
    std::vector<std::string_view> parts;
    while (!sv.empty()) {
        auto pos = sv.find(delim);
        if (pos == std::string_view::npos) {
            parts.push_back(sv);
            break;
        }
        parts.push_back(sv.substr(0, pos));
        sv.remove_prefix(pos + 1);
    }
    return parts;
}

// ============================================================================
// Summary parser
// ============================================================================

/// Parse the Ribbit v2 summary response.
/// Format: pipe-separated TSV — "Product!STRING:0|Seqn!DEC:4|Flags!STRING:0\n"
/// followed by data rows like "wow|12345|\n".
static std::vector<CdnProductInfo> parseSummary(const std::vector<uint8_t>& data) {
    std::vector<CdnProductInfo> results;
    if (data.empty())
        return results;

    std::string_view const text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = splitLines(text);
    if (lines.size() < 2)
        return results;

    // Parse header to find column indices.
    auto headerCols = split(lines[0], '|');

    int iProduct = -1, iSeqn = -1, iFlags = -1;
    for (int i = 0; i < static_cast<int>(headerCols.size()); ++i) {
        auto col = trim(headerCols[i]);
        // Strip !TYPE:SIZE suffix.
        auto bang = col.find('!');
        if (bang != std::string_view::npos)
            col = col.substr(0, bang);
        if (col == "Product")
            iProduct = i;
        else if (col == "Seqn")
            iSeqn = i;
        else if (col == "Flags")
            iFlags = i;
    }

    if (iProduct < 0)
        return results;

    for (size_t row = 1; row < lines.size(); ++row) {
        auto line = trim(lines[row]);
        if (line.empty())
            continue;

        auto fields = split(line, '|');

        auto getField = [&](int idx) -> std::string_view {
            if (idx >= 0 && static_cast<size_t>(idx) < fields.size())
                return trim(fields[idx]);
            return {};
        };

        CdnProductInfo info;
        auto prod = getField(iProduct);
        if (prod.empty())
            continue;
        info.product = std::string(prod);

        if (iSeqn >= 0) {
            auto sv = getField(iSeqn);
            std::from_chars(sv.data(), sv.data() + sv.size(), info.seqn);
        }
        if (iFlags >= 0)
            info.flags = std::string(getField(iFlags));

        results.push_back(std::move(info));
    }

    return results;
}

// ============================================================================
// Per-product version parser
// ============================================================================

static std::vector<CdnProductVersion> parseVersions(const std::vector<uint8_t>& data) {
    std::vector<CdnProductVersion> results;
    if (data.empty())
        return results;

    std::string_view const text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = splitLines(text);
    if (lines.size() < 2)
        return results;

    auto headerCols = split(lines[0], '|');
    int iRegion = -1, iBuildConfig = -1, iCdnConfig = -1, iBuildId = -1, iVersion = -1;
    for (int i = 0; i < static_cast<int>(headerCols.size()); ++i) {
        auto col = trim(headerCols[i]);
        auto bang = col.find('!');
        if (bang != std::string_view::npos)
            col = col.substr(0, bang);
        if (col == "Region")
            iRegion = i;
        else if (col == "BuildConfig")
            iBuildConfig = i;
        else if (col == "CDNConfig")
            iCdnConfig = i;
        else if (col == "BuildId")
            iBuildId = i;
        else if (col == "VersionsName")
            iVersion = i;
    }

    for (size_t row = 1; row < lines.size(); ++row) {
        auto line = trim(lines[row]);
        if (line.empty())
            continue;

        auto fields = split(line, '|');
        auto getField = [&](int idx) -> std::string_view {
            if (idx >= 0 && static_cast<size_t>(idx) < fields.size())
                return trim(fields[idx]);
            return {};
        };

        CdnProductVersion v;
        if (iRegion >= 0)
            v.region = std::string(getField(iRegion));
        if (iBuildConfig >= 0)
            v.buildConfig = std::string(getField(iBuildConfig));
        if (iCdnConfig >= 0)
            v.cdnConfig = std::string(getField(iCdnConfig));
        if (iBuildId >= 0) {
            auto sv = getField(iBuildId);
            std::from_chars(sv.data(), sv.data() + sv.size(), v.buildId);
        }
        if (iVersion >= 0)
            v.versionName = std::string(getField(iVersion));

        results.push_back(std::move(v));
    }

    return results;
}

// ============================================================================
// Per-product CDN parser
// ============================================================================

static std::vector<CdnProductCdn> parseCdns(const std::vector<uint8_t>& data) {
    std::vector<CdnProductCdn> results;
    if (data.empty())
        return results;

    std::string_view const text(reinterpret_cast<const char*>(data.data()), data.size());
    auto lines = splitLines(text);
    if (lines.size() < 2)
        return results;

    auto headerCols = split(lines[0], '|');
    int iName = -1, iPath = -1, iHosts = -1, iServers = -1, iConfigPath = -1;
    for (int i = 0; i < static_cast<int>(headerCols.size()); ++i) {
        auto col = trim(headerCols[i]);
        auto bang = col.find('!');
        if (bang != std::string_view::npos)
            col = col.substr(0, bang);
        if (col == "Name")
            iName = i;
        else if (col == "Path")
            iPath = i;
        else if (col == "Hosts")
            iHosts = i;
        else if (col == "Servers")
            iServers = i;
        else if (col == "ConfigPath")
            iConfigPath = i;
    }

    for (size_t row = 1; row < lines.size(); ++row) {
        auto line = trim(lines[row]);
        if (line.empty())
            continue;

        auto fields = split(line, '|');
        auto getField = [&](int idx) -> std::string_view {
            if (idx >= 0 && static_cast<size_t>(idx) < fields.size())
                return trim(fields[idx]);
            return {};
        };

        CdnProductCdn c;
        if (iName >= 0)
            c.region = std::string(getField(iName));
        if (iPath >= 0)
            c.path = std::string(getField(iPath));
        if (iConfigPath >= 0)
            c.configPath = std::string(getField(iConfigPath));

        // Hosts are space-separated; Servers field has full URLs.
        auto hostsField = getField(iHosts);
        if (!hostsField.empty()) {
            auto parts = split(hostsField, ' ');
            for (auto& p : parts) {
                auto h = trim(p);
                if (!h.empty())
                    c.hosts.emplace_back(h);
            }
        } else {
            auto serversField = getField(iServers);
            if (!serversField.empty()) {
                auto parts = split(serversField, ' ');
                for (auto& p : parts) {
                    auto sv = trim(p);
                    if (sv.substr(0, 8) == "https://")
                        sv.remove_prefix(8);
                    else if (sv.substr(0, 7) == "http://")
                        sv.remove_prefix(7);
                    // Strip trailing slash.
                    if (!sv.empty() && sv.back() == '/')
                        sv.remove_suffix(1);
                    if (!sv.empty())
                        c.hosts.emplace_back(sv);
                }
            }
        }

        results.push_back(std::move(c));
    }

    return results;
}

// ============================================================================
// Synchronous HTTP fetch helper
// ============================================================================

static std::optional<std::vector<uint8_t>> fetchSync(interfaces::HttpHandler* http,
                                                     const std::string& url) {
    struct State {
        std::optional<std::vector<uint8_t>> data;
        std::mutex mtx;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<State>();

    http->getAsync(url, [state](interfaces::HttpResponse resp) {
        {
            std::lock_guard<std::mutex> const lk(state->mtx);
            if (resp.statusCode == 200)
                state->data = std::move(resp.body);
            state->done = true;
        }
        state->cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(state->mtx);
    state->cv.wait_for(lk, std::chrono::seconds(30), [&] { return state->done; });
    return state->data;
}

// ============================================================================
// Public API
// ============================================================================

std::vector<CdnProductInfo> findCdnProducts(const CdnProductFinderOptions& opts) {
    if (!opts.http)
        return {};

    // Step 1: Fetch the Ribbit v2 summary.
    std::string const summaryUrl = "https://" + opts.region + ".version.battle.net/v2/summary";

    auto summaryData = fetchSync(opts.http, summaryUrl);
    if (!summaryData)
        return {};

    auto products = parseSummary(*summaryData);
    if (products.empty())
        return products;

    // Step 2: Optionally fetch per-product details.
    if (!opts.queryVersions && !opts.queryCdns)
        return products;

    const uint32_t total = static_cast<uint32_t>(products.size());
    const uint32_t requestsPerProduct = (opts.queryVersions ? 1u : 0u) + (opts.queryCdns ? 1u : 0u);

    // Shared state for all pending requests.
    struct DetailState {
        std::atomic<uint32_t> completed{0};
        std::mutex mtx;
        std::condition_variable cv;
    };
    auto ds = std::make_shared<DetailState>();

    const uint32_t totalRequests = total * requestsPerProduct;

    auto onRequestDone = [ds]() {
        ds->completed.fetch_add(1, std::memory_order_acq_rel);
        std::lock_guard<std::mutex> const lk(ds->mtx);
        ds->cv.notify_one();
    };

    // Fire all detail requests.
    for (uint32_t i = 0; i < total; ++i) {
        auto& product = products[i];
        const std::string& code = product.product;

        if (opts.queryVersions) {
            std::string const url =
                "https://" + opts.region + ".version.battle.net/v2/products/" + code + "/versions";
            opts.http->getAsync(url, [&product, onRequestDone](interfaces::HttpResponse resp) {
                if (resp.statusCode == 200)
                    product.versions = parseVersions(resp.body);
                onRequestDone();
            });
        }

        if (opts.queryCdns) {
            std::string const url =
                "https://" + opts.region + ".version.battle.net/v2/products/" + code + "/cdns";
            opts.http->getAsync(url, [&product, onRequestDone](interfaces::HttpResponse resp) {
                if (resp.statusCode == 200)
                    product.cdns = parseCdns(resp.body);
                onRequestDone();
            });
        }
    }

    // Wait for all detail requests, with periodic progress reporting.
    {
        std::unique_lock<std::mutex> lk(ds->mtx);
        ds->cv.wait_for(lk, std::chrono::seconds(120), [&] {
            uint32_t const done = ds->completed.load(std::memory_order_acquire);
            if (opts.progressCallback) {
                uint32_t const productsDone = done / requestsPerProduct;
                if (!opts.progressCallback(productsDone, total))
                    return true; // cancelled — stop waiting
            }
            return done >= totalRequests;
        });
    }

    return products;
}

} // namespace whiteout::utils
