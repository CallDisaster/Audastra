#pragma once

// 在TerrainShadowRealTests.cpp的TerrainSaveTests命名空间、Require/Take/Check之后包含。
// 只生成原创地图成员；CliffTypes和全部接收模型必须取自本机实际游戏来源。
namespace {
void CheckRealShadowReceiverDependencies(const TerrainShadowReceiverResources& receiver, bool ramp) {
    const auto facts = receiver.Dependencies();
    bool table = false, model = false, lowerRamp = false, upperRamp = false;
    for (const auto& fact : facts) {
        // owner按CanonicalImportPath保留大写反斜杠身份；本地分类视图统一分隔符。
        auto path = Text::LowerAscii(fact.name);
        std::replace(path.begin(), path.end(), '\\', '/');
        const bool cliffTable = path == "terrainart/clifftypes.slk";
        const bool terrainModel = path.starts_with("doodads/terrain/")
            && (path.ends_with(".mdx") || path.ends_with(".mdl"));
        if (!cliffTable && !terrainModel) continue;
        if (fact.source == TerrainShadowCasterSource::Missing) continue; // 变体探测的实际absence也由owner保存。
        Require(fact.source == TerrainShadowCasterSource::Game && !fact.gameContainer.empty()
            && fact.sha256.size() == 64 && fact.bytes != 0,
            "real receiver consumes the actual game table/model winner, container and full SHA rather than synthetic map resources");
        if (cliffTable) table = true;
        else {
            model = true;
            const auto tail = std::string_view(path).substr(std::string_view("doodads/terrain/").size());
            const auto slash = tail.find('/');
            Require(slash != std::string_view::npos && slash != 0
                && tail.substr(slash + 1).starts_with(tail.substr(0, slash)),
                "real cliff/ramp resource retains the native repeated model directory in its filename");
            lowerRamp |= path.ends_with("hlab0.mdx");
            upperRamp |= path.ends_with("balh0.mdx");
        }
        std::cout << "actual_shadow_receiver_dependency kind=" << (ramp ? "ramp" : "cliff")
            << " member=" << fact.name << " container=" << fact.gameContainer
            << " sha256=" << fact.sha256 << " bytes=" << fact.bytes << '\n';
    }
    Require(table && model && receiver.RecordCount() != 0,
        "real receiver owns actual CliffTypes, at least one game model and nonempty ordered records");
    if (ramp) Require(lowerRamp && upperRamp,
        "the typed three-point straight ramp loads both native HLAB and BALH anchor models from the real game");
}

void CheckRealShadowReceiverOrdinaryResave(const MapArchiveSession& shadowSaved,
    const std::filesystem::path& scenario, const std::filesystem::path& tool,
    const std::filesystem::path& game, std::span<const std::byte> shadowBytes) {
    const auto settingsBytes = Take(shadowSaved.ReadMember("war3map.w3i"));
    const auto document = MapDocument::FromArchive(shadowSaved);
    const auto* name = FindMapField(MapDataKind::Settings, "map_name");
    Require(name != nullptr, "ordinary resave uses the shared typed map-name field");
    const auto newName = Take(ParseMapFieldInput(*name, "Real terrain receiver ordinary resave"));
    auto expectedSettings = Take(MapSettings::Parse(settingsBytes)); expectedSettings.mapName = newName.bytes;
    const auto expectedSettingsBytes = Take(expectedSettings.Serialize());
    MapEditSaveRequest request;
    request.sourceMap = shadowSaved.SourcePath(); request.targetCandidate = scenario / "ordinary-saved.w3x";
    request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = shadowSaved.SourceSha256();
    request.fieldEdits = {{Take(Sha256(settingsBytes)), {MapDataKind::Settings, 0,
        Take(MapFieldRowKey(document, MapDataKind::Settings, 0)), "map_name",
        Take(ReadMapField(document, MapDataKind::Settings, 0, *name)), newName}}};
    Require(!request.rebuildTerrainShadow, "ordinary typed field save does not implicitly recalculate SHD");
    const auto receipt = Take(SaveMapEditCandidate(request));
    const auto readback = Take(MapArchiveSession::Open(receipt.candidate));
    const auto rereadShadow = Take(readback.ReadMember("war3map.shd"));
    Require(std::equal(rereadShadow.begin(), rereadShadow.end(), shadowBytes.begin(), shadowBytes.end())
        && Take(readback.ReadMember("war3map.w3i")) == expectedSettingsBytes,
        "ordinary save preserves the entire explicitly generated SHD and reads back the complete exact typed W3I edit");
    for (const auto& member : shadowSaved.Members()) {
        const auto memberName = Text::LowerAscii(member.name);
        if (memberName == "war3map.w3i" || memberName == "war3map.j"
            || memberName == "scripts/war3map.j") continue;
        Require(Take(readback.ReadMember(member.name)) == member.data,
            "ordinary resave preserves every unrelated source member byte-for-byte");
    }
    const auto script = Take(readback.ReadMember(Script::MapScriptMemberName(readback)));
    const std::string_view scriptText(reinterpret_cast<const char*>(script.data()), script.size());
    Require(!receipt.scriptPreserved
        && scriptText.find("Real terrain receiver ordinary resave") != std::string_view::npos,
        "ordinary map-name save rebuilds initialization with the actual final map name");
    Require(receipt.appliedFieldEditCount == 1 && receipt.appliedStrokeCount == 0
        && !receipt.generatedMissingMinimap && receipt.sourceSha256 == shadowSaved.SourceSha256()
        && receipt.candidateSha256 == readback.SourceSha256()
        && receipt.candidateSha256 == Take(Sha256File(receipt.candidate))
        && Take(Sha256File(shadowSaved.SourcePath())) == shadowSaved.SourceSha256(),
        "ordinary save receipt binds the real source/candidate identities without changing source or minimap");
}
} // namespace

