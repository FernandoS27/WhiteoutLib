
#include <m2/m2.h>
#include <iostream>
#include <iomanip>

void printModelInfo(const m2::M2File& model) {
    std::cout << "=== M2 Model Information ===" << std::endl;
    std::cout << "Version: " << model.header.version << std::endl;
    std::cout << "Bounding Sphere Radius: " << model.header.boundingSphereRadius << std::endl;
    
    std::cout << "\n=== Animation Sequences ===" << std::endl;
    std::cout << "Number of sequences: " << model.header.sequences.size() << std::endl;
    
    std::cout << "\n=== Skeleton ===" << std::endl;
    std::cout << "Number of bones: " << model.header.bones.size() << std::endl;
    std::cout << "Key bones: " << model.header.keyBoneIds.size() << std::endl;
    
    std::cout << "\n=== Geometry ===" << std::endl;
    std::cout << "Number of vertices: " << model.header.vertices.size() << std::endl;
    std::cout << "Number of skin profiles: " << model.header.numSkinProfiles << std::endl;
    
    std::cout << "\n=== Textures and Materials ===" << std::endl;
    std::cout << "Number of textures: " << model.header.textures.size() << std::endl;
    std::cout << "Number of materials: " << model.header.materials.size() << std::endl;
    
    std::cout << "\n=== Effects ===" << std::endl;
    std::cout << "Number of lights: " << model.header.lights.size() << std::endl;
    std::cout << "Number of cameras: " << model.header.cameras.size() << std::endl;
    std::cout << "Number of attachments: " << model.header.attachments.size() << std::endl;
    std::cout << "Number of events: " << model.header.events.size() << std::endl;
    std::cout << "Number of particle emitters: " << model.header.particleEmitters.size() << std::endl;
    std::cout << "Number of ribbon emitters: " << model.header.ribbonEmitters.size() << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <m2_file>" << std::endl;
        std::cout << "\nExample: " << argv[0] << " creature.m2" << std::endl;
        return 1;
    }
    
    std::string m2FilePath = argv[1];
    
    try {
        m2::M2Parser parser(m2::ParseMode::Lenient);
        
        std::cout << "Loading M2 file: " << m2FilePath << std::endl;
        
        m2::M2File model = parser.parse(m2FilePath);
        
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

