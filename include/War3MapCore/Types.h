#pragma once

#include "War3RenderCore/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace War3Map {

using StableId = std::uint64_t;
using TransactionId = std::uint64_t;

struct Status {
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool Succeeded() const noexcept;
    [[nodiscard]] static Status Ok();
    [[nodiscard]] static Status Failure(
        std::string subsystem,
        std::string code,
        std::string message);
};

struct ProjectRevision {
    std::uint64_t generation = 0;
    std::string sourceSha256;
    std::string uiCatalogSha256;

    [[nodiscard]] bool operator==(const ProjectRevision&) const noexcept = default;
};

enum class CompatibilityLevel : std::uint8_t {
    Parsed = 0,
    ByteRoundTrip = 1,
    StructuralOracle = 2,
    EditorOpen = 3,
    EditorTrueSave = 4,
    RuntimeGate = 5,
};

struct SemanticChange {
    std::string kind;
    std::filesystem::path path;
    std::string summary;
    std::string beforeSha256;
    std::string afterSha256;
};

struct SemanticDiff {
    TransactionId transactionId = 0;
    ProjectRevision baseRevision;
    std::vector<SemanticChange> changes;

    [[nodiscard]] bool Empty() const noexcept;
};

struct ValidationReport {
    CompatibilityLevel requestedLevel = CompatibilityLevel::Parsed;
    CompatibilityLevel highestProvenLevel = CompatibilityLevel::Parsed;
    bool satisfied = false;
    std::vector<Diagnostic> diagnostics;
};

} // namespace War3Map
