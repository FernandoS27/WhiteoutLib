// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

/// M3 writer example: reads an M3 file and writes it back.

#include <whiteout/m3/m3.h>
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <input_m3> <output_m3>" << std::endl;
        std::cout << "\nExample: " << argv[0] << " model.m3 model_copy.m3" << std::endl;
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

    std::cout << "Loading input M3 file: " << inputPath << std::endl;
    whiteout::m3::Parser parser(whiteout::m3::Parser::ParseMode::Lenient);
    whiteout::m3::Model model = parser.parse(inputPath);

    const auto& issues = parser.getIssues();
    if (!issues.empty()) {
        std::cout << "Parsing warnings:" << std::endl;
        for (const auto& issue : issues) {
            std::cout << "  - " << issue << std::endl;
        }
    }

    std::cout << "  Bones: " << model.bones.size() << std::endl;
    std::cout << "  Sequences: " << model.sequences.size() << std::endl;

    std::cout << "\nWriting output M3 file: " << outputPath << std::endl;

    whiteout::m3::Writer writer;
    writer.write(outputPath, model);

    std::cout << "\nModel written successfully!" << std::endl;

    std::ifstream outFile(outputPath, std::ios::binary);
    if (outFile.good()) {
        outFile.seekg(0, std::ios::end);
        size_t fileSize = outFile.tellg();
        std::cout << "Output file size: " << fileSize << " bytes" << std::endl;
    }

    return 0;
}
