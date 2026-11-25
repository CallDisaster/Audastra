#include "../TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainHeightfield.h"
#include "War3MapCore/MapData/TerrainStrokeJournal.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace War3Map;
using namespace War3Map::TerrainCornerRampFixtures;

template<class T> bool HasCode(const T& value, std::string_view code) {
    return std::any_of(value.diagnostics.begin(), value.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}

Bytes Json(std::string_view text) { return TerrainCornerRampFixtures::Text(text); }

Bytes Float32(std::initializer_list<float> values) {
    Bytes wire;
    for (const auto value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned b = 0; b < 4; ++b) wire.push_back(std::byte((bits >> (8 * b)) & 255));
    }
    return wire;
}

// 与产品摘要实现分开写的载荷wire：标签、算法、悬崖层、宽高（小端）和逐顶点地面值。
std::string LiteralPayloadSha(const TerrainHeightfieldIntent& intent) {
    Bytes wire;
    for (const auto c : std::string_view("Audastra.TerrainHeightfield.v1")) wire.push_back(std::byte(c));
    const auto put = [&](std::uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) wire.push_back(std::byte((value >> (8 * i)) & 255));
    };
    put(intent.algorithm, 1); put(intent.cliffLevel, 1); put(intent.width, 4); put(intent.height, 4);
    for (const auto raw : intent.groundRaw) put(raw, 2);
    return Take(Sha256(wire));
}

void ManifestAndSamples() {
    const auto good = Take(ParseTerrainHeightfieldManifest(Json(
        R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[0,-128.0],"fillHeight":50})")));
    Require(good.columns == 4 && good.rows == 3 && good.originWorld == std::array<std::int32_t, 2>{0, -128}
        && good.fillHeight == 50.0f && good.dataPath == "heightfield.f32" && good.heights.empty(),
        "清单读出尺寸、原点、填充值，数据路径默认同目录 heightfield.f32");
    const std::pair<const char*, const char*> rejected[] = {
        {R"({"format":"other","cellSize":128,"columns":4,"rows":3,"originWorld":[0,0],"fillHeight":0})", "TERRAIN_HEIGHTFIELD_FORMAT"},
        {R"({"format":"sonne-heightfield/v1","cellSize":64,"columns":4,"rows":3,"originWorld":[0,0],"fillHeight":0})", "TERRAIN_HEIGHTFIELD_CELL_SIZE"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":1,"rows":3,"originWorld":[0,0],"fillHeight":0})", "TERRAIN_HEIGHTFIELD_SIZE"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[100,0],"fillHeight":0})", "TERRAIN_HEIGHTFIELD_ORIGIN"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[0,0],"fillHeight":0,"extra":1})", "TERRAIN_HEIGHTFIELD_MANIFEST"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[0,0],"fillHeight":0,"data":"../x.f32"})", "TERRAIN_HEIGHTFIELD_DATA_PATH"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[0,0],"fillHeight":0,"data":"C:/x.f32"})", "TERRAIN_HEIGHTFIELD_DATA_PATH"},
        {R"({"format":"sonne-heightfield/v1","cellSize":128,"columns":4,"rows":3,"originWorld":[0,0]})", "TERRAIN_HEIGHTFIELD_MANIFEST"},
    };
    for (const auto& [text, code] : rejected)
        Require(HasCode(ParseTerrainHeightfieldManifest(Json(text)), code), "非法清单逐项给出具体诊断码");
    auto source = good;
    Require(HasCode(LoadTerrainHeightfieldSamples(source, Float32({1, 2, 3})), "TERRAIN_HEIGHTFIELD_DATA_SIZE")
        && source.heights.empty(), "数据长度与 columns×rows 不符时整份拒绝");
    Require(HasCode(LoadTerrainHeightfieldSamples(source, Float32({1, 2, 3, 4, 5, std::numeric_limits<float>::quiet_NaN(),
        7, 8, 9, 10, 11, 12})), "TERRAIN_HEIGHTFIELD_DATA_VALUE") && source.heights.empty(), "NaN 整份拒绝");
    Require(LoadTerrainHeightfieldSamples(source, Float32({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12})).Succeeded()
        && source.heights.size() == 12 && source.heights[5] == 6.0f, "按小端 float32 行序装入");
}

