// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include "whiteout/models/cross/m2_wc3_attachments.h"

#include <iterator>

namespace whiteout {
namespace models {
namespace cross {

namespace {

// The ids' names as wowdev's M2 page gives them (the client keeps none), a
// space before each word.
// clang-format off
constexpr const char* kWowNames[] = {
    "Shield",              "Hand Right",          "Hand Left",            "Elbow Right",
    "Elbow Left",          "Shoulder Right",      "Shoulder Left",        "Knee Right",
    "Knee Left",           "Hip Right",           "Hip Left",             "Helm",
    "Back",                "Shoulder Flap Right", "Shoulder Flap Left",   "Chest Blood Front",
    "Chest Blood Back",    "Breath",              "Player Name",          "Base",
    "Head",                "Spell Left Hand",     "Spell Right Hand",     "Special 1",
    "Special 2",           "Special 3",           "Sheath Main Hand",     "Sheath Off Hand",
    "Sheath Shield",       "Player Name Mounted", "Large Weapon Left",    "Large Weapon Right",
    "Hip Weapon Left",     "Hip Weapon Right",    "Chest",                "Hand Arrow",
    "Bullet",              "Spell Hand Omni",     "Spell Hand Directed",  "Vehicle Seat 1",
    "Vehicle Seat 2",      "Vehicle Seat 3",      "Vehicle Seat 4",       "Vehicle Seat 5",
    "Vehicle Seat 6",      "Vehicle Seat 7",      "Vehicle Seat 8",       "Left Foot",
    "Right Foot",          "Shield No Glove",     "Spine Low",            "Altered Shoulder R",
    "Altered Shoulder L",  "Belt Buckle",         "Sheath Crossbow",      "Head Top",
};
// clang-format on
static_assert(std::size(kWowNames) == 56);

struct Equivalent {
    u32 id;
    const char* wc3;
};

// The points both games have. `PlayerName` is where the name plate sits, which
// is Warcraft III's overhead.
constexpr Equivalent kEquivalents[] = {
    {1, "Hand Right Ref"}, {2, "Hand Left Ref"}, {18, "Overhead Ref"},  {19, "Origin Ref"},
    {20, "Head Ref"},      {34, "Chest Ref"},    {47, "Foot Left Ref"}, {48, "Foot Right Ref"},
};

} // namespace

std::string M2AttachmentWords(u32 id) {
    if (id < std::size(kWowNames)) {
        return kWowNames[id];
    }
    return "Attachment " + std::to_string(id);
}

std::optional<std::string_view> M2Wc3AttachmentEquivalent(u32 id) {
    for (const Equivalent& row : kEquivalents) {
        if (row.id == id) {
            return row.wc3;
        }
    }
    return std::nullopt;
}

std::string M2Wc3AttachmentName(u32 id) {
    if (const std::optional<std::string_view> wc3 = M2Wc3AttachmentEquivalent(id)) {
        return std::string(*wc3);
    }
    std::string word = M2AttachmentWords(id);
    std::erase(word, ' ');
    return word + " Ref";
}

M2AttachmentReport CrossM2Attachments(wem::Document& document) {
    M2AttachmentReport report;
    for (wem::Model& model : document.models) {
        for (wem::Node& node : model.nodes.nodes) {
            const wem::NativeBag::Entry* id = node.native.find("m2AttachmentId");
            if (node.kind != wem::NodeKind::Attachment || id == nullptr || id->value < 0 ||
                id->value > 0xFFFFFFFF) {
                continue;
            }
            const u32 point = static_cast<u32>(id->value);
            node.name = M2Wc3AttachmentName(point);
            if (M2Wc3AttachmentEquivalent(point)) {
                ++report.matched;
            } else {
                ++report.kept;
            }
        }
    }
    if (report.matched != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.matched) +
                                    " attachment point(s) renamed to Warcraft III's "
                                    "('HandRight' -> 'Hand Right Ref')",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    if (report.kept != 0) {
        report.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                std::to_string(report.kept) +
                                    " attachment point(s) Warcraft III has no name for keep World "
                                    "of Warcraft's ('ShoulderRight Ref'); the game finds none of "
                                    "them by name",
                                wem::ElementRef(wem::ElementKind::Document, 0));
    }
    return report;
}

} // namespace cross
} // namespace models
} // namespace whiteout
