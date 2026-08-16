// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// @file casc_progress_test.cpp
/// @brief Progress reporting contract for Storage::open and the deferred load.

#include <whiteout/storages/casc/storage.h>
#include <whiteout/storages/casc/storage_writable.h>
#include <whiteout/utils/simple_thread_pool.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::storages::casc;

namespace {

std::vector<u8> makeTestData(size_t size, u8 seed) {
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i)
        data[i] = static_cast<u8>((i + seed) & 0xFF);
    return data;
}

bool buildStorage(const std::string& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    CreateOptions createOpts;
    createOpts.product = "test";
    createOpts.version = "1.0.0";
    auto storage = StorageWritable::create(createOpts);
    if (!storage)
        return false;
    if (!storage.writeFile("dir/file1.txt", makeTestData(2048, 0x11)))
        return false;
    if (!storage.writeFile("dir/file2.bin", makeTestData(70000, 0x22)))
        return false;
    return storage.save(dir);
}

/// One recorded event, with the object copied out — the view the callback sees
/// is only valid for the duration of the call.
struct Event {
    ProgressStep step;
    ProgressState state;
    std::string object;
    u64 current;
    u64 total;
    u32 stepIndex;
    u32 stepCount;
    double overall;
};

struct Recorder {
    std::vector<Event> events;

    ProgressCallback callback() {
        return [this](const ProgressInfo& info) {
            events.push_back(Event{info.step, info.state, std::string(info.object), info.current,
                                   info.total, info.stepIndex, info.stepCount,
                                   info.overallFraction()});
            return true;
        };
    }

    bool has(ProgressStep step, ProgressState state) const {
        for (auto& e : events)
            if (e.step == step && e.state == state)
                return true;
        return false;
    }

    const Event* find(ProgressStep step, ProgressState state) const {
        for (auto& e : events)
            if (e.step == step && e.state == state)
                return &e;
        return nullptr;
    }
};

/// Every Begin is closed by an End for the same step, in order.
void checkBracketing(const std::vector<Event>& events) {
    bool open = false;
    ProgressStep openStep = ProgressStep::Ready;
    for (auto& e : events) {
        INFO("step " << progressStepName(e.step) << " state " << int(e.state));
        if (e.state == ProgressState::Begin) {
            CHECK_FALSE(open); // no nesting
            open = true;
            openStep = e.step;
        } else if (e.state == ProgressState::Update) {
            CHECK(open);
            CHECK(e.step == openStep);
        } else if (e.step != ProgressStep::Ready) { // Ready is terminal, not a body
            CHECK(open);
            CHECK(e.step == openStep);
            open = false;
        }
    }
    CHECK_FALSE(open);
}

} // namespace

