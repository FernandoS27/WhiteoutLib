// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
//
// PHYS validation: every `.phys` in the corpus, and every PFDC payload carried
// inline by an `.m2` there, is parsed and written back. The write has to
// reproduce the original bytes exactly — a chunk read with the wrong stride or
// written in the wrong order shows up immediately as a length or offset
// mismatch, which structural comparison would miss.

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>

namespace fs = std::filesystem;
using namespace whiteout;

namespace {

std::vector<u8> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string corpusDir() {
    if (const char* env = std::getenv("M2_CORPUS_DIR"); env && fs::is_directory(env)) {
        return env;
    }
    for (auto candidate : {"Corpus/WoW", "../Corpus/WoW", "../../Corpus/WoW"}) {
        if (fs::is_directory(candidate)) {
            return candidate;
        }
    }
    return {};
}

constexpr u32 PFDC_TAG = m2::makeTag("PFDC");

/// The PFDC payload of a chunked `.m2`, padding included, or empty.
std::vector<u8> extractPfdc(const std::vector<u8>& m2) {
    constexpr u32 md21 = m2::MD21_TAG;
    if (m2.size() < 8 || std::memcmp(m2.data(), &md21, 4) != 0) {
        return {};
    }
    for (size_t pos = 0; pos + 8 <= m2.size();) {
        u32 tag = 0;
        u32 size = 0;
        std::memcpy(&tag, m2.data() + pos, 4);
        std::memcpy(&size, m2.data() + pos + 4, 4);
        if (pos + 8 + size > m2.size()) {
            break;
        }
        if (tag == PFDC_TAG) {
            return std::vector<u8>(m2.begin() + pos + 8, m2.begin() + pos + 8 + size);
        }
        pos += 8 + size;
    }
    return {};
}

/// Where two buffers first differ, for a failure message that says something.
std::string describeMismatch(const std::vector<u8>& expected, const std::vector<u8>& actual) {
    std::string out =
        "size " + std::to_string(actual.size()) + " vs expected " + std::to_string(expected.size());
    const size_t common = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < common; ++i) {
        if (expected[i] != actual[i]) {
            out += ", first differing byte at " + std::to_string(i);
            break;
        }
    }
    return out;
}

struct Totals {
    size_t files = 0;
    size_t parsed = 0;
    size_t exact = 0;
    std::map<u16, size_t> versions;
    std::vector<std::string> failures;
};

void checkRoundTrip(Totals& totals, const std::string& name, const std::vector<u8>& original,
                    size_t padTo) {
    totals.files++;

    std::vector<std::string> issues;
    auto physics = m2::parsePhysics(original, &issues);
    if (!physics) {
        totals.failures.push_back(name + ": no PHYS chunk");
        return;
    }
    totals.parsed++;
    totals.versions[physics->version]++;

    for (const auto& issue : issues) {
        totals.failures.push_back(name + ": " + issue);
    }

    auto rewritten = m2::writePhysics(*physics);
    if (padTo > 1) {
        rewritten.resize((rewritten.size() + padTo - 1) / padTo * padTo, 0);
    }

    if (rewritten == original) {
        totals.exact++;
    } else {
        totals.failures.push_back(name + ": " + describeMismatch(original, rewritten));
    }
}

void report(const char* label, const Totals& totals) {
    std::cout << "  " << label << ": " << totals.parsed << "/" << totals.files << " parsed, "
              << totals.exact << " byte-exact round-trips" << std::endl;
    std::cout << "    versions:";
    for (const auto& [version, count] : totals.versions) {
        std::cout << " v" << version << "=" << count;
    }
    std::cout << std::endl;
    for (size_t i = 0; i < totals.failures.size() && i < 20; ++i) {
        std::cout << "    [FAIL] " << totals.failures[i] << std::endl;
    }
}

} // namespace

