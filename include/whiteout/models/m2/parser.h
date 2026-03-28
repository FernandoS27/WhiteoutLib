
#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../compatibility.h"
#include "structures.h"

namespace whiteout {

namespace interfaces {
class CascFileSystem;
class VirtualPathFileSystem;
}

namespace common {
class BinaryReader;
}

namespace m2 {

using common::BinaryReader;

class Parser {
public:

    enum class ParseMode {
        Strict,
        Lenient
    };

    explicit Parser(ParseMode mode = ParseMode::Lenient);

    ~Parser();

    Model parse(interfaces::VirtualPathFileSystem& fs, const std::string& filePath);

    Model parse(interfaces::CascFileSystem& cascFs,
                     std::span<const uint8_t> buffer);

    bool hasIssues() const;

    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace m2
} // namespace whiteout
