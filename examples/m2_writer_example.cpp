// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow


#include <whiteout/models/m2/m2.h>
#include <whiteout/utils/os_file_system.h>
#include <filesystem>
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <input_m2> <output_m2>" << std::endl;
        std::cout << "\nExample: " << argv[0] << " creature.m2 creature_modified.m2" << std::endl;
        return 1;
    }
    
    std::string inputPath = argv[1];
    std::string outputPath = argv[2];
    
    try {
        std::cout << "Loading input M2 file: " << inputPath << std::endl;
        std::filesystem::path p(inputPath);
        whiteout::utils::OsFileSystem vfs(p.parent_path().string());
        whiteout::m2::Parser parser(whiteout::m2::Parser::ParseMode::Lenient);
        whiteout::m2::Model model = parser.parse(vfs, inputPath);
        
        const auto& issues = parser.getIssues();
        if (!issues.empty()) {
            std::cout << "Parsing warnings/errors encountered:" << std::endl;
            for (const auto& issue : issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }
        
        std::cout << "\nModifying model..." << std::endl;
        std::cout << "  - Bones: " << model.bones.size() << std::endl;
        std::cout << "  - Vertices: " << model.vertices.size() << std::endl;
        
        std::filesystem::path outP(outputPath);
        whiteout::utils::OsFileSystem outVfs(outP.parent_path().string());

        std::cout << "\nWriting output M2 file: " << outputPath << std::endl;

        whiteout::m2::Writer writer;
        writer.write(outVfs, outputPath, model);
        
        std::cout << "\nModel written successfully!" << std::endl;
        
        std::ifstream outFile(outputPath, std::ios::binary);
        if (outFile.good()) {
            outFile.seekg(0, std::ios::end);
            size_t fileSize = outFile.tellg();
            std::cout << "Output file size: " << fileSize << " bytes" << std::endl;
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

