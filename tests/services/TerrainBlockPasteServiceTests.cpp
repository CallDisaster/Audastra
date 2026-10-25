#include "../TerrainCornerRampFixtures.h"
#include "War3MapCore/MapData/TerrainBlockEditing.h"

#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::TerrainCornerRampFixtures;

struct BlockFixture : Fixture {
    static constexpr TerrainCellRect sourceRect{4, 4, 8, 8};
    BlockFixture() {
        grid.header.groundTilesets = {"Tg01", "Tg02"};
        for (auto& vertex : grid.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 7936;
            vertex.textureAndFlagsRaw = 0; vertex.variationRaw = 3; vertex.cliffAndLayerRaw = 2;
        }
        auto& hill = grid.vertices[6 * 33 + 6]; hill.groundHeightRaw = 8320;
        hill.cliffAndLayerRaw = 3; hill.textureAndFlagsRaw = 0x20;
        auto& wet = grid.vertices[5 * 33 + 5]; wet.textureAndFlagsRaw = 0x40; wet.waterHeightAndFlagsRaw = 8448;
        auto& material = grid.vertices[5 * 33 + 7]; material.textureAndFlagsRaw = 1; material.variationRaw = 17;
        grid.vertices[7 * 33 + 4].variationRaw = 2 | (2 << 5);
        RefreshTerrainDerived(grid, 0, 0, 33, 33);
        for (std::size_t i = 0; i < units.units.size(); ++i) {
            const float xy = i == 0 ? 2816.0f : 0.0f;
            units.units[i].position[0] = units.units[i].position[1] = xy;
            doodads.doodads[i].position[0] = doodads.doodads[i].position[1] = xy;
        }
        members[0].data = TerrainLiteral(false); members[1].data = Pathing(false);
        members[3].data = Take(units.Serialize()); members[4].data = Take(doodads.Serialize());
        for (auto& member : members) {
            if (member.name == "TerrainArt/Terrain.slk")
                member.data = Slk({"tileID", "walkable", "flyable", "buildable"},
                    {{"Tg01", "1", "1", "1"}, {"Tg02", "0", "1", "0"}});
            if (member.name == "UI/MiscData.txt")
                member.data = Text("[Terrain]\nMaxSlope=90\nMinHeight=-256\nMaxHeight=1536\n[Water]\nDeepLevelPathing=64\n"
                    "[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n");
        }
        source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root / "block-source.w3x", members)).path)));
        edits = FreshSession();
    }
    Bytes TerrainLiteral(bool pasted) const {
        auto expected = grid;
        if (pasted) for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x)
            expected.vertices[(20 + y) * 33 + 20 + x] = grid.vertices[(4 + y) * 33 + 4 + x];
        return Take(SerializeMapTerrain(expected));
    }
    static Bytes Pathing(bool pasted) {
        auto bytes = PathingLiteral(false);
        // 独立完整字面：每座单顶点一级土丘只阻断周围四格，深水只覆盖最近顶点16像素。
        // 作者11位保留，荒芜与不可走/不可建材质各由明确坐标给定；不调用产品投影。
        for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
            const unsigned cx = x / 4, cy = y / 4, nx = (x + 2) / 4, ny = (y + 2) / 4;
            const bool cliff = (cx >= 5 && cx <= 6 && cy >= 5 && cy <= 6)
                || (pasted && cx >= 21 && cx <= 22 && cy >= 21 && cy <= 22);
            const bool deep = (nx == 5 && ny == 5) || (pasted && nx == 21 && ny == 21);
            const bool blight = (nx == 6 && ny == 6) || (pasted && nx == 22 && ny == 22);
            const bool material = (nx == 7 && ny == 5) || (pasted && nx == 23 && ny == 21);
            unsigned value = cliff ? 0xca : 0x40;
            if (material) value |= 0x0a;
            if (deep) value = 0x0a;
            if (blight) value |= 0x20;
            bytes[16 + y * 128 + x] = std::byte(value | 0x11);
        }
        return bytes;
    }
    TerrainBlockPasteIntent Intent(std::array<std::uint32_t, 2> destination = {20, 20}) const {
        return {1, sourceRect, destination,
            Take(Services::CaptureTerrainBlockFingerprint(*edits, Document(), Binding(), sourceRect))};
    }
    Bytes UnitsLiteral(bool followed) const {
        auto expected = units; if (followed) expected.units[0].position[2] += 160;
        return Take(expected.Serialize());
    }
    Bytes DoodadsLiteral(bool followed) const {
        auto expected = doodads; if (followed) expected.doodads[0].position[2] += 160;
        return Take(expected.Serialize());
    }
    void Check(bool pasted, bool followed) const {
        const auto snapshot = edits->Snapshot();
        Require(Take(SerializeMapTerrain(*edits->Terrain())) == TerrainLiteral(pasted)
            && Take(snapshot.Units()->Serialize()) == UnitsLiteral(pasted && followed)
            && Take(snapshot.Doodads()->Serialize()) == DoodadsLiteral(pasted && followed),
            "整块绝对原字段与既有摆放完整载荷匹配独立预期");
    }
};

