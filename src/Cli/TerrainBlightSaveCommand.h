#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 只接受已绑定源/W3E SHA 的荒芜领域操作；执行脚本构建并写出独立地图候选。
int RunTerrainBlightSave(const std::filesystem::path& sourceMap,
    std::string_view sourceSha256, std::string_view terrainSha256,
    const std::filesystem::path& toolRoot, const std::filesystem::path& gameRoot,
    std::string_view vertexX, std::string_view vertexY, std::string_view radius,
    std::string_view operation, const std::filesystem::path& candidate,
    std::optional<std::vector<std::string>> effectiveLayers = std::nullopt);
