#include "War3MapCore/Render/ModelGameLimits.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace War3Map::Render {
namespace {
constexpr std::size_t kListedGeosets = 8;
} // namespace

bool GeosetOverGameLimit(std::size_t vertices, std::size_t indices) noexcept {
    return vertices > kGameGeosetMaxVertices || indices > kGameGeosetMaxIndices;
}

std::vector<GeosetGameLimitFinding> FindGeosetsOverGameLimit(const MdxModel& model) {
    std::vector<GeosetGameLimitFinding> findings;
    for (std::size_t index = 0; index < model.geosets.size(); ++index) {
        const auto& geoset = model.geosets[index];
        if (GeosetOverGameLimit(geoset.positions.size(), geoset.indices.size()))
            findings.push_back({index, geoset.positions.size(), geoset.indices.size()});
    }
    return findings;
}

std::optional<Diagnostic> GeosetGameLimitWarning(const MdxModel& model, std::string_view modelPath) {
    const auto findings = FindGeosetsOverGameLimit(model);
    if (findings.empty()) return std::nullopt;
    std::string message = "模型 " + std::string(modelPath) + "：";
    for (std::size_t i = 0; i < std::min(findings.size(), kListedGeosets); ++i) {
        const auto& finding = findings[i];
        message += (i ? "；" : "") + std::string("第 ") + std::to_string(finding.geoset + 1) + " 个几何体 "
            + std::to_string(finding.vertices) + " 顶点、" + std::to_string(finding.indices) + " 索引";
    }
    if (findings.size() > kListedGeosets) message += "；另有 " + std::to_string(findings.size() - kListedGeosets) + " 个";
    message += "。超过游戏上限（" + std::to_string(kGameGeosetMaxVertices) + " 顶点 / " + std::to_string(kGameGeosetMaxIndices)
        + " 索引）：1.27a + WarVK 里这些几何体整块不画；旧版 WarVK 里，视野内有它们时同帧阴影可能整帧放弃。建议拆到每块不超过 "
        + std::to_string(kGameGeosetSafeVertices) + " 顶点、" + std::to_string(kGameGeosetSafeIndices) + " 索引。";
    return Diagnostic{DiagnosticSeverity::Warning, "Render.Model", std::string(kGeosetOverGameLimitCode), std::move(message),
        OriginSpan{std::filesystem::path(std::u8string(modelPath.begin(), modelPath.end())), 0, 0, {}}};
}

std::optional<Diagnostic> GeosetGameLimitWarning(std::span<const std::byte> mdx, std::string_view modelPath) {
    const auto model = ParseMdx(mdx);
    if (!model.Succeeded() || !model.value) return std::nullopt;
    return GeosetGameLimitWarning(*model.value, modelPath);
}

std::optional<Diagnostic> GeosetGameLimitWarningForFile(const std::filesystem::path& file,
    std::string_view modelPath, std::size_t maxBytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error || size == 0 || size > maxBytes) return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) return std::nullopt;
    return GeosetGameLimitWarning(bytes, modelPath);
}

bool IsMdxPath(std::string_view path) noexcept {
    if (path.size() < 4) return false;
    const auto tail = path.substr(path.size() - 4);
    return tail[0] == '.' && (tail[1] == 'm' || tail[1] == 'M') && (tail[2] == 'd' || tail[2] == 'D')
        && (tail[3] == 'x' || tail[3] == 'X');
}

} // namespace War3Map::Render