std::unique_ptr<MapDataEditSession> RawBlockDraft(const BlockFixture& f) {
    auto raw = std::make_unique<MapDataEditSession>(MapDocument::FromArchive(*f.source));
    const auto originalTerrain = Take(f.source->ReadMember("war3map.w3e"));
    const auto originalPathing = Take(f.source->ReadMember("war3map.wpm"));
    Require(raw->AttachTerrain(originalTerrain).Succeeded() && raw->AttachPathing(originalPathing).Succeeded(),
        "冷草稿只附真实当前来源");
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
        && raw->MatchesTerrainJournalDraft(*f.edits), "冷草稿完整回读摆放原字，但不借热会话typed历史");
    return raw;
}

void ColdRecoveryAndFormalSaveKeepExactBlockAndTargetObjects() {
    for (const bool inPlace : {false, true}) for (const bool follow : {false, true}) {
        BlockFixture f; const auto originalPath = f.source->SourcePath(); const auto originalSha = f.source->SourceSha256();
        const auto intent = f.Intent(); const auto resources = f.Rules(); const auto beforeRevision = f.edits->Revision();
        const auto frozen = Take(f.edits->FreezeForSave(kBudget));
        const auto plan = Take(PrepareTerrainBlockPasteEdit(*frozen, f.Document(), f.Binding(), resources, intent, follow));
        Require(plan.Plan().BlockPasteIntent() && plan.Plan().WaterPreparationWork() > 0
            && plan.Plan().WaterPreparationBytes() > 0, "块候选密封意图并报告真实准备预算");
        f.Check(false, follow);
        Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan, [](const auto&) {
            return Status::Failure("test", "BLOCK_PREPUBLISH_REFUSED", "reject block");
        }).Succeeded() && f.edits->Revision() == beforeRevision, "提交回调拒绝不安装块前缀");
        f.Check(false, follow); Take(ApplyTerrainEdit(*f.edits, f.Document(), plan)); f.Check(true, follow);
        Require(!ApplyTerrainEdit(*f.edits, f.Document(), plan).Succeeded(), "密封块候选不能复用旧修订");
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
        const auto wire = Take(EncodeTerrainStrokeJournal(journal));
        Require(wire[3] == std::byte{'D'} && journal.strokes.size() == 1 && journal.strokes[0].intent.blockPaste
            && journal.strokes[0].rulesSha256 == Take(resources.WaterBrushDependencyFingerprint()),
            "真实块事务生成FTJD与实际规则摘要，不伪造普通笔刷历史");
        const std::array publicInputs{journal.strokes[0].intent};
        const auto replayed = Take(TerrainPathingSave::ReplayStrokes(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data, publicInputs, f.game));
        Require(replayed.receipts.size() == 1 && replayed.waterOwners.size() == 1
            && replayed.session->MatchesTerrainJournalDraft(*f.edits),
            "公共typed重放与实际服务块落笔产生同一完整草稿，不依赖私有journal输出授予资格");
        const auto raw = RawBlockDraft(f);
        const auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data, Take(DecodeTerrainStrokeJournal(wire)), *raw, f.game));
        Require(restored.receipts.size() == 1 && restored.waterOwners.size() == 1
            && restored.session->MatchesTerrainJournalDraft(*f.edits), "冷恢复从原来源重新读取源块并产生相同完整草稿");
        Require(restored.session->Replay(restored.receipts.front(), false).Succeeded()
            && Take(SerializeMapTerrain(*restored.session->Terrain())) == f.TerrainLiteral(false)
            && restored.session->Replay(restored.receipts.front(), true).Succeeded()
            && Take(SerializeMapTerrain(*restored.session->Terrain())) == f.TerrainLiteral(true), "恢复receipt保留整块单步撤销重做");
        for (const bool rulesDrift : {false, true}) {
            auto invalid = journal;
            if (rulesDrift) invalid.strokes[0].rulesSha256.assign(64, 'f');
            else invalid.strokes[0].intent.blockPaste->expectedBlockSha256.assign(64, 'f');
            Require(!TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
                f.members[0].data, f.members[1].data, invalid, *raw, f.game).Succeeded(),
                "源块或实际规则漂移不能用记录中的摘要授予恢复资格");
        }
        const auto save = [&](bool pasted, const char* name) {
            f.Check(pasted, follow); const auto native = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
            Require(native.SaveGate().Succeeded(), "块正式保存走完整原版G0/G1门");
            MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "冻结实际归档身份");
            task.inPlace = inPlace; task.target = inPlace ? f.source->SourcePath() : f.root / name;
            task.inPlaceOptions.backupDirectory = f.root / "block-backups"; task.inPlaceOptions.keepBackups = 0;
            task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
            Require(native.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "安装目标实际通行派生，不复制源WPM像素");
            task.beforePublish = [&] { return native.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
            const auto previousSha = f.source->SourceSha256(); MapSaveService::Run(task);
            Require(task.executed && task.succeeded && task.nextSession, "正式发布后重开实际地图归档");
            if (inPlace) Require(!task.backup.empty() && Take(Sha256File(task.backup)) == previousSha, "原地保存备份是完整前版");
            auto readback = Take(MapArchiveSession::Open(task.written));
            Require(native.VerifyReadback(readback).Succeeded()
                && Take(readback.ReadMember("war3map.w3e")) == f.TerrainLiteral(pasted)
                && Take(readback.ReadMember("war3map.wpm")) == BlockFixture::Pathing(pasted)
                && Take(readback.ReadMember("war3mapUnits.doo")) == f.UnitsLiteral(pasted && follow)
                && Take(readback.ReadMember("war3map.doo")) == f.DoodadsLiteral(pasted && follow),
                "完整W3E/WPM/Units/DOO回读匹配独立字段和像素预期");
            for (const auto& member : f.members) if (member.name != "war3map.w3e" && member.name != "war3map.wpm"
                && member.name != "war3mapUnits.doo" && member.name != "war3map.doo")
                Require(Take(readback.ReadMember(member.name)) == member.data, "脚本及所有无关作者成员逐字保持");
            Require(native.CommitSaved(*f.edits, MapDocument::FromArchive(readback)).Succeeded() && !f.edits->IsDirty(),
                "块保存后重基线仍保留同一撤销历史");
            f.source = std::make_unique<MapArchiveSession>(std::move(readback));
        };
        save(true, "block-pasted.w3x");
        Require(f.edits->Undo(), "保存后一次Undo恢复整块"); save(false, "block-undone.w3x");
        Require(f.edits->Redo(), "保存后Redo重做整块"); save(true, "block-redone.w3x");
        if (!inPlace) Require(Take(Sha256File(originalPath)) == originalSha, "所有另存为和恢复保持原归档只读");
    }
}

