#pragma once

#include "TerrainWetInnerCornerRampFixtures.h"

namespace War3Map::TerrainWetInnerCornerRampCases {
using namespace MapData;
namespace W = TerrainWetInnerCornerRampFixtures;
namespace C = TerrainInnerCornerRampFixtures;
using W::Require;
using W::Take;

inline constexpr std::array kThresholds{W::Threshold::BelowSurface, W::Threshold::SurfaceEquality,
    W::Threshold::AboveSurface, W::Threshold::BelowDeep, W::Threshold::DeepEquality, W::Threshold::AboveDeep};

inline TerrainNativePathingInput PathingInput(const TerrainGridSnapshot& terrain,
    std::span<const TerrainNativeGroundRule> ground) {
    TerrainNativePathingInput input;
    input.terrain = &terrain; input.groundRules = ground;
    input.playableBounds = {0, 0, double(terrain.header.width - 1) * 128, double(terrain.header.height - 1) * 128};
    input.maximumCliffLevel = 14; input.water = TerrainNativeWaterRules{0.25f, 64, false};
    return input;
}

inline void Rejected(const auto& result, std::string_view code = {}) {
    Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(),
        "湿内角失败不得返回部分候选，且必须带明确诊断");
    if (!code.empty()) Require(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; }), "湿内角失败必须命中预期门禁");
}

inline void FourDirectionsSixThresholdsAndFullStreams() {
    static_assert(static_cast<unsigned>(TerrainCornerRampAlgorithm::DryOnly) == 1);
    static_assert(static_cast<unsigned>(TerrainCornerRampAlgorithm::WetTerrain) == 2);
    static_assert(static_cast<unsigned>(TerrainCornerRampAlgorithm::NativeCorners) == 3);
    static_assert(static_cast<unsigned>(TerrainCornerRampAlgorithm::NativeWetCorners) == 4);
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
    const DoodadPlacements doodads;
    const auto originalDoo = Take(doodads.Serialize());
    constexpr std::array<std::string_view, 5> labels{"AAABA", "ALRHA", "ARSHA", "BHHHA", "AAAAA"};
    for (const auto threshold : kThresholds) for (const bool mixed : {false, true})
        for (const int dx : {1, -1}) for (const int dy : {1, -1}) {
            const W::Options options{threshold, mixed};
            const auto source = W::Grid(options, 17, dx, dy); const auto point = C::Point(17, dx, dy);
            const auto original = W::TerrainLiteral(false, options, 17, dx, dy);
            Require(Take(SerializeMapTerrain(source)) == original, "四向湿内角源W3E须等于手写完整原字");
            Require(Take(GenerateTerrainNativePathing(PathingInput(source, ground)))
                == W::PathingPixels(false, options, 17, dx, dy), "四向水深阈值源WPM须等于独立逐像素矩阵");
            const auto plan = Take(PlanTerrainCornerRamp(source, point));
            Require(plan.inner && plan.highX == unsigned(8 + dx) && plan.highY == unsigned(8 + dy)
                && plan.directionX == -dx && plan.directionY == -dy,
                "默认算法4须按真实内角选对侧高端，保留两个方向符号");
            for (const auto old : {TerrainCornerRampAlgorithm::DryOnly, TerrainCornerRampAlgorithm::WetTerrain,
                TerrainCornerRampAlgorithm::NativeCorners}) {
                Rejected(PlanTerrainCornerRamp(source, point, old));
                Rejected(PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(), std::span(&point, 1),
                    doodads, {}, {}, old));
            }
            const auto created = Take(PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(),
                std::span(&point, 1), doodads));
            Require(created.result.changedVertices == 9 && !created.result.changed.Empty()
                && created.result.requiresPathingRebuild && created.result.requiresObjectValidityRefresh
                && created.result.removedSpecialSourceRecords.empty(), "湿内角创建应仅改九个R位并声明实际后续工作");
            Require(Take(SerializeMapTerrain(created.terrain)) == W::TerrainLiteral(true, options, 17, dx, dy)
                && Take(created.doodads.Serialize()) == originalDoo,
                "创建须匹配完整手写W3E，保留水位/水标志和完整DOO");
            const auto generated = Take(GenerateTerrainNativePathing(PathingInput(created.terrain, ground)));
            Require(generated == W::PathingPixels(true, options, 17, dx, dy),
                "创建后的完整WPM须匹配手算S96/R64/H0、两肩和三个开口矩阵");
            for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
                const auto state = Take(QueryNativeTerrainRamp(created.terrain, 14, unsigned(8 + x * dx), unsigned(8 + y * dy)));
                Require(state.corner == labels[y + 2][x + 2], "湿地标志不得改变手算内角H/R/S标签矩阵");
                const float offset = state.corner == 'R' ? 64.0f : state.corner == 'S' ? 96.0f : 0.0f;
                Require(state.heightOffset == offset, "湿内角水查询R加64、S加96，H和其余角不加128");
            }
            const auto noop = Take(PrepareTerrainCornerRampStroke(created.terrain, W::Rules(), C::Brush(),
                std::span(&point, 1), doodads));
            Require(noop.result.changed.Empty() && noop.result.changedVertices == 0
                && !noop.result.processed.Empty() && !noop.result.requiresPathingRebuild
                && !noop.result.requiresObjectValidityRefresh
                && Take(SerializeMapTerrain(noop.terrain)) == W::TerrainLiteral(true, options, 17, dx, dy)
                && Take(GenerateTerrainNativePathing(PathingInput(noop.terrain, ground))) == generated,
                "完整湿内角再次创建是验证过接受域后的完整W3E/WPM no-op");
            const auto removed = Take(PrepareTerrainCornerRampStroke(created.terrain, W::Rules(), C::Brush(false),
                std::span(&point, 1), doodads));
            Require(removed.result.changedVertices == 9 && Take(SerializeMapTerrain(removed.terrain)) == original
                && Take(GenerateTerrainNativePathing(PathingInput(removed.terrain, ground)))
                    == W::PathingPixels(false, options, 17, dx, dy)
                && Take(removed.doodads.Serialize()) == originalDoo, "移除须恢复全部W3E/WPM，绝不重设原水位或删除DOO");
            const auto removeNoop = Take(PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(false),
                std::span(&point, 1), doodads));
            Require(removeNoop.result.changed.Empty() && Take(SerializeMapTerrain(removeNoop.terrain)) == original,
                "未创建湿内角的移除仍须完整检查并返回精确no-op");
            Require(Take(SerializeMapTerrain(source)) == original && Take(doodads.Serialize()) == originalDoo,
                "创建、移除及重复操作均不得改动传入源地形或DOO");
        }
}

