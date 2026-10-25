#pragma once

void TerrainResizeKeepsStrokeCoordinatesInCommandOrder() {
    namespace V = War3Map::Services::MapEditValidation;
    namespace M = War3Map::MapData;
    using War3Map::Services::MapEditPointSpace;
    using War3Map::Services::MapEditTerrainStroke;
    using AudastraTest::TerrainResize::Check;
    using AudastraTest::TerrainResize::Take;
    M::TerrainHeader frame;
    frame.width = frame.height = 33; frame.offset = {-2048, -2048};
    MapEditTerrainStroke vertex;
    vertex.pointSpace = MapEditPointSpace::TerrainVertex; vertex.points = {{32, 32}};
    Check(V::AdvanceTerrainPointFrame(frame, M::TerrainResizeIntent{1, {-32, -32, 32, 32}}));
    Require(frame.width == 65 && frame.height == 65 && frame.offset[0] == -6144 && frame.offset[1] == -6144,
        "west/south resize advances only the current input coordinate frame");
    const auto originalSouthwest = Take(V::WorldPoints(vertex, frame));
    Require(originalSouthwest.size() == 1 && originalSouthwest[0].x == -2048 && originalSouthwest[0].y == -2048,
        "after west/south expansion vertex 32 maps to the literal original southwest, not source-frame world 2048");

    Check(V::AdvanceTerrainPointFrame(frame, M::TerrainResizeIntent{1, {0, 0, 96, 96}}));
    MapEditTerrainStroke world; world.points = {{2176, 2176}};
    const auto addedNortheast = Take(V::WorldPoints(world, frame));
    Require(addedNortheast.size() == 1 && addedNortheast[0].x == 2176 && addedNortheast[0].y == 2176,
        "world points in newly added east/north cells use the current canvas bounds");
    Check(V::AdvanceTerrainPointFrame(frame, M::TerrainResizeIntent{1, {-32, 0, 96, 96}}));
    vertex.points = {{64, 32}};
    const auto afterSecondWest = Take(V::WorldPoints(vertex, frame));
    Require(frame.width == 129 && frame.height == 97 && frame.offset[0] == -10240 && frame.offset[1] == -6144
        && afterSecondWest.size() == 1 && afterSecondWest[0].x == -2048 && afterSecondWest[0].y == -2048,
        "multiple resizes compose from each preceding southwest origin");

    M::TerrainHeader crop;
    crop.width = crop.height = 65; crop.offset = {-2048, -2048};
    Check(V::AdvanceTerrainPointFrame(crop, M::TerrainResizeIntent{1, {0, 0, 32, 32}}));
    vertex.points = {{40, 16}};
    const auto vertexOutside = V::WorldPoints(vertex, crop);
    const auto worldOutside = V::WorldPoints(world, crop);
    Require(!vertexOutside.Succeeded() && !vertexOutside.value && !worldOutside.Succeeded() && !worldOutside.value,
        "cropped-away vertices and world points are rejected against the current canvas");
    Require(AudastraTest::TerrainResize::HasCode(War3Map::Status{vertexOutside.diagnostics}, "SAVE_VERTEX_BOUNDS")
        && AudastraTest::TerrainResize::HasCode(War3Map::Status{worldOutside.diagnostics}, "SAVE_STROKE_POINT_BOUNDS"),
        "cropped point diagnostics identify the correct point-space bounds");

    M::TerrainHeader coordinateLimit;
    coordinateLimit.width = coordinateLimit.height = 33; coordinateLimit.offset = {16773120, 0};
    const auto tooFar = V::AdvanceTerrainPointFrame(coordinateLimit, M::TerrainResizeIntent{1, {0, 0, 64, 32}});
    Require(!tooFar.Succeeded() && AudastraTest::TerrainResize::HasCode(tooFar, "TERRAIN_RESIZE_COORDINATE")
        && coordinateLimit.width == 33 && coordinateLimit.height == 33
        && coordinateLimit.offset[0] == 16773120 && coordinateLimit.offset[1] == 0,
        "input coordinate overflow fails without partly advancing the frame");
}

