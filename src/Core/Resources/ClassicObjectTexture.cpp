#include "War3MapCore/Resources/ClassicObjectTexture.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>

namespace War3Map::Resources {

Result<std::array<std::string, 2>> ClassicObjectTextureCandidates(std::string_view path) {
    using Output = Result<std::array<std::string, 2>>;
    // 在去掉扩展名之前验证，不能把不安全的原始路径变成可接受的候选。
    auto valid = CanonicalImportPath(path);
    if (!valid.Succeeded()) return {std::nullopt, std::move(valid.diagnostics)};
    std::string stem(path);
    std::replace(stem.begin(), stem.end(), '/', '\\');
    const auto separator = stem.find_last_of('\\');
    const auto extension = stem.find_last_of('.');
    if (extension != std::string::npos && (separator == std::string::npos || extension > separator))
        stem.erase(extension);
    std::array<std::string, 2> candidates{stem + ".tga", stem + ".blp"};
    for (const auto& candidate : candidates) {
        valid = CanonicalImportPath(candidate);
        if (!valid.Succeeded()) return {std::nullopt, std::move(valid.diagnostics)};
    }
    if (stem.empty() || stem.back() == '\\')
        return Output::Failure("Resources.ClassicObjectTexture", "OBJECT_TEXTURE_NAME_EMPTY",
            "物编替换贴图没有文件名，不能生成候选路径。");
    return Output::Success(std::move(candidates));
}

bool ClassicObjectTextureMissing(std::span<const Diagnostic> diagnostics) noexcept {
    return !diagnostics.empty() && std::all_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error && diagnostic.code == "RESOURCE_NOT_FOUND";
    });
}

} // namespace War3Map::Resources
