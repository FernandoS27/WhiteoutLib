// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "text_dump.h"

namespace whiteout {
namespace models {
namespace wem {

std::string TextDumpVisitor::quote(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        // Only the two characters that would break the line-oriented shape.
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

void TextDumpVisitor::line(const char* name, const std::string& value) {
    out_ << std::string(static_cast<std::size_t>(depth_) * 2, ' ');
    if (name != nullptr && *name != '\0') {
        out_ << name << ": ";
    }
    out_ << value << "\n";
}

void TextDumpVisitor::open(const char* name, const std::string& suffix) {
    line(name, suffix.empty() ? "{" : suffix + " {");
    ++depth_;
}

void TextDumpVisitor::close() {
    --depth_;
    line("", "}");
}

} // namespace wem
} // namespace models
} // namespace whiteout