// BOXS, SPHJ, DSTJ, PRSJ, BDY2 and the pre-motor SHOJ appear in no corpus file,
// so the only thing that can be checked about them here is that the reader and
// the writer agree — a field one side writes and the other skips shows up as a
// shifted stride the moment a second record follows it.
TEST_CASE("PHYS layouts absent from the corpus are read as written", "[m2][phys]") {
    const u16 version = GENERATE(u16{0}, u16{2}, u16{3}, u16{5}, u16{6});

    m2::PhysicsData source;
    source.version = version;
    source.phyt = 3;

    for (u32 i = 0; i < 3; ++i) {
        const f32 f = static_cast<f32>(i) + 1.0f;
        m2::PhysicsFrame frame;
        frame.axisX = {f, f * 2, f * 3};
        frame.axisY = {f * 4, f * 5, f * 6};
        frame.axisZ = {f * 7, f * 8, f * 9};
        frame.origin = {f * 10, f * 11, f * 12};

        m2::BoxShape box;
        box.frame = frame;
        box.halfExtents = {f, f * 2, f * 3};
        source.boxShapes.push_back(box);

        m2::PhysicsShape shape;
        shape.shapeType = m2::PhysicsShapeType::Box;
        shape.shapeIndex = static_cast<i16>(i);
        shape.friction = f;
        shape.restitution = f * 2;
        shape.density = f * 3;
        shape.unknown14 = f * 4;
        shape.scale = f;
        shape.unknown1c = static_cast<u16>(i);
        shape.padding1e = static_cast<u16>(i * 7);
        source.shapes.push_back(shape);

        m2::PhysicsBody body;
        body.type = i == 0 ? m2::PhysicsBodyType::Kinematic : m2::PhysicsBodyType::Dynamic;
        body.boneIndex = static_cast<u16>(i * 4);
        body.position = {f, -f, f * 0.5f};
        body.shapeIndex = static_cast<i32>(i);
        body.shapeCount = 1;
        body.gravityScale = f;
        body.inertiaScale = f * 2;
        body.linearDamping = f * 3;
        body.angularDamping = f * 4;
        body.unknown28 = f * 5;
        body.unknown2c = static_cast<u16>(0x8000 | i);
        source.bodies.push_back(body);

        m2::SphericalJoint spherical;
        spherical.anchorA = {f, f, f};
        spherical.anchorB = {-f, -f, -f};
        spherical.frictionTorque = f * 6;
        source.sphericalJoints.push_back(spherical);

        m2::DistanceJoint distance;
        distance.localAnchorA = {f, 0, 0};
        distance.localAnchorB = {0, f, 0};
        distance.distance = f * 7;
        source.distanceJoints.push_back(distance);

        m2::PrismaticJoint prismatic;
        prismatic.frameA = frame;
        prismatic.frameB = frame;
        prismatic.lowerLimit = -f;
        prismatic.upperLimit = f;
        prismatic.unknown68 = f * 2;
        prismatic.maxMotorForce = f * 3;
        prismatic.unknown70 = f * 4;
        prismatic.motorMode = i;
        prismatic.motorFrequencyHz = f * 5;
        prismatic.motorDampingRatio = f * 6;
        source.prismaticJoints.push_back(prismatic);

        m2::ShoulderJoint shoulder;
        shoulder.frameA = frame;
        shoulder.frameB = frame;
        shoulder.lowerTwistAngle = -f;
        shoulder.upperTwistAngle = f;
        shoulder.coneAngle = f * 2;
        shoulder.maxMotorTorque = f * 3;
        shoulder.motorMode = i;
        shoulder.motorFrequencyHz = f * 4;
        shoulder.motorDampingRatio = f * 5;
        source.shoulderJoints.push_back(shoulder);

        m2::PhysicsJoint joint;
        joint.bodyAIndex = 0;
        joint.bodyBIndex = i;
        joint.jointType = m2::PhysicsJointType::Spherical;
        joint.jointId = static_cast<i16>(i);
        source.joints.push_back(joint);
    }

    const auto bytes = m2::writePhysics(source);
    std::vector<std::string> issues;
    const auto parsed = m2::parsePhysics(bytes, &issues);

    INFO("version " << version);
    CAPTURE(issues);
    // A stride the writer and reader disagree on shows up here: the chunk size
    // stops being a whole number of records.
    CHECK(issues.empty());
    REQUIRE(parsed);

    CHECK(parsed->version == version);
    REQUIRE(parsed->bodies.size() == source.bodies.size());
    REQUIRE(parsed->shapes.size() == source.shapes.size());
    REQUIRE(parsed->boxShapes.size() == source.boxShapes.size());
    REQUIRE(parsed->sphericalJoints.size() == source.sphericalJoints.size());
    REQUIRE(parsed->distanceJoints.size() == source.distanceJoints.size());
    REQUIRE(parsed->prismaticJoints.size() == source.prismaticJoints.size());
    REQUIRE(parsed->shoulderJoints.size() == source.shoulderJoints.size());

    // Fields every layout carries survive verbatim; ones the target layout has
    // no room for come back as defaults, so they are not compared here.
    for (size_t i = 0; i < source.bodies.size(); ++i) {
        CHECK(parsed->bodies[i].type == source.bodies[i].type);
        CHECK(parsed->bodies[i].boneIndex == source.bodies[i].boneIndex);
        CHECK(parsed->bodies[i].position.x == source.bodies[i].position.x);
        CHECK(parsed->bodies[i].shapeIndex == source.bodies[i].shapeIndex);
        CHECK(parsed->bodies[i].shapeCount == source.bodies[i].shapeCount);

        CHECK(parsed->boxShapes[i].frame.axisX.x == source.boxShapes[i].frame.axisX.x);
        CHECK(parsed->boxShapes[i].frame.origin.z == source.boxShapes[i].frame.origin.z);
        CHECK(parsed->boxShapes[i].halfExtents.z == source.boxShapes[i].halfExtents.z);

        CHECK(parsed->shapes[i].shapeType == source.shapes[i].shapeType);
        CHECK(parsed->shapes[i].density == source.shapes[i].density);

        CHECK(parsed->sphericalJoints[i].frictionTorque ==
              source.sphericalJoints[i].frictionTorque);
        CHECK(parsed->distanceJoints[i].distance == source.distanceJoints[i].distance);
        CHECK(parsed->prismaticJoints[i].maxMotorForce == source.prismaticJoints[i].maxMotorForce);
        CHECK(parsed->shoulderJoints[i].coneAngle == source.shoulderJoints[i].coneAngle);
    }

    // Writing what was read reproduces the buffer, whatever the layout dropped.
    CHECK(m2::writePhysics(*parsed) == bytes);
}