TEST_CASE("Open reports a bracketed, monotonic progress stream", "[casc][progress]") {
    const std::string testDir = "test_casc_progress_stream";
    REQUIRE(buildStorage(testDir));

    Recorder rec;
    OpenOptions opts;
    opts.path = testDir;
    opts.progressCallback = rec.callback();

    auto storage = Storage::open(opts);
    REQUIRE(storage.has_value());

    REQUIRE_FALSE(rec.events.empty());
    checkBracketing(rec.events);

    // Terminal event, exactly once, last.
    CHECK(rec.events.back().step == ProgressStep::Ready);
    CHECK(rec.events.back().state == ProgressState::End);
    CHECK(rec.events.back().overall == 1.0);

    // The phases a UI needs to name are all present.
    CHECK(rec.has(ProgressStep::LoadingBuildConfig, ProgressState::Begin));
    CHECK(rec.has(ProgressStep::LoadingCdnConfig, ProgressState::Begin));
    CHECK(rec.has(ProgressStep::LoadingIndexFiles, ProgressState::Begin));
    CHECK(rec.has(ProgressStep::MappingArchives, ProgressState::Begin));
    CHECK(rec.has(ProgressStep::LoadingEncodingTable, ProgressState::Begin));
    CHECK(rec.has(ProgressStep::LoadingRootManifest, ProgressState::Begin));

    // Position never regresses and the plan is known from the first event on.
    u32 lastIndex = 0;
    double lastOverall = 0.0;
    for (auto& e : rec.events) {
        INFO("step " << progressStepName(e.step) << " index " << e.stepIndex);
        CHECK(e.stepCount > 0);
        CHECK(e.stepIndex >= lastIndex);
        CHECK(e.overall >= lastOverall);
        CHECK(e.overall <= 1.0);
        lastIndex = e.stepIndex;
        lastOverall = e.overall;
    }

    // Steps that count their work say so by the time they close.
    const Event* idxEnd = rec.find(ProgressStep::LoadingIndexFiles, ProgressState::End);
    REQUIRE(idxEnd != nullptr);
    CHECK(idxEnd->total > 0);
    CHECK(idxEnd->current == idxEnd->total);

    const Event* mapEnd = rec.find(ProgressStep::MappingArchives, ProgressState::End);
    REQUIRE(mapEnd != nullptr);
    CHECK(mapEnd->total > 0);

    // Objects name the thing being worked on, not just the phase.
    const Event* encEnd = rec.find(ProgressStep::LoadingEncodingTable, ProgressState::End);
    REQUIRE(encEnd != nullptr);
    CHECK(encEnd->object == "ENCODING");
    CHECK(encEnd->current > 0);

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("A progress callback can cancel the open", "[casc][progress]") {
    const std::string testDir = "test_casc_progress_cancel";
    REQUIRE(buildStorage(testDir));

    SECTION("cancel on the first event") {
        int calls = 0;
        OpenOptions opts;
        opts.path = testDir;
        opts.progressCallback = [&calls](const ProgressInfo&) {
            ++calls;
            return false;
        };

        auto storage = Storage::open(opts);
        CHECK_FALSE(storage.has_value());
        CHECK(Storage::lastError() == CascError::Cancelled);
        CHECK(calls == 1); // no further events after a cancel
    }

    SECTION("cancel midway through") {
        OpenOptions opts;
        opts.path = testDir;
        opts.progressCallback = [](const ProgressInfo& info) {
            return info.step != ProgressStep::MappingArchives;
        };

        auto storage = Storage::open(opts);
        CHECK_FALSE(storage.has_value());
        CHECK(Storage::lastError() == CascError::Cancelled);
    }

    SECTION("a callback that never cancels opens normally") {
        OpenOptions opts;
        opts.path = testDir;
        opts.progressCallback = [](const ProgressInfo&) { return true; };
        CHECK(Storage::open(opts).has_value());
    }

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Deferred load reports through setProgressCallback", "[casc][progress]") {
    const std::string testDir = "test_casc_progress_deferred";
    REQUIRE(buildStorage(testDir));

    Recorder openRec;
    OpenOptions opts;
    opts.path = testDir;
    opts.flags = StorageFeatureFlags::LoadOnDemand;
    opts.progressCallback = openRec.callback();

    auto storage = Storage::open(opts);
    REQUIRE(storage.has_value());

    // Open finished without touching encoding or root — the bar it drew must
    // not have claimed otherwise.
    CHECK(openRec.events.back().step == ProgressStep::Ready);
    CHECK_FALSE(openRec.has(ProgressStep::LoadingEncodingTable, ProgressState::Begin));
    CHECK_FALSE(openRec.has(ProgressStep::LoadingRootManifest, ProgressState::Begin));

    // The deferred work is its own operation on its own callback.
    Recorder deferRec;
    storage->setProgressCallback(deferRec.callback());

    auto data = storage->readFile("dir/file1.txt");
    REQUIRE(data.has_value());

    REQUIRE_FALSE(deferRec.events.empty());
    checkBracketing(deferRec.events);
    CHECK(deferRec.has(ProgressStep::LoadingEncodingTable, ProgressState::Begin));
    CHECK(deferRec.has(ProgressStep::LoadingRootManifest, ProgressState::End));
    CHECK(deferRec.events.back().step == ProgressStep::Ready);

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Progress survives a worker pool", "[casc][progress]") {
    const std::string testDir = "test_casc_progress_pool";
    REQUIRE(buildStorage(testDir));

    whiteout::utils::SimpleThreadPool pool(4);

    Recorder rec;
    OpenOptions opts;
    opts.path = testDir;
    opts.pool = &pool;
    opts.progressCallback = rec.callback();

    auto storage = Storage::open(opts);
    REQUIRE(storage.has_value());

    // Reporting from worker threads is serialised, so the recorder — which is
    // not itself synchronised — still sees a well-formed stream.
    checkBracketing(rec.events);
    CHECK(rec.events.back().step == ProgressStep::Ready);

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}

TEST_CASE("Opening without a callback costs nothing observable", "[casc][progress]") {
    const std::string testDir = "test_casc_progress_none";
    REQUIRE(buildStorage(testDir));

    OpenOptions opts;
    opts.path = testDir;
    CHECK(Storage::open(opts).has_value());

    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
}
