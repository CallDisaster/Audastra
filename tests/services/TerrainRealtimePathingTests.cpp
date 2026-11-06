#include "../TerrainCornerRampFixtures.h"

#include <fstream>
#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::TerrainCornerRampFixtures;

void EqualPathing(const Bytes& actual, const Bytes& expected, const char* message) {
    if (actual == expected) return;
    std::string details = message;
    details += ": actual=" + std::to_string(actual.size()) + ", expected=" + std::to_string(expected.size());
    for (std::size_t index = 0; index < std::min(actual.size(), expected.size()); ++index) {
        if (actual[index] == expected[index]) continue;
        details += ", first_byte=" + std::to_string(index) + ", actual="
            + std::to_string(std::to_integer<unsigned>(actual[index])) + ", expected="
            + std::to_string(std::to_integer<unsigned>(expected[index]));
        break;
    }
    throw std::runtime_error(details);
}

Bytes SavedPathing(const Fixture& fixture, const MapDataEditSession& session) {
    auto plan = Take(TerrainPathingSave::PrepareForSave(*fixture.source, session, fixture.game));
    Require(plan.SaveGate().Succeeded(), "真实保存门通过");
    for (const auto& member : plan.Replacements())
        if (member.name == "war3map.wpm") return member.data;
    return fixture.members[1].data;
}

Bytes QueriedPathing(const Fixture& fixture) {
    auto result = fixture.members[1].data;
    const auto& header = fixture.edits->Terrain()->header;
    const auto width = (header.width - 1) * 4;
    for (std::uint32_t y = 0; y < (header.height - 1) * 4; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto sampled = Take(fixture.edits->InspectTerrainPoint(
                {header.offset[0] + x * 32 + 16, header.offset[1] + y * 32 + 16}));
            Require(sampled.pathingState == TerrainPathingState::Available && sampled.pathingFlags, "实时逐格查询可用");
            result[16 + y * width + x] = std::byte(*sampled.pathingFlags);
        }
    }
    return result;
}

void WaterBlightCliffAndHistoryKeepSaveBytes() {
    Fixture fixture;
    auto reference = fixture.FreshSession();
    const auto source = std::shared_ptr<const MapArchiveSession>(fixture.source.get(), [](const auto*) {});
    TerrainPathingSave::BindRealtimePathing(*fixture.edits, source, fixture.game);
    std::vector<Bytes> history{fixture.members[1].data};
    const std::array points{TerrainStrokePoint{3072, 3072}};
    const auto check = [&] {
        std::cout << "Checking stroke " << history.size() << '\n';
        const auto actual = SavedPathing(fixture, *fixture.edits);
        EqualPathing(actual, SavedPathing(fixture, *reference), "有实时层与旧保存时重建WPM逐字节相同");
        EqualPathing(QueriedPathing(fixture), actual, "实时逐格查询等于最终保存WPM");
        history.push_back(actual);
    };
    const auto rules = fixture.Rules();
    TerrainWaterBrush water{TerrainWaterBrushKind::Shallow, TerrainBrushShape::Square, 2, 0, 7,
        Take(CaptureTerrainWaterStrokeAnchor(*fixture.edits->Terrain(), points.front()))};
    Take(fixture.edits->ApplyPreparedTerrainEdit(Take(fixture.edits->PrepareTerrainWaterEdit(rules, water, points, false))));
    Take(reference->ApplyPreparedTerrainEdit(Take(reference->PrepareTerrainWaterEdit(rules, water, points, false))));
    check();
    TerrainBrush blight;
    blight.kind = TerrainBrushKind::Blight;
    blight.shape = TerrainBrushShape::Square;
    blight.radius = 2;
    blight.value = true;
    Take(fixture.edits->EditTerrain(blight, points));
    Take(reference->EditTerrain(blight, points));
    check();
    TerrainBrush cliff;
    cliff.kind = TerrainBrushKind::CliffLevel;
    cliff.shape = TerrainBrushShape::Square;
    cliff.size = 3;
    cliff.levelDelta = 1;
    const auto cliffRules = fixture.Rules();
    Take(fixture.edits->ApplyPreparedTerrainEdit(Take(fixture.edits->PrepareTerrainCliffEdit(cliffRules, cliff, points, false))));
    Take(reference->ApplyPreparedTerrainEdit(Take(reference->PrepareTerrainCliffEdit(cliffRules, cliff, points, false))));
    check();
    for (std::size_t index = history.size() - 1; index; --index) {
        Require(fixture.edits->Undo() && reference->Undo(), "地形和通行一起撤销");
        Require(QueriedPathing(fixture) == history[index - 1], "撤销恢复完整实时WPM");
        Require(SavedPathing(fixture, *fixture.edits) == SavedPathing(fixture, *reference), "撤销不改变保存结果");
    }
    for (std::size_t index = 1; index < history.size(); ++index) {
        Require(fixture.edits->Redo() && reference->Redo(), "地形和通行一起重做");
        Require(QueriedPathing(fixture) == history[index], "重做恢复完整实时WPM");
    }
}

