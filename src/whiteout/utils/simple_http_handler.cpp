// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/utils/simple_http_handler.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
// MinGW/clang-mingw resolve winhttp via CMake target_link_libraries instead.

// WINHTTP_PROTOCOL_FLAG_HTTP2 requires Windows 10 1607 SDK or later.
#ifndef WINHTTP_PROTOCOL_FLAG_HTTP2
#define WINHTTP_PROTOCOL_FLAG_HTTP2 0x1
#endif
#ifndef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
#define WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL 133
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace whiteout::utils {

// ============================================================================
// Helpers
// ============================================================================

/// Parse a URL string into WinHTTP components.
struct ParsedUrl {
    std::wstring host;
    std::wstring path;    // includes query string
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool https = true;
    bool valid = false;
};

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

static ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl out;
    auto wurl = toWide(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = (DWORD)std::size(hostBuf);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = (DWORD)std::size(pathBuf);

    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return out;

    out.host = hostBuf;
    out.path = pathBuf;
    out.port = uc.nPort;
    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.valid = true;
    return out;
}

// ============================================================================
// Request job
// ============================================================================

struct HttpJob {
    std::string url;
    interfaces::HttpCallback callback;
    bool rangeRequest = false;
    u64 rangeStart = 0;
    u64 rangeEnd = 0;
};

// ============================================================================
// Impl
// ============================================================================

struct SimpleHttpHandler::Impl {
    HINTERNET hSession = nullptr;
    std::vector<std::thread> workers;
    std::deque<HttpJob> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> shutdown{false};

    Impl(size_t nThreads) {
        hSession = WinHttpOpen(L"WhiteoutLib/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS,
                               0);
        if (hSession) {
            // Enable HTTP/2 if available (Windows 10 1607+).
            DWORD option = WINHTTP_PROTOCOL_FLAG_HTTP2;
            WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                             &option, sizeof(option));

            // Set reasonable timeouts (connect=15s, send=30s, receive=60s).
            WinHttpSetTimeouts(hSession, 0, 15000, 30000, 60000);
        }

        workers.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mutex);
            shutdown.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
        if (hSession) WinHttpCloseHandle(hSession);
    }

    void enqueue(HttpJob job) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void workerLoop() {
        while (true) {
            HttpJob job;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] {
                    return shutdown.load(std::memory_order_relaxed) || !queue.empty();
                });
                if (shutdown.load(std::memory_order_relaxed) && queue.empty())
                    return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            executeJob(std::move(job));
        }
    }

    void executeJob(HttpJob job) {
        interfaces::HttpResponse resp;

        auto parsed = parseUrl(job.url);
        if (!parsed.valid || !hSession) {
            resp.error = "invalid URL or WinHTTP not initialized";
            job.callback(std::move(resp));
            return;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, parsed.host.c_str(),
                                            parsed.port, 0);
        if (!hConnect) {
            resp.error = "WinHttpConnect failed (error " +
                         std::to_string(GetLastError()) + ")";
            job.callback(std::move(resp));
            return;
        }

        DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
                                                 parsed.path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 flags);
        if (!hRequest) {
            resp.error = "WinHttpOpenRequest failed (error " +
                         std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hConnect);
            job.callback(std::move(resp));
            return;
        }

        // Enable HTTP/2 on this request.
        DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
                         &http2, sizeof(http2));

        // Add Range header if needed.
        if (job.rangeRequest) {
            auto rangeHeader = L"Range: bytes=" +
                               std::to_wstring(job.rangeStart) + L"-" +
                               std::to_wstring(job.rangeEnd);
            WinHttpAddRequestHeaders(hRequest, rangeHeader.c_str(),
                                     (DWORD)rangeHeader.size(),
                                     WINHTTP_ADDREQ_FLAG_ADD);
        }

        // Send request.
        if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            resp.error = "WinHttpSendRequest failed (error " +
                         std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            job.callback(std::move(resp));
            return;
        }

        // Receive response.
        if (!WinHttpReceiveResponse(hRequest, nullptr)) {
            resp.error = "WinHttpReceiveResponse failed (error " +
                         std::to_string(GetLastError()) + ")";
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            job.callback(std::move(resp));
            return;
        }

        // Read status code.
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        resp.statusCode = static_cast<i32>(statusCode);

        // Read body.
        std::vector<u8> body;
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) &&
               bytesAvailable > 0) {
            size_t prevSize = body.size();
            body.resize(prevSize + bytesAvailable);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest,
                            body.data() + prevSize,
                            bytesAvailable,
                            &bytesRead)) {
                body.resize(prevSize);
                break;
            }
            body.resize(prevSize + bytesRead);
            if (bytesRead == 0) break;
            bytesAvailable = 0;
        }
        resp.body = std::move(body);

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);

        job.callback(std::move(resp));
    }
};

