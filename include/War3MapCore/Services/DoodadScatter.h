#pragma once

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/Types.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::Services {

// 装饰物散布笔刷：界面、命令行和 MCP 共用的取点规则。坐标、半径、间距都是游戏单位；
// 表面由调用方提供（模型表面索引或地形），没有表面的点直接放弃，不会退回别的高度。
struct ScatterPaletteType {
    std::string rawcode;
    double weight = 1;            // 相对权重，> 0
    std::uint32_t variations = 1; // 物编“变体数”，≥ 1
};

enum class ScatterDensityMode { PerArea, PerDab };

struct ScatterBrushSettings {
    double radius = 256;
    ScatterDensityMode densityMode = ScatterDensityMode::PerArea;
    double perTenThousandSquareUnits = 1; // PerArea：每 100×100 游戏单位的期望个数
    std::uint32_t countPerDab = 5;        // PerDab：每一笔落点的个数
    double yawMinDegrees = 0, yawMaxDegrees = 360;
    double scaleMin = 1, scaleMax = 1;    // 等比缩放
    bool randomVariation = true;          // 否则一律用变体 0
    double minSpacing = 0;                // 与已有同调色板类型、本笔已放的水平最小间距
    double slopeMinDegrees = 0, slopeMaxDegrees = 90;
};

struct ScatterSurfaceSample {
    std::array<double, 3> position{};
    std::array<double, 3> normal{0, 0, 1};
};
using ScatterSurfaceProbe = std::function<std::optional<ScatterSurfaceSample>(double x, double y)>;

struct ScatterCandidate {
    std::string rawcode;
    std::uint32_t variation = 0;
    std::array<double, 3> position{};
    double yawRadians = 0;
    double scale = 1;
};

struct ScatterDabReport {
    std::vector<ScatterCandidate> placed;
    std::uint32_t requested = 0, noSurface = 0, slope = 0, spacing = 0;
};

// 与平台无关的确定性随机数（SplitMix64），同一种子同一笔得到同样结果。
class ScatterRandom {
public:
    explicit ScatterRandom(std::uint64_t seed) : state_(seed) {}
    [[nodiscard]] std::uint64_t Next() noexcept;
    [[nodiscard]] double Uniform() noexcept; // [0, 1)
private:
    std::uint64_t state_;
};

// 水平最小间距查询，按间距分格。
class ScatterSpacingIndex {
public:
    explicit ScatterSpacingIndex(double spacing) : spacing_(spacing) {}
    void Add(double x, double y);
    [[nodiscard]] bool Free(double x, double y) const;
    [[nodiscard]] double Spacing() const noexcept { return spacing_; }
private:
    double spacing_;
    std::unordered_map<std::uint64_t, std::vector<std::array<double, 2>>> cells_;
    [[nodiscard]] std::uint64_t Key(std::int64_t cx, std::int64_t cy) const noexcept;
};

// 设置与调色板的范围检查；失败时说明是哪一项。
[[nodiscard]] Status ValidateScatter(const ScatterBrushSettings& settings, std::span<const ScatterPaletteType> palette);

// 一笔（以 centerX, centerY 为圆心）的落点。通过的点加入 spacing。
[[nodiscard]] Result<ScatterDabReport> ScatterDab(double centerX, double centerY, const ScatterBrushSettings& settings,
    std::span<const ScatterPaletteType> palette, const ScatterSurfaceProbe& probe, ScatterSpacingIndex& spacing, ScatterRandom& random);

// 擦除：半径内（水平距离）、类型在调色板里的装饰物序号。
[[nodiscard]] std::vector<std::uint32_t> SelectScatterErase(double centerX, double centerY, double radius,
    std::span<const ScatterPaletteType> palette, std::span<const MapData::DoodadPlacement> doodads);

// 落点写成普通装饰物记录：固定高度（0x04），朝向、等比缩放、变体按落点。
[[nodiscard]] MapData::DoodadPlacement MakeScatterDoodad(const ScatterCandidate& candidate);

// 表面过滤：pattern 按分号或空白分成多条，每条可含 * 和 ?，不分大小写、/ 与 \ 视为相同；
// 与 rawcode 或模型路径任一匹配即算。空过滤匹配全部。
[[nodiscard]] bool ScatterSurfaceFilterMatches(std::string_view patterns, std::string_view rawcode, std::string_view modelPath);

// 每 100 平方米的个数换算为每 100×100 游戏单位的个数（unitsPerMetre 游戏单位 = 1 米）。
[[nodiscard]] double ScatterDensityFromSquareMetres(double perHundredSquareMetres, double unitsPerMetre);
[[nodiscard]] double ScatterDensityToSquareMetres(double perTenThousandSquareUnits, double unitsPerMetre);

} // namespace War3Map::Services