void PureBlightAndRejectedBudgetAreAtomic() {
    Fixture fixture;
    const auto source = std::shared_ptr<const MapArchiveSession>(fixture.source.get(), [](const auto*) {});
    TerrainPathingSave::BindRealtimePathing(*fixture.edits, source, fixture.game);
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Blight;
    brush.radius = 2;
    brush.value = true;
    const std::array points{TerrainStrokePoint{3072, 3072}};
    Take(fixture.edits->EditTerrain(brush, points));
    Require(QueriedPathing(fixture) == SavedPathing(fixture, *fixture.edits), "纯荒芜沿用原保存分支");
    Require(fixture.edits->Undo(), "预算负例从原基线开始");
    TerrainPathingSave::BindRealtimePathing(*fixture.edits, source, fixture.game, 1024);
    const auto before = Take(SerializeMapTerrain(*fixture.edits->Terrain()));
    const auto revision = fixture.edits->Revision();
    Require(!fixture.edits->EditTerrain(brush, points).Succeeded(), "预算不足拒绝整笔");
    Require(fixture.edits->Revision() == revision && !fixture.edits->CanUndo() && fixture.edits->CanRedo()
        && Take(SerializeMapTerrain(*fixture.edits->Terrain())) == before
        && QueriedPathing(fixture) == fixture.members[1].data, "失败不发布地形、通行或历史前缀");
}

void DirectoryCandidateUsesSameLayerAndRejectsDrift() {
    Fixture fixture;
    const auto directory = fixture.root / "workspace";
    for (const auto& member : fixture.members) {
        const auto path = directory / std::filesystem::path(member.name);
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(member.data.data()), static_cast<std::streamsize>(member.data.size()));
        Require(output.good(), "写入原创目录夹具");
    }
    const auto workspace = Take(DirectoryWorkspace::Open(directory));
    TerrainBrush brush;
    brush.kind = TerrainBrushKind::Blight;
    brush.radius = 2;
    brush.value = true;
    const std::array points{TerrainStrokePoint{3072, 3072}};
    Take(fixture.edits->EditTerrain(brush, points));
    const auto revision = fixture.edits->Revision();
    Require(TerrainPathingSave::RefreshRealtimePathing(*fixture.edits, workspace.Snapshot(), fixture.game).Succeeded(),
        "目录重放的未发布候选更新派生层");
    EqualPathing(QueriedPathing(fixture), SavedPathing(fixture, *fixture.edits), "目录与归档共用保存WPM");
    Require(fixture.edits->Revision() == revision, "只读派生刷新不消费编辑修订");
    const auto before = QueriedPathing(fixture);
    {
        std::ofstream output(directory / "TerrainArt/Terrain.slk", std::ios::binary | std::ios::app);
        output << "\n// changed fixture source\n";
        Require(output.good(), "制造真实来源漂移");
    }
    Require(!TerrainPathingSave::RefreshRealtimePathing(*fixture.edits, workspace.Snapshot(), fixture.game).Succeeded()
        && fixture.edits->Revision() == revision && QueriedPathing(fixture) == before,
        "目录来源漂移不能发布派生层或消费修订");
}
} // namespace

int main() {
    try {
        WaterBlightCliffAndHistoryKeepSaveBytes();
        PureBlightAndRejectedBudgetAreAtomic();
        DirectoryCandidateUsesSameLayerAndRejectsDrift();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
