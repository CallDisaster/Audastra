#include "War3MapCore/Services/DoodadScatter.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "unexpected failure";
        for (const auto& diagnostic : result.diagnostics) message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
bool FailsWith(const Status& status, std::string_view code) {
    return !status.Succeeded() && std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& item) { return item.code == code; });
}

const std::vector<ScatterPaletteType> kPalette{{"STr0", 3, 4}, {"SRk1", 1, 1}};

ScatterSurfaceProbe Flat(double z, double tiltDegrees = 0) {
    const double radians = tiltDegrees * std::numbers::pi / 180.0;
    return [=](double x, double y) -> std::optional<ScatterSurfaceSample> {
        return ScatterSurfaceSample{{x, y, z}, {std::sin(radians), 0, std::cos(radians)}};
    };
}

ScatterBrushSettings PerDab(std::uint32_t count) {
    ScatterBrushSettings settings;
    settings.radius = 100; settings.densityMode = ScatterDensityMode::PerDab; settings.countPerDab = count;
    settings.yawMinDegrees = 30; settings.yawMaxDegrees = 60; settings.scaleMin = 0.8; settings.scaleMax = 1.2;
    return settings;
}

void ValidationNamesTheBadField() {
    auto settings = PerDab(5);
    Require(ValidateScatter(settings, kPalette).Succeeded(), "合理设置通过");
    auto bad = settings; bad.radius = 0;
    Require(FailsWith(ValidateScatter(bad, kPalette), "SCATTER_RADIUS"), "半径为 0 拒绝");
    bad = settings; bad.slopeMinDegrees = 40; bad.slopeMaxDegrees = 20;
    Require(FailsWith(ValidateScatter(bad, kPalette), "SCATTER_SLOPE"), "坡度最小大于最大拒绝");
    bad = settings; bad.scaleMin = 2; bad.scaleMax = 1;
    Require(FailsWith(ValidateScatter(bad, kPalette), "SCATTER_SCALE"), "缩放范围颠倒拒绝");
    bad = settings; bad.countPerDab = 0;
    Require(FailsWith(ValidateScatter(bad, kPalette), "SCATTER_DENSITY"), "每笔 0 个拒绝");
    Require(FailsWith(ValidateScatter(settings, {}), "SCATTER_PALETTE_EMPTY"), "空调色板拒绝");
    const std::vector<ScatterPaletteType> weightless{{"STr0", 0, 1}};
    Require(FailsWith(ValidateScatter(settings, weightless), "SCATTER_PALETTE_WEIGHT"), "零权重拒绝");
    const std::vector<ScatterPaletteType> longCode{{"STree", 1, 1}};
    Require(FailsWith(ValidateScatter(settings, longCode), "SCATTER_PALETTE_TYPE"), "非四字符代码拒绝");
}

void DabPlacesWithinRangesOnTheSurface() {
    ScatterSpacingIndex spacing(0);
    ScatterRandom random(42);
    const auto report = Take(ScatterDab(500, -300, PerDab(40), kPalette, Flat(75), spacing, random));
    Require(report.requested == 40 && report.placed.size() == 40, "平面上每笔个数全部落下");
    std::size_t trees = 0;
    for (const auto& item : report.placed) {
        Require(std::hypot(item.position[0] - 500, item.position[1] + 300) <= 100 + 1e-9, "落点在半径内");
        Require(item.position[2] == 75, "高度取表面");
        const double degrees = item.yawRadians * 180 / std::numbers::pi;
        Require(degrees >= 30 - 1e-9 && degrees <= 60 + 1e-9, "朝向在范围内");
        Require(item.scale >= 0.8 && item.scale <= 1.2, "缩放在范围内");
        if (item.rawcode == "STr0") { ++trees; Require(item.variation < 4, "变体不超过变体数"); }
        else Require(item.rawcode == "SRk1" && item.variation == 0, "单变体类型用 0");
    }
    Require(trees > 20 && trees < 40, "按 3:1 权重混合两种类型");
    const auto record = MakeScatterDoodad(report.placed.front());
    Require((record.flags & MapData::kDoodadFlagFixedZ) && (record.flags & 2) && record.position[2] == 75.0f
        && record.scale[0] == record.scale[1] && record.scale[1] == record.scale[2]
        && record.rawcode == report.placed.front().rawcode, "写成固定高度的普通装饰物记录");
}

void SameSeedSameStroke() {
    const auto run = [](std::uint64_t seed) {
        ScatterSpacingIndex spacing(10);
        ScatterRandom random(seed);
        return Take(ScatterDab(0, 0, PerDab(25), kPalette, Flat(0), spacing, random)).placed;
    };
    const auto a = run(7), b = run(7), c = run(8);
    const auto same = [](const auto& left, const auto& right) {
        return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](const auto& x, const auto& y) {
            return x.rawcode == y.rawcode && x.variation == y.variation && x.position == y.position && x.yawRadians == y.yawRadians && x.scale == y.scale; });
    };
    Require(same(a, b), "同一种子结果相同");
    Require(!same(a, c), "不同种子结果不同");
}

