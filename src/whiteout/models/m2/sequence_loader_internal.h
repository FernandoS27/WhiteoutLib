
#pragma once

// The lazy loader's state, kept out of the public header: it is built from a
// WoWFileSystem, which is a parser internal. A Model holds one of these only
// when it was parsed with Parser::setLazyAnimations — see sequence_loader.h for
// what the free functions do with it.

#include <whiteout/common_types.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace whiteout {
namespace m2 {

class WoWFileSystem;

class SequenceLoader {
public:
    enum class State : u8 {
        /// Keys are in a `.anim` that has not been read.
        Pending,
        /// Keys are in the model — either the `.m2` carried them or a load
        /// already brought them in.
        Resident,
        /// The `.anim` could not be read. Not retried.
        Missing,
    };

    /// @p fs is the file system the parse ran on, kept alive because the
    /// `.anim` siblings are read through it long after the parse returned.
    SequenceLoader(std::shared_ptr<WoWFileSystem> fs, std::size_t sequenceCount);
    ~SequenceLoader();

    SequenceLoader(const SequenceLoader&) = delete;
    SequenceLoader& operator=(const SequenceLoader&) = delete;

    WoWFileSystem& fs() {
        return *fs_;
    }
    const WoWFileSystem& fs() const {
        return *fs_;
    }

    std::vector<State>& state() {
        return state_;
    }
    const std::vector<State>& state() const {
        return state_;
    }

private:
    std::shared_ptr<WoWFileSystem> fs_;
    std::vector<State> state_;
};

struct Model;

/// @brief Free the per-sequence KeySpanRefs. For a model that will never defer
///        a load — an eager parse, once it has loaded everything.
void dropSequenceKeyRefs(Model& model);

} // namespace m2
} // namespace whiteout
