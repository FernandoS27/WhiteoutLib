#pragma once

#include "common_types.h"
#include <istream>
#include <cstring>
#include <vector>
#include <string>

namespace Common {

class BinaryReader {
public:
    BinaryReader(std::istream& inputStream) : file(inputStream) {
        file.seekg(0, std::ios::end);
        fileSize = file.tellg();
        file.seekg(0, std::ios::beg);
    }
    
    ~BinaryReader() {
        // Do not close the stream, as it is managed externally
    }
    
    // Read basic types
    u8 readUInt8() {
        u8 val;
        file.read(reinterpret_cast<char*>(&val), 1);
        return val;
    }

    i8 readInt8() {
        i8 val;
        file.read(reinterpret_cast<char*>(&val), 1);
        return val;
    }
    
    u16 readUInt16() {
        u16 val;
        file.read(reinterpret_cast<char*>(&val), 2);
        return val;
    }

    i16 readInt16() {
        i16 val;
        file.read(reinterpret_cast<char*>(&val), 2);
        return val;
    }
    
    u32 readUInt32() {
        u32 val;
        file.read(reinterpret_cast<char*>(&val), 4);
        return val;
    }
    
    i32 readInt32() {
        i32 val;
        file.read(reinterpret_cast<char*>(&val), 4);
        return val;
    }
    
    f32 readFloat32() {
        f32 val;
        file.read(reinterpret_cast<char*>(&val), 4);
        return val;
    }
    
    // Read array of basic types
    std::vector<u8> readUInt8Array(u32 count) {
        std::vector<u8> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count);
        return arr;
    }
    
    std::vector<u32> readUInt32Array(u32 count) {
        std::vector<u32> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * 4);
        return arr;
    }
    
    std::vector<f32> readFloat32Array(u32 count) {
        std::vector<f32> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * 4);
        return arr;
    }
    
    std::vector<u16> readUInt16Array(u32 count) {
        std::vector<u16> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * 2);
        return arr;
    }

    std::vector<Vector3f> readVector3fArray(u32 count) {
        std::vector<Vector3f> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * sizeof(Vector3f));
        return arr;
    }

    std::vector<Vector4f> readVector4fArray(u32 count) {
        std::vector<Vector4f> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * sizeof(Vector4f));
        return arr;
    }

    std::vector<Vector2f> readVector2fArray(u32 count) {
        std::vector<Vector2f> arr(count);
        file.read(reinterpret_cast<char*>(arr.data()), count * sizeof(Vector2f));
        return arr;
    }

    void readBytes(char* buffer, u32 count) {
        file.read(buffer, count);
    }
    
    // Read vector types
    Vector3f readVector3f() {
        return Vector3f(readFloat32(), readFloat32(), readFloat32());
    }
    
    Vector4f readVector4f() {
        return Vector4f(readFloat32(), readFloat32(), readFloat32(), readFloat32());
    }
    
    Vector2f readVector2f() {
        return Vector2f(readFloat32(), readFloat32());
    }
    
    // Read extent
    Extent readExtent() {
        Extent ext;
        ext.boundsRadius = readFloat32();
        ext.minimum = readVector3f();
        ext.maximum = readVector3f();
        return ext;
    }
    
    // Read string with fixed size
    std::string readString(u32 size) {
        std::vector<char> buffer(size);
        file.read(buffer.data(), size);
        
        // Find null terminator
        size_t len = 0;
        while (len < size && buffer[len] != '\0') {
            len++;
        }
        return std::string(buffer.begin(), buffer.begin() + len);
    }
    
    // Read variable length string (null-terminated)
    std::string readZString() {
        std::string result;
        char c;
        while (file.get(c) && c != '\0') {
            result += c;
        }
        return result;
    }
    
    // Get current position
    u32 getPosition() const {
        return file.tellg();
    }
    
    // Set position
    void setPosition(u32 pos) {
        file.seekg(pos, std::ios::beg);
    }
    
    // Skip bytes
    void skip(u32 count) {
        file.seekg(count, std::ios::cur);
    }
    
    // Check if we have remaining data
    bool hasRemaining() {
        return file.tellg() < fileSize;
    }
    
    // Get remaining bytes
    u32 getRemainingBytes() {
        u32 current = file.tellg();
        return fileSize - current;
    }
    
    // Check if stream is valid
    bool isValid() const {
        return file.good();
    }
    
private:
    std::istream& file;
    u32 fileSize = 0;
};

} // namespace Common