void UnchangedSourceRepeatsAndChangedSourceRejects() {
    BlockFixture f; const auto intent = f.Intent(); const auto before = f.TerrainLiteral(false);
    auto noOp = intent; noOp.destinationVertex = {4, 4};
    const auto unchanged = Take(PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), noOp));
    const auto revision = f.edits->Revision(); Take(ApplyTerrainEdit(*f.edits, f.Document(), unchanged));
    Require(!unchanged.Plan().HasChanges() && f.edits->Revision() == revision && !f.edits->CanUndo(), "同块同位置不制造空历史");
    const auto staleBinding = f.Binding();
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), intent))));
    auto another = intent; another.destinationVertex = {12, 20};
    Require(!PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), staleBinding, f.Rules(), another).Succeeded(), "粘贴必须使用当前CAS");
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), another))));
    Require(f.edits->Undo() && f.edits->Undo() && Take(SerializeMapTerrain(*f.edits->Terrain())) == before
        && !f.edits->IsDirty() && Take(f.edits->ExportTerrainStrokeJournal()).strokes.empty(),
        "源未变可跨修订重复粘贴，两次撤销回净零且不留下伪历史前缀");
    auto overlap = intent; overlap.destinationVertex = {5, 4};
    auto expected = f.grid;
    for (unsigned y = 0; y <= 4; ++y) for (unsigned x = 0; x <= 4; ++x)
        expected.vertices[(4 + y) * 33 + 5 + x] = f.grid.vertices[(4 + y) * 33 + 4 + x];
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), overlap))));
    Require(Take(SerializeMapTerrain(*f.edits->Terrain())) == Take(SerializeMapTerrain(expected)), "重叠块先冻读完整before再写，不边写边采样");
    const auto overlapBytes = Take(SerializeMapTerrain(*f.edits->Terrain())); const auto afterOverlap = f.edits->Revision();
    Require(!PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), intent).Succeeded()
        && f.edits->Revision() == afterOverlap && Take(SerializeMapTerrain(*f.edits->Terrain())) == overlapBytes,
        "重叠改变源块后必须重新复制，拒绝不安装任何前缀");
    auto wrongDocument = f.Document(); ++wrongDocument.generation;
    Require(!Services::CaptureTerrainBlockFingerprint(*f.edits, wrongDocument, f.Binding(), BlockFixture::sourceRect).Succeeded(),
        "复制查询同样拒绝换代文档身份");
}