void SpacingSlopeAndMissingSurface() {
    auto settings = PerDab(200); settings.minSpacing = 40;
    ScatterSpacingIndex spacing(settings.minSpacing);
    ScatterRandom random(1);
    const auto spaced = Take(ScatterDab(0, 0, settings, kPalette, Flat(0), spacing, random));
    Require(spaced.placed.size() < 200 && spaced.spacing > 0 && spaced.placed.size() + spaced.spacing == 200, "间距不够的点放弃并计数");
    for (std::size_t i = 0; i < spaced.placed.size(); ++i) for (std::size_t j = i + 1; j < spaced.placed.size(); ++j)
        Require(std::hypot(spaced.placed[i].position[0] - spaced.placed[j].position[0],
            spaced.placed[i].position[1] - spaced.placed[j].position[1]) >= 40, "任意两点不近于最小间距");
    ScatterSpacingIndex again(settings.minSpacing);
    for (const auto& item : spaced.placed) again.Add(item.position[0], item.position[1]);
    const auto repeat = Take(ScatterDab(0, 0, PerDab(20), kPalette, Flat(0), again, random));
    for (const auto& item : repeat.placed) for (const auto& old : spaced.placed)
        Require(std::hypot(item.position[0] - old.position[0], item.position[1] - old.position[1]) >= 40, "下一笔也避开已有的点");

    auto steep = PerDab(30); steep.slopeMaxDegrees = 20;
    ScatterSpacingIndex none(0);
    const auto rejected = Take(ScatterDab(0, 0, steep, kPalette, Flat(0, 30), none, random));
    Require(rejected.placed.empty() && rejected.slope == 30, "坡度超出最大值全部放弃");
    steep.slopeMinDegrees = 25; steep.slopeMaxDegrees = 35;
    Require(Take(ScatterDab(0, 0, steep, kPalette, Flat(0, 30), none, random)).placed.size() == 30, "坡度在范围内全部落下");
    Require(Take(ScatterDab(0, 0, steep, kPalette, Flat(0, 10), none, random)).slope == 30, "坡度低于最小值同样放弃");

    const ScatterSurfaceProbe half = [](double x, double y) -> std::optional<ScatterSurfaceSample> {
        if (x < 0) return std::nullopt;
        return ScatterSurfaceSample{{x, y, 5}, {0, 0, 1}};
    };
    const auto partial = Take(ScatterDab(0, 0, PerDab(100), kPalette, half, none, random));
    Require(std::all_of(partial.placed.begin(), partial.placed.end(), [](const auto& item) { return item.position[0] >= 0; }),
        "没有表面的位置不放");
    Require(partial.placed.size() + partial.noSurface == 100 && !partial.placed.empty(), "无表面的计数");
    Require(!ScatterDab(0, 0, PerDab(1), kPalette, {}, none, random).Succeeded(), "没有表面函数时失败");
}

void AreaDensityAveragesToTheSetting() {
    ScatterBrushSettings settings;
    settings.radius = 100; settings.densityMode = ScatterDensityMode::PerArea; settings.perTenThousandSquareUnits = 1;
    ScatterSpacingIndex spacing(0);
    ScatterRandom random(99);
    std::size_t total = 0;
    constexpr int dabs = 4000;
    for (int i = 0; i < dabs; ++i) total += Take(ScatterDab(0, 0, settings, kPalette, Flat(0), spacing, random)).placed.size();
    const double average = static_cast<double>(total) / dabs;
    Require(std::abs(average - std::numbers::pi) < 0.05, "每笔平均个数等于密度乘面积");
}

void EraseOnlyPaletteTypesInRadius() {
    std::vector<MapData::DoodadPlacement> doodads(4);
    doodads[0].rawcode = "STr0"; doodads[0].position = {10, 0, 0};
    doodads[1].rawcode = "XCh1"; doodads[1].position = {0, 10, 0};   // 表面块，不在调色板
    doodads[2].rawcode = "SRk1"; doodads[2].position = {300, 0, 0};  // 半径外
    doodads[3].rawcode = "SRk1"; doodads[3].position = {-60, 70, 900}; // 半径内（只看水平距离）
    const auto selected = SelectScatterErase(0, 0, 100, kPalette, doodads);
    Require(selected == std::vector<std::uint32_t>{0, 3}, "只擦调色板类型、按水平距离");
    Require(SelectScatterErase(0, 0, 0, kPalette, doodads).empty(), "半径为 0 不擦");
}

void FilterAndUnits() {
    Require(ScatterSurfaceFilterMatches("Sonne\\Models\\near_*", "XCh1", "sonne/models/NEAR_01.mdx"), "路径通配不分大小写与斜杠");
    Require(!ScatterSurfaceFilterMatches("Sonne\\Models\\near_*", "XCh1", "Sonne\\Models\\mid_01.mdx"), "不匹配的路径");
    Require(ScatterSurfaceFilterMatches("mid_*; XC??", "XCh1", "Doodads\\Rock.mdx"), "多条之一按类型代码匹配");
    Require(ScatterSurfaceFilterMatches("", "Abcd", "x.mdx") && ScatterSurfaceFilterMatches("  ; ", "Abcd", "x.mdx"), "空过滤匹配全部");
    Require(!ScatterSurfaceFilterMatches("*.mdl", "Abcd", "x.mdx"), "扩展名也要匹配");
    const double perUnits = ScatterDensityFromSquareMetres(1, 40);
    Require(std::abs(perUnits - 0.0625) < 1e-12, "40 单位每米时，每 100 平方米 1 个 = 每 100×100 单位 0.0625 个");
    Require(std::abs(ScatterDensityToSquareMetres(perUnits, 40) - 1) < 1e-12, "换算往返");
}
} // namespace

int main() {
    try {
        ValidationNamesTheBadField();
        DabPlacesWithinRangesOnTheSurface();
        SameSeedSameStroke();
        SpacingSlopeAndMissingSurface();
        AreaDensityAveragesToTheSetting();
        EraseOnlyPaletteTypesInRadius();
        FilterAndUnits();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
