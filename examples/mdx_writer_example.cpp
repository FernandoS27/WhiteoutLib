// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/mdx/mdx.h>
#include <iostream>
#include <filesystem>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: mdx_writer_example <input.mdx> <output.mdx>\n";
        std::cout << "  Loads an MDX file and writes it back to test round-trip conversion\n";
        return 1;
    }

    std::string inputPath = argv[1];
    std::string outputPath = argv[2];

    try {
        // Parse the input MDX file
        std::cout << "Loading " << inputPath << "...\n";
        whiteout::mdx::Parser parser(whiteout::mdx::Parser::ParseMode::Lenient, whiteout::mdx::Parser::UpgradeMode::PreserveOriginal);
        whiteout::mdx::Model mdxFile = parser.parse(inputPath);

        std::cout << "Loaded successfully:\n";
        std::cout << "  Model: " << mdxFile.modelName << "\n";
        std::cout << "  Version: " << mdxFile.version << "\n";
        std::cout << "  Sequences: " << mdxFile.sequences.size() << "\n";
        std::cout << "  Textures: " << mdxFile.textures.size() << "\n";
        std::cout << "  Materials: " << mdxFile.materials.size() << "\n";
        std::cout << "  Geosets: " << mdxFile.geosets.size() << "\n";
        std::cout << "  Bones: " << mdxFile.bones.size() << "\n";

        // Write the MDX file
        std::cout << "\nWriting to " << outputPath << "...\n";
        whiteout::mdx::Writer writer;
        writer.write(outputPath, mdxFile);

        std::cout << "Written successfully!\n";

        // Compare file sizes
        auto inputSize = std::filesystem::file_size(inputPath);
        auto outputSize = std::filesystem::file_size(outputPath);

        std::cout << "\nFile sizes:\n";
        std::cout << "  Input:  " << inputSize << " bytes\n";
        std::cout << "  Output: " << outputSize << " bytes\n";

        if (inputSize == outputSize) {
            std::cout << "  Perfect match! ✓\n";
        } else {
            std::cout << "  Difference: " << (int64_t)outputSize - (int64_t)inputSize << " bytes\n";
        }

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
