#include <mdx/mdx.h>
#include <iostream>
#include <iomanip>

void printVector3(const whiteout::Vector3f& v, const std::string& name = "") {
    if (!name.empty()) std::cout << name << ": ";
    std::cout << "(" << v.x << ", " << v.y << ", " << v.z << ")" << std::endl;
}

void printVector4(const whiteout::Vector4f& v, const std::string& name = "") {
    if (!name.empty()) std::cout << name << ": ";
    std::cout << "(" << v.x << ", " << v.y << ", " << v.z << ", " << v.w << ")" << std::endl;
}

void printModelInfo(const whiteout::mdx::MDXFile& mdx) {
    std::cout << "=== MDX File Information ===" << std::endl;
    std::cout << "Version: " << mdx.version << std::endl;
    std::cout << "Model Name: " << mdx.modelName << std::endl;
    std::cout << "Animation File: " << mdx.animationFileName << std::endl;
    std::cout << "Blend Time: " << mdx.blendTime << std::endl;
    
    std::cout << "\n=== Extents ===" << std::endl;
    std::cout << "Bounds Radius: " << mdx.modelExtent.boundsRadius << std::endl;
    printVector3(mdx.modelExtent.minimum, "Minimum");
    printVector3(mdx.modelExtent.maximum, "Maximum");
    
    std::cout << "\n=== Object Counts ===" << std::endl;
    std::cout << "Sequences: " << mdx.sequences.size() << std::endl;
    std::cout << "Global Sequences: " << mdx.globalSequences.size() << std::endl;
    std::cout << "Textures: " << mdx.textures.size() << std::endl;
    std::cout << "Materials: " << mdx.materials.size() << std::endl;
    std::cout << "Geosets: " << mdx.geosets.size() << std::endl;
    std::cout << "Geoset Animations: " << mdx.geosetAnimations.size() << std::endl;
    std::cout << "Bones: " << mdx.bones.size() << std::endl;
    std::cout << "Lights: " << mdx.lights.size() << std::endl;
    std::cout << "Helpers: " << mdx.helpers.size() << std::endl;
    std::cout << "Attachments: " << mdx.attachments.size() << std::endl;
    std::cout << "Particle Emitters: " << mdx.particleEmitters.size() << std::endl;
    std::cout << "Particle Emitters 2: " << mdx.particleEmitters2.size() << std::endl;
    std::cout << "Ribbon Emitters: " << mdx.ribbonEmitters.size() << std::endl;
    std::cout << "Event Objects: " << mdx.eventObjects.size() << std::endl;
    std::cout << "Cameras: " << mdx.cameras.size() << std::endl;
    std::cout << "Collision Shapes: " << mdx.collisionShapes.size() << std::endl;
    std::cout << "Pivot Points: " << mdx.pivotPoints.size() << std::endl;
    std::cout << "Bind Poses: " << mdx.bindPoses.size() << std::endl;
}

void printSequences(const whiteout::mdx::MDXFile& mdx) {
    if (mdx.sequences.empty()) {
        std::cout << "No sequences found." << std::endl;
        return;
    }
    
    std::cout << "\n=== Sequences ===" << std::endl;
    for (size_t i = 0; i < mdx.sequences.size(); i++) {
        const auto& seq = mdx.sequences[i];
        std::cout << "\nSequence " << i << ": " << seq.name << std::endl;
        std::cout << "  Interval: " << seq.intervalStart << " - " << seq.intervalEnd << std::endl;
        std::cout << "  Move Speed: " << seq.moveSpeed << std::endl;
        std::cout << "  Flags: " << seq.flags << std::endl;
        std::cout << "  Rarity: " << seq.rarity << std::endl;
    }
}

void printTextures(const whiteout::mdx::MDXFile& mdx) {
    if (mdx.textures.empty()) {
        std::cout << "No textures found." << std::endl;
        return;
    }
    
    std::cout << "\n=== Textures ===" << std::endl;
    for (size_t i = 0; i < mdx.textures.size(); i++) {
        const auto& tex = mdx.textures[i];
        std::cout << "\nTexture " << i << std::endl;
        std::cout << "  Replaceable ID: " << tex.replaceableId << std::endl;
        std::cout << "  File: " << tex.fileName << std::endl;
        std::cout << "  Flags: " << tex.flags << std::endl;
    }
}

