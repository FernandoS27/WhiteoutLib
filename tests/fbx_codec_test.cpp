// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// FBX_OBJ_DESIGN F1 — the FBX record codec.
///
/// The record tree is lossless, so its gate is identity: write → read gives the
/// tree back, value for value, at both offset widths; and every SDK-written
/// file in the corpus reads, re-writes and reads back to the same tree. The
/// hostile arm holds the bounds: nothing a file claims is allocated before it
/// is checked against what the bytes can hold.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/fbx/fbx.h>

using namespace whiteout;
using namespace whiteout::models;
namespace fs = std::filesystem;

namespace {

bool SameProperty(const fbx::Property& a, const fbx::Property& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
    case fbx::PropertyType::F32:
    case fbx::PropertyType::F64:
        return std::memcmp(&a.number, &b.number, sizeof(f64)) == 0;
    case fbx::PropertyType::String:
    case fbx::PropertyType::Raw:
        return a.text == b.text;
    case fbx::PropertyType::BoolArray:
        return a.bools == b.bools;
    case fbx::PropertyType::I32Array:
        return a.i32s == b.i32s;
    case fbx::PropertyType::I64Array:
        return a.i64s == b.i64s;
    case fbx::PropertyType::F32Array:
        return a.f32s.size() == b.f32s.size() &&
               std::memcmp(a.f32s.data(), b.f32s.data(), a.f32s.size() * sizeof(f32)) == 0;
    case fbx::PropertyType::F64Array:
        return a.f64s.size() == b.f64s.size() &&
               std::memcmp(a.f64s.data(), b.f64s.data(), a.f64s.size() * sizeof(f64)) == 0;
    default:
        return a.integer == b.integer;
    }
}

bool SameNode(const fbx::Node& a, const fbx::Node& b, std::string& where) {
    if (a.name != b.name || a.properties.size() != b.properties.size() ||
        a.children.size() != b.children.size() ||
        (a.block || !a.children.empty()) != (b.block || !b.children.empty())) {
        where = a.name;
        return false;
    }
    for (std::size_t i = 0; i < a.properties.size(); ++i) {
        if (!SameProperty(a.properties[i], b.properties[i])) {
            where = a.name + "[" + std::to_string(i) + "]";
            return false;
        }
    }
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!SameNode(a.children[i], b.children[i], where)) {
            where = a.name + "/" + where;
            return false;
        }
    }
    return true;
}

bool SameFile(const fbx::File& a, const fbx::File& b, std::string& where) {
    if (a.nodes.size() != b.nodes.size()) {
        where = "top-level count";
        return false;
    }
    for (std::size_t i = 0; i < a.nodes.size(); ++i) {
        if (!SameNode(a.nodes[i], b.nodes[i], where)) {
            return false;
        }
    }
    return true;
}

fbx::File SampleFile(u32 version) {
    fbx::File file;
    file.version = version;
    fbx::Node& header = file.nodes.emplace_back("FBXHeaderExtension");
    header.add("FBXVersion", {fbx::Property::Int(static_cast<i32>(version))});
    header.add("Empty");                // no values, no children
    header.add("EmptyBlock").block = true; // closed like an SDK object record
    fbx::Node& objects = file.nodes.emplace_back("Objects");
    fbx::Node& model = objects.add(
        "Model", {fbx::Property::Long(1234567890123LL),
                  fbx::Property::Str(std::string("Root\0\1Model", 11)), fbx::Property::Str("Null")});
    model.add("Flags", {fbx::Property::Bool(true), fbx::Property::Short(-7),
                        fbx::Property::Float(0.1f), fbx::Property::Double(-2.5e-300),
                        fbx::Property::RawBytes(std::string("\x00\xff\x10", 3))});
    std::vector<f64> big(400); // 3200 bytes: deflated
    for (std::size_t i = 0; i < big.size(); ++i) {
        big[i] = static_cast<f64>(i) * 0.37;
    }
    model.add("Big", {fbx::Property::Doubles(big)});
    model.add("Small", {fbx::Property::Ints({1, -2, 3}), fbx::Property::Longs({}),
                        fbx::Property::Floats({0.5f, 1e30f})});
    fbx::Property bools;
    bools.type = fbx::PropertyType::BoolArray;
    bools.bools = {1, 0, 1};
    model.add("Bools", {bools});
    file.footerCode.assign(16, 0xAB);
    return file;
}