void SavedAuthorRestrictionsRemainAtTheirCoordinates() {
    BlockFixture f;
    TerrainBrush restriction; restriction.kind = TerrainBrushKind::PathingRestriction;
    restriction.size = 1; restriction.pathingRestrictions = 0x04;
    // 原块内(18,16)和目标内(90,92)各一像素不可飞；对应的粘贴像素(82,80)没有作者限制。
    const std::array points{TerrainStrokePoint{18 * 32 + 16, 16 * 32 + 16},
        TerrainStrokePoint{90 * 32 + 16, 92 * 32 + 16}};
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainEdit(*f.edits,
        f.Document(), f.Binding(), restriction, points))));
    const auto expectedPathing = [](bool pasted) {
        auto bytes = BlockFixture::Pathing(pasted);
        bytes[16 + 16 * 128 + 18] |= std::byte{0x04};
        bytes[16 + 92 * 128 + 90] |= std::byte{0x04};
        return bytes;
    };
    const auto save = [&](const char* filename, bool pasted) {
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
        Require(plan.SaveGate().Succeeded(), "作者P通过真实事务保存，不把未知WPM附加位冒充已知来源");
        MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "冻结P限制来源");
        task.target = f.root / filename; task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
        Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "实际G1合成保留既有P");
        task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
        MapSaveService::Run(task); Require(task.executed && task.succeeded && task.nextSession, "正式保存带作者P的完整地图");
        auto readback = Take(MapArchiveSession::Open(task.written));
        const auto pathing = Take(readback.ReadMember("war3map.wpm"));
        Require(plan.VerifyReadback(readback).Succeeded() && pathing == expectedPathing(pasted)
            && Take(readback.ReadMember("war3map.w3e")) == f.TerrainLiteral(pasted)
            && (pathing[16 + 80 * 128 + 82] & std::byte{0x04}) == std::byte{0},
            "逐像素完整WPM证明源P未复制，目标P未清除，无关位仍逐字保留");
        Require(plan.CommitSaved(*f.edits, MapDocument::FromArchive(readback)).Succeeded(), "P与真实地图同时重基线");
        f.source = std::make_unique<MapArchiveSession>(std::move(readback));
    };
    save("block-saved-author-p.w3x", false);
    const auto baselineTerrain = Take(f.source->ReadMember("war3map.w3e"));
    const auto baselinePathing = Take(f.source->ReadMember("war3map.wpm"));
    const auto baselineDocument = MapDocument::FromArchive(*f.source);
    const auto intent = f.Intent();
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainBlockPasteEdit(*f.edits,
        f.Document(), f.Binding(), f.Rules(), intent))));
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    Require(journal.strokes.size() == 1 && !journal.baselinePathingLayers.empty(),
        "保存过的P作为真实基线层绑定，块记录不携带源WPM payload");
    const auto raw = RawBlockDraft(f);
    auto recovered = Take(TerrainPathingSave::RestoreJournal(*f.source, baselineDocument,
        baselineTerrain, baselinePathing, Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal)))),
        *raw, f.game));
    Require(recovered.receipts.size() == 1 && recovered.session->MatchesTerrainJournalDraft(*f.edits),
        "冷恢复同时核对已保存P和源块，保留完整作者限制层");
    f.edits = std::move(recovered.session); save("block-with-author-p.w3x", true);
    Require(f.edits->Undo(), "保存后块撤销保留作者P基线"); save("block-author-p-undone.w3x", false);
    Require(f.edits->Redo(), "保存后块重做仍不复制作者P"); save("block-author-p-redone.w3x", true);
}

