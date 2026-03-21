// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// M3-to-WEM example: loads an M3 file, converts it to WEM, prints a summary,
/// and optionally writes the WEM file to disk.

#include <whiteout/models/wem/wem.h>
#include <whiteout/models/m3/m3.h>
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
        std::cout << "Usage: " << argv[0] << " <input.m3> [output.wem]" << std::endl;
        std::cout << "\nConverts an M3 file to WEM format." << std::endl;
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath;
    if (argc >= 3) {
        outputPath = argv[2];
    }

    try {
        // Load M3
        std::cout << "Loading M3: " << inputPath << std::endl;
        m3::Parser m3Parser(m3::Parser::ParseMode::Lenient);
        m3::Model m3Model = m3Parser.parse(inputPath);

        const auto& m3Issues = m3Parser.getIssues();
        if (!m3Issues.empty()) {
            std::cout << "\nM3 parsing issues:" << std::endl;
            for (const auto& issue : m3Issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }

        std::cout << "M3 loaded: " << m3Model.name << std::endl;
        std::cout << "  Divisions: " << m3Model.divisions.size() << std::endl;
        std::cout << "  Standard Materials: " << m3Model.standardMaterials.size() << std::endl;
        std::cout << "  Composite Materials: " << m3Model.compositeMaterials.size() << std::endl;

        // Convert to WEM
        std::cout << "\nConverting M3 -> WEM..." << std::endl;
        auto result = wem::fromM3(m3Model);

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

        // Round-trip back to M3 for verification
        std::cout << "\nRound-trip: WEM -> M3..." << std::endl;
        auto m3Result = wem::toM3(result.model);

        if (!m3Result.issues.empty()) {
            std::cout << "Round-trip issues:" << std::endl;
            for (const auto& issue : m3Result.issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }

        std::cout << "Round-trip M3:" << std::endl;
        std::cout << "  Divisions: " << m3Result.model.divisions.size()
                  << " (original: " << m3Model.divisions.size() << ")" << std::endl;
        std::cout << "  Standard Materials: " << m3Result.model.standardMaterials.size()
                  << " (original: " << m3Model.standardMaterials.size() << ")" << std::endl;
        std::cout << "  Composite Materials: " << m3Result.model.compositeMaterials.size()
                  << " (original: " << m3Model.compositeMaterials.size() << ")" << std::endl;

        std::cout << "\nDone." << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
