#pragma once

void TerrainTextureSlotCliActuallySaves() {
    namespace P = AudastraTest::TextureSlot;
    namespace M = War3Map::MapData;
    P::Fixture fixture;
    auto request = fixture.Request("direct-cli");
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
    auto expected = P::Take(War3Map::MapArchiveSession::Open(request.targetCandidate));
    auto actual = P::Take(War3Map::MapArchiveSession::Open(candidate));
    for (const char* member : {"war3map.w3e", "war3map.wpm", "war3map.w3i", "war3map.doo", "unchanged.bin"})
        Require(P::Take(expected.ReadMember(member)) == P::Take(actual.ReadMember(member)), "CLI/common-service saved native members agree");
    args[7] = (fixture.root / "cli-stale.w3x").wstring(); args.back() = L"Miss";
    Require(Invoke(args).exitCode != 0 && !std::filesystem::exists(fixture.root / "cli-stale.w3x"), "CLI failure cannot save a prefix");
    Require(P::Take(War3Map::Sha256File(fixture.source.path)) == fixture.source.sha256, "CLI source unchanged");
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
}