void CheckRealTerrainShadowReceiversSave(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& tool,
    const std::filesystem::path& game) {
    const auto flat = *Take(ParseMapTerrain(Take(original.ReadMember("war3map.w3e")))).Snapshot();
    Require(flat.header.width == 65 && flat.header.height == 65 && flat.vertices.size() == 65 * 65
        && !flat.header.cliffTilesets.empty(), "real receiver fixture starts from the full original 64-cell cliff palette");
    Require(std::all_of(flat.vertices.begin(), flat.vertices.end(), [&](const auto& vertex) {
        return !vertex.IsRamp() && !vertex.HasWater() && vertex.groundHeightRaw == flat.vertices.front().groundHeightRaw
            && vertex.CliffLevel() == flat.vertices.front().CliffLevel();
    }), "real receiver scenarios are authored only from the verified dry flat original");
    const auto emptyDoodads = Take(DoodadPlacements{}.Serialize());
    const auto emptyUnits = Take(UnitPlacements{}.Serialize());
    const auto emptyMarkers = Take(SerializeMapMinimapMarkers(MinimapMarkersSnapshot{}));
    constexpr std::size_t pixels = 256 * 256;
    const Bytes oldShadow(pixels, std::byte{37});
    const Bytes sentinel{std::byte{0x80}, std::byte{'R'}, std::byte{0}, std::byte{0xff}, std::byte{0x19}};
    for (const bool ramp : {false, true}) {
        const auto scenario = directory / (ramp ? "playable-real-ramp-receiver" : "playable-real-cliff-receiver");
        Require(std::filesystem::create_directory(scenario), "exclusive actual cliff/ramp shadow scenario");
        auto before = flat;
        for (std::uint32_t y = 0; y < 65; ++y) for (std::uint32_t x = 0; x < 65; ++x)
            before.vertices[std::size_t{y} * 65 + x].cliffAndLayerRaw = static_cast<std::uint8_t>(x <= 31 ? 3 : 2);
        RefreshTerrainDerived(before, 0, 0, 65, 65);
        const auto beforeBytes = Take(SerializeMapTerrain(before));
        const std::array<MemberReplacement, 6> seed{{{"war3map.w3e", beforeBytes},
            {"war3map.doo", emptyDoodads}, {"war3mapUnits.doo", emptyUnits}, {"war3map.shd", oldShadow},
            {"war3map.mmp", emptyMarkers}, {"Fixture/ReceiverUnrelated.bin", sentinel}}};
        const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
        // 仅制作与真实来源一致的WPM夹具；这是产品GenerateNative，绝不是原版通行像素oracle。
        const auto initialNative = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kBytes));
        const auto initialCells = Take(initialNative.GenerateNative(before, kBytes));
        auto baselinePathing = Take(original.ReadMember("war3map.wpm"));
        Require(baselinePathing.size() == 16 + pixels && initialCells.size() == pixels,
            "actual native fixture pathing covers every cell without increasing real-test budgets");
        for (std::size_t i = 0; i < pixels; ++i)
            baselinePathing[i + 16] = (baselinePathing[i + 16] & std::byte{0x11}) | initialCells[i];
        const std::array<MemberReplacement, 1> pathingSeed{{{"war3map.wpm", baselinePathing}}};
        const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathingSeed)).path));
        auto after = before; auto expectedPathing = baselinePathing;
        // 实际单点事务要求地格四分之一处的canonical落点；高侧x31、中心y32。
        const TerrainStrokePoint point{before.header.offset[0] + 31 * 128 + 32,
            before.header.offset[1] + 32 * 128 + 32};
        TerrainBrush brush; brush.kind = TerrainBrushKind::StraightRamp; brush.value = true;
        if (ramp) {
            MapDataEditSession session(MapDocument::FromArchive(source));
            Check(session.AttachTerrain(beforeBytes)); Check(session.AttachPathing(baselinePathing));
            const auto rules = Take(TerrainPathingSave::CaptureWaterBrushRules(source, before, before,
                DoodadPlacements{}, game, true, kBytes));
            const MapDataQueryBinding document{"real-shadow-ramp", 1, 0, source.SourceSha256()};
            const TerrainEditBinding binding{document, session.SessionIdentity(), session.Revision(), session.TerrainSourceRevision()};
            const auto prepared = Take(PrepareTerrainStraightRampEdit(session, document, binding, rules, brush,
                std::span(&point, 1), false, {}, TerrainStraightRampAlgorithm::Connected));
            Take(ApplyTerrainEdit(session, document, prepared)); after = *session.Terrain();
            auto literal = before;
            for (unsigned x = 31; x <= 33; ++x) literal.vertices[32 * 65 + x].textureAndFlagsRaw |= 0x10;
            RefreshTerrainDerived(literal, 0, 0, 65, 65);
            Require(Take(SerializeMapTerrain(after)) == Take(SerializeMapTerrain(literal)),
                "actual-rule typed straight ramp creates exactly the authored three R vertices without rewriting other raw terrain");
            const auto native = Take(TerrainPathingSave::CaptureNative(source, before, after, game, kBytes));
            const auto cells = Take(native.GenerateNative(after, TerrainNativeStage::After, kBytes));
            Require(cells.size() == pixels, "typed actual ramp retains a complete final WPM fixture");
            for (std::size_t i = 0; i < pixels; ++i)
                expectedPathing[i + 16] = (expectedPathing[i + 16] & std::byte{0x11}) | cells[i];
        }
        const auto afterBytes = Take(SerializeMapTerrain(after));
        const std::array<MemberReplacement, 1> finalTerrain{{{"war3map.w3e", afterBytes}}};
        const auto light = Take(TerrainShadowResources::Capture(game, Game::NativeEditorStartupRules::Classic127aExpansion, kBytes));
        const auto casters = Take(TerrainShadowCasterResources::Capture(source, game, finalTerrain));
        Require(casters->Casters().empty() && casters->SpecialDoodads().empty(),
            "actual cliff/ramp regression does not bypass unsupported special geometry or complex caster Z");
        const auto receiver = Take(TerrainShadowReceiverResources::Capture(source, game, casters, light, finalTerrain));
        CheckRealShadowReceiverDependencies(*receiver, ramp); Check(receiver->VerifySources(source, finalTerrain));

        MapEditSaveRequest request;
        request.sourceMap = source.SourcePath(); request.targetCandidate = scenario / "explicit-shadow-saved.w3x";
        request.toolRoot = tool; request.gameRoot = game; request.expectedSourceSha256 = source.SourceSha256();
        request.expectedTerrainSha256 = Take(Sha256(beforeBytes)); request.rebuildTerrainShadow = true;
        if (ramp) { MapEditTerrainStroke stroke; stroke.brush = brush; stroke.points = {point}; request.terrainStrokes = {stroke}; }
        const auto receipt = Take(SaveMapEditCandidate(request));
        const auto saved = Take(MapArchiveSession::Open(receipt.candidate));
        const auto shadow = Take(saved.ReadMember("war3map.shd"));
        const auto decoded = Take(ParseMapShadow(shadow, 256, 256));
        Require(shadow.size() == pixels && shadow != oldShadow && decoded.intensities.size() == pixels
            && std::all_of(decoded.intensities.begin(), decoded.intensities.end(), [](auto value) { return value == 0 || value == 255; }),
            "actual game cliff/ramp explicit shadow saves and independently decodes every binary SHD pixel rather than retaining the old sentinel");
        Require(Take(saved.ReadMember("war3map.w3e")) == afterBytes && Take(saved.ReadMember("war3map.wpm")) == expectedPathing
            && Take(saved.ReadMember("war3map.doo")) == emptyDoodads && Take(saved.ReadMember("war3mapUnits.doo")) == emptyUnits
            && Take(saved.ReadMember("war3map.mmp")) == emptyMarkers,
            "actual resource shadow save preserves complete W3E/WPM and the exact empty ordinary/special/units/marker payloads");
        for (const auto& member : source.Members()) {
            const auto name = Text::LowerAscii(member.name);
            if (name == "war3map.shd" || name == "war3map.w3e" || name == "war3map.wpm"
                || (name == "war3mapmap.blp" && receipt.generatedMissingMinimap)) continue;
            Require(Take(saved.ReadMember(member.name)) == member.data,
                "actual cliff/ramp save keeps every unrelated authored member byte-for-byte");
        }
        Require(receipt.scriptPreserved && receipt.appliedStrokeCount == (ramp ? 1u : 0u)
            && receipt.appliedFieldEditCount == 0 && receipt.appliedPlacementCount == 0
            && receipt.sourceSha256 == source.SourceSha256() && receipt.candidateSha256 == saved.SourceSha256()
            && receipt.candidateSha256 == Take(Sha256File(receipt.candidate))
            && receipt.savedTerrainSha256 == Take(Sha256(afterBytes)),
            "actual game receiver receipt binds the complete saved candidate and typed stroke result");
        Check(receiver->VerifyReadback(saved)); Check(light.VerifySources(kBytes));
        CheckRealShadowReceiverOrdinaryResave(saved, scenario, tool, game, shadow);
        Require(Take(Sha256File(source.SourcePath())) == source.SourceSha256()
            && Take(Sha256File(seeded.SourcePath())) == seeded.SourceSha256()
            && Take(Sha256File(original.SourcePath())) == original.SourceSha256(),
            "actual receiver and ordinary resave leave every source archive immutable");
        std::cout << "actual_shadow_receiver_save_passed kind=" << (ramp ? "ramp" : "cliff")
            << " candidate_sha256=" << receipt.candidateSha256 << " shd_sha256=" << Take(Sha256(shadow))
            << " shd_bytes=" << shadow.size() << " receiver_records=" << receiver->RecordCount()
            << " wpm_fixture_uses_product_generator=true native_pixel_oracle=false"
            << " native_editor_verified=false game_verified=false ordinary_resave_verified=true\n";
    }
}
