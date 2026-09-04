// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

#include <whiteout/models/wem/diagnostics.h>

#include <algorithm>
#include <array>

namespace whiteout {
namespace models {
namespace wem {

namespace {

/// One row per `DiagCode`, in enum order. The table is indexed, not searched, and
/// `wem_diagnostics_test` asserts it covers `DiagCode::Count` exactly — a code
/// added without a name is a compile-time-invisible, test-visible mistake.
constexpr const char* kDiagCodeNames[] = {
    "Unspecified",

    "NonManifoldEdgeSplit",
    "NonManifoldVertexSplit",
    "DegenerateFaceDropped",
    "DuplicateFaceDropped",
    "InconsistentWindingFlipped",
    "NgonTriangulated",
    "IsolatedVertexDropped",

    "ConnectivityCorrupt",
    "IndexOutOfRange",
    "AttributeCountMismatch",
    "SkinBindingMalformed",

    "LossyBlendMode",
    "DroppedNativeBlock",
    "SlotNotBound",
    "MixedShadedUnshadedStack",
    "SceneReadingMultiLayerStack",
    "CollapseRefused",
    "UnknownShaderCombo",
    "UnsupportedMaterialKind",
    "LossyKindConversion",
    "FeatureDropped",
    "LayerDropped",
    "TextureUnresolved",
    "MaterialSlotUnused",
    "MaterialBodyInvalid",
    "LookBindingMalformed",
    "LookDropped",

    "ProfileNotCarried",
    "ProfileCoverageIncomplete",
    "BoneInfluenceLimit",
    "UvSetLimit",
    "IndexWidthExceeded",
    "NgonUnsupported",
    "VertexColorUnsupported",
    "NativeKindProfileMismatch",
    "BonePaletteLimit",
    "GeometryMismatch",
    "SectionUndrawn",

    "NodeRemovalRefused",
    "SkinInfluenceReassigned",
    "DanglingNodeReference",
    "BindPoseRecomposed",
    "RigConventionChanged",
    "BoneShearSplit",
    "BoneShearProjected",
    "NonUniformScaleFlattened",

    "MixedInterpolationInTrack",
    "AnimChannelInvalidated",
    "ClipTargetMissing",
    "AnimTrackDropped",
    "AnimTrackApproximated",
    "AnimClipRetimed",

    "EventPayloadMismatch",
    "AssetUnresolved",
    "HardpointUnresolved",

    "UnknownChunkPreserved",
    "OrphanChunk",
    "UnsupportedVersion",
    "LegacyDocumentUpgraded",
    "OperationUnsupported",
};

static_assert(sizeof(kDiagCodeNames) / sizeof(kDiagCodeNames[0]) ==
                  static_cast<std::size_t>(DiagCode::Count),
              "kDiagCodeNames must have one entry per DiagCode");

constexpr const char* kElementKindNames[] = {
    "",        "document", "mesh",     "face",  "vertex",  "halfedge", "edge",
    "section", "node",     "material", "layer", "slot",    "texture",  "look",
    "feature", "actor",    "event",    "clip",  "channel", "track",    "chunk",
};

static_assert(sizeof(kElementKindNames) / sizeof(kElementKindNames[0]) ==
                  static_cast<std::size_t>(ElementKind::Chunk) + 1,
              "kElementKindNames must have one entry per ElementKind");

void appendUint(std::string& out, u32 value) {
    char buffer[12];
    int length = 0;
    if (value == 0) {
        buffer[length++] = '0';
    }
    while (value != 0) {
        buffer[length++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }
    while (length > 0) {
        out.push_back(buffer[--length]);
    }
}

} // namespace

// ============================================================================

const char* ToString(Severity severity) {
    switch (severity) {
    case Severity::Info:
        return "info";
    case Severity::Warning:
        return "warning";
    case Severity::Error:
        return "error";
    }
    return "invalid";
}

const char* ToString(DiagCode code) {
    const auto index = static_cast<std::size_t>(code);
    if (index >= static_cast<std::size_t>(DiagCode::Count)) {
        return "Invalid";
    }
    return kDiagCodeNames[index];
}

const char* ToString(ElementKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    if (index > static_cast<std::size_t>(ElementKind::Chunk)) {
        return "";
    }
    return kElementKindNames[index];
}

std::string Describe(const ElementRef& ref) {
    if (!ref.valid()) {
        return std::string();
    }
    std::string out = ToString(ref.kind);
    if (ref.index != ElementRef::kInvalidIndex) {
        out.push_back('[');
        appendUint(out, ref.index);
        out.push_back(']');
    }
    if (ref.sub != ElementRef::kInvalidIndex) {
        out.append(".sub[");
        appendUint(out, ref.sub);
        out.push_back(']');
    }
    return out;
}

// ============================================================================

void Diagnostics::add(Severity severity, DiagCode code, std::string message, ElementRef where,
                      ProfileId profile) {
    Diagnostic entry;
    entry.severity = severity;
    entry.code = code;
    entry.message = std::move(message);
    entry.where = where;
    entry.profile = profile;
    if (severity == Severity::Error) {
        ++errorCount_;
    }
    entries_.push_back(std::move(entry));
}

void Diagnostics::info(DiagCode code, std::string message, ElementRef where, ProfileId profile) {
    add(Severity::Info, code, std::move(message), where, profile);
}

void Diagnostics::warn(DiagCode code, std::string message, ElementRef where, ProfileId profile) {
    add(Severity::Warning, code, std::move(message), where, profile);
}

void Diagnostics::error(DiagCode code, std::string message, ElementRef where, ProfileId profile) {
    add(Severity::Error, code, std::move(message), where, profile);
}

void Diagnostics::append(const Diagnostics& other) {
    entries_.insert(entries_.end(), other.entries_.begin(), other.entries_.end());
    errorCount_ += other.errorCount_;
}

bool Diagnostics::hasErrors() const {
    return errorCount_ != 0;
}

std::vector<Diagnostic> Diagnostics::byCode(DiagCode code) const {
    std::vector<Diagnostic> out;
    for (const Diagnostic& entry : entries_) {
        if (entry.code == code) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<Diagnostic> Diagnostics::byProfile(ProfileId profile) const {
    std::vector<Diagnostic> out;
    for (const Diagnostic& entry : entries_) {
        if (entry.profile == profile) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<Diagnostic> Diagnostics::bySeverity(Severity severity) const {
    std::vector<Diagnostic> out;
    for (const Diagnostic& entry : entries_) {
        if (entry.severity == severity) {
            out.push_back(entry);
        }
    }
    return out;
}

u32 Diagnostics::countOf(DiagCode code) const {
    u32 count = 0;
    for (const Diagnostic& entry : entries_) {
        if (entry.code == code) {
            ++count;
        }
    }
    return count;
}

std::vector<Diagnostics::CodeCount> Diagnostics::histogram() const {
    // A fixed-size tally keyed by the enum, so the result is in ascending code
    // order without a sort and without an unordered container — the golden file
    // is a literal diff and must not depend on insertion order or hashing.
    std::array<u32, static_cast<std::size_t>(DiagCode::Count)> tally{};
    for (const Diagnostic& entry : entries_) {
        const auto index = static_cast<std::size_t>(entry.code);
        if (index < tally.size()) {
            ++tally[index];
        }
    }
    std::vector<CodeCount> out;
    for (std::size_t i = 0; i < tally.size(); ++i) {
        if (tally[i] != 0) {
            out.push_back(CodeCount{static_cast<DiagCode>(i), tally[i]});
        }
    }
    return out;
}

std::string Diagnostics::formatHistogram() const {
    std::string out;
    for (const CodeCount& row : histogram()) {
        out.append(ToString(row.code));
        out.push_back(' ');
        appendUint(out, row.count);
        out.push_back('\n');
    }
    return out;
}

} // namespace wem
} // namespace models
} // namespace whiteout