inline void StrictSurfaceDeepThresholdsAndShoulders() {
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}}; const DoodadPlacements doodads;
    const auto point = C::Point();
    // 原始水位/水面分别相差四分之一世界单位；下面的expected不经产品水分类器。
    constexpr std::array<std::uint16_t, 6> raw{8448, 8449, 8450, 8704, 8705, 8706};
    constexpr std::array<unsigned, 6> solidS{0x40, 0x48, 0x08, 0x08, 0x0a, 0x0a};
    constexpr std::array<unsigned, 6> mixedS{0x40, 0x48, 0x48, 0x48, 0x0a, 0x0a};
    constexpr std::array<unsigned, 6> solidR{0x48, 0x48, 0x08, 0x0a, 0x0a, 0x0a};
    constexpr std::array<unsigned, 6> mixedR{0x48, 0x48, 0x48, 0x0a, 0x0a, 0x0a};
    constexpr std::array<unsigned, 6> solidH{0x40, 0x40, 0x40, 0x08, 0x08, 0x08};
    const auto at = [](const W::Bytes& pixels, unsigned x, unsigned y) { return std::to_integer<unsigned>(pixels[y * 64 + x]); };
    for (std::size_t index = 0; index < kThresholds.size(); ++index) for (const bool mixed : {false, true}) {
        const W::Options options{kThresholds[index], mixed};
        Require(W::WaterRaw(options) == raw[index] && W::GroundQuarter(true, 0, 0) == 385
            && W::GroundQuarter(true, 0, -1) == 257 && W::GroundQuarter(true, 1, 0) == 513,
            "水面与手写有效地高保持整数四分之一单位，不靠浮点容差掩盖相等条件");
        const auto created = Take(PrepareTerrainCornerRampStroke(W::Grid(options), W::Rules(), C::Brush(),
            std::span(&point, 1), doodads));
        const auto pixels = Take(GenerateTerrainNativePathing(PathingInput(created.terrain, ground)));
        // SW低地格的NE象限取S，SE象限取R；新开口东南象限取H。
        Require(at(pixels, 30, 30) == (mixed ? mixedS[index] : solidS[index]),
            "G==surface仍为浅水但strict为false；G==surface-deep必须为深水");
        Require(at(pixels, 30, 28) == (mixed ? mixedR[index] : solidR[index]),
            "同一地格最近R64顶点不可误用SW或S96地高");
        Require(at(pixels, 34, 32) == (mixed ? 0x40u : solidH[index]),
            "H角的水查询偏移是零；不能误加128或套用S96");
        if (index < 3) {
            Require(at(pixels, 30, 26) == (mixed ? 0xcau : 0x0au)
                && at(pixels, 26, 30) == (mixed ? 0xcau : 0x0au),
                "两肩各只有一个R，保持不可行走，混合水角不可抹去悬崖限制");
        }
    }
}

