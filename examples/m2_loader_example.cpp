// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow


#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>
#include <filesystem>
#include <iostream>
#include <iomanip>

void printModelInfo(const whiteout::m2::BaseFile& base) {
    const auto& header = base.header.model;
    std::cout << "=== M2 Model Information ===" << std::endl;
    std::cout << "Version: " << base.header.version << std::endl;
    std::cout << "Bounding Sphere Radius: " << header.bounding.sphereRadius << std::endl;
    
    std::cout << "\n=== Animation Sequences ===" << std::endl;
    std::cout << "Number of sequences: " << header.sequences.size() << std::endl;
    
    std::cout << "\n=== Skeleton ===" << std::endl;
    std::cout << "Number of bones: " << header.bones.size() << std::endl;
    std::cout << "Key bones: " << header.keyBoneIds.size() << std::endl;
    
    std::cout << "\n=== Geometry ===" << std::endl;
    std::cout << "Number of vertices: " << header.vertices.size() << std::endl;
    std::cout << "Number of skin profiles: " << header.numSkinProfiles << std::endl;
    
    std::cout << "\n=== Textures and Materials ===" << std::endl;
    std::cout << "Number of textures: " << header.textures.size() << std::endl;
    std::cout << "Number of materials: " << header.materials.size() << std::endl;
    
    std::cout << "\n=== Effects ===" << std::endl;
    std::cout << "Number of lights: " << header.lights.size() << std::endl;
    std::cout << "Number of cameras: " << header.cameras.size() << std::endl;
    std::cout << "Number of attachments: " << header.attachments.size() << std::endl;
    std::cout << "Number of events: " << header.events.size() << std::endl;
    std::cout << "Number of particle emitters: " << header.particleEmitters.size() << std::endl;
    std::cout << "Number of ribbon emitters: " << header.ribbonEmitters.size() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <m2_file>" << std::endl;
        std::cout << "\nExample: " << argv[0] << " creature.m2" << std::endl;
        return 1;
    }
    
    std::string m2FilePath = argv[1];
    
    try {
        std::filesystem::path p(m2FilePath);
        whiteout::utils::OsFileSystem vfs(p.parent_path().string());
        whiteout::m2::Parser parser(whiteout::m2::Parser::ParseMode::Lenient);
        
        std::cout << "Loading M2 file: " << m2FilePath << std::endl;
        
        whiteout::m2::FileSystem model = parser.parse(vfs, m2FilePath);
        
        const auto& issues = parser.getIssues();
        if (!issues.empty()) {
            std::cout << "\n=== Parsing Issues ===" << std::endl;
            for (const auto& issue : issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }
        
        printModelInfo(model.base);
        
        std::cout << "\nModel loaded successfully!" << std::endl;
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

