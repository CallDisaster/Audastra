// 宿主 TerrainSavePlanTests.cpp 已提供原创MPQ、目录和事务fixture。
// 双阶段真实物编/贴图回归位于Services与Qt；这里不构造私有validator资格。
void RunTerrainDualStageSourceIdentityCases() {
    TempDirectory directory;
    Fixture fixture;
    const auto path = directory.Path() / "source.w3x";
    const auto bytes = OpaqueArchiveBytes(OpaqueFiles(fixture, "war3map.w3e\nwar3map.wpm\n"));
    Require(IO::WriteFileBytes(path, bytes).Succeeded(), "write private dual-stage source identity fixture");
    const auto source = Take(MapArchiveSession::Open(path));
    fixture.Stroke(TerrainBrushKind::Raise);
    const auto revision = fixture.edits.Revision();
    const auto plan = Take(TerrainSavePlan::Prepare(source, fixture.edits));
    Require(plan.VerifySourceInputs(source).Succeeded(), "unchanged true source passes pre-publish identity check");
    TerrainSavePlanLimits expanded;
    expanded.maximumBytes += 1;
    Require(HasCode(plan.VerifySourceInputs(source, expanded).diagnostics, "TERRAIN_SAVE_PLAN_BUDGET"),
        "pre-publish source verification cannot expand the fixed budget");
    auto otherFiles = OpaqueFiles(fixture, "war3map.w3e\nwar3map.wpm\n");
    otherFiles.push_back({"unrelated.bin", {std::byte{1}}});
    const auto otherPath = directory.Path() / "other.w3x";
    Require(IO::WriteFileBytes(otherPath, OpaqueArchiveBytes(otherFiles)).Succeeded(), "write different actual archive");
    const auto other = Take(MapArchiveSession::Open(otherPath));
    Require(HasCode(plan.VerifySourceInputs(other).diagnostics, "TERRAIN_SAVE_PLAN_SOURCE_MISMATCH"),
        "same terrain bytes do not substitute a different full source archive");
    auto changed = bytes;
    changed[32] ^= std::byte{1};
    Require(IO::WriteFileBytes(path, changed).Succeeded(), "simulate source drift after successful preparation");
    Require(!plan.VerifySourceInputs(source).Succeeded(), "old archive object does not conceal actual file drift");
    Require(fixture.edits.Revision() == revision && fixture.edits.CanUndo(), "source refusal leaves revision and history untouched");

    const auto workspaceRoot = directory.Path() / "workspace";
    Require(std::filesystem::create_directories(workspaceRoot / "map"), "create private source workspace");
    Require(IO::WriteFileBytes(workspaceRoot / "map/war3map.w3e", fixture.w3e).Succeeded()
        && IO::WriteFileBytes(workspaceRoot / "map/war3map.wpm", fixture.wpm).Succeeded(), "write true workspace terrain members");
    const auto workspace = Take(DirectoryWorkspace::Open(workspaceRoot));
    const auto workspacePlan = Take(TerrainSavePlan::Prepare(workspace.Snapshot(), fixture.edits));
    Require(workspacePlan.VerifySourceInputs(workspace.Snapshot()).Succeeded(), "true unchanged workspace source is verified");
    auto wrongSnapshot = workspace.Snapshot(); wrongSnapshot.aggregateSha256.assign(64, 'a');
    Require(HasCode(workspacePlan.VerifySourceInputs(wrongSnapshot).diagnostics, "TERRAIN_SAVE_PLAN_SOURCE_MISMATCH"),
        "workspace preflight cannot borrow another aggregate identity");
    Require(IO::WriteFileBytes(workspaceRoot / "unexpected.bin", Bytes{std::byte{2}}).Succeeded(), "add unrelated source member");
    Require(HasCode(workspacePlan.VerifySourceInputs(workspace.Snapshot()).diagnostics, "TERRAIN_SAVE_PLAN_SOURCE_MISMATCH"),
        "frozen workspace snapshot cannot conceal an added file on disk");
    Require(HasCode(workspacePlan.VerifySourceInputs(workspace.Snapshot(), expanded).diagnostics, "TERRAIN_SAVE_PLAN_BUDGET"),
        "workspace source verification also refuses larger budgets");

    Fixture wet;
    wet.Stroke(TerrainBrushKind::Raise);
    auto terrain = *wet.edits.Terrain(); terrain.vertices[4 * 9 + 4].textureAndFlagsRaw |= 0x40;
    Require(wet.edits.SetTerrain(Take(SerializeMapTerrain(terrain))).Succeeded(), "prepare unsupported wet fixture without native rules");
    const auto rejected = Take(TerrainSavePlan::Prepare(other, wet.edits));
    Require(!rejected.SaveGate().Succeeded()
        && HasCode(rejected.VerifySourceInputs(other).diagnostics, "MAP_TERRAIN_PATHING_UNVERIFIED"),
        "source checks preserve a failed save gate rather than authorizing a plan");
}
