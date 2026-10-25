// 复用同文件原创9x9地形夹具；独立预期不从生成器反推。
void PureAssetsRebuildStaticPathingWithoutTerrainChanges() {
    Fixture f;
    Require(!TerrainSavePlan::HasTerrainChanges(*f.edits), "fixture has no terrain or placement delta");
    auto output = f.Final();
    auto prepared = Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output));
    Require(prepared.has_value(), "actual occupied texture change prepares a plan despite unchanged W3E and author TGA");
    Require(prepared->RequiresNativeVerification(), "resource delta still requires native acceptance");
    auto saved = f.Save(*prepared, output, Fixture::ExpectedPathing(false, 8, true));
    Require(Take(saved.ReadMember("war3mapMap.tga")) == f.Member("war3mapMap.tga"), "author TGA remains byte exact");
    Check(prepared->CommitSaved(*f.edits, MapDocument::FromMembers(saved.Members(), saved.SourcePath())));
    Require(!TerrainSavePlan::HasTerrainChanges(*f.edits), "resource-only save commits the complete matching baseline");
}

void PureNativePathChangePreservesAuthorBlp() {
    Fixture f;
    std::erase_if(f.members, [](const auto& member) { return member.name == "war3mapMap.tga"; });
    f.members.push_back({"war3mapMap.blp", Text("owned author BLP must not be replaced by the pathing stage")});
    auto imports = Resources::ImportTable::CreateEmpty(); Check(imports.Upsert("war3mapMap.blp"));
    f.members.push_back({"war3map.imp", Take(imports.Serialize())}); f.Open();
    auto output = f.Final(true);
    auto prepared = Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output));
    Require(prepared.has_value(), "pure native pathTex override prepares a plan independently from author BLP protection");
    auto saved = f.Save(*prepared, output, Fixture::ExpectedPathing(false, 8, true));
    Require(Take(saved.ReadMember("war3mapMap.blp")) == f.Member("war3mapMap.blp"), "author BLP remains byte exact");
    Require(Take(saved.ReadMember("war3map.imp")) == f.Member("war3map.imp"), "author BLP import record stays byte exact");
}

void SaveUnchangedDependencies(Fixture& f, std::vector<MemberReplacement> output) {
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "unrelated actual final members do not acquire a native reconstruction plan");
    const auto oldSourceSha = Take(Sha256File(f.archive->SourcePath()));
    Services::MapSaveTask task; Check(task.FreezeArchive(*f.archive)); task.target = f.root / "unchanged-dependencies.w3x";
    task.replacements = output;
    task.beforePublish = [&] { return TerrainPathingSave::VerifyUnchangedFinalInputs(*f.archive,
        f.edits.get(), f.gameRoot, {}, task.replacements, task.removals); };
    Services::MapSaveService::Run(task);
    if (!task.succeeded) Check({task.diagnostics});
    Require(task.succeeded && task.nextSession, "unrelated member save reaches the formal archive writer and readback");
    auto saved = Take(MapArchiveSession::Open(task.written));
    Require(Take(saved.ReadMember("war3map.wpm")) == f.Member("war3map.wpm"), "unrelated save preserves the complete original WPM");
    Require(Take(saved.ReadMember("war3map.w3e")) == f.Member("war3map.w3e"), "unrelated save preserves the complete original W3E");
    Require(Take(saved.ReadMember("war3mapMap.tga")) == f.Member("war3mapMap.tga"), "unrelated save preserves the author minimap");
    for (const auto& member : output) Require(Take(saved.ReadMember(member.name)) == member.data, "unrelated final output survives exact readback");
    Require(Take(Sha256File(f.archive->SourcePath())) == oldSourceSha, "unrelated save leaves the source archive untouched");
}

void UnitHitPointsDoNotRequireTerrainCapabilities() {
    Fixture f;
    auto units = Take(NativeObjectDocument::CreateEmpty(ObjectType::Unit));
    Check(units.AddOriginalObject("hfoo"));
    Check(units.AddModification({ObjectType::Unit, "hfoo"}, {"uhpm"}, NativeValueType::Int32, std::int32_t{1}));
    std::vector<MemberReplacement> output{{"war3map.w3u", Take(NativeObjectCodec(ObjectType::Unit).Serialize(units))}};
    // 原WPM并非本生成器能够解释；生命值保存不能因此新增原生重建门。
    f.Member("war3map.wpm")[16] = std::byte{0xff}; f.Open();
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, nullptr, {}, {}, output)),
        "actual W3U HP edit does not need an attached terrain session or game root");
    SaveUnchangedDependencies(f, output);
}