inline void HaloOnlyWaterAndHistoricalAlgorithmDomains() {
    const DoodadPlacements doodads;
    for (const int dx : {1, -1}) for (const int dy : {1, -1}) {
        const auto point = C::Point(17, dx, dy);
        const W::Options options{W::Threshold::SurfaceEquality, true};
        auto source = W::Grid(options, 17, dx, dy);
        auto original = W::TerrainLiteral(false, options, 17, dx, dy);
        auto expected = W::TerrainLiteral(true, options, 17, dx, dy);
        // 仅actual dirty/halo的(-4,-4)有水；九个目标顶点均无水。
        for (const auto relative : {std::array{0, 0}, std::array{0, -1}}) {
            const auto index = std::size_t(8 + relative[1] * dy) * 17 + unsigned(8 + relative[0] * dx);
            source.vertices[index].textureAndFlagsRaw &= ~0x40;
            for (auto* bytes : {&original, &expected}) (*bytes)[45 + index * 7 + 4] &= std::byte{0xbf};
        }
        RefreshTerrainDerived(source, 0, 0, 17, 17);
        Require(Take(SerializeMapTerrain(source)) == original, "仅halo有水的源仍须逐字匹配独立原字");
        Require(PlanTerrainCornerRamp(source, point, TerrainCornerRampAlgorithm::NativeCorners).Succeeded(),
            "历史3只看九点的预览不得冒充完整halo许可");
        Rejected(PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(), std::span(&point, 1), doodads,
            {}, {}, TerrainCornerRampAlgorithm::NativeCorners));
        const auto created = Take(PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(), std::span(&point, 1), doodads));
        Require(Take(SerializeMapTerrain(created.terrain)) == expected, "算法4须显式覆盖仅实际halo含水的完整内角");
        const auto noop = Take(PrepareTerrainCornerRampStroke(created.terrain, W::Rules(), C::Brush(), std::span(&point, 1), doodads));
        Require(noop.result.changed.Empty() && Take(SerializeMapTerrain(noop.terrain)) == expected,
            "湿halo的完整坡道no-op仍应通过真实新算法检查");
        const auto dry = C::Grid(17, dx, dy);
        for (const auto algorithm : {TerrainCornerRampAlgorithm::NativeCorners, TerrainCornerRampAlgorithm::NativeWetCorners}) {
            const auto result = Take(PrepareTerrainCornerRampStroke(dry, C::Rules(), C::Brush(), std::span(&point, 1), doodads,
                {}, {}, algorithm));
            Require(Take(SerializeMapTerrain(result.terrain)) == C::TerrainLiteral(true, 17, dx, dy),
                "算法3的既有干内角输出与算法4保持一致，不能改写旧域");
        }
        auto outer = W::Grid(options, 17, dx, dy);
        auto outerOriginal = W::TerrainLiteral(false, options, 17, dx, dy);
        for (std::size_t index = 0; index < outer.vertices.size(); ++index) {
            outer.vertices[index].cliffAndLayerRaw = outer.vertices[index].CliffLevel() == 2 ? 3 : 2;
            auto& level = outerOriginal[45 + index * 7 + 6];
            level = level == std::byte{2} ? std::byte{3} : std::byte{2};
        }
        RefreshTerrainDerived(outer, 0, 0, 17, 17);
        Require(Take(SerializeMapTerrain(outer)) == outerOriginal, "旧湿外角源完整原字来自手写镜像层级");
        auto outerExpected = outerOriginal;
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x) {
            const auto index = std::size_t(8 + y * dy) * 17 + unsigned(8 + x * dx);
            outerExpected[45 + index * 7 + 4] |= std::byte{0x10};
        }
        Rejected(PlanTerrainCornerRamp(outer, point, TerrainCornerRampAlgorithm::DryOnly));
        for (const auto algorithm : {TerrainCornerRampAlgorithm::WetTerrain, TerrainCornerRampAlgorithm::NativeCorners,
            TerrainCornerRampAlgorithm::NativeWetCorners}) {
            const auto result = Take(PrepareTerrainCornerRampStroke(outer, W::Rules(), C::Brush(), std::span(&point, 1), doodads,
                {}, {}, algorithm));
            Require(!Take(PlanTerrainCornerRamp(outer, point, algorithm)).inner
                && Take(SerializeMapTerrain(result.terrain)) == outerExpected,
                "算法2/3的既有湿外角仍匹配相同手写完整原字，算法4不得收窄它们");
        }
    }
}