void ConversionReportAndDigest() {
    TerrainHeader header; header.width = 5; header.height = 4; header.offset = {-256.0f, -128.0f};
    TerrainHeightfieldSource source; source.columns = 4; source.rows = 3; source.originWorld = {0, 0}; source.fillHeight = 50;
    source.heights.assign(12, 100.0f);
    source.heights[0] = 3000.0f; source.heights[1] = -2000.0f; source.heights[2] = 1000.0f;
    const auto converted = Take(ConvertTerrainHeightfield(header, source, {5, 24.0f}));
    const auto& intent = converted.intent; const auto& report = converted.report;
    // 原点相对地图西南角偏移 (2,1) 格；第 3 列落在地图外。L=5 时底数 384，原始值 = round((h-24-384)*4)+8192。
    const auto at = [&](unsigned x, unsigned y) { return intent.groundRaw[y * 5 + x]; };
    Require(intent.width == 5 && intent.height == 4 && intent.cliffLevel == 5 && intent.groundRaw.size() == 20
        && at(2, 1) == 16383 && at(3, 1) == 0 && at(4, 1) == 10560 && at(2, 2) == 6960 && at(4, 3) == 6960
        && at(0, 0) == 6760 && at(1, 3) == 6760 && at(4, 0) == 6760, "逐顶点换算、截断与填充值按独立公式");
    Require(report.importedVertices == 9 && report.filledVertices == 11 && report.outsideVertices == 3
        && report.clampedHigh == 1 && report.clampedLow == 1
        && report.clampedBounds == std::array<std::uint32_t, 4>{2, 1, 3, 1}
        && report.coveredBounds == std::array<std::uint32_t, 4>{2, 1, 4, 3}
        && report.storableMinimum == -1664.0f && report.storableMaximum == 2431.75f
        && report.minimumHeight == -2024.0f && report.maximumHeight == 2976.0f, "报告覆盖、填充、越界与截断计数");
    Require(report.clampedSamples.size() == 2 && report.clampedSamples[0].worldX == 0.0f && report.clampedSamples[0].worldY == 0.0f
        && report.clampedSamples[0].height == 2976.0f && report.clampedSamples[1].worldX == 128.0f
        && report.clampedSamples[1].height == -2024.0f, "截断样本给出世界坐标与减去下沉后的原值");
    Require(intent.payloadSha256 == LiteralPayloadSha(intent) && ValidateTerrainHeightfieldIntent(intent).Succeeded(),
        "载荷摘要与独立wire一致并通过结构检查");
    auto tampered = intent; tampered.groundRaw[0] ^= 1;
    Require(HasCode(ValidateTerrainHeightfieldIntent(tampered), "TERRAIN_HEIGHTFIELD_INTENT_DIGEST"), "改动载荷后摘要不再成立");
    tampered = intent; tampered.groundRaw[0] = 0x4000;
    Require(HasCode(ValidateTerrainHeightfieldIntent(tampered), "TERRAIN_HEIGHTFIELD_INTENT"), "超出 0..16383 的地面值被拒绝");
    auto misaligned = header; misaligned.offset = {-200.0f, -128.0f};
    Require(HasCode(ConvertTerrainHeightfield(misaligned, source, {5, 24.0f}), "TERRAIN_HEIGHTFIELD_ALIGNMENT"),
        "原点不在地图网格上时拒绝，不做重采样");
    Require(HasCode(ConvertTerrainHeightfield(header, source, {15, 0.0f}), "TERRAIN_HEIGHTFIELD_CLIFF_LEVEL"), "悬崖层超过 14 被拒绝");
}

float HeightAt(unsigned x, unsigned y) { return 32.0f * float(x) - 16.0f * float(y) + 7.25f; }

TerrainHeightfieldSource WholeMapSource() {
    TerrainHeightfieldSource source; source.columns = 33; source.rows = 33; source.fillHeight = -9999;
    source.heights.resize(33 * 33);
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) source.heights[y * 33 + x] = HeightAt(x, y);
    return source;
}

// 独立预期：整图统一悬崖层2、地面值 = h*4+8192（L=2、无下沉时底数为 0），清坡道位，其余原字段不动。
Bytes ExpectedTerrain(const Fixture& f) {
    auto expected = f.grid;
    for (unsigned y = 0; y < 33; ++y) for (unsigned x = 0; x < 33; ++x) {
        auto& vertex = expected.vertices[y * 33 + x];
        vertex.groundHeightRaw = std::uint16_t(std::lround(HeightAt(x, y) * 4.0f) + 8192);
        vertex.cliffAndLayerRaw = std::uint8_t((vertex.cliffAndLayerRaw & 0xf0) | 2);
        vertex.textureAndFlagsRaw = std::uint8_t(vertex.textureAndFlagsRaw & ~0x10u);
    }
    RefreshTerrainDerived(expected, 0, 0, 33, 33);
    return Take(SerializeMapTerrain(expected));
}

