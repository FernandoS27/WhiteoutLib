// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// M3 loader example: reads an M3 file and prints model information.

#include <whiteout/models/m3/m3.h>
#include <iostream>
#include <iomanip>

void printModelInfo(const whiteout::m3::Model& model) {
    std::cout << "=== M3 Model Information ===" << std::endl;
    std::cout << "Name: " << model.name << std::endl;
    std::cout << "Bones: " << model.bones.size() << std::endl;
    std::cout << "Sequences: " << model.sequences.size() << std::endl;
    std::cout << "Divisions: " << model.divisions.size() << std::endl;
    std::cout << "Standard Materials: " << model.standardMaterials.size() << std::endl;
    std::cout << "Attachment Points: " << model.attachmentPoints.size() << std::endl;
    std::cout << "Particle Emitters: " << model.particleEmitters.size() << std::endl;
    std::cout << "Lights: " << model.lights.size() << std::endl;
    std::cout << "Cameras: " << model.cameras.size() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <m3_file>" << std::endl;
        std::cout << "\nExample: " << argv[0] << " model.m3" << std::endl;
        return 1;
    }

    std::string filePath = argv[1];

    try {
        whiteout::m3::Parser parser;

        std::cout << "Loading M3 file: " << filePath << std::endl;

        whiteout::m3::Model model = parser.parse(filePath);

        const auto& issues = parser.getIssues();
        if (!issues.empty()) {
            std::cout << "\n=== Parsing Issues ===" << std::endl;
            for (const auto& issue : issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }

        printModelInfo(model);

        std::cout << "\nModel loaded successfully!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
