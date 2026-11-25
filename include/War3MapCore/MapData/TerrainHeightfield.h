#pragma once

#include "War3MapCore/MapData/TerrainBlockEditing.h"
#include "War3MapCore/MapData/TerrainWaterBrush.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::MapData {

// 外部高度场（sonne-heightfield/v1）：json 清单 + 小端 float32 顶点高度。
// 坐标与地形网格相同：x 向东、y 向北，128 单位一格，第 0 行在最南边，每行由西向东。
inline constexpr std::string_view TerrainHeightfieldFormatV1 = "sonne-heightfield/v1";
inline constexpr std::uint32_t TerrainHeightfieldMaximumSide = 2049; // 顶点数，与经典 W3E 坐标域一致
inline constexpr std::size_t TerrainHeightfieldManifestMaximumBytes = 64u * 1024u;

struct TerrainHeightfieldSource {
    std::uint32_t columns = 0, rows = 0;
    std::array<std::int32_t, 2> originWorld{}; // 第 0 列第 0 行顶点的世界坐标，128 的整数倍
    float fillHeight = 0;                       // 地图上高度场没盖到的顶点
    std::string dataPath;                       // 相对清单目录；省略时为 heightfield.f32
    std::vector<float> heights;                 // columns×rows；只读清单时为空
};

// 只解析清单；高度数据另由 LoadTerrainHeightfieldSamples 装入。
[[nodiscard]] Result<TerrainHeightfieldSource> ParseTerrainHeightfieldManifest(std::span<const std::byte> json);
// 字节数必须正好是 4×columns×rows；NaN/无穷大整份拒绝。
[[nodiscard]] Status LoadTerrainHeightfieldSamples(TerrainHeightfieldSource& source, std::span<const std::byte> f32);
// 宿主便捷入口：读清单和同目录下的数据文件。数据路径不能是绝对路径，也不能含“..”。
[[nodiscard]] Result<TerrainHeightfieldSource> ReadTerrainHeightfield(const std::filesystem::path& manifest);

struct TerrainHeightfieldOptions {
    std::uint8_t cliffLevel = 2; // 整图统一的悬崖层，0..14，并受实际规则的最高层限制
    float sink = 0;              // 整体下沉量（单位），换算前从每个高度中减去
};

// 已换算的整图载荷：每个顶点的地面原始值，加上整图统一的悬崖层。
// 摘要绑定载荷本身，供工程历史和重放核对；不代表来源或保存资格。
struct TerrainHeightfieldIntent {
    std::uint8_t algorithm = 1;
    std::uint8_t cliffLevel = 0;
    std::uint32_t width = 0, height = 0; // 顶点数，重放时必须等于当时的地形网格
    std::vector<std::uint16_t> groundRaw; // width×height，行序同 W3E
    std::string payloadSha256;
    bool operator==(const TerrainHeightfieldIntent&) const = default;
};

struct TerrainHeightfieldClampSample {
    float worldX = 0, worldY = 0, height = 0; // height 为减去下沉量后、截断前的值
};

struct TerrainHeightfieldReport {
    std::uint64_t importedVertices = 0; // 取自高度场的地图顶点
    std::uint64_t filledVertices = 0;   // 用 fillHeight 的地图顶点
    std::uint64_t outsideVertices = 0;  // 落在地图外、没有导入的高度场顶点
    std::uint64_t clampedLow = 0, clampedHigh = 0;
    float minimumHeight = 0, maximumHeight = 0;     // 减去下沉量后，整图（含填充）的范围
    float storableMinimum = 0, storableMaximum = 0; // 当前悬崖层下可存的范围
    // 被截断顶点的范围（顶点索引，闭区间）；没有截断时全为 0 且 clampedLow/High 为 0。
    std::array<std::uint32_t, 4> clampedBounds{};
    std::vector<TerrainHeightfieldClampSample> clampedSamples; // 行序最前的至多 32 个
    // 地图上被高度场覆盖的顶点范围（闭区间）；没有覆盖时 importedVertices 为 0。
    std::array<std::uint32_t, 4> coveredBounds{};
};

struct TerrainHeightfieldConversion {
    TerrainHeightfieldIntent intent;
    TerrainHeightfieldReport report;
};

// 原点与地形网格不对齐时拒绝，不重采样。只读 header 的宽高与原点。
[[nodiscard]] Result<TerrainHeightfieldConversion> ConvertTerrainHeightfield(const TerrainHeader& header,
    const TerrainHeightfieldSource& source, const TerrainHeightfieldOptions& options);
// 仅结构检查（含摘要）；工程历史和宿主重放共用，不读取地形。
[[nodiscard]] Status ValidateTerrainHeightfieldIntent(const TerrainHeightfieldIntent& intent);

// 与地形块候选同形，共用会话的统一提交流程。
using TerrainHeightfieldStrokeCandidate = TerrainBlockStrokeCandidate;

// 纯 owned 候选：写入每个顶点的地面高度和统一悬崖层，清除坡道标记；贴图、水、荒芜、边界和外缘标记不变。
// 悬崖层须在实际规则内；与有高差的特殊地形模型冲突时整笔拒绝。
[[nodiscard]] Result<TerrainHeightfieldStrokeCandidate> PrepareTerrainHeightfieldStroke(
    const TerrainGridSnapshot& source, const TerrainWaterLoadedRules& rules, const TerrainHeightfieldIntent& intent,
    std::span<const TerrainWaterSpecialDoodadShape> specialShapes = {}, const TerrainWaterBrushLimits& limits = {});

} // namespace War3Map::MapData
