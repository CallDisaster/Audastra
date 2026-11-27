#pragma once

void TerrainTextureSlotCliActuallySaves() {
    namespace P = AudastraTest::TextureSlot;
    namespace M = War3Map::MapData;
    P::Fixture fixture;
    {
        const auto source = P::Take(War3Map::MapArchiveSession::Open(fixture.source.path));
        const std::vector<War3Map::MemberReplacement> opaque{{"x.ai", P::Bytes("// original harmless opaque fixture\n")}};
        fixture.source = P::Take(source.SaveCandidate(fixture.root / "opaque-source.w3x", opaque));
    }
    auto request = fixture.Request("direct-cli");
    Require(!request.inProcessOnly, "ordinary save request preserves configured fallback by default");
    request.terrainStrokes = {P::Command(true, M::TerrainTextureTable::Ground, 1, "", "Tg02"),
        P::Command(false, M::TerrainTextureTable::Ground, 1, "Tg02", "Tg03"),
        P::Command(true, M::TerrainTextureTable::Cliff, 1, "", "Cl02")};
    (void)P::Take(War3Map::Services::SaveMapEditCandidate(request));
    const auto candidate = fixture.root / "cli.w3x";
    std::vector<std::wstring> args{L"war3-map-cli", L"map-edit-save", fixture.source.path.wstring(),
        std::wstring(fixture.source.sha256.begin(), fixture.source.sha256.end()),
        std::wstring(fixture.terrainSha.begin(), fixture.terrainSha.end()), fixture.inputs.tool.wstring(), fixture.inputs.game.wstring(), candidate.wstring(),
        L"--terrain-slot-append", L"ground", L"1", L"Tg02", L"--terrain-slot-replace", L"ground", L"1", L"Tg02", L"Tg03",
        L"--terrain-slot-append", L"cliff", L"1", L"Cl02"};
    const auto saved = Invoke(args);
    if (saved.exitCode != 0) throw std::runtime_error(saved.error);
    Require(saved.output.find("script_backend_policy=configured") != std::string::npos
        && saved.output.find("opaque_script_member_count=1") != std::string::npos
        && saved.output.find("opaque_script_member_0=x.ai") != std::string::npos,
        "CLI reports backend policy and opaque identity without extracting script contents");
    auto expected = P::Take(War3Map::MapArchiveSession::Open(request.targetCandidate));
    auto actual = P::Take(War3Map::MapArchiveSession::Open(candidate));
    for (const char* member : {"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3map.doo", "unchanged.bin"})
        Require(P::Take(expected.ReadMember(member)) == P::Take(actual.ReadMember(member)), "CLI/common-service saved native members agree");
    auto isolated = args;
    isolated[7] = (fixture.root / "cli-isolated.w3x").wstring();
    isolated.push_back(L"--in-process-only");
    const auto isolatedSaved = Invoke(isolated);
    Require(isolatedSaved.exitCode == 0 && isolatedSaved.output.find("script_backend_policy=in_process_only") != std::string::npos,
        "explicit CLI flag is propagated into the shared receipt");
    isolated.push_back(L"--in-process-only");
    Rejected(Invoke(isolated), "cannot be repeated");
    args[7] = (fixture.root / "cli-stale.w3x").wstring(); args.back() = L"Miss";
    Require(Invoke(args).exitCode != 0 && !std::filesystem::exists(fixture.root / "cli-stale.w3x"), "CLI failure cannot save a prefix");
    Require(P::Take(War3Map::Sha256File(fixture.source.path)) == fixture.source.sha256, "CLI source unchanged");
    {
        P::Fixture missing;
        const auto original = P::Take(War3Map::MapArchiveSession::Open(missing.source.path));
        const std::array<std::string, 1> remove{"war3map.wpm"};
        missing.source = P::Take(original.SaveCandidate(missing.root / "without-pathing.w3x", {}, remove));
        const auto target = missing.root / "cli-created-pathing.w3x";
        const std::vector<std::wstring> create{L"war3-map-cli", L"map-edit-save", missing.source.path.wstring(),
            std::wstring(missing.source.sha256.begin(), missing.source.sha256.end()),
            std::wstring(missing.terrainSha.begin(), missing.terrainSha.end()), missing.inputs.tool.wstring(), missing.inputs.game.wstring(), target.wstring(),
            L"--in-process-only", L"--terrain-slot-append", L"ground", L"1", L"Tg02"};
        const auto savedMissing = Invoke(create);
        if (savedMissing.exitCode != 0) throw std::runtime_error(savedMissing.error);
        Require(savedMissing.error.find("TERRAIN_SAVE_GENERATED_MISSING_WPM") != std::string::npos,
            "CLI exposes first WPM generation from an exact missing source");
        const auto created = P::Take(War3Map::MapArchiveSession::Open(target));
        Require(!original.HasOpaqueMembers() && !P::Take(War3Map::MapArchiveSession::Open(missing.source.path)).HasMember("war3map.wpm")
            && created.HasMember("war3map.wpm"), "real CLI readback adds WPM while its source remains absent");
    }
}