// ============================================================================
// Public API
// ============================================================================

SimpleHttpHandler::SimpleHttpHandler(size_t nThreads)
    : m_impl(std::make_unique<Impl>(nThreads)) {}

SimpleHttpHandler::~SimpleHttpHandler() = default;

u32 SimpleHttpHandler::capabilities() const noexcept {
    return interfaces::HttpCapability::Http2Multiplexing;
}

void SimpleHttpHandler::getAsync(const std::string& url,
                                 interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    m_impl->enqueue(std::move(job));
}

void SimpleHttpHandler::getRangeAsync(const std::string& url, u64 start, u64 end,
                                      interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    job.rangeRequest = true;
    job.rangeStart = start;
    job.rangeEnd = end;
    m_impl->enqueue(std::move(job));
}

} // namespace whiteout::utils

#elif defined(WHITEOUT_HAVE_CURL)

// ── libcurl backend (Linux, macOS, BSD, …) ───────────────────────────

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace whiteout::utils {

namespace {

// Lazily one-shot-init libcurl.  curl_global_init touches process-wide state
// (TLS engine, signal handlers, etc.) and is not safe to call concurrently;
// std::call_once gives us the right barrier.
std::once_flag g_curl_init_flag;
void ensureCurlInit() {
    std::call_once(g_curl_init_flag, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        std::atexit([] { curl_global_cleanup(); });
    });
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    auto* body = static_cast<std::vector<u8>*>(userdata);
    const size_t total = size * nmemb;
    body->insert(body->end(),
                 reinterpret_cast<u8*>(ptr),
                 reinterpret_cast<u8*>(ptr) + total);
    return total;
}

} // namespace

struct HttpJob {
    std::string url;
    interfaces::HttpCallback callback;
    bool rangeRequest = false;
    u64 rangeStart = 0;
    u64 rangeEnd = 0;
};

struct SimpleHttpHandler::Impl {
    std::vector<std::thread> workers;
    std::deque<HttpJob> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> shutdown{false};

    explicit Impl(size_t nThreads) {
        ensureCurlInit();
        workers.reserve(nThreads);
        for (size_t i = 0; i < nThreads; ++i) {
            workers.emplace_back([this] { workerLoop(); });
        }
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(mutex);
            shutdown.store(true, std::memory_order_relaxed);
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

    void enqueue(HttpJob job) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            queue.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void workerLoop() {
        // One CURL easy handle per worker — libcurl reuses the connection
        // cache across curl_easy_perform calls on the same handle, so we
        // get keep-alive for free without sharing handles across threads
        // (which is not safe).
        CURL* curl = curl_easy_init();
        if (!curl) {
            // Drain pending jobs with a clear error so callers don't hang.
            while (true) {
                HttpJob job;
                {
                    std::unique_lock<std::mutex> lk(mutex);
                    cv.wait(lk, [&] {
                        return shutdown.load(std::memory_order_relaxed) ||
                               !queue.empty();
                    });
                    if (shutdown.load(std::memory_order_relaxed) && queue.empty())
                        return;
                    job = std::move(queue.front());
                    queue.pop_front();
                }
                interfaces::HttpResponse resp;
                resp.error = "libcurl: curl_easy_init failed";
                job.callback(std::move(resp));
            }
        }

        while (true) {
            HttpJob job;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [&] {
                    return shutdown.load(std::memory_order_relaxed) ||
                           !queue.empty();
                });
                if (shutdown.load(std::memory_order_relaxed) && queue.empty()) {
                    curl_easy_cleanup(curl);
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            executeJob(curl, std::move(job));
        }
    }

    void executeJob(CURL* curl, HttpJob job) {
        interfaces::HttpResponse resp;

        // Wipe per-request options but keep the connection cache.
        curl_easy_reset(curl);

        curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "WhiteoutLib/1.0");
        // NOSIGNAL: libcurl's default DNS resolver uses SIGALRM for timeouts
        // and is not thread-safe under that mode.  Disabling signals forces
        // the threaded resolver path (built in by default on modern libcurl).
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                         static_cast<long>(CURL_HTTP_VERSION_2TLS));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