void MixedBlockWaterDoodadsAndUnitsRestoreTheirActualOrder() {
    BlockFixture f;
    struct State { Bytes terrain, units, doodads; };
    const auto capture = [&]() {
        const auto snapshot = f.edits->Snapshot();
        return State{Take(SerializeMapTerrain(*f.edits->Terrain())), Take(snapshot.Units()->Serialize()),
            Take(snapshot.Doodads()->Serialize())};
    };
    std::array<State, 5> states; states[0] = capture();
    const auto intent = f.Intent();
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainBlockPasteEdit(*f.edits,
        f.Document(), f.Binding(), f.Rules(), intent, true))));
    f.Check(true, true); states[1] = capture();
    TerrainWaterStrokeOptions water; water.shape = TerrainBrushShape::Square; water.randomSeed = 1;
    const TerrainStrokePoint waterPoint{12 * 128, 12 * 128};
    Take(ApplyTerrainEdit(*f.edits, f.Document(), Take(PrepareTerrainWaterEdit(*f.edits,
        f.Document(), f.Binding(), f.Rules(), water, std::span(&waterPoint, 1)))));
    states[2] = capture();
    Require(f.edits->Terrain()->vertices[12 * 33 + 12].HasWater() && states[2].terrain != states[1].terrain
        && states[2].units == states[1].units && states[2].doodads == states[1].doodads,
        "独立远区水刷形成第二条真实命令，保留块抬升的完整摆放字段");
    auto doodads = *f.edits->Snapshot().Doodads(); doodads.doodads[0].lifePercent = 67;
    Require(f.edits->EditDoodads(doodads).Succeeded(), "第三条普通DOO事务保留块随高与水刷结果");
    states[3] = capture();
    auto units = *f.edits->Snapshot().Units(); units.units[0].hitPoints = 41;
    units.units[0].inventory.push_back({1, "gold"});
    Require(f.edits->EditUnits(units).Succeeded(), "第四条Units事务保留既有位置与其余载荷");
    states[4] = capture();
    const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
    const auto wire = Take(EncodeTerrainStrokeJournal(journal));
    const auto decoded = Take(DecodeTerrainStrokeJournal(wire));
    Require(wire[3] == std::byte{'D'} && decoded.strokes.size() == 4 && decoded.strokes[0].intent.blockPaste
        && decoded.strokes[1].intent.waterBrush && decoded.strokes[2].ordinaryDoodads && decoded.strokes[3].units,
        "FTJD准确保留block→water→普通DOO→Units四种记录与原始顺序");
    const auto raw = RawBlockDraft(f);
    const auto restore = [&](const TerrainStrokeJournal& value) {
        return TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data, value, *raw, f.game);
    };
    auto recovered = Take(restore(decoded));
    Require(recovered.receipts.size() == 4 && recovered.waterOwners.size() == 2
        && recovered.session->MatchesTerrainJournalDraft(*f.edits), "混合恢复产生四个真实receipt和两个独立规则owner");
    const auto check = [&](std::size_t index) {
        const auto snapshot = recovered.session->Snapshot();
        Require(Take(SerializeMapTerrain(*recovered.session->Terrain())) == states[index].terrain
            && Take(snapshot.Units()->Serialize()) == states[index].units
            && Take(snapshot.Doodads()->Serialize()) == states[index].doodads,
            "混合恢复的每个撤销/重做前缀匹配实际原命令的完整W3E/Units/DOO");
    };
    for (std::size_t i = recovered.receipts.size(); i != 0; --i) {
        Require(recovered.session->Replay(recovered.receipts[i - 1], false).Succeeded(), "按原顺序反向撤销混合receipt");
        check(i - 1);
    }
    Require(!recovered.session->IsDirty(), "混合全部撤销回未修改原创源图");
    for (std::size_t i = 0; i < recovered.receipts.size(); ++i) {
        Require(recovered.session->Replay(recovered.receipts[i], true).Succeeded(), "按原顺序重做混合receipt"); check(i + 1);
    }
    Require(recovered.session->MatchesTerrainJournalDraft(*f.edits), "混合redo还原完整最终草稿和P层");
    auto changed = decoded; std::swap(changed.strokes[0], changed.strokes[3]);
    Require(!restore(changed).Succeeded(), "Units完整before不能越过块随高顺序依赖");
    changed = decoded; std::swap(changed.strokes[0], changed.strokes[2]);
    Require(!restore(changed).Succeeded(), "普通DOO完整before不能越过块随高顺序依赖");
}