void printBones(const whiteout::mdx::MDXFile& mdx) {
    if (mdx.bones.empty()) {
        std::cout << "No bones found." << std::endl;
        return;
    }
    
    std::cout << "\n=== Bones ===" << std::endl;
    for (size_t i = 0; i < mdx.bones.size(); i++) {
        const auto& bone = mdx.bones[i];
        std::cout << "\nBone " << i << std::endl;
        std::cout << "  Name: " << bone.node.name << std::endl;
        std::cout << "  Object ID: " << bone.node.objectId << std::endl;
        std::cout << "  Parent ID: " << bone.node.parentId << std::endl;
        std::cout << "  Geoset ID: " << bone.geosetId << std::endl;
        std::cout << "  Geoset Animation ID: " << bone.geosetAnimationId << std::endl;
        std::cout << "  Translation Tracks: " << bone.node.translationTracks.keyCount << std::endl;
        std::cout << "  Rotation Tracks: " << bone.node.rotationTracks.keyCount << std::endl;
        std::cout << "  Scaling Tracks: " << bone.node.scalingTracks.keyCount << std::endl;
    }
}

void printGeosets(const whiteout::mdx::MDXFile& mdx) {
    if (mdx.geosets.empty()) {
        std::cout << "No geosets found." << std::endl;
        return;
    }
    
    std::cout << "\n=== Geosets ===" << std::endl;
    for (size_t i = 0; i < mdx.geosets.size(); i++) {
        const auto& geo = mdx.geosets[i];
        std::cout << "\nGeoset " << i << std::endl;
        std::cout << "  Vertices: " << geo.vertexPositions.size() << std::endl;
        std::cout << "  Normals: " << geo.vertexNormals.size() << std::endl;
        std::cout << "  Faces: " << geo.faces.size() << std::endl;
        std::cout << "  Face Type Groups: " << geo.faceTypeGroups.size() << std::endl;
        std::cout << "  Face Groups: " << geo.faceGroups.size() << std::endl;
        std::cout << "  Vertex Groups: " << geo.vertexGroups.size() << std::endl;
        std::cout << "  Matrix Groups: " << geo.matrixGroups.size() << std::endl;
        std::cout << "  Matrix Indices: " << geo.matrixIndices.size() << std::endl;
        std::cout << "  Material ID: " << geo.materialId << std::endl;
        std::cout << "  Selection Group: " << geo.selectionGroup << std::endl;
        std::cout << "  Texture Coordinate Sets: " << geo.textureCoordinateSets.size() << std::endl;
        if (!geo.tangents.empty()) {
            std::cout << "  Tangents: " << geo.tangents.size() << std::endl;
        }
        if (!geo.skinData.empty()) {
            std::cout << "  Skin Data: " << geo.skinData.size() << " bytes" << std::endl;
        }
        if (geo.lod > 0) {
            std::cout << "  LOD: " << geo.lod;
            if (!geo.lodName.empty()) {
                std::cout << " (" << geo.lodName << ")";
            }
            std::cout << std::endl;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: mdx_loader_example <mdx_file>" << std::endl;
        std::cout << "\nExample:" << std::endl;
        std::cout << "  mdx_loader_example model.mdx" << std::endl;
        return 1;
    }
    
    std::string filePath = argv[1];
    

        std::cout << "Loading MDX file: " << filePath << std::endl;
        
        whiteout::mdx::MDXParser parser;
        whiteout::mdx::MDXFile mdx = parser.parse(filePath);
        
        std::cout << "\n=== File Successfully Loaded ===" << std::endl;
        
        // Print information
        printModelInfo(mdx);
        printSequences(mdx);
        printTextures(mdx);
        printGeosets(mdx);
        printBones(mdx);
        
        std::cout << "\n=== Parsing Complete ===" << std::endl;
        return 0;
        

}