TEST_CASE("PHYS corpus round-trip", "[m2][phys][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }

    Totals standalone;
    Totals inlined;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".phys") {
            checkRoundTrip(standalone, entry.path().filename().string(), readFile(entry.path()), 1);
        } else if (ext == ".m2") {
            auto payload = extractPfdc(readFile(entry.path()));
            if (!payload.empty()) {
                // PFDC zero-pads its payload out to a 16-byte multiple.
                checkRoundTrip(inlined, entry.path().filename().string(), payload, 16);
            }
        }
    }

    std::cout << "=== PHYS corpus ===" << std::endl;
    report("standalone .phys", standalone);
    report("inline PFDC", inlined);

    REQUIRE(standalone.files + inlined.files > 0);
    CHECK(standalone.failures.empty());
    CHECK(inlined.failures.empty());
    CHECK(standalone.exact == standalone.files);
    CHECK(inlined.exact == inlined.files);
}

TEST_CASE("PHYS structure is coherent", "[m2][phys][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }

    size_t checked = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".phys") {
            continue;
        }
        auto physics = m2::parsePhysics(readFile(entry.path()));
        REQUIRE(physics);
        const std::string name = entry.path().filename().string();

        for (const auto& body : physics->bodies) {
            INFO(name << ": body shape range");
            REQUIRE(body.shapeIndex >= 0);
            REQUIRE(static_cast<size_t>(body.shapeIndex) + body.shapeCount <=
                    physics->shapes.size());
        }

        for (const auto& shape : physics->shapes) {
            INFO(name << ": shape index");
            REQUIRE(shape.shapeIndex >= 0);
            const size_t index = static_cast<size_t>(shape.shapeIndex);
            switch (shape.shapeType) {
            case m2::PhysicsShapeType::Box:
                REQUIRE(index < physics->boxShapes.size());
                break;
            case m2::PhysicsShapeType::Capsule:
                REQUIRE(index < physics->capsuleShapes.size());
                break;
            case m2::PhysicsShapeType::Sphere:
                REQUIRE(index < physics->sphereShapes.size());
                break;
            case m2::PhysicsShapeType::Polytope:
                REQUIRE(index < physics->polytopeShapes.size());
                break;
            }
        }

        for (const auto& joint : physics->joints) {
            INFO(name << ": joint bodies");
            REQUIRE(joint.bodyAIndex < physics->bodies.size());
            REQUIRE(joint.bodyBIndex < physics->bodies.size());
            REQUIRE(joint.jointId >= 0);
            const size_t id = static_cast<size_t>(joint.jointId);
            switch (joint.jointType) {
            case m2::PhysicsJointType::Spherical:
                REQUIRE(id < physics->sphericalJoints.size());
                break;
            case m2::PhysicsJointType::Shoulder:
                REQUIRE(id < physics->shoulderJoints.size());
                break;
            case m2::PhysicsJointType::Weld:
                REQUIRE(id < physics->weldJoints.size());
                break;
            case m2::PhysicsJointType::Revolute:
                REQUIRE(id < physics->revoluteJoints.size());
                break;
            case m2::PhysicsJointType::Prismatic:
                REQUIRE(id < physics->prismaticJoints.size());
                break;
            case m2::PhysicsJointType::Distance:
                REQUIRE(id < physics->distanceJoints.size());
                break;
            }
        }

        // The half-edge mesh indexes its own hull; a mis-split payload scatters
        // these out of range long before it changes the byte count. The twin
        // and face-cycle rules come straight from dmPolytope::FindEdge and
        // dmPolytope::GetSupportFace, so they pin the byte roles as well.
        for (const auto& polytope : physics->polytopeShapes) {
            INFO(name << ": polytope connectivity");
            REQUIRE(polytope.faceFirstEdges.size() == polytope.facePlanes.size());
            const size_t edgeCount = polytope.edges.size();
            for (size_t e = 0; e < edgeCount; ++e) {
                const auto& edge = polytope.edges[e];
                REQUIRE((edge.twinOffset == 1 || edge.twinOffset == -1));
                REQUIRE(edge.originVertex < polytope.vertices.size());
                REQUIRE(edge.faceIndex < polytope.facePlanes.size());
                REQUIRE(edge.nextEdge < edgeCount);

                const size_t twin = static_cast<size_t>(static_cast<i64>(e) + edge.twinOffset);
                REQUIRE(twin < edgeCount);
                REQUIRE(static_cast<i64>(twin) + polytope.edges[twin].twinOffset ==
                        static_cast<i64>(e));
                // Following the edge lands on the vertex its twin starts from.
                REQUIRE(polytope.edges[twin].originVertex ==
                        polytope.edges[edge.nextEdge].originVertex);
            }
            for (size_t f = 0; f < polytope.faceFirstEdges.size(); ++f) {
                size_t cursor = polytope.faceFirstEdges[f];
                REQUIRE(cursor < edgeCount);
                size_t steps = 0;
                do {
                    REQUIRE(polytope.edges[cursor].faceIndex == f);
                    cursor = polytope.edges[cursor].nextEdge;
                    REQUIRE(++steps <= edgeCount);
                } while (cursor != polytope.faceFirstEdges[f]);
                REQUIRE(steps >= 3);
            }
            REQUIRE(polytope.volume > 0.0f);
            REQUIRE(polytope.surfaceArea > 0.0f);
        }
        checked++;
    }
    REQUIRE(checked > 0);
}

