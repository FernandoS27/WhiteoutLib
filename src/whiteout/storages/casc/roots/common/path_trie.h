// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow
/// @file path_trie.h
/// @brief Flat-array path trie for prefix-based enumeration of RootEntries.
///
/// Used by TVFS (and potentially other roots) for O(prefix-depth + matches)
/// enumeration under a path prefix, rather than a full linear scan.
///
/// Internal header — not part of the public include path.
#pragma once

#include "../root.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::storages::casc {

/// Flat-array path trie node for prefix-based enumeration.
/// Children are sorted by segment for binary-search traversal and ordered DFS.
struct PathTrieNode {
    struct Child {
        std::string segment;
        u32 nodeIndex; ///< Index into the flat trie node array.
    };
    std::vector<Child> children;
    std::vector<u32> entryIndices; ///< Entries at exactly this path (leaf).
};

/// Find a child by segment using binary search (children must be sorted).
inline const PathTrieNode::Child* trieFindChild(const PathTrieNode& node,
                                                std::string_view segment) {
    auto it = std::lower_bound(
        node.children.begin(), node.children.end(), segment,
        [](const PathTrieNode::Child& c, std::string_view s) { return c.segment < s; });
    if (it != node.children.end() && it->segment == segment)
        return &*it;
    return nullptr;
}

/// Walk the trie to the node matching a normalized prefix path.
/// Returns the node index, or UINT32_MAX if not found.
inline u32 trieWalkTo(const std::vector<PathTrieNode>& nodes, const std::string& normalizedPrefix) {
    if (nodes.empty())
        return UINT32_MAX;
    if (normalizedPrefix.empty())
        return 0; // root

    u32 cur = 0;
    size_t pos = 0;
    while (pos < normalizedPrefix.size()) {
        size_t sep = normalizedPrefix.find('\\', pos);
        std::string_view segment;
        if (sep == std::string::npos) {
            segment = std::string_view(normalizedPrefix).substr(pos);
            pos = normalizedPrefix.size();
        } else {
            segment = std::string_view(normalizedPrefix).substr(pos, sep - pos);
            pos = sep + 1;
        }
        if (segment.empty())
            continue;

        auto* child = trieFindChild(nodes[cur], segment);
        if (!child)
            return UINT32_MAX;
        cur = child->nodeIndex;
    }
    return cur;
}

/// DFS enumeration from a trie node, invoking callback for every entry.
/// Returns false if callback requested early stop.
inline bool trieDfs(const std::vector<PathTrieNode>& nodes, u32 nodeIdx,
                    const std::vector<RootEntry>& entries,
                    std::function<bool(const RootEntry&)>& callback) {
    const auto& node = nodes[nodeIdx];
    for (u32 idx : node.entryIndices) {
        if (!callback(entries[idx]))
            return false;
    }
    for (const auto& child : node.children) {
        if (!trieDfs(nodes, child.nodeIndex, entries, callback))
            return false;
    }
    return true;
}

/// Insert a path into the trie, creating nodes as needed.
/// Children are unsorted during building; call trieSortAll() when done.
inline void trieInsert(std::vector<PathTrieNode>& nodes, const std::string& path, u32 entryIndex) {
    u32 cur = 0; // root
    size_t pos = 0;
    while (pos < path.size()) {
        size_t sep = path.find('\\', pos);
        std::string_view segment;
        if (sep == std::string::npos) {
            segment = std::string_view(path).substr(pos);
            pos = path.size();
        } else {
            segment = std::string_view(path).substr(pos, sep - pos);
            pos = sep + 1;
        }
        if (segment.empty())
            continue;

        // Linear scan during building (children unsorted).
        auto& children = nodes[cur].children;
        u32 found = UINT32_MAX;
        for (size_t i = 0; i < children.size(); ++i) {
            if (children[i].segment == segment) {
                found = children[i].nodeIndex;
                break;
            }
        }
        if (found == UINT32_MAX) {
            u32 newIdx = static_cast<u32>(nodes.size());
            // Save segment before push_back may invalidate references.
            std::string segStr(segment);
            nodes.emplace_back();
            // Re-acquire children ref after potential reallocation.
            nodes[cur].children.push_back({std::move(segStr), newIdx});
            cur = newIdx;
        } else {
            cur = found;
        }
    }
    nodes[cur].entryIndices.push_back(entryIndex);
}

/// Recursively sort all children arrays for binary search.
inline void trieSortAll(std::vector<PathTrieNode>& nodes, u32 nodeIdx = 0) {
    auto& children = nodes[nodeIdx].children;
    std::sort(children.begin(), children.end(),
              [](const PathTrieNode::Child& a, const PathTrieNode::Child& b) {
                  return a.segment < b.segment;
              });
    for (auto& c : children)
        trieSortAll(nodes, c.nodeIndex);
}

} // namespace whiteout::storages::casc
