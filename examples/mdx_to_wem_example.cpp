// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// MDX-to-WEM example: loads an MDX file, converts it to WEM, prints a summary,
/// and optionally writes the WEM file to disk.

#include <whiteout/models/wem/wem.h>
#include <whiteout/models/mdx/mdx.h>
#include <iostream>

using namespace whiteout;
using namespace whiteout::models;

static void printWemSummary(const wem::Model& model) {
    std::cout << "\n=== WEM Model Summary ===" << std::endl;
    std::cout << "Name: " << model.name << std::endl;
    std::cout << "Bounds radius: " << model.bounds.sphereRadius << std::endl;
    std::cout << "Textures: " << model.textures.size() << std::endl;
    for (size_t i = 0; i < model.textures.size(); ++i) {
        std::cout << "  [" << i << "] " << model.textures[i].path << std::endl;
    }
    std::cout << "Materials: " << model.materials.size() << std::endl;
    for (size_t i = 0; i < model.materials.size(); ++i) {
        const auto& mat = model.materials[i];
        const char* typeStr = (mat.type == wem::MaterialType::Standard) ? "Standard" : "Composite";
        std::cout << "  [" << i << "] \"" << mat.name << "\" (" << typeStr
                  << ", slots=" << mat.textureSlots.size() << ")" << std::endl;
    }
    std::cout << "Meshes: " << model.meshes.size() << std::endl;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const auto& mesh = model.meshes[i];
        std::cout << "  [" << i << "] \"" << mesh.name << "\""
                  << " verts=" << mesh.positions.size()
                  << " tris=" << (mesh.indices.size() / 3)
                  << " submeshes=" << mesh.submeshes.size()
                  << " uvSets=" << mesh.uvSets.size()
                  << std::endl;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <input.mdx> [output.wem]" << std::endl;
        std::cout << "\nConverts an MDX file to WEM format." << std::endl;
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath;
    if (argc >= 3) {
        outputPath = argv[2];
    }

    // Load MDX
    std::cout << "Loading MDX: " << inputPath << std::endl;
    mdx::Parser mdxParser;
    mdx::Model mdxModel = mdxParser.parse(inputPath);

    std::cout << "MDX loaded: " << mdxModel.modelName << std::endl;
    std::cout << "  Geosets: " << mdxModel.geosets.size() << std::endl;
    std::cout << "  Materials: " << mdxModel.materials.size() << std::endl;
    std::cout << "  Textures: " << mdxModel.textures.size() << std::endl;

    // Convert to WEM
    std::cout << "\nConverting MDX -> WEM..." << std::endl;
    auto result = wem::fromMdx(mdxModel);

    if (!result.issues.empty()) {
        std::cout << "\nConversion issues:" << std::endl;
        for (const auto& issue : result.issues) {
            std::cout << "  - " << issue << std::endl;
        }
    }

    printWemSummary(result.model);

    // Optionally write WEM file
    if (!outputPath.empty()) {
        std::cout << "\nWriting WEM: " << outputPath << std::endl;
        wem::Writer writer;
        if (writer.write(outputPath, result.model)) {
            std::cout << "Written successfully." << std::endl;
        } else {
            std::cerr << "Failed to write WEM file." << std::endl;
            return 1;
        }
    }

    // Round-trip back to MDX for verification
    std::cout << "\nRound-trip: WEM -> MDX..." << std::endl;
    auto mdxResult = wem::toMdx(result.model);

    if (!mdxResult.issues.empty()) {
        std::cout << "Round-trip issues:" << std::endl;
        for (const auto& issue : mdxResult.issues) {
            std::cout << "  - " << issue << std::endl;
        }
    }

    std::cout << "Round-trip MDX:" << std::endl;
    std::cout << "  Geosets: " << mdxResult.model.geosets.size()
              << " (original: " << mdxModel.geosets.size() << ")" << std::endl;
    std::cout << "  Materials: " << mdxResult.model.materials.size()
              << " (original: " << mdxModel.materials.size() << ")" << std::endl;
    std::cout << "  Textures: " << mdxResult.model.textures.size()
              << " (original: " << mdxModel.textures.size() << ")" << std::endl;

    std::cout << "\nDone." << std::endl;
    return 0;
}