std::unique_ptr<MapDataEditSession> RawDraft(const Fixture& f) {
    auto raw = std::make_unique<MapDataEditSession>(MapDocument::FromArchive(*f.source));
    const auto originalTerrain = Take(f.source->ReadMember("war3map.w3e"));
    const auto originalPathing = Take(f.source->ReadMember("war3map.wpm"));
    Require(raw->AttachTerrain(originalTerrain).Succeeded() && raw->AttachPathing(originalPathing).Succeeded(), "冷草稿只附真实来源");
    const auto replacements = f.edits->Replacements();
    const auto changed = std::find_if(replacements.begin(), replacements.end(), [](const auto& item) { return item.name == "war3map.wpm"; });
    const auto& pathing = changed == replacements.end() ? originalPathing : changed->data;
    const auto terrain = Take(SerializeMapTerrain(*f.edits->Terrain()));
    const auto layers = Take(f.edits->SerializePathingRestrictionDraft());
    Require((layers.empty() ? raw->SetTerrainAndPathing(terrain, pathing)
        : raw->SetTerrainAndPathingDraft(terrain, pathing, layers)).Succeeded(), "冷草稿回读实际W3E/WPM和P声明");
    const auto snapshot = f.edits->Snapshot();
    Require(raw->SetSerialized(MapDataKind::Units, Take(snapshot.Units()->Serialize())).Succeeded()
        && raw->SetSerialized(MapDataKind::Doodads, Take(snapshot.Doodads()->Serialize())).Succeeded()
        && raw->MatchesTerrainJournalDraft(*f.edits), "冷草稿完整回读摆放原字，不借热会话typed历史");
    return raw;
}

void SessionJournalReplayRestoreAndSave() {
    Fixture f; const auto originalPath = f.source->SourcePath(); const auto originalSha = f.source->SourceSha256();
    const auto expected = ExpectedTerrain(f);
    const auto intent = Take(ConvertTerrainHeightfield(f.edits->Terrain()->header, WholeMapSource(), {2, 0.0f})).intent;
    const auto resources = f.Rules(); const auto beforeRevision = f.edits->Revision();
    const auto direct = Take(PrepareTerrainHeightfieldStroke(f.grid, *resources.WaterBrushRules(), intent));
    Require(Take(SerializeMapTerrain(direct.terrain)) == expected && direct.result.changedVertices == 33 * 33
        && direct.result.requiresPathingRebuild, "纯候选与独立逐顶点预期逐字节相同");
    const auto plan = Take(PrepareTerrainHeightfieldEdit(*f.edits, f.Document(), f.Binding(), resources, intent, false));
    Require(plan.Plan().HeightfieldIntent() && *plan.Plan().HeightfieldIntent() == intent && !plan.Plan().BlockPasteIntent()
        && f.edits->Revision() == beforeRevision, "服务准备密封同一载荷，准备阶段不改会话");
    Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan, [](const auto&) {
        return Status::Failure("test", "HEIGHTFIELD_PREPUBLISH_REFUSED", "reject");
    }).Succeeded() && f.edits->Revision() == beforeRevision, "提交回调拒绝时不安装任何前缀");
    Take(ApplyTerrainEdit(*f.edits, f.Document(), plan));
    const auto snapshot = f.edits->Snapshot();
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == expected
        && Take(snapshot.Units()->Serialize()) == Take(f.units.Serialize())
        && Take(snapshot.Doodads()->Serialize()) == Take(f.doodads.Serialize()), "提交后地形符合预期，不跟随时摆放原字不变");
    Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan).Succeeded(), "同一候选不能再次提交");

    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    Require(journal.strokes.size() == 1 && journal.strokes[0].intent.heightfield == intent
        && journal.strokes[0].rulesSha256 == Take(resources.WaterBrushDependencyFingerprint()), "工程历史记下完整typed载荷和实际规则摘要");
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire.size() > 4 && wire[3] == std::byte{'N'} && decoded.strokes.size() == 1
        && decoded.strokes[0].intent.heightfield == intent && Take(EncodeTerrainStrokeJournal(decoded)) == wire,
        "高度场使历史升到 FTJN 并逐字节往返");
    auto corrupt = wire; corrupt[3] = std::byte{'M'};
    Require(!DecodeTerrainStrokeJournal(corrupt).Succeeded(), "旧版本号不能读出高度场记录");

    const std::array inputs{journal.strokes[0].intent};
    const auto replayed = Take(TerrainPathingSave::ReplayStrokes(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, inputs, f.game));
    Require(replayed.receipts.size() == 1 && replayed.session->MatchesTerrainJournalDraft(*f.edits),
        "公共typed重放（命令行保存使用）得到同一完整草稿");
    const auto raw = RawDraft(f);
    const auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, decoded, *raw, f.game));
    Require(restored.receipts.size() == 1 && restored.session->MatchesTerrainJournalDraft(*f.edits),
        "工程冷恢复从原来源重放出同一草稿");
    auto drifted = decoded; drifted.strokes[0].rulesSha256.assign(64, 'f');
    Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
        f.members[0].data, f.members[1].data, drifted, *raw, f.game).Succeeded(), "规则摘要漂移时拒绝恢复");

    Require(f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == f.members[0].data, "一次撤销恢复原 W3E");
    Require(f.edits->Redo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == expected, "重做恢复导入结果");

    const auto native = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
    Require(native.SaveGate().Succeeded(), "统一悬崖层后的地形通过完整保存门");
    MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "冻结实际来源");
    task.target = f.root / "heightfield.w3x";
    task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
    Require(native.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "安装按新地形重建的通行");
    task.beforePublish = [&] { return native.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
    MapSaveService::Run(task);
    Require(task.executed && task.succeeded && task.nextSession, "另存为写出并重开");
    auto readback = Take(MapArchiveSession::Open(task.written));
    Require(native.VerifyReadback(readback).Succeeded() && Take(readback.ReadMember("war3map.w3e")) == expected
        && Take(readback.ReadMember("war3map.wpm")) != f.members[1].data, "回读 W3E 与预期一致，通行按新地形重建");
    for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm")
        Require(Take(readback.ReadMember(member.name)) == member.data, "无关成员逐字保持");
    Require(Take(Sha256File(originalPath)) == originalSha, "另存为不改原图");
    Require(native.CommitSaved(*f.edits, MapDocument::FromArchive(readback)).Succeeded() && !f.edits->IsDirty()
        && f.edits->CanUndo(), "保存后重基线保留导入的撤销历史");
}

