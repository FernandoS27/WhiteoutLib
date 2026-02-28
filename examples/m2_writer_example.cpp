
#include <m2/m2.h>
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <input_m2> <output_m2> [format]" << std::endl;
        std::cout << "\nExample: " << argv[0] << " creature.m2 creature_modified.m2 legion" << std::endl;
        std::cout << "\nFormat options:" << std::endl;
        std::cout << "  classic - Write in classic MD20 format (default)" << std::endl;
        std::cout << "  legion  - Write in Legion+ MD21 chunked format" << std::endl;
        return 1;
    }
    
    std::string inputPath = argv[1];
    std::string outputPath = argv[2];
    std::string formatStr = (argc > 3) ? argv[3] : "classic";
    
    m2::M2Format outputFormat = m2::M2Format::ClassicMD20;
    if (formatStr == "legion" || formatStr == "md21") {
        outputFormat = m2::M2Format::LegionMD21;
    }
    
    try {
        std::cout << "Loading input M2 file: " << inputPath << std::endl;
        m2::M2Parser parser(m2::ParseMode::Lenient);
        m2::M2File model = parser.parse(inputPath);
        
        const auto& issues = parser.getIssues();
        if (!issues.empty()) {
            std::cout << "Parsing warnings/errors encountered:" << std::endl;
            for (const auto& issue : issues) {
                std::cout << "  - " << issue << std::endl;
            }
        }
        
        std::cout << "\nModifying model..." << std::endl;
        std::cout << "  - Bones: " << model.header.bones.size() << std::endl;
        std::cout << "  - Vertices: " << model.header.vertices.size() << std::endl;
        
        std::cout << "\nWriting output M2 file: " << outputPath << std::endl;
        std::cout << "Format: " << (outputFormat == m2::M2Format::ClassicMD20 ? "Classic MD20" : "Legion MD21") << std::endl;
        
        m2::M2Writer writer;
        writer.write(outputPath, model);
        
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