        std::string rangeStr;
        if (job.rangeRequest) {
            rangeStr = std::to_string(job.rangeStart) + "-" +
                       std::to_string(job.rangeEnd);
            curl_easy_setopt(curl, CURLOPT_RANGE, rangeStr.c_str());
        }

        const CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK) {
            resp.body.clear();
            resp.error = std::string("libcurl: ") + curl_easy_strerror(rc);
            job.callback(std::move(resp));
            return;
        }

        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.statusCode = static_cast<i32>(code);

        job.callback(std::move(resp));
    }
};

SimpleHttpHandler::SimpleHttpHandler(size_t nThreads)
    : m_impl(std::make_unique<Impl>(nThreads)) {}

SimpleHttpHandler::~SimpleHttpHandler() = default;

u32 SimpleHttpHandler::capabilities() const noexcept {
    // libcurl negotiates HTTP/2 per-connection via CURL_HTTP_VERSION_2TLS;
    // it does not multiplex requests across our worker handles, but the
    // wire-level capability is present.
    return interfaces::HttpCapability::Http2Multiplexing;
}

void SimpleHttpHandler::getAsync(const std::string& url,
                                 interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    m_impl->enqueue(std::move(job));
}

void SimpleHttpHandler::getRangeAsync(const std::string& url, u64 start, u64 end,
                                      interfaces::HttpCallback callback) {
    HttpJob job;
    job.url = url;
    job.callback = std::move(callback);
    job.rangeRequest = true;
    job.rangeStart = start;
    job.rangeEnd = end;
    m_impl->enqueue(std::move(job));
}

} // namespace whiteout::utils

#else // !_WIN32 && !WHITEOUT_HAVE_CURL

// ── Stub when no backend is available ────────────────────────────────

namespace whiteout::utils {

struct SimpleHttpHandler::Impl {};

SimpleHttpHandler::SimpleHttpHandler(size_t /*nThreads*/)
    : m_impl(std::make_unique<Impl>()) {}

SimpleHttpHandler::~SimpleHttpHandler() = default;

u32 SimpleHttpHandler::capabilities() const noexcept {
    return interfaces::HttpCapability::None;
}

void SimpleHttpHandler::getAsync(const std::string& /*url*/,
                                 interfaces::HttpCallback callback) {
    interfaces::HttpResponse resp;
    resp.error = "SimpleHttpHandler: no HTTP backend compiled in "
                 "(build with libcurl or provide your own HttpHandler)";
    callback(std::move(resp));
}

void SimpleHttpHandler::getRangeAsync(const std::string& /*url*/,
                                      u64 /*start*/, u64 /*end*/,
                                      interfaces::HttpCallback callback) {
    interfaces::HttpResponse resp;
    resp.error = "SimpleHttpHandler: no HTTP backend compiled in "
                 "(build with libcurl or provide your own HttpHandler)";
    callback(std::move(resp));
}

} // namespace whiteout::utils

#endif // backend selection