inline void RejectsUnsafeDomainsAndBudgetsAtomically() {
    const DoodadPlacements doodads; const auto doo = Take(doodads.Serialize()); const auto point = C::Point();
    const auto reject = [&](TerrainGridSnapshot source, const TerrainWaterBrushLimits& limits = {}, bool created = false) {
        RefreshTerrainDerived(source, 0, 0, source.header.width, source.header.height);
        const auto original = Take(SerializeMapTerrain(source));
        const auto result = PrepareTerrainCornerRampStroke(source, W::Rules(), C::Brush(!created), std::span(&point, 1), doodads,
            {}, limits);
        Rejected(result);
        Require(Take(SerializeMapTerrain(source)) == original && Take(doodads.Serialize()) == doo,
            "湿内角拒绝必须保持完整源W3E和DOO，不能发布部分修改");
    };
    auto source = W::Grid(); source.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x10; reject(source);
    source = W::Grid(); source.vertices[4 * 17 + 4].cliffAndLayerRaw = 3; reject(source);
    source = W::Grid(); source.vertices[4 * 17 + 4].groundHeightRaw += 1; reject(source);
    source = W::Grid(); source.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x80; reject(source);
    source = W::Grid(); source.vertices[4 * 17 + 4].waterHeightAndFlagsRaw |= 0x4000; reject(source);
    source = W::Grid(); source.vertices[4 * 17 + 4].waterHeightAndFlagsRaw |= 0x8000; reject(source);
    source = W::Grid(); source.vertices[8 * 17 + 8].textureAndFlagsRaw |= 0x10; reject(source);
    TerrainWaterBrushLimits limits; limits.maximumCandidateBytes = 1; reject(W::Grid(), limits);
    limits = {}; limits.maximumWork = 1; reject(W::Grid(), limits);
    limits = {}; limits.maximumPoints = 0; reject(W::Grid(), limits);
    const auto clean = W::Grid();
    const auto complete = Take(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), std::span(&point, 1), doodads));
    limits = {}; limits.maximumWork = complete.result.work;
    Require(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), std::span(&point, 1), doodads, {}, limits).Succeeded(),
        "湿内角完整前检与owned事务共享累计预算，精确工作边界通过");
    --limits.maximumWork; reject(clean, limits);
    auto malformed = complete.terrain; malformed.vertices[4 * 17 + 4].textureAndFlagsRaw |= 0x80;
    reject(malformed, {}, false); reject(malformed, {}, true);
    const std::array ground{TerrainNativeGroundRule{"Tg01", true, true, true}};
    const auto fullSize = W::PathingPixels(true).size() + complete.terrain.vertices.size();
    const auto input = PathingInput(complete.terrain, ground);
    Require(Take(GenerateTerrainNativePathing(input, fullSize)) == W::PathingPixels(true),
        "完整WPM与全图坡道标签累计输出预算精确边界通过");
    Rejected(GenerateTerrainNativePathing(input, fullSize - 1), "TERRAIN_NATIVE_BUDGET");
    for (const auto invalid : {TerrainStrokePoint{1024, 1056}, TerrainStrokePoint{1088, 1056},
        TerrainStrokePoint{std::numeric_limits<double>::infinity(), 1056}, TerrainStrokePoint{32, 32}})
        Rejected(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), std::span(&invalid, 1), doodads));
    const std::array duplicate{point, point};
    Rejected(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), duplicate, doodads), "TERRAIN_CORNER_RAMP_BRUSH");
    Rejected(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), std::span(&point, 1), doodads,
        {}, {}, static_cast<TerrainCornerRampAlgorithm>(5)), "TERRAIN_CORNER_RAMP_ALGORITHM");
    auto special = doodads; special.specialDoodads.push_back({"Spc0", 0, {8, 8}});
    const auto specialBefore = Take(special.Serialize());
    const std::array shapes{TerrainWaterSpecialDoodadShape{0, "Spc0", 0, {8, 8}, 8, 8, 9, 9, {0, 1, 1, 1}}};
    Rejected(PrepareTerrainCornerRampStroke(clean, W::Rules(), C::Brush(), std::span(&point, 1), special, shapes),
        "TERRAIN_CORNER_RAMP_SPECIAL");
    Require(Take(SerializeMapTerrain(clean)) == W::TerrainLiteral(false) && Take(special.Serialize()) == specialBefore,
        "special相交拒绝不能改写完整源地形或删除special尾段");
}

inline void Check() {
    FourDirectionsSixThresholdsAndFullStreams(); StrictSurfaceDeepThresholdsAndShoulders();
    HaloOnlyWaterAndHistoricalAlgorithmDomains(); RejectsUnsafeDomainsAndBudgetsAtomically();
}
} // namespace War3Map::TerrainWetInnerCornerRampCases