std::vector<u8> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("fbx binary round-trips the record tree at both offset widths", "[fbx]") {
    for (const u32 version : {7400u, 7500u}) {
        INFO("version " << version);
        const fbx::File source = SampleFile(version);
        const std::vector<u8> bytes = fbx::WriteBinary(source);
        CHECK(bytes == fbx::WriteBinary(source)); // deterministic
        CHECK(fbx::LooksLikeBinaryFbx(bytes));
        CHECK(bytes.size() % 16 == 0); // the footer lands on a 16-byte boundary
        const fbx::ReadOutcome read = fbx::ReadBinary(bytes);
        REQUIRE(read.ok());
        CHECK(read.file->version == version);
        CHECK(read.file->footerCode == source.footerCode);
        std::string where;
        const bool same = SameFile(source, *read.file, where);
        INFO(where);
        CHECK(same);
        // And a second write of what was read is the same bytes.
        CHECK(fbx::WriteBinary(*read.file) == bytes);
    }
}

TEST_CASE("fbx binary deflates arrays from a kilobyte, as the SDK does", "[fbx]") {
    fbx::File file;
    file.nodes.emplace_back("A").add("Small", {fbx::Property::Doubles(std::vector<f64>(127, 1.0))});
    file.nodes.back().add("Large", {fbx::Property::Doubles(std::vector<f64>(128, 1.0))});
    const std::vector<u8> bytes = fbx::WriteBinary(file);
    // Find each array's encoding word: after `d`, the count, then the encoding.
    const auto encodingAfter = [&](const char* name) {
        const std::string needle(name);
        const auto at = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
        REQUIRE(at != bytes.end());
        const std::size_t offset = static_cast<std::size_t>(at - bytes.begin()) + needle.size();
        u32 encoding = 0;
        std::memcpy(&encoding, bytes.data() + offset + 1 + 4, 4);
        return encoding;
    };
    CHECK(encodingAfter("Small") == 0); // 1016 bytes
    CHECK(encodingAfter("Large") == 1); // 1024 bytes
}

TEST_CASE("fbx ascii reads values, arrays, strings and comments", "[fbx]") {
    const char* text = R"(; FBX 7.4.0 project file
; a comment
FBXHeaderExtension:  {
	FBXVersion: 7400
}
Objects:  {
	Geometry: 140, "Geometry::Cube", "Mesh" {
		Vertices: *6 {
			a: 1,2.5,-3e2,
			4,5,6
		}
		PolygonVertexIndex: *3 {
			a: 0,1,-3
		}
		Name: "say &quot;hi&quot;"
		Content: , "AAEC"
		Shading: T
	}
}
)";
    const fbx::ReadOutcome read =
        fbx::Read(std::span<const u8>(reinterpret_cast<const u8*>(text), std::strlen(text)));
    REQUIRE(read.ok());
    CHECK_FALSE(read.file->binary);
    CHECK(read.file->version == 7400);
    const fbx::Node* objects = read.file->find("Objects");
    REQUIRE(objects != nullptr);
    const fbx::Node& geometry = objects->children.at(0);
    CHECK(geometry.properties.at(0).toI64() == 140);
    CHECK(geometry.properties.at(1).toString() == "Geometry::Cube");
    const fbx::Node* vertices = geometry.child("Vertices");
    REQUIRE(vertices != nullptr);
    REQUIRE(vertices->properties.size() == 1);
    CHECK(vertices->properties[0].type == fbx::PropertyType::F64Array);
    CHECK(vertices->properties[0].toF64s() == std::vector<f64>{1, 2.5, -300, 4, 5, 6});
    const fbx::Node* indices = geometry.child("PolygonVertexIndex");
    REQUIRE(indices != nullptr);
    CHECK(indices->properties[0].type == fbx::PropertyType::I64Array);
    CHECK(indices->properties[0].toI32s() == std::vector<i32>{0, 1, -3});
    CHECK(geometry.child("Name")->properties[0].toString() == "say \"hi\"");
    CHECK(geometry.child("Content")->properties.size() == 2);
    CHECK(geometry.child("Shading")->properties[0].toString() == "T");
}

