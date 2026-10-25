#pragma once

// 由TerrainNativeObjectRealSourceTests.cpp的私有命名空间包含，共用真实来源、
// 环境变量和证据目录；不复制游戏表，不启动第二次工具链或测试进程。
void RealFinalStaticInputsUseActualCatalog(const MapArchiveSession& original,
    const std::filesystem::path& directory, const std::filesystem::path& game,
    const StandardObjectCatalog& catalog, const ObjectFieldSchema& schema,
    const std::string& parent, const std::string& originalPath) {
    const auto terrainBytes = Take(original.ReadMember("war3map.w3e"));
    const auto before = *Take(ParseMapTerrain(terrainBytes)).Snapshot();
    Require(before.header.mainTileset == 'L' && before.header.width > 40 && before.header.height > 40,
        "final static input regression uses actual Lordaeron terrain resources");
    const auto* standard = catalog.Find(ObjectType::Doodad, parent);
    Require(standard && !catalog.Find(ObjectType::Destructable, parent)
        && standard->Field("category") != "C" && standard->Field("pathtex") == originalPath,
        "final input parent and path come from the actual unambiguous non-C game catalog");
    ObjectFieldContext context; context.rawcode = parent; context.baseCode = standard->Field("code");
    const auto field = schema.MatchSourceField(ObjectType::Doodad, "pathtex", context);
    Require(field.definition && field.safeToEdit && field.sourceKey == "pathtex" && !field.sourceIndex
        && field.definition->nativeValueType == NativeValueType::String
        && field.definition->repeat == 0 && field.definition->data == 0
        && Text::LowerAscii(field.definition->sourceTable) == "doodaddata",
        "final input override uses the actual editable String metadata address");
    const auto originalDoo = Take(ParseDoodadPlacements(Take(original.ReadMember("war3map.doo"))));
    Require(originalDoo.doodads.empty() && originalDoo.specialDoodads.empty(),
        "independent terrain baseline has no object footprint contributions");
    const auto originalSha = Take(Sha256File(original.SourcePath()));
    const auto baselineRules = Take(TerrainPathingSave::CaptureNative(original, before, before, game, kSaveBytes));
    const auto baselineNative = Take(baselineRules.GenerateNative(before, kSaveBytes));
    const auto oldTexture = [] {
        std::vector<std::byte> bytes(18 + 8 * 8 * 4, std::byte{0});
        bytes[2] = std::byte{2}; bytes[12] = bytes[14] = std::byte{8};
        bytes[16] = std::byte{32}; bytes[17] = std::byte{8};
        for (std::size_t at = 18; at < bytes.size(); at += 4) bytes[at + 2] = std::byte{0xff};
        return bytes;
    }();
    auto newTexture = oldTexture;
    for (std::size_t at = 18; at < newTexture.size(); at += 4) {
        newTexture[at] = std::byte{0xff}; newTexture[at + 2] = std::byte{0};
    }
    constexpr std::string_view newPath = "PathTextures/AudastraFinalInputs8.tga";
    Require(Text::LowerAscii(originalPath) != Text::LowerAscii(newPath)
        && !original.HasMember(originalPath) && !original.HasMember(std::string(newPath)),
        "original synthetic textures do not shadow an existing map import");
    DoodadPlacement placed;
    placed.rawcode = parent; placed.creationId = 1; placed.flags = 2; placed.scale = {1, 1, 1};
    placed.position = {before.header.offset[0] + 32 * 128, before.header.offset[1] + 32 * 128, 0};
    DoodadPlacements placements; placements.doodads.push_back(placed);
    const auto doo = Take(placements.Serialize());
    const auto overridePath = [&](std::string path) {
        auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
        Check(document.AddOriginalObject(parent));
        Check(document.AddModification({ObjectType::Doodad, parent}, field.modification,
            NativeValueType::String, std::move(path)));
        return Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
    };
    const auto oldW3d = overridePath(originalPath), newW3d = overridePath(std::string(newPath));
    const auto columns = std::size_t{before.header.width - 1} * 4;
    const auto rows = std::size_t{before.header.height - 1} * 4;
    Require(baselineNative.size() == columns * rows, "complete actual terrain baseline has the expected WPM shape");
    const auto addLiteralFootprint = [&](std::vector<std::byte> native, std::byte bits) {
        // 独立字面格：原点+4096、8像素、2格吸附，覆盖[124,132)^2。
        // 红色原版位c2，蓝色08；不调用占地投影或通道编码器制造expected。
        for (std::size_t y = 124; y < 132; ++y)
            for (std::size_t x = 124; x < 132; ++x) native[y * columns + x] |= bits;
        return native;
    };
    const auto expectedOldNative = addLiteralFootprint(baselineNative, std::byte{0xc2});
    constexpr std::array<const char*, 3> cases{"final-texture-only", "final-object-only", "final-terrain-and-texture"};
    for (unsigned index = 0; index < cases.size(); ++index) {
        const auto scenario = directory / cases[index];
        Require(std::filesystem::create_directory(scenario), "exclusive real final input scenario directory");
        const bool objectOnly = index == 1, terrainAndTexture = index == 2;
        std::vector<MemberReplacement> seed{{"war3map.doo", doo}, {originalPath, oldTexture}};
        if (objectOnly) {
            seed.push_back({"war3map.w3d", oldW3d});
            seed.push_back({std::string(newPath), newTexture});
        }
        const auto seeded = Take(MapArchiveSession::Open(Take(original.SaveCandidate(scenario / "seeded.w3x", seed)).path));
        const auto sourceRules = Take(TerrainPathingSave::CaptureNative(seeded, before, before, game, kSaveBytes));
        Require(Take(sourceRules.GenerateNative(before, kSaveBytes)) == expectedOldNative,
            "actual catalog and imported 8x8 red TGA reproduce the complete literal source footprint");
        auto sourceWpm = Take(seeded.ReadMember("war3map.wpm"));
        Require(sourceWpm.size() == expectedOldNative.size() + 16, "real final input WPM has an exact header and full grid");
        // 在实际变化区另放作者两位，检查六位重建不会误删其自定义限制。
        sourceWpm[16 + 124 * columns + 124] |= std::byte{0x11};
        for (std::size_t i = 0; i < expectedOldNative.size(); ++i)
            sourceWpm[16 + i] = (sourceWpm[16 + i] & std::byte{0x11}) | expectedOldNative[i];
        const std::array<MemberReplacement, 1> pathing{{{"war3map.wpm", sourceWpm}}};
        const auto source = Take(MapArchiveSession::Open(Take(seeded.SaveCandidate(scenario / "source.w3x", pathing)).path));
        const auto sourceSha = Take(Sha256File(source.SourcePath()));
        MapDataEditSession edits(MapDocument::FromArchive(source));
        Check(edits.AttachTerrain(terrainBytes)); Check(edits.AttachPathing(sourceWpm));
        if (terrainAndTexture) {
            TerrainBrush brush; brush.kind = TerrainBrushKind::Raise; brush.radius = 0; brush.strength = 39;
            const TerrainStrokePoint point{before.header.offset[0] + 16 * 128, before.header.offset[1] + 16 * 128};
            Take(edits.EditTerrain(brush, std::span(&point, 1), false));
        }
        const auto expectedTerrain = Take(SerializeMapTerrain(*edits.Terrain()));
        Require(edits.IsDirty() == terrainAndTexture && (expectedTerrain != terrainBytes) == terrainAndTexture,
            "texture and object cases have no artificial terrain edit");
        auto output = edits.Replacements();
        if (objectOnly) output.push_back({"war3map.w3d", newW3d});
        else output.push_back({originalPath, newTexture});
        // 只用无对象原图独立捕获地形底层，再叠字面蓝色矩阵。
        // 这是实际资源/保存编排回归，不把生成器复用冒充原版算法或游戏验收。
        const std::array<MemberReplacement, 1> terrainOnly{{{"war3map.w3e", expectedTerrain}}};
        const auto terrainArchive = Take(MapArchiveSession::Open(Take(original.SaveCandidate(
            scenario / "expected-terrain-input.w3x", terrainOnly)).path));
        const auto finalTerrainRules = Take(TerrainPathingSave::CaptureNative(terrainArchive,
            *edits.Terrain(), *edits.Terrain(), game, kSaveBytes));
        const auto expectedNewNative = addLiteralFootprint(
            Take(finalTerrainRules.GenerateNative(*edits.Terrain(), kSaveBytes)), std::byte{0x08});
        auto expectedWpm = sourceWpm;
        for (std::size_t i = 0; i < expectedNewNative.size(); ++i)
            expectedWpm[16 + i] = (sourceWpm[16 + i] & std::byte{0x11}) | expectedNewNative[i];
        Require(expectedWpm != sourceWpm, "each final input case changes the complete expected static pathing output");
        auto prepared = Take(TerrainPathingSave::PrepareFinalForSave(source, &edits, game, {}, output));
        Require(prepared.has_value(), "real resource or native override change produces a final pathing plan");
        auto plan = std::move(*prepared);
        Check(plan.SaveGate()); Require(plan.RebuiltPathing() && plan.RequiresNativeVerification(),
            "actual final static input change performs complete source and final reconstruction");
        Check(plan.InstallPathingOutput(edits, output));
        Check(plan.VerifyForSave(edits, source.SourceSha256(), output));
        Check(plan.VerifyFrozenOutput(source.SourceSha256(), output)); Check(plan.VerifySourceInputs(source));
        const auto readback = Take(MapArchiveSession::Open(Take(source.SaveCandidate(scenario / "saved.w3x", output)).path));
        Check(plan.VerifyReadback(readback));
        Require(Take(readback.ReadMember("war3map.wpm")) == expectedWpm
            && Take(readback.ReadMember("war3map.w3e")) == expectedTerrain
            && Take(readback.ReadMember("war3map.doo")) == doo,
            "final pathing save reads back every expected WPM, W3E and DOO byte");
        Require(Take(readback.ReadMember(originalPath)) == (objectOnly ? oldTexture : newTexture),
            "texture-only and mixed changes save the exact new imported pixels");
        if (objectOnly) Require(Take(readback.ReadMember("war3map.w3d")) == newW3d
            && Take(readback.ReadMember(std::string(newPath))) == newTexture,
            "object-only save changes its exact native override and preserves the target import");
        Require(Take(readback.ReadMember("war3map.j")) == Take(source.ReadMember("war3map.j")),
            "final static input changes preserve the checked script bytes");
        Require(Take(Sha256File(source.SourcePath())) == sourceSha
            && Take(Sha256File(original.SourcePath())) == originalSha,
            "all actual resource final input saves preserve both original source archives");
        std::cout << "real_final_static_inputs_passed scenario=" << cases[index]
            << " source_sha256=" << sourceSha << " candidate_sha256=" << readback.SourceSha256()
            << " wpm_sha256=" << Take(Sha256(expectedWpm)) << " object=" << parent
            << " path=" << originalPath << " field=" << field.modification.fieldId
            << " entry=PrepareFinalForSave native_editor_verified=false game_verified=false\n";
    }
}