void TerrainResizeReachesSharedScriptGateOnOwnedSource() {
    namespace R = AudastraTest::TerrainResize;
    using namespace War3Map;
    using namespace War3Map::Services;
    for (const bool shadowPresent : {false, true}) {
        R::Fixture fixture(true, shadowPresent);
        const auto source = fixture.archive->SourcePath();
        const auto sourceSha = fixture.archive->SourceSha256();
        const auto terrainSha = R::Take(Sha256(R::TerrainLiteral()));
        const auto ui = fixture.directory / "ui";
        Require(std::filesystem::create_directories(ui / "base"), "create owned resize CLI UI layer");
        R::Check(IO::WriteFileBytes(ui / "config", R::Text("base\n")));
        R::Check(IO::WriteFileBytes(ui / "base" / "define.txt",
            R::Text("[TriggerTypes]\ninteger=0,1,1,Integer,integer\n")));
        for (const auto* name : {"event.txt", "condition.txt", "action.txt", "call.txt"})
            R::Check(IO::WriteFileBytes(ui / "base" / name, R::Text("")));
        const auto target = fixture.directory / "resize-not-published.w3x";
        std::vector<std::wstring> args{L"war3-map-cli", L"map-edit-save", source.wstring(),
            std::wstring(sourceSha.begin(), sourceSha.end()), std::wstring(terrainSha.begin(), terrainSha.end()),
            ui.wstring(), fixture.gameRoot.wstring(), target.wstring(),
            L"--terrain-resize", L"-32", L"-32", L"32", L"32"};
        const auto cli = Invoke(args);
        // 故意没有WTG/WCT。只有实际来源、typed重放和完整成员范围都通过，才会到这道脚本门。
        if (cli.exitCode != 1 || !cli.output.empty() || cli.error.find("SAVE_TRIGGER_PAIR_MISSING") == std::string::npos)
            throw std::runtime_error("owned resize CLI must reach the shared script gate: " + cli.error);
        auto orderedArgs = args;
        orderedArgs.insert(orderedArgs.end(), {L"--stroke", L"texture", L"circle", L"48", L"48", L"0", L"Tg01"});
        const auto orderedCli = Invoke(orderedArgs);
        if (orderedCli.exitCode != 1 || !orderedCli.output.empty()
            || orderedCli.error.find("SAVE_TRIGGER_PAIR_MISSING") == std::string::npos)
            throw std::runtime_error("CLI stroke in the expanded canvas must reach the shared script gate: " + orderedCli.error);

        MapEditSaveRequest request;
        request.sourceMap = source; request.targetCandidate = target;
        request.toolRoot = ui; request.gameRoot = fixture.gameRoot;
        request.expectedSourceSha256 = sourceSha; request.expectedTerrainSha256 = terrainSha;
        MapEditTerrainStroke resize;
        resize.resize = MapData::TerrainResizeIntent{1, {-32, -32, 32, 32}};
        request.terrainStrokes = {resize};
        const auto expect = [&](const auto& result, const char* code) {
            if (result.Succeeded() || result.value || !R::HasCode(Status{result.diagnostics}, code)) {
                std::string detail;
                for (const auto& diagnostic : result.diagnostics) detail += diagnostic.code + ": " + diagnostic.message + "\n";
                throw std::runtime_error(std::string("resize shared save gate expected ") + code + ": " + detail);
            }
            Require(!std::filesystem::exists(target) && R::Take(Sha256File(source)) == sourceSha,
                "rejected resize save never publishes a prefix or modifies its source");
        };
        expect(SaveMapEditCandidate(request), "SAVE_TRIGGER_PAIR_MISSING");
        const auto document = MapDocument::FromMembers(fixture.members);
        const auto* nameField = MapData::FindMapField(MapDataKind::Settings, "map_name");
        Require(nameField != nullptr, "map name descriptor exists for resize companion edit");
        MapEditFieldOperation name;
        name.expectedMemberSha256 = R::Take(Sha256(fixture.Member("war3map.w3i")));
        name.edit.kind = MapDataKind::Settings;
        name.edit.expectedRowKey = R::Take(MapData::MapFieldRowKey(document, MapDataKind::Settings, 0));
        name.edit.field = "map_name";
        name.edit.expected = R::Take(MapData::ReadMapField(document, MapDataKind::Settings, 0, *nameField));
        name.edit.value = R::Take(MapData::ParseMapFieldInput(*nameField, "Resized synthetic terrain canvas"));
        request.fieldEdits = {name};
        expect(SaveMapEditCandidate(request), "SAVE_TRIGGER_PAIR_MISSING");
        request.fieldEdits.clear();
        request.terrainStrokes[0].resize->canvas.east = 31;
        expect(SaveMapEditCandidate(request), "TERRAIN_RESIZE_RULES");
        request.terrainStrokes = {resize};
        request.expectedSourceSha256.front() = sourceSha.front() == '0' ? '1' : '0';
        expect(SaveMapEditCandidate(request), "SAVE_SOURCE_STALE");
        request.expectedSourceSha256 = sourceSha;
        MapEditTerrainStroke paint;
        paint.brush.kind = MapData::TerrainBrushKind::Blight; paint.brush.radius = 0;
        paint.points.assign(2048, MapData::TerrainStrokePoint{0, 0});
        auto clear = paint; clear.brush.value = false;
        request.terrainStrokes = {paint, resize, clear};
        expect(SaveMapEditCandidate(request), "SAVE_TRIGGER_PAIR_MISSING");
        request.terrainStrokes.back().points.push_back({0, 0});
        expect(SaveMapEditCandidate(request), "SAVE_STROKE_POINTS");
        for (const bool before : {false, true}) {
            request.terrainStrokes = before ? std::vector<MapEditTerrainStroke>{MapEditTerrainStroke{}, resize}
                : std::vector<MapEditTerrainStroke>{resize, MapEditTerrainStroke{}};
            expect(SaveMapEditCandidate(request), "SAVE_STROKE_POINTS");
        }
        MapEditTerrainStroke point;
        point.brush.kind = MapData::TerrainBrushKind::Blight; point.brush.radius = 0;
        point.pointSpace = MapEditPointSpace::TerrainVertex; point.points = {{32, 32}};
        request.terrainStrokes = {resize, point};
        expect(SaveMapEditCandidate(request), "SAVE_TRIGGER_PAIR_MISSING");
        auto northeast = resize; northeast.resize->canvas = {0, 0, 64, 64};
        point.pointSpace = MapEditPointSpace::World; point.points = {{2176, 2176}};
        request.terrainStrokes = {northeast, point};
        expect(SaveMapEditCandidate(request), "SAVE_TRIGGER_PAIR_MISSING");
        auto crop = resize; crop.resize->canvas = {0, 0, 32, 32};
        point.pointSpace = MapEditPointSpace::TerrainVertex; point.points = {{40, 16}};
        request.terrainStrokes = {northeast, crop, point};
        expect(SaveMapEditCandidate(request), "SAVE_VERTEX_BOUNDS");
        point.pointSpace = MapEditPointSpace::World; point.points = {{2176, 2176}};
        request.terrainStrokes = {northeast, crop, point};
        expect(SaveMapEditCandidate(request), "SAVE_STROKE_POINT_BOUNDS");
    }
}