void BudgetsAndPublicReplayVariantsStayClosed() {
    BlockFixture f; const auto intent = f.Intent(); const auto before = f.TerrainLiteral(false);
    for (const bool memory : {false, true}) {
        TerrainWaterBrushLimits limits; if (memory) limits.maximumCandidateBytes = 1; else limits.maximumWork = 1;
        Require(!PrepareTerrainBlockPasteEdit(*f.edits, f.Document(), f.Binding(), f.Rules(), intent, false, limits).Succeeded()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == before && !f.edits->CanUndo(), "块预算不能由owner补足或部分发布");
    }
    for (unsigned conflict = 0; conflict < 9; ++conflict) {
        TerrainStrokeIntent invalid; invalid.blockPaste = intent;
        if (conflict == 0) invalid.brush.seed = 1;
        if (conflict == 1) invalid.points = {{512, 512}};
        if (conflict == 2) invalid.waterBrush = TerrainWaterStrokeOptions{};
        if (conflict == 3) invalid.bounds = TerrainBoundsEditIntent{};
        if (conflict == 4) invalid.palette = TerrainPaletteEditIntent{};
        if (conflict == 5) invalid.cornerRampAlgorithm = TerrainCornerRampAlgorithm::WetTerrain;
        if (conflict == 6) invalid.straightRampAlgorithm = TerrainStraightRampAlgorithm::Connected;
        if (conflict == 7) invalid.blockPaste->algorithm = 2;
        if (conflict == 8) invalid.blockPaste->expectedBlockSha256.clear();
        const std::array inputs{invalid};
        Require(!TerrainPathingSave::ReplayStrokes(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data, inputs, f.game).Succeeded()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == before && !f.edits->CanUndo(),
            "公共replay预检和实际重放均不允许块混用其它variant或猜摘要");
    }
}
} // namespace

int main() {
    try {
        ColdRecoveryAndFormalSaveKeepExactBlockAndTargetObjects();
        UnchangedSourceRepeatsAndChangedSourceRejects(); SavedAuthorRestrictionsRemainAtTheirCoordinates();
        MixedBlockWaterDoodadsAndUnitsRestoreTheirActualOrder(); BudgetsAndPublicReplayVariantsStayClosed(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
