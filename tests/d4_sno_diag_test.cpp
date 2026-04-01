// Diagnostic test: parse a D4 SNO file and check for null values.
#include <catch2/catch_all.hpp>

#include <whiteout/sno/sno_reader.h>
#include <whiteout/sno/sno_types.h>
#include <whiteout/sno/sno_value.h>

#include <cstring>
#include <vector>

using namespace whiteout;
using namespace whiteout::sno;

static void countValues(const SnoValue& v, int& total, int& nulls, int depth = 0) {
    ++total;
    if (v.isNull()) {
        ++nulls;
        return;
    }
    if (depth > 10) return;
    if (v.isObject()) {
        for (auto& [k, child] : v.asObject()) {
            countValues(child, total, nulls, depth + 1);
        }
    } else if (v.isArray()) {
        for (size_t i = 0; i < v.size(); ++i) {
            auto* e = v.at(i);
            if (e) countValues(*e, total, nulls, depth + 1);
        }
    }
}

TEST_CASE("D4 SNO synthetic parse", "[sno][d4][diag]") {
    // Build a minimal SNO binary: 0xDEADBEEF header + known format hash + payload.
    constexpr u32 kMagic = 0xDEADBEEF;
    constexpr u32 kFmtHash = 123576590u;

    std::vector<u8> data(16 + 72, 0);
    std::memcpy(data.data(), &kMagic, 4);
    std::memcpy(data.data() + 4, &kFmtHash, 4);

    i32 testInt = 42;
    std::memcpy(data.data() + 16 + 4, &testInt, 4);

    float testFloat = 3.14f;
    std::memcpy(data.data() + 16 + 8, &testFloat, 4);

    SnoReader reader;
    auto file = reader.parse(data);

    REQUIRE(file.has_value());
    REQUIRE_FALSE(file->root.isNull());
    REQUIRE(file->root.isObject());

    const auto& obj = file->root.asObject();
    CHECK(obj.size() > 0);

    int total = 0, nulls = 0;
    countValues(file->root, total, nulls);
    CHECK(total > 0);
}