void TerrainResizeUsesSignedBeforeCanvasEdges() {
    TerrainResizeKeepsStrokeCoordinatesInCommandOrder();
    const auto base = [] {
        auto args = Arguments(); args.insert(args.end(), {L"--terrain-resize", L"-32", L"-32", L"64", L"64"}); return args;
    };
    const auto valid = Invoke(base());
    Require(valid.exitCode != 0 && valid.output.empty() && valid.error.find("SOURCE_LEASE_FAILED") != std::string::npos,
        "signed resize reaches the actual source gate without authorizing rules, clipping or object movement");
    for (const auto index : {9, 10, 11, 12}) {
        for (const auto value : {L"1.5", L"nan", L"2147483648", L"-2147483649"}) {
            auto args = base(); args[index] = value; Rejected(Invoke(args), "nonempty signed integer half-open");
        }
    }
    auto invalid = base(); invalid[11] = L"-32"; Rejected(Invoke(invalid), "nonempty signed integer half-open");
    invalid = base(); invalid[12] = L"-33"; Rejected(Invoke(invalid), "nonempty signed integer half-open");
    invalid = base(); invalid.pop_back(); Rejected(Invoke(invalid), "requires west south east north");
    invalid = base(); invalid.insert(invalid.end(), {L"--terrain-resize-move-objects", L"true"});
    Rejected(Invoke(invalid), "unknown map-edit-save option");
    auto tooMany = Arguments();
    for (unsigned i = 0; i < 129; ++i) tooMany.insert(tooMany.end(), {L"--terrain-resize", L"-32", L"-32", L"64", L"64"});
    Rejected(Invoke(tooMany), "at most 128 terrain");
    TerrainResizeReachesSharedScriptGateOnOwnedSource();
}