void RejectedInputsLeaveSessionUntouched() {
    Fixture f; const auto resources = f.Rules(); const auto before = f.edits->Revision();
    TerrainHeader small; small.width = 5; small.height = 4;
    TerrainHeightfieldSource source; source.columns = 2; source.rows = 2; source.heights = {0, 0, 0, 0};
    const auto wrongSize = Take(ConvertTerrainHeightfield(small, source, {2, 0.0f})).intent;
    Require(HasCode(PrepareTerrainHeightfieldEdit(*f.edits, f.Document(), f.Binding(), resources, wrongSize), "TERRAIN_HEIGHTFIELD_LAYOUT"),
        "载荷网格与当前地图尺寸不同则拒绝");
    const auto maximum = resources.WaterBrushRules()->maximumCliffLevel;
    if (maximum < 14) {
        const auto tooHigh = Take(ConvertTerrainHeightfield(f.edits->Terrain()->header, WholeMapSource(),
            {static_cast<std::uint8_t>(maximum + 1), 0.0f})).intent;
        Require(HasCode(PrepareTerrainHeightfieldEdit(*f.edits, f.Document(), f.Binding(), resources, tooHigh),
            "TERRAIN_HEIGHTFIELD_CLIFF_LEVEL"), "悬崖层超过实际规则上限则拒绝");
    }
    const auto intent = Take(ConvertTerrainHeightfield(f.edits->Terrain()->header, WholeMapSource(), {2, 0.0f})).intent;
    std::vector<TerrainWaterSpecialDoodadShape> shapes(1);
    shapes[0].rawcode = "Spc0"; shapes[0].x0 = 3; shapes[0].y0 = 3; shapes[0].x1 = 4; shapes[0].y1 = 4;
    shapes[0].relativeCliffLevels = {0, 1, 0, 0};
    Require(HasCode(PrepareTerrainHeightfieldStroke(f.grid, *resources.WaterBrushRules(), intent, shapes),
        "TERRAIN_HEIGHTFIELD_SPECIAL"), "带高差的特殊地形模型会失配，整笔拒绝");
    auto ownerOff = f.Rules(false);
    Require(HasCode(PrepareTerrainHeightfieldEdit(*f.edits, f.Document(), f.Binding(), ownerOff, intent),
        "TERRAIN_HEIGHTFIELD_RULE_OWNER"), "必须使用 enforceWaterLimits=true 的实际规则");
    Require(f.edits->Revision() == before && !f.edits->CanUndo(), "所有拒绝都不改会话");
}
} // namespace

int main() {
    try {
        ManifestAndSamples(); ConversionReportAndDigest();
        SessionJournalReplayRestoreAndSave(); RejectedInputsLeaveSessionUntouched();
        std::cout << "TerrainHeightfieldServiceTests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
