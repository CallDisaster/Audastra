#include "War3MapCore/Types.h"
#include "War3MapCore/Codec.h"

#include <algorithm>

namespace War3Map {

bool Status::Succeeded() const noexcept {
    return std::none_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

Status Status::Ok() {
    return {};
}

Status Status::Failure(
    std::string subsystem,
    std::string code,
    std::string message) {
    Status status;
    status.diagnostics.push_back(Diagnostic{
        DiagnosticSeverity::Error,
        std::move(subsystem),
        std::move(code),
        std::move(message),
        std::nullopt,
    });
    return status;
}

bool SemanticDiff::Empty() const noexcept {
    return changes.empty();
}

bool CodecParseResult::Succeeded() const noexcept {
    if (!document) {
        return false;
    }
    return std::none_of(
        diagnostics.begin(),
        diagnostics.end(),
        [](const Diagnostic& diagnostic) {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

} // namespace War3Map
