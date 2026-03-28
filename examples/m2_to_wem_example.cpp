// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// M2-to-WEM example: loads an M2 file bundle, converts it to WEM, prints a
/// summary, and optionally writes the WEM file to disk.

#include <whiteout/models/wem/wem.h>
#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>
#include <filesystem>
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
        std::cout << "Usage: " << argv[0] << " <input.m2> [output.wem]" << std::endl;
        std::cout << "\nConverts an M2 file bundle to WEM format." << std::endl;
        std::cout << "The parser auto-discovers .skin, .anim, .skel, and .bone sibling files." << std::endl;
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath;
    if (argc >= 3) {
        outputPath = argv[2];
    }

    try {
        // Load M2
        std::cout << "Loading M2: " << inputPath << std::endl;
        std::filesystem::path p(inputPath);
        whiteout::utils::OsFileSystem vfs(p.parent_path().string());
        m2::Parser m2Parser(m2::Parser::ParseMode::Lenient);
        auto m2Model = m2Parser.parse(vfs, inputPath);

        const auto& m2Issues = m2Parser.getIssues();
        if (!m2Issues.empty()) {
            std::cout << "\nM2 parsing issues:" << std::endl;
            for (const auto& issue : m2Issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }

        std::cout << "M2 loaded: " << m2Model.modelName << std::endl;
        std::cout << "  Skin profiles: " << m2Model.skinProfiles.size() << std::endl;
        std::cout << "  Vertices: " << m2Model.vertices.size() << std::endl;
        std::cout << "  Materials: " << m2Model.materials.size() << std::endl;
        std::cout << "  Textures: " << m2Model.textures.size() << std::endl;

        // Convert to WEM
        std::cout << "\nConverting M2 -> WEM..." << std::endl;
        auto result = wem::fromM2(m2Model);

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

        // Round-trip back to M2 for verification
        std::cout << "\nRound-trip: WEM -> M2..." << std::endl;
        auto m2Result = wem::toM2(result.model);

        if (!m2Result.issues.empty()) {
            std::cout << "Round-trip issues:" << std::endl;
            for (const auto& issue : m2Result.issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }

        const auto& rtModel = m2Result.model;
        std::cout << "Round-trip M2:" << std::endl;
        std::cout << "  Skin profiles: " << rtModel.skinProfiles.size()
                  << " (original: " << m2Model.skinProfiles.size() << ")" << std::endl;
        std::cout << "  Vertices: " << rtModel.vertices.size()
                  << " (original: " << m2Model.vertices.size() << ")" << std::endl;
        std::cout << "  Materials: " << rtModel.materials.size()
                  << " (original: " << m2Model.materials.size() << ")" << std::endl;

        std::cout << "\nDone." << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
