#pragma once

// 包含在water service原创夹具之后。完整旧/新WPM、DOO顺序、保存、Undo及工程恢复
// 由同一组既有独立预期再跑RLE输入；这里补共同路径像素owner的预算和失败原子性。
void SpecialRleCaptureKeepsBudgetsSourceIdentityAndFailureAtomicity() {
    Fixture plain(true, true), compressed(true, true, true);
    const auto plainRules = plain.Rules(), compressedRules = compressed.Rules();
    Require(compressedRules.CaptureBudgetBytes() - compressedRules.SourceBytes()
        == plainRules.CaptureBudgetBytes() - plainRules.SourceBytes() + 4 * 4 * 4,
        "four special instances sharing one RLE path charge its exact decoded BGRA storage once");
    const auto capture = [&](const Fixture& f, std::size_t budget) {
        const auto doodads = Take(f.edits->Snapshot(MapDataKind::Doodads, kResourceBudget));
        return TerrainPathingSave::CaptureWaterBrushRules(*f.source, f.grid, *f.edits->Terrain(),
            *doodads.Doodads(), f.game, true, budget);
    };
    Require(capture(compressed, compressedRules.CaptureBudgetBytes()).Succeeded(),
        "the exact aggregate special RLE capture budget is sufficient");
    const auto tight = capture(compressed, compressedRules.CaptureBudgetBytes() - 1);
    Require(!tight.Succeeded() && !tight.value, "one byte less cannot return a partially qualified RLE owner");

    const auto directory = Directory(compressed);
    const auto original = std::find_if(plain.members.begin(), plain.members.end(), [](const auto& member) {
        return member.name == "PathTextures/replay-special.tga";
    });
    Require(original != plain.members.end(), "the original type2 matrix is present");
    Require(IO::WriteFileBytes(directory.Snapshot().root / original->name, original->data).Succeeded(),
        "replace only the private directory RLE member by identical type2 pixels");
    const auto sourceDrift = compressedRules.VerifyReadback(directory.Snapshot(), kResourceBudget);
    Require(!sourceDrift.Succeeded() && HasCode(sourceDrift, "TERRAIN_PATHING_RESOURCE_CHANGED"),
        "equal decoded pixels cannot replace the captured compression and member source identity");

    const auto setFootprint = [&](Fixture& f, Bytes image, std::string_view caseName) {
        for (auto& member : f.members) if (member.name == "PathTextures/replay-special.tga") member.data = std::move(image);
        f.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(Take(WriteVerifiedArchive(
            f.root / (std::string(caseName) + ".w3x"), f.members)).path)));
    };
    const auto image = std::find_if(compressed.members.begin(), compressed.members.end(), [](const auto& member) {
        return member.name == "PathTextures/replay-special.tga";
    })->data;
    for (const auto kind : {0, 1, 2, 3}) {
        Fixture invalid(true, true, true); auto bytes = image;
        const char* code = nullptr;
        switch (kind) {
            case 0: bytes.pop_back(); code = "TERRAIN_UNIFORM_TGA_TRUNCATED"; break;
            case 1: bytes[18] = std::byte{0xff}; code = "TERRAIN_DOODAD_TGA_RLE_OVERRUN"; break;
            case 2: bytes.push_back(std::byte{0}); code = "TERRAIN_UNIFORM_TGA_TRAILING"; break;
            default:
                // 唯一重复包的BGRA alpha64不属于原版0/1/2层，不能因压缩而略过层级检查。
                bytes[22] = std::byte{64}; code = "TERRAIN_SPECIAL_ALPHA"; break;
        }
        setFootprint(invalid, std::move(bytes), "invalid-special-rle");
        const auto baseline = Take(Sha256File(invalid.source->SourcePath()));
        const auto denied = capture(invalid, kResourceBudget);
        Require(!denied.Succeeded() && !denied.value && HasCode(denied, code),
            "malformed RLE or invalid alpha returns its original typed refusal");
        Require(invalid.edits->Revision() == 0 && !invalid.edits->CanUndo()
            && Take(Sha256File(invalid.source->SourcePath())) == baseline,
            "failed special capture neither publishes history nor modifies the synthetic source archive");
    }
}
