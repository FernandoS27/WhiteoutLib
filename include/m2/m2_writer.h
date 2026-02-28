#pragma once

#include "structures.h"
#include <string>
#include <cstdint>

namespace common {
    class BinaryWriter;
}

namespace m2 {

using common::BinaryWriter;

class M2Writer {
public:
    explicit M2Writer();
    
    void write(const std::string& filePath, const M2File& model);
    
    std::vector<uint8_t> writeToBuffer(const M2File& model);
    
private:

    void write(const M2File& model);

    void writeChunked(BinaryWriter& writer, const M2File& model);
};

} // namespace m2
