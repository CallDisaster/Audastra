#pragma once

namespace War3Map::TerrainSaveTests {
namespace {
void PalettePaintAndCornerShareOneSave() {
    using namespace TerrainCornerRampFixtures;
    for (const bool append : {false, true}) {
        Fixture f;
        if (!append) f.grid.header.groundTilesets.push_back("Tg03");
        f.members[0].data = Take(SerializeMapTerrain(f.grid));
        for (auto& member : f.members) if (member.name == "TerrainArt/Terrain.slk")
            member.data = Slk({"tileID", "walkable", "flyable", "buildable"},
                {{"Tg01", "1", "1", "1"}, {"Tg02", "1", "1", "1"}, {"Tg03", "1", "1", "1"}});
        f.source = std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(f.root / "palette-source.w3x", f.members)).path)));
        f.edits = f.FreshSession();
        const auto sourceSha = f.source->SourceSha256();
        Take(TerrainPathingSave::EditTextureSlot(*f.source, *f.edits,
            TerrainTextureTable::Ground, 1, "Tg02", f.game));
        TerrainBrush paint; paint.kind = TerrainBrushKind::Texture;
        paint.radius = 0; paint.groundTexture = "Tg02"; paint.variation = 0;
        Take(f.edits->EditTerrain(paint, std::span(&f.point, 1), false));
        const auto rules = f.Rules();
        const auto command = Take(PrepareTerrainCornerRampEdit(*f.edits, f.Document(), f.Binding(),
            rules, CornerBrush(), std::span(&f.point, 1), false, {}, TerrainCornerRampAlgorithm::NativeWetCorners));
        Take(ApplyTerrainEdit(*f.edits, f.Document(), command));
        const auto expected = Take(SerializeMapTerrain(*f.edits->Terrain()));
        const auto journal = Take(TerrainPathingSave::CaptureJournal(*f.source, *f.edits));
        Require(journal.strokes.size() == 3, "palette, paint and corner retain three ordered identities");
        auto restored = Take(TerrainPathingSave::RestoreJournal(*f.source, MapDocument::FromMembers(f.members),
            f.members[0].data, f.members[1].data,
            Take(DecodeTerrainStrokeJournal(Take(EncodeTerrainStrokeJournal(journal)))), *f.edits, f.game));
        Require(restored.session->MatchesTerrainJournalDraft(*f.edits), "mixed palette ramp cold replay matches the whole draft");
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source, *f.edits, f.game));
        Require(plan.SaveGate().Succeeded(), "mixed palette paint and corner obtains native save proof");
        MapSaveTask task; Require(task.FreezeArchive(*f.source).Succeeded(), "freeze exact mixed source");
        task.target = f.root / "palette-corner.w3x";
        task.replacements = f.edits->Replacements(); task.removals = f.edits->Removals();
        Require(plan.InstallPathingOutput(*f.edits, task.replacements).Succeeded(), "install mixed native pathing");
        task.beforePublish = [&] { return plan.VerifyForSave(*f.edits, task.sourceSha256, task.replacements, task.removals); };
        MapSaveService::Run(task); Require(task.succeeded, "formal mixed save publishes atomically");
        const auto saved = Take(MapArchiveSession::Open(task.target));
        Require(Take(saved.ReadMember("war3map.w3e")) == expected, "mixed archive preserves exact edited terrain");
        Require(f.edits->Undo() && f.edits->Redo()
            && Take(SerializeMapTerrain(*f.edits->Terrain())) == expected, "ramp undo redo retains the preceding palette and paint");
        Require(Take(Sha256File(f.source->SourcePath())) == sourceSha, "mixed save does not modify original fixture");
    }
}
} // namespace
} // namespace War3Map::TerrainSaveTests
