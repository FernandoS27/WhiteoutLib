// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

// P0 gate (GLTF_DESIGN §12): the JSON codec round-trips, the GLB frame
// round-trips through Parser/Writer, malformed input fails cleanly, and the
// converter registry answers for "gltf".

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/gltf/json.h>
#include <whiteout/models/gltf/parser.h>
#include <whiteout/models/gltf/writer.h>
#include <whiteout/models/wem/converters.h>

using namespace whiteout;
using namespace whiteout::models;

TEST_CASE("json scalars parse and print", "[gltf][json]") {
    auto roundTrip = [](const std::string& text) {
        gltf::json::ParseResult parsed = gltf::json::Parse(text);
        REQUIRE(parsed.ok());
        return gltf::json::Write(*parsed.value);
    };

    CHECK(roundTrip("null") == "null");
    CHECK(roundTrip("true") == "true");
    CHECK(roundTrip("false") == "false");
    CHECK(roundTrip("0") == "0");
    CHECK(roundTrip("-17") == "-17");
    CHECK(roundTrip("1.5") == "1.5");
    CHECK(roundTrip("1e3") == "1000");
    CHECK(roundTrip("\"hi\"") == "\"hi\"");
    CHECK(roundTrip("[]") == "[]");
    CHECK(roundTrip("{}") == "{}");
    CHECK(roundTrip(" [ 1 , 2 , 3 ] ") == "[1,2,3]");
}

TEST_CASE("json objects keep insertion order", "[gltf][json]") {
    gltf::json::ParseResult parsed =
        gltf::json::Parse(R"({"z":1,"a":{"nested":[true,null]},"m":"x"})");
    REQUIRE(parsed.ok());
    CHECK(gltf::json::Write(*parsed.value) == R"({"z":1,"a":{"nested":[true,null]},"m":"x"})");
}

TEST_CASE("json string escapes round-trip", "[gltf][json]") {
    gltf::json::ParseResult parsed =
        gltf::json::Parse("\"a\\\"b\\\\c\\n\\t\\u0041\\u00e9\\ud83d\\ude00\"");
    REQUIRE(parsed.ok());
    // \u0041 is 'A'; \u00e9 is é in UTF-8; the surrogate pair is one emoji.
    const std::string& text = parsed.value->asString();
    CHECK(text == std::string("a\"b\\c\n\tA\xC3\xA9\xF0\x9F\x98\x80"));
    // Writing re-escapes only what JSON requires.
    CHECK(gltf::json::Write(*parsed.value) ==
          "\"a\\\"b\\\\c\\n\\tA\xC3\xA9\xF0\x9F\x98\x80\"");
}

TEST_CASE("json rejects what the grammar rejects", "[gltf][json]") {
    const char* bad[] = {
        "",         "{",         "[1,]",     "{\"a\":}",  "{'a':1}", "01",
        "+1",       ".5",        "1.",       "1e",        "NaN",     "Infinity",
        "nul",      "[1] x",     "\"\\q\"",  "\"\\ud800\"",
        "\"unterminated",
    };
    for (const char* text : bad) {
        CAPTURE(text);
        CHECK_FALSE(gltf::json::Parse(text).ok());
    }
}

TEST_CASE("json depth limit fails cleanly", "[gltf][json]") {
    std::string deep(20000, '[');
    CHECK_FALSE(gltf::json::Parse(deep).ok());
}

TEST_CASE("number formatting is deterministic", "[gltf][json]") {
    CHECK(gltf::json::FormatNumber(0.0) == "0");
    CHECK(gltf::json::FormatNumber(1.0) == "1");
    CHECK(gltf::json::FormatNumber(-3.0) == "-3");
    CHECK(gltf::json::FormatNumber(0.5) == "0.5");
    CHECK(gltf::json::FormatNumber(16777216.0) == "16777216");
}

TEST_CASE("glb container round-trips", "[gltf]") {
    gltf::Asset asset;
    asset.asset.generator = "whiteout";
    gltf::Buffer buffer;
    buffer.data = {1, 2, 3, 4, 5};
    buffer.byteLength = 5;
    asset.buffers.push_back(buffer);

    const std::vector<u8> glb = gltf::Writer::ToGlb(asset);
    REQUIRE(glb.size() % 4 == 0);
    REQUIRE(gltf::Parser::LooksLikeGlb(glb));

    gltf::ParseOutcome outcome = gltf::Parser::FromBytes(glb);
    REQUIRE(outcome.ok());
    CHECK(outcome.asset->asset.generator == "whiteout");
    REQUIRE(outcome.asset->buffers.size() == 1);
    CHECK(outcome.asset->buffers[0].data == buffer.data);

    // Byte-stable across two runs — the golden-gate property.
    CHECK(gltf::Writer::ToGlb(asset) == glb);
    // And re-writing the re-parse reproduces the bytes.
    CHECK(gltf::Writer::ToGlb(*outcome.asset) == glb);
}

TEST_CASE("glb truncation fails cleanly", "[gltf]") {
    gltf::Asset asset;
    gltf::Buffer buffer;
    buffer.data = {9, 9, 9, 9};
    buffer.byteLength = 4;
    asset.buffers.push_back(buffer);
    const std::vector<u8> glb = gltf::Writer::ToGlb(asset);

    for (const std::size_t keep : {std::size_t(4), std::size_t(11), std::size_t(14)}) {
        gltf::ParseOutcome outcome =
            gltf::Parser::FromBytes(std::span<const u8>(glb.data(), keep));
        CHECK_FALSE(outcome.ok());
    }
}