void MapDescriptionDoesNotRequireObjectMetadata() {
    Fixture f;
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.front().rawcode = "Myst";
    f.Member("war3map.doo") = Take(placements.Serialize());
    f.Member("war3map.wpm")[16] = std::byte{0xff}; f.Open();
    auto settings = Take(MapSettings::Parse(f.Member("war3map.w3i")));
    settings.mapName = "Owned renamed fixture";
    settings.description = "Only the map name and description changed.";
    std::vector<MemberReplacement> output{{"war3map.w3i", Take(settings.Serialize())}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, nullptr, {}, {}, output)),
        "map name and description changes do not parse unknown existing placement metadata");
    SaveUnchangedDependencies(f, output);
}

Bytes DoodadArtAndUnknown(bool unknown, std::string value) {
    auto native = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
    Check(native.AddOriginalObject("Doo0"));
    Check(native.AddModification({ObjectType::Doodad, "Doo0"}, {unknown ? "uN01" : "dmdl"}, NativeValueType::String, std::move(value)));
    return Take(NativeObjectCodec(ObjectType::Doodad).Serialize(native));
}

void UnrelatedArtAndTemplatesDoNotAcquireNativeGates() {
    Fixture f;
    f.Member("Doodads/DoodadMetaData.slk") = Slk({"ID", "field", "slk", "type", "repeat", "index"},
        {{"dpt0", "pathTex", "DoodadData", "pathingTexture", "0", "0"},
         {"dmdl", "file", "DoodadData", "model", "0", "0"}});
    f.members.push_back({"war3map.w3d", DoodadArtAndUnknown(false, "Models/owned-original.mdx")});
    f.Member("war3map.wpm")[16] = std::byte{0xff}; f.Open();
    std::vector<MemberReplacement> output{{"Icons/owned-new.blp", Text("owned icon; not a footprint")},
        {"Models/owned-new.mdx", Text("owned model; not a footprint")},
        {"war3map.w3d", f.Member("war3map.w3d")}};
    SaveUnchangedDependencies(f, output);

    // 只有已知外观字段改变，实际通行getter仍然相同；不能使用完整重建检查器当probe。
    output[2].data = DoodadArtAndUnknown(false, "Models/owned-new.mdx");
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "a known art override is not a static pathing dependency delta");
    Check(TerrainPathingSave::VerifyUnchangedFinalInputs(*f.archive, f.edits.get(), f.gameRoot, {}, output));
}

void UnknownChangesRemainUnavailableAndUnchangedUnknownsRemainPreservable() {
    Fixture f; f.members.push_back({"war3map.w3d", DoodadArtAndUnknown(true, "unchanged unknown wire")}); f.Open();
    std::vector<MemberReplacement> output{{"Icons/owned-new.tga", Text("unrelated; no footprint reference")}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "existing unchanged unknown fields are not an artificial gate for an unrelated asset");
    output.push_back({"war3map.w3d", DoodadArtAndUnknown(true, "changed unknown wire")});
    const auto changed = TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output);
    Require(!changed.Succeeded() && HasCode(changed, "TERRAIN_FINAL_UNKNOWN_FIELD_CHANGED"),
        "changing an unknown static object field remains explicitly unavailable");
}

void DynamicUnknownChangesAreNotClassifiedAsUnrelated() {
    Fixture f;
    auto placements = Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.front().rawcode = "Dst0";
    f.Member("war3map.doo") = Take(placements.Serialize());
    const auto unknown = [](std::string value) {
        auto native = Take(NativeObjectDocument::CreateEmpty(ObjectType::Destructable));
        Check(native.AddOriginalObject("Dst0"));
        Check(native.AddModification({ObjectType::Destructable, "Dst0"}, {"uN02"},
            NativeValueType::String, std::move(value)));
        return Take(NativeObjectCodec(ObjectType::Destructable).Serialize(native));
    };
    f.members.push_back({"war3map.w3b", unknown("unchanged dynamic unknown wire")});
    f.Open();
    std::vector<MemberReplacement> output{{"Icons/owned-dynamic-icon.blp", Text("unrelated image")}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "existing unchanged dynamic unknown wire remains preservable for unrelated imports");
    output.push_back({"war3map.w3b", unknown("changed dynamic unknown wire")});
    const auto changed = TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output);
    Require(!changed.Succeeded() && HasCode(changed, "TERRAIN_FINAL_UNKNOWN_FIELD_CHANGED"),
        "a dynamic contribution cannot hide changed unknown native wire fields");
}