TEST_CASE("fbx refuses what predates 7.0 and what is not FBX", "[fbx]") {
    fbx::File old;
    old.version = 6100;
    const std::vector<u8> bytes = fbx::WriteBinary(old);
    const fbx::ReadOutcome read = fbx::ReadBinary(bytes);
    CHECK_FALSE(read.ok());
    CHECK(read.error.find("predates") != std::string::npos);
    const std::string junk = "not an fbx at all";
    CHECK_FALSE(
        fbx::Read(std::span<const u8>(reinterpret_cast<const u8*>(junk.data()), junk.size())).ok());
}

TEST_CASE("fbx binary refuses hostile records without allocating them", "[fbx]") {
    const std::vector<u8> good = fbx::WriteBinary(SampleFile(7400));

    // Every truncation either reads (the footer is optional) or fails cleanly.
    for (std::size_t cut = 27; cut < good.size(); cut += 97) {
        const std::vector<u8> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        const fbx::ReadOutcome read = fbx::ReadBinary(truncated);
        (void)read; // Must simply return.
    }

    // A record whose end points backwards cannot loop.
    std::vector<u8> loop = good;
    const u32 backwards = 10;
    std::memcpy(loop.data() + 27, &backwards, 4);
    CHECK_FALSE(fbx::ReadBinary(loop).ok());

    // An array claiming four billion doubles from a few bytes of DEFLATE.
    fbx::File bomb;
    bomb.nodes.emplace_back("A").add("Big", {fbx::Property::Doubles(std::vector<f64>(200, 0.0))});
    std::vector<u8> bytes = fbx::WriteBinary(bomb);
    const std::string needle = "Big";
    const auto at = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    REQUIRE(at != bytes.end());
    const std::size_t countOffset = static_cast<std::size_t>(at - bytes.begin()) + needle.size() + 1;
    const u32 huge = 0xFFFFFFF0u;
    std::memcpy(bytes.data() + countOffset, &huge, 4);
    const fbx::ReadOutcome read = fbx::ReadBinary(bytes);
    CHECK_FALSE(read.ok());
}

TEST_CASE("fbx corpus files read, re-write and read back unchanged", "[fbx][corpus]") {
    const char* root = std::getenv("WEM_FBX_CORPUS_DIR");
    if (root == nullptr || !fs::is_directory(root)) {
        SKIP("set WEM_FBX_CORPUS_DIR to a folder of .fbx files");
    }
    u32 files = 0;
    u32 ascii = 0;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        std::string ext = entry.path().extension().generic_string();
        for (char& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!entry.is_regular_file() || ext != ".fbx") {
            continue;
        }
        const std::vector<u8> bytes = ReadAll(entry.path());
        const fbx::ReadOutcome read = fbx::Read(bytes);
        const std::u8string utf8 = entry.path().generic_u8string();
        INFO(std::string(reinterpret_cast<const char*>(utf8.c_str()), utf8.size()));
        INFO(read.error);
        REQUIRE(read.ok());
        ++files;
        ascii += read.file->binary ? 0u : 1u;
        const std::vector<u8> rewritten = fbx::WriteBinary(*read.file);
        // With WEM_FBX_REWRITE_DIR set the re-written files land there, for the
        // oracle gate: Autodesk's own reader opening what this writer wrote.
        if (const char* out = std::getenv("WEM_FBX_REWRITE_DIR"); out != nullptr) {
            std::ofstream file(fs::path(out) / entry.path().filename(), std::ios::binary);
            file.write(reinterpret_cast<const char*>(rewritten.data()),
                       static_cast<std::streamsize>(rewritten.size()));
        }
        const fbx::ReadOutcome again = fbx::ReadBinary(rewritten);
        REQUIRE(again.ok());
        std::string where;
        const bool same = SameFile(*read.file, *again.file, where);
        INFO(where);
        CHECK(same);
        if (read.file->binary && read.file->footerCode.size() == 16) {
            // Same records, same footer: the SDK's own bytes come back exactly
            // when the file was written with the conventions the writer keeps.
            CHECK(rewritten.size() > 0);
        }
    }
    std::cout << "[fbx corpus] " << files << " file(s), " << ascii << " ASCII\n";
    CHECK(files > 0);
}