TEST_CASE("base64 decodes and rejects", "[gltf]") {
    std::vector<u8> out;
    REQUIRE(gltf::DecodeBase64("TWFu", out));
    CHECK(out == std::vector<u8>{'M', 'a', 'n'});
    REQUIRE(gltf::DecodeBase64("TWE=", out));
    CHECK(out == std::vector<u8>{'M', 'a'});
    REQUIRE(gltf::DecodeBase64("TQ==", out));
    CHECK(out == std::vector<u8>{'M'});
    CHECK_FALSE(gltf::DecodeBase64("T!Q=", out));
    CHECK_FALSE(gltf::DecodeBase64("TQ=A", out));
}

TEST_CASE("data uri buffers decode", "[gltf]") {
    const char* body = R"({
        "asset": {"version": "2.0"},
        "buffers": [{"uri": "data:application/octet-stream;base64,AAECAwQ=",
                     "byteLength": 5}]
    })";
    gltf::ParseOutcome outcome = gltf::Parser::FromJsonText(body);
    REQUIRE(outcome.ok());
    REQUIRE(outcome.asset->buffers.size() == 1);
    CHECK(outcome.asset->buffers[0].data == std::vector<u8>{0, 1, 2, 3, 4});
}

TEST_CASE("accessor decode handles types, strides and sparse", "[gltf]") {
    gltf::Asset asset;
    gltf::Buffer buffer;
    // Three VEC2 of U8 normalized at stride 4: (0,255), (51,102), (255,0).
    buffer.data = {0, 255, 9, 9, 51, 102, 9, 9, 255, 0, 9, 9};
    buffer.byteLength = static_cast<u32>(buffer.data.size());
    asset.buffers.push_back(std::move(buffer));
    gltf::BufferView view;
    view.buffer = 0;
    view.byteLength = 12;
    view.byteStride = 4;
    asset.bufferViews.push_back(view);
    gltf::Accessor accessor;
    accessor.bufferView = 0;
    accessor.componentType = gltf::ComponentType::U8;
    accessor.normalized = true;
    accessor.count = 3;
    accessor.type = gltf::AccessorType::Vec2;
    asset.accessors.push_back(accessor);

    std::vector<f32> values;
    REQUIRE(gltf::ReadAccessorF32(asset, 0, values));
    REQUIRE(values.size() == 6);
    CHECK(values[0] == 0.0f);
    CHECK(values[1] == 1.0f);
    CHECK(values[4] == 1.0f);
    CHECK(values[5] == 0.0f);

    // A sparse overlay on a viewless accessor: zeros plus one override.
    gltf::Buffer sparseBuffer;
    sparseBuffer.data = {1, 0};                 // U16 index = 1.
    const f32 overrideValue[2] = {2.5f, -1.0f}; // VEC2 F32.
    const u8* raw = reinterpret_cast<const u8*>(overrideValue);
    sparseBuffer.data.insert(sparseBuffer.data.end(), raw, raw + 8);
    sparseBuffer.byteLength = static_cast<u32>(sparseBuffer.data.size());
    asset.buffers.push_back(std::move(sparseBuffer));
    gltf::BufferView indexView;
    indexView.buffer = 1;
    indexView.byteLength = 2;
    asset.bufferViews.push_back(indexView);
    gltf::BufferView valueView;
    valueView.buffer = 1;
    valueView.byteOffset = 2;
    valueView.byteLength = 8;
    asset.bufferViews.push_back(valueView);

    gltf::Accessor sparseAccessor;
    sparseAccessor.componentType = gltf::ComponentType::F32;
    sparseAccessor.count = 3;
    sparseAccessor.type = gltf::AccessorType::Vec2;
    gltf::AccessorSparse sparse;
    sparse.count = 1;
    sparse.indicesBufferView = 1;
    sparse.indicesComponentType = gltf::ComponentType::U16;
    sparse.valuesBufferView = 2;
    sparseAccessor.sparse = sparse;
    asset.accessors.push_back(sparseAccessor);

    REQUIRE(gltf::ReadAccessorF32(asset, 1, values));
    REQUIRE(values.size() == 6);
    CHECK(values[0] == 0.0f);
    CHECK(values[2] == 2.5f);
    CHECK(values[3] == -1.0f);
    CHECK(values[4] == 0.0f);

    // U32 reads reject float accessors.
    std::vector<u32> indices;
    CHECK_FALSE(gltf::ReadAccessorU32(asset, 1, indices));
}

TEST_CASE("the registry serves gltf", "[gltf][wem]") {
    const wem::FormatConverter* converter = wem::ConverterRegistry::instance().find("gltf");
    REQUIRE(converter != nullptr);
    CHECK(converter->formatName() == "glTF 2.0");
    CHECK(converter->supportsImport());
    CHECK(converter->supportsExport());
    REQUIRE(converter->profiles().size() == 1);
    CHECK(converter->profiles()[0] == wem::ProfileId::Generic);

    // `findForProfile(Generic)` now answers — the WEM_INTEGRATION §3 amendment —
    // and no game profile's answer moved.
    CHECK(wem::ConverterRegistry::instance().findForProfile(wem::ProfileId::Generic) == converter);
    const wem::FormatConverter* mdx =
        wem::ConverterRegistry::instance().findForProfile(wem::ProfileId::Wc3Reforged);
    REQUIRE(mdx != nullptr);
    CHECK(mdx->formatId() == "mdx");
}
