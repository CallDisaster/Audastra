#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace War3Map {

class CodecDocument {
public:
    virtual ~CodecDocument() = default;
    [[nodiscard]] virtual std::string FormatName() const = 0;
};

struct CodecParseResult {
    std::unique_ptr<CodecDocument> document;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool Succeeded() const noexcept;
};

struct CodecInspection {
    std::string formatName;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<Diagnostic> diagnostics;
};

class IFormatCodec {
public:
    virtual ~IFormatCodec() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    [[nodiscard]] virtual CodecParseResult Parse(
        std::span<const std::byte> input) const = 0;
    [[nodiscard]] virtual Status Validate(const CodecDocument& document) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> Serialize(
        const CodecDocument& document) const = 0;
    [[nodiscard]] virtual SemanticDiff Diff(
        const CodecDocument& before,
        const CodecDocument& after) const = 0;
    [[nodiscard]] virtual CodecInspection Inspect(
        const CodecDocument& document) const = 0;
};

} // namespace War3Map