void NulloptIsReprobedAgainstTheActualFinalList() {
    Fixture f;
    std::vector<MemberReplacement> output{{"Icons/owned-icon.blp", Text("unrelated imported image")}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "initial unrelated import has no native plan");
    output.push_back({"war3map.mmp", Text("later marker output")});
    output.push_back({"war3map.shd", Text("later shadow output")});
    output.push_back({"war3mapMap.blp", Text("later automatic minimap output")});
    Check(TerrainPathingSave::VerifyUnchangedFinalInputs(*f.archive, f.edits.get(), f.gameRoot, {}, output));
    output.push_back({std::string(kOldPath), Texture(8, true)});
    const auto changed = TerrainPathingSave::VerifyUnchangedFinalInputs(*f.archive, f.edits.get(), f.gameRoot, {}, output);
    Require(!changed.Succeeded() && HasCode(changed, "TERRAIN_FINAL_INPUTS_CHANGED"),
        "write-time final footprint change is refused instead of silently preserving old WPM or rebuilding in the guard");
}

void ActualWinnersAndBaselineRequirementsArePreserved() {
    Fixture f;
    // 实际TGA已存在；同路径BLP是低优先级候选，新增它不能触发无意义重建。
    auto unusedBlp = std::string(kOldPath); unusedBlp.replace(unusedBlp.size() - 4, 4, ".blp");
    std::vector<MemberReplacement> output{{unusedBlp, Text("not the selected footprint winner")}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "an actual present TGA wins over a new BLP candidate");
    output = f.Final();
    const auto absentBaseline = TerrainPathingSave::PrepareFinalForSave(*f.archive, nullptr, f.gameRoot, {}, output);
    Require(!absentBaseline.Succeeded() && HasCode(absentBaseline, "TERRAIN_FINAL_BASELINE_MISSING"),
        "relevant resource changes cannot create a different hidden edit-session identity");
    const auto tiny = TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {2048}, output);
    Require(!tiny.Succeeded(), "probe and native reconstruction do not each receive a fresh full budget");
    auto bad = output; bad.push_back({"PATHTEXTURES\\DUAL-STAGE-ORIGINAL.TGA", Texture(4, false)});
    Require(!TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, bad).Succeeded(),
        "canonical duplicates still reject before acquiring a plan");
}

void UnsupportedPathDeclarationsOnlyGateTouchedCandidates() {
    Fixture f;
    f.Member("Doodads/Doodads.slk") = Slk({"doodID", "code", "pathTex", "category", "fixedRot"},
        {{"Doo0", "Doo0", "PathTextures/dual-stage-original.xxx", "E", "0"}});
    f.Open();
    std::vector<MemberReplacement> output{{"Icons/unrelated-owned-icon.blp", Text("unrelated image")}};
    Require(!Take(TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output)),
        "an old unsupported path declaration does not block unrelated icon imports");
    output.push_back({std::string(kOldPath), Texture(8, true)});
    const auto related = TerrainPathingSave::PrepareFinalForSave(*f.archive, f.edits.get(), f.gameRoot, {}, output);
    Require(!related.Succeeded() && HasCode(related, "TERRAIN_NATIVE_FOOTPRINT_FORMAT"),
        "a touched basename TGA candidate reaches the full explicit unsupported-format gate");
}

void DirectoryFinalDependenciesUseTheSameFactory() {
    Fixture f; const auto directory = f.root / "final-dependencies-workspace";
    Require(std::filesystem::create_directory(directory), "create owned final dependency directory source");
    for (const auto& member : f.members) {
        const auto path = directory / member.name; std::filesystem::create_directories(path.parent_path());
        Check(IO::WriteFileBytes(path, member.data));
    }
    const auto workspace = Take(DirectoryWorkspace::Open(directory)); auto output = f.Final(true);
    auto plan = Take(TerrainPathingSave::PrepareFinalForSave(workspace.Snapshot(), f.edits.get(), f.gameRoot, {}, output));
    Require(plan.has_value(), "directory source detects actual native path change without a terrain stroke");
    Check(plan->SaveGate()); Check(plan->InstallPathingOutput(*f.edits, output));
    const auto wpm = std::find_if(output.begin(), output.end(), [](const auto& member) { return member.name == "war3map.wpm"; });
    Require(wpm != output.end() && wpm->data == Fixture::ExpectedPathing(false, 8, true),
        "directory source has the same full independent resource-only WPM expectation");
    Check(workspace.VerifyUnchanged());
}

void FinalDependencyCases() {
    PureAssetsRebuildStaticPathingWithoutTerrainChanges();
    PureNativePathChangePreservesAuthorBlp();
    UnitHitPointsDoNotRequireTerrainCapabilities();
    MapDescriptionDoesNotRequireObjectMetadata();
    UnrelatedArtAndTemplatesDoNotAcquireNativeGates();
    UnknownChangesRemainUnavailableAndUnchangedUnknownsRemainPreservable();
    DynamicUnknownChangesAreNotClassifiedAsUnrelated();
    NulloptIsReprobedAgainstTheActualFinalList();
    ActualWinnersAndBaselineRequirementsArePreserved();
    UnsupportedPathDeclarationsOnlyGateTouchedCandidates();
    DirectoryFinalDependenciesUseTheSameFactory();
}
