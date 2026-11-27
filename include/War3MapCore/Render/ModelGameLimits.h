#pragma once

#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace War3Map::Render {

// 1.27a + WarVK：单个几何体顶点多于 16384（Game.dll 动态顶点缓冲 0x4000）或三角形索引多于 49152 时，游戏里这块整个不画；
// 旧版 WarVK 里视野内有它时同帧阴影可能整帧放弃（新版已修复）。恰好等于上限的能画。编辑器预览照常画，作者看不出来。
// 判定只此一处：场景载入、单模型预览、导入、保存、MCP 与命令行共用。
inline constexpr std::size_t kGameGeosetMaxVertices = 16384;
inline constexpr std::size_t kGameGeosetMaxIndices = 49152;
// 建议拆到的安全线（留余量），只写进提示，不参与判定。
inline constexpr std::size_t kGameGeosetSafeVertices = 16000;
inline constexpr std::size_t kGameGeosetSafeIndices = 48000;
inline constexpr std::string_view kGeosetOverGameLimitCode = "MODEL_GEOSET_OVER_GAME_LIMIT";

struct GeosetGameLimitFinding {
    std::size_t geoset = 0;   // GEOS 中的序号，从 0 起
    std::size_t vertices = 0;
    std::size_t indices = 0;  // 只数三角形组的 16 位索引
};

[[nodiscard]] bool GeosetOverGameLimit(std::size_t vertices, std::size_t indices) noexcept;
[[nodiscard]] std::vector<GeosetGameLimitFinding> FindGeosetsOverGameLimit(const MdxModel& model);
// 有超限几何体时给一条警告：列出几何体、数值、游戏上限与建议值；origin 为模型路径，供按模型定位与去重。
[[nodiscard]] std::optional<Diagnostic> GeosetGameLimitWarning(const MdxModel& model, std::string_view modelPath);
// 从 MDX 字节判定（导入、保存、MCP 用）。解析失败不在这里报告（各路径已有自己的解析诊断），返回空。
[[nodiscard]] std::optional<Diagnostic> GeosetGameLimitWarning(std::span<const std::byte> mdx, std::string_view modelPath);
// 从磁盘文件判定（资源页导入/替换用）；读不到或超过 maxBytes 时返回空，导入本身另有读取与校验。
inline constexpr std::size_t kGeosetGameLimitFileBytes = 64ull * 1024 * 1024;
[[nodiscard]] std::optional<Diagnostic> GeosetGameLimitWarningForFile(const std::filesystem::path& file,
    std::string_view modelPath, std::size_t maxBytes = kGeosetGameLimitFileBytes);
// 按扩展名判断是否二进制 MDX（大小写不敏感）。
[[nodiscard]] bool IsMdxPath(std::string_view path) noexcept;

} // namespace War3Map::Render