void TerrainReplaceTileCliChoosesReplaceOrRemap() {
    namespace P = AudastraTest::TextureSlot;
    namespace M = War3Map::MapData;
    P::Fixture fixture;
    const auto run = [&](const std::filesystem::path& source, const wchar_t* from, const wchar_t* to,
        std::optional<std::filesystem::path> out) {
        std::vector<std::wstring> args{L"war3-map-cli", L"terrain-replace-tile", source.wstring(), from, to,
            L"--game-root", fixture.inputs.game.wstring(), L"--tool-root", fixture.inputs.tool.wstring()};
        if (out) { args.push_back(L"--out"); args.push_back(out->wstring()); }
        return Invoke(args, RunTerrainReplaceTile);
    };
    const auto terrain = [](const std::filesystem::path& map) {
        auto archive = P::Take(War3Map::MapArchiveSession::Open(map));
        return P::Take(M::ParseMapTerrain(P::Take(archive.ReadMember("war3map.w3e"))));
    };
    // 目标不在表里：直接换槽。
    const auto replaced = fixture.root / "tile-replace.w3x";
    const auto first = run(fixture.source.path, L"Tg01", L"Tg03", replaced);
    if (first.exitCode != 0) throw std::runtime_error(first.error);
    Require(first.output.find("operation=replace") != std::string::npos
        && terrain(replaced).Snapshot()->header.groundTilesets == std::vector<std::string>{"Tg03"}, "absent target replaces the slot");
    // 目标已在表里：重映射全部顶点；不给 --out 时写到源文件旁，已存在则不覆盖。
    auto request = fixture.Request("tile-two");
    request.terrainStrokes = {P::Command(true, M::TerrainTextureTable::Ground, 1, "", "Tg02")};
    const auto two = P::Take(War3Map::Services::SaveMapEditCandidate(request)).candidate;
    const auto second = run(two, L"Tg01", L"Tg02", std::nullopt);
    if (second.exitCode != 0) throw std::runtime_error(second.error);
    const auto remapped = terrain(two.parent_path() / (two.stem().wstring() + L".tiles.w3x"));
    Require(second.output.find("operation=remap") != std::string::npos
        && remapped.Snapshot()->header.groundTilesets == std::vector<std::string>{"Tg01", "Tg02"}
        && std::all_of(remapped.Snapshot()->vertices.begin(), remapped.Snapshot()->vertices.end(),
            [](const auto& vertex) { return vertex.GroundTextureIndex() == 1; }), "present target remaps every vertex and keeps the table");
    const auto again = run(two, L"Tg01", L"Tg02", std::nullopt);
    Require(again.exitCode == 1 && again.error.find("TERRAIN_TILE_OUT_EXISTS") != std::string::npos, "existing output is never overwritten");
    const auto missing = run(fixture.source.path, L"Miss", L"Tg02", fixture.root / "tile-miss.w3x");
    Require(missing.exitCode == 1 && missing.error.find("TERRAIN_TILE_FROM_MISSING") != std::string::npos
        && !std::filesystem::exists(fixture.root / "tile-miss.w3x"), "unknown source tile saves nothing");
    Rejected(run(fixture.source.path, L"Tg01", L"Tg01", fixture.root / "tile-same.w3x"), "same");
    Rejected(run(fixture.source.path, L"Tg01", L"bad", fixture.root / "tile-bad.w3x"), "rawcode");
    Rejected(Invoke({L"war3-map-cli", L"terrain-replace-tile", L"x.w3x", L"Tg01", L"Tg02"}, RunTerrainReplaceTile), "required");
    Rejected(Invoke({L"war3-map-cli", L"terrain-replace-tile", L"x.w3x", L"Tg01", L"Tg02", L"--bogus", L"v"}, RunTerrainReplaceTile),
        "unknown terrain-replace-tile option");
    Require(P::Take(War3Map::Sha256File(fixture.source.path)) == fixture.source.sha256, "tile CLI source unchanged");
    // map-edit-save 的重映射参数：只收地面表。
    auto args = Arguments();
    args.insert(args.end(), {L"--terrain-tile-remap", L"ground", L"0", L"Old1", L"New1"});
    Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos, "remap reaches the shared source-bound save gate");
    args = Arguments();
    args.insert(args.end(), {L"--terrain-tile-remap", L"cliff", L"0", L"Old1", L"New1"});
    Rejected(Invoke(args), "地面表");
}

void TerrainTextureSlotsUseSharedOrderedSave() {
    for (const auto table : {L"ground", L"cliff"}) for (const bool append : {false, true}) {
        auto args = Arguments();
        args.insert(args.end(), {append ? L"--terrain-slot-append" : L"--terrain-slot-replace", table, L"1"});
        if (!append) args.push_back(L"Old1");
        args.push_back(L"New1");
        Require(Invoke(args).error.find("SOURCE_LEASE_FAILED") != std::string::npos,
            "both palette commands reach the shared source-bound save gate");
        auto bad = args; bad.pop_back(); Rejected(Invoke(bad), "requires table");
        bad = args; bad[9] = L"water"; Rejected(Invoke(bad), "ground|cliff");
        bad = args; bad[10] = L"1.5"; Rejected(Invoke(bad), "unsigned integer");
        bad = args; bad[10] = L"-1"; Rejected(Invoke(bad), "unsigned integer");
        bad = args; bad[10] = L"16"; Rejected(Invoke(bad), "槽号");
        bad = args; bad.back() = L"bad"; Rejected(Invoke(bad), "rawcode");
        bad = args; bad.push_back(L"extra"); Rejected(Invoke(bad), "unknown map-edit-save option");
    }
    auto args = Arguments();
    for (unsigned i = 0; i < 129; ++i)
        args.insert(args.end(), {L"--terrain-slot-append", L"ground", L"1", L"New1"});
    Rejected(Invoke(args), "at most 128 terrain");
    args = Arguments(); args.insert(args.end(), {L"--terrain-copy", L"0", L"0", L"1", L"1", L"2", L"2",
        L"--terrain-slot-append", L"ground", L"1", L"New1"});
    Rejected(Invoke(args), "only terrain command");
    TerrainTextureSlotCliActuallySaves();
    TerrainReplaceTileCliChoosesReplaceOrRemap();
}
