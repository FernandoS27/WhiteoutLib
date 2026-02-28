#pragma once

#include "common_types.h"
#include <ostream>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <string>

namespace Common {

class BinaryWriter {
public:
    BinaryWriter(std::ostream& outputStream) : file(outputStream) {
        // No need to open the stream, as it is managed externally
    }
    
    ~BinaryWriter() {
        // Do not close the stream, as it is managed externally
    }
    
    // Write basic types
    void writeUInt8(u8 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(u8));
    }

    void writeInt8(i8 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(i8));
    }
    
    void writeUInt16(u16 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(u16));
    }

    void writeInt16(i16 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(i16));
    }
    
    void writeUInt32(u32 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(u32));
    }
    
    void writeInt32(i32 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(i32));
    }
    
    void writeFloat32(f32 val) {
        file.write(reinterpret_cast<const char*>(&val), sizeof(f32));
    }
    
    // Write array of basic types
    void writeUInt8Array(const std::vector<u8>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(u8));
    }
    
    void writeUInt32Array(const std::vector<u32>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(u32));
    }
    
    void writeFloat32Array(const std::vector<f32>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(f32));
    }
    
    void writeUInt16Array(const std::vector<u16>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(u16));
    }
    
    void writeVector3fArray(const std::vector<Vector3f>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(Vector3f));
    }
    
    void writeVector4fArray(const std::vector<Vector4f>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(Vector4f));
    }
    
    void writeVector2fArray(const std::vector<Vector2f>& arr) {
        file.write(reinterpret_cast<const char*>(arr.data()), arr.size() * sizeof(Vector2f));
    }
    
    // Write vector types
    void writeVector3f(const Vector3f& v) {
        file.write(reinterpret_cast<const char*>(v.data.data()), sizeof(Vector3f));
    }
    
    void writeVector4f(const Vector4f& v) {
        file.write(reinterpret_cast<const char*>(v.data.data()), sizeof(Vector4f));
    }

    void writeQuaternion(const Quaternion& v) {
        file.write(reinterpret_cast<const char*>(v.data.data()), sizeof(Quaternion));
    }
    
    void writeVector2f(const Vector2f& v) {
        file.write(reinterpret_cast<const char*>(v.data.data()), sizeof(Vector2f));
    }
    
    // Write extent
    void writeExtent(const Extent& ext) {
        writeFloat32(ext.boundsRadius);
        writeVector3f(ext.minimum);
        writeVector3f(ext.maximum);
    }
    
    // Write string with fixed size (padded with zeros)
    void writeString(const std::string& str, u32 size) {
        std::vector<char> buffer(size, '\0');
        size_t copySize = std::min(static_cast<size_t>(size), str.size());
        std::memcpy(buffer.data(), str.c_str(), copySize);
        file.write(buffer.data(), size);
    }

    void writeString(const std::string& str) {
        file.write(str.c_str(), str.size());
    }
    
    // Write variable length string (null-terminated)
    void writeZString(const std::string& str) {
        file.write(str.c_str(), str.size());
        writeUInt8(0); // null terminator
    }
    
    // Get current position
    u32 getPosition() {
        return file.tellp();
    }
    
    // Set position
    void setPosition(u32 pos) {
        file.seekp(pos, std::ios::beg);
    }
    
    // Write padding
    void writePadding(u32 count) {
        for (u32 i = 0; i < count; i++) {
            writeUInt8(0);
        }
    }

    void AlignTo(u32 alignment) {
        u32 currentPos = getPosition();
        u32 padding = (alignment - (currentPos % alignment)) % alignment;
        writePadding(padding);
    }
    
    // Check if stream is valid
    bool isValid() const {
        return file.good();
    }
    
private:
    std::ostream& file;
};

} // namespace Common