// A model reaches its physics three ways; all three have to end up in
// Model::physics and survive a write.
TEST_CASE("M2 resolves physics through PFDC and PFID", "[m2][phys][corpus]") {
    const std::string dir = corpusDir();
    if (dir.empty()) {
        SKIP("WoW corpus not found");
    }

    size_t inlineModels = 0;
    size_t referencedModels = 0;

    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".m2") {
            continue;
        }
        const auto raw = readFile(entry.path());
        const bool hasPfdc = !extractPfdc(raw).empty();
        const bool hasSibling = fs::exists(fs::path(entry.path()).replace_extension(".phys"));
        if (!hasPfdc && !hasSibling) {
            continue;
        }
        if (hasPfdc ? inlineModels >= 5 : referencedModels >= 5) {
            continue;
        }

        const fs::path absPath = fs::absolute(entry.path());
        utils::OsFileSystem vfs(absPath.parent_path().string());
        m2::Parser parser;
        m2::Model model = parser.parse(vfs, absPath.string());

        INFO(entry.path().filename().string());
        REQUIRE(model.physics);
        REQUIRE_FALSE(model.physics->bodies.empty());
        CHECK(model.physics->version >= 4);
        // Inline physics is not also named by id, and vice versa — which one
        // the model used decides how the writer puts it back.
        CHECK(model.physicsFileId.has_value() == !hasPfdc);

        const fs::path outDir = fs::temp_directory_path() / "m2_phys_test";
        fs::create_directories(outDir);
        const fs::path outPath = outDir / entry.path().filename();
        utils::OsFileSystem outVfs(outDir.string());
        m2::Writer writer;
        writer.write(outVfs, outPath.string(), model);

        utils::OsFileSystem reVfs(outDir.string());
        m2::Parser reParser;
        m2::Model reModel = reParser.parse(reVfs, outPath.string());

        REQUIRE(reModel.physics);
        CHECK(reModel.physics->version == model.physics->version);
        CHECK(reModel.physics->bodies.size() == model.physics->bodies.size());
        CHECK(reModel.physics->shapes.size() == model.physics->shapes.size());
        CHECK(reModel.physics->joints.size() == model.physics->joints.size());
        CHECK(reModel.physics->polytopeShapes.size() == model.physics->polytopeShapes.size());
        CHECK(m2::writePhysics(*reModel.physics) == m2::writePhysics(*model.physics));

        fs::remove_all(outDir);

        if (hasPfdc) {
            inlineModels++;
        } else {
            referencedModels++;
        }
    }

    std::cout << "=== M2 physics resolution ===" << std::endl;
    std::cout << "  inline (PFDC): " << inlineModels
              << ", referenced (.phys sibling): " << referencedModels << std::endl;
    CHECK(inlineModels > 0);
    CHECK(referencedModels > 0);
}
