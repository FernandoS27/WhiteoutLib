#pragma once

#include "structures.h"
#include <string>
#include <vector>
#include <stdexcept>


namespace common {
    class BinaryReader;
}

namespace m2 {

enum class ParseMode {
    Strict,
    Lenient,
};

class M2Parser {
public:
    explicit M2Parser(ParseMode mode = ParseMode::Lenient);
    
    M2File parse(const std::string& filePath);
    
    M2File parse(std::span<const uint8_t> buffer);
    
    const std::vector<std::string>& getIssues() const { return issues; }
    
    void clearIssues() { issues.clear(); }
    
private:
    friend class common::BinaryReader;
    
    ParseMode parseMode;
    std::vector<std::string> issues;

    M2File parse(common::BinaryReader& reader);
    
    void parseChunked(common::BinaryReader& reader, M2File& m2file);

    void reportIssue(const std::string& message);
    void skipUnknownChunk(common::BinaryReader& reader, uint32_t tag, uint32_t size);
};

} // namespace m2
