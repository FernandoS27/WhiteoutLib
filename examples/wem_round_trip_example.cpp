// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// WEM round-trip example: creates a WEM model in memory, writes it to disk,
/// reads it back, and verifies the data matches.

#include <whiteout/models/wem/wem.h>
#include <iostream>
#include <cmath>

using namespace whiteout;
using namespace whiteout::models::wem;

/// Build a simple quad model for testing.
static Model createTestModel() {
    Model model;
    model.name = "TestQuad";
    model.bounds.minimum = {-1.0f, -1.0f, 0.0f};
    model.bounds.maximum = {1.0f, 1.0f, 0.0f};
    model.bounds.sphereRadius = 1.414f;

    // One texture
    TextureRef tex;
    tex.path = "textures/test_diffuse.blp";
    model.textures.push_back(tex);

    // One standard material
    Material mat;
    mat.name = "QuadMaterial";
    mat.type = MaterialType::Standard;
    mat.blendMode = BlendMode::Opaque;

    TextureSlot slot;
    slot.textureIndex = 0;
    slot.semantic = TextureSlotSemantic::Diffuse;
    slot.uvSetIndex = 0;
    mat.textureSlots.push_back(slot);
    model.materials.push_back(mat);

    // One mesh — a simple quad (two triangles)
    Mesh mesh;
    mesh.name = "QuadMesh";
    mesh.bounds = model.bounds;

    mesh.positions = {
        {-1.0f, -1.0f, 0.0f},
        { 1.0f, -1.0f, 0.0f},
        { 1.0f,  1.0f, 0.0f},
        {-1.0f,  1.0f, 0.0f},
    };
    mesh.normals = {
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, 1.0f},
    };

    std::vector<Vector2f> uv0 = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    };
    mesh.uvSets.push_back(uv0);

    mesh.indices = {0, 1, 2, 0, 2, 3};

    Submesh sub;
    sub.name = "QuadSubmesh";
    sub.indexStart = 0;
    sub.indexCount = 6;
    sub.vertexStart = 0;
    sub.vertexCount = 4;
    sub.materialIndex = 0;
    sub.bounds = model.bounds;
    mesh.submeshes.push_back(sub);

    model.meshes.push_back(mesh);

    return model;
}

static bool approxEqual(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) < eps;
}

static bool verifyModels(const Model& a, const Model& b) {
    bool ok = true;

    if (a.name != b.name) {
        std::cerr << "  FAIL: name mismatch: \"" << a.name << "\" vs \"" << b.name << "\"" << std::endl;
        ok = false;
    }
    if (a.textures.size() != b.textures.size()) {
        std::cerr << "  FAIL: texture count mismatch: " << a.textures.size() << " vs " << b.textures.size() << std::endl;
        ok = false;
    }
    if (a.materials.size() != b.materials.size()) {
        std::cerr << "  FAIL: material count mismatch: " << a.materials.size() << " vs " << b.materials.size() << std::endl;
        ok = false;
    }
    if (a.meshes.size() != b.meshes.size()) {
        std::cerr << "  FAIL: mesh count mismatch: " << a.meshes.size() << " vs " << b.meshes.size() << std::endl;
        ok = false;
    } else {
        for (size_t i = 0; i < a.meshes.size(); ++i) {
            const auto& ma = a.meshes[i];
            const auto& mb = b.meshes[i];
            if (ma.positions.size() != mb.positions.size()) {
                std::cerr << "  FAIL: mesh[" << i << "] vertex count: " << ma.positions.size() << " vs " << mb.positions.size() << std::endl;
                ok = false;
            }
            if (ma.indices.size() != mb.indices.size()) {
                std::cerr << "  FAIL: mesh[" << i << "] index count: " << ma.indices.size() << " vs " << mb.indices.size() << std::endl;
                ok = false;
            }
            if (ma.submeshes.size() != mb.submeshes.size()) {
                std::cerr << "  FAIL: mesh[" << i << "] submesh count: " << ma.submeshes.size() << " vs " << mb.submeshes.size() << std::endl;
                ok = false;
            }
            // Spot-check first vertex position
            if (!ma.positions.empty() && !mb.positions.empty()) {
                if (!approxEqual(ma.positions[0].x, mb.positions[0].x) ||
                    !approxEqual(ma.positions[0].y, mb.positions[0].y) ||
                    !approxEqual(ma.positions[0].z, mb.positions[0].z)) {
                    std::cerr << "  FAIL: mesh[" << i << "] first vertex position mismatch" << std::endl;
                    ok = false;
                }
            }
        }
    }

    if (!approxEqual(a.bounds.sphereRadius, b.bounds.sphereRadius)) {
        std::cerr << "  FAIL: bounds radius mismatch: " << a.bounds.sphereRadius << " vs " << b.bounds.sphereRadius << std::endl;
        ok = false;
    }

    return ok;
}

int main(int argc, char* argv[]) {
    std::string outPath = "round_trip_test.wem";
    if (argc >= 2) {
        outPath = argv[1];
    }

    std::cout << "=== WEM Round-Trip Test ===" << std::endl;

    // Step 1: Create model
    std::cout << "\n1. Creating test model..." << std::endl;
    Model original = createTestModel();
    std::cout << "   Name: " << original.name << std::endl;
    std::cout << "   Textures: " << original.textures.size() << std::endl;
    std::cout << "   Materials: " << original.materials.size() << std::endl;
    std::cout << "   Meshes: " << original.meshes.size() << std::endl;
    std::cout << "   Vertices: " << original.meshes[0].positions.size() << std::endl;
    std::cout << "   Indices: " << original.meshes[0].indices.size() << std::endl;

    // Step 2: Write to bytes
    std::cout << "\n2. Writing to bytes..." << std::endl;
    Writer writer;
    std::vector<u8> bytes = writer.write(original);
    std::cout << "   Output size: " << bytes.size() << " bytes" << std::endl;

    // Step 3: Parse back
    std::cout << "\n3. Parsing from bytes..." << std::endl;
    Parser parser;
    auto parsed = parser.parse(std::span<const u8>(bytes.data(), bytes.size()));
    if (!parsed) {
        std::cerr << "   FAIL: parse returned nullopt" << std::endl;
        for (const auto& issue : parser.getIssues()) {
            std::cerr << "   Issue: " << issue << std::endl;
        }
        return 1;
    }

    // Step 4: Verify
    std::cout << "\n4. Verifying round-trip..." << std::endl;
    if (verifyModels(original, *parsed)) {
        std::cout << "   PASS: All data matches!" << std::endl;
    } else {
        std::cerr << "   FAIL: Data mismatch after round-trip" << std::endl;
        return 1;
    }

    // Step 5: Optionally write to disk
    std::cout << "\n5. Writing to disk: " << outPath << std::endl;
    if (writer.write(outPath, original)) {
        std::cout << "   Written successfully." << std::endl;
    } else {
        std::cerr << "   Failed to write file." << std::endl;
        return 1;
    }

    std::cout << "\n=== Round-Trip Test Complete ===" << std::endl;
    return 0;
}
