
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
} // namespace interfaces

namespace common {
class BinaryReader;
}

namespace m2 {

using common::BinaryReader;

/// @bind methods, js_name=M2Parser
class Parser {
public:
    Parser();

    ~Parser();

    Model parse(interfaces::VirtualPathFileSystem& fs, const std::string& filePath);

    Model parse(interfaces::CascFileSystem& cascFs, std::span<const uint8_t> buffer);

    /// @brief Leave the keys of sequences stored in `.anim` siblings unread
    ///        until m2::loadSequence() asks for them. Off by default.
    ///
    /// This is what the client does — nothing reads a `.anim` until something
    /// plays that sequence — and on a character model with a hundred of them it
    /// is the difference between a load that reads one file and one that reads
    /// a hundred. See sequence_loader.h.
    ///
    /// The @p fs passed to parse() has to outlive the returned Model, because
    /// the deferred reads go back through it.
    void setLazyAnimations(bool enable);

    bool hasIssues() const;

    const std::vector<std::string>& getIssues() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace m2
} // namespace whiteout
