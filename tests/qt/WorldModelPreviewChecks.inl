// 只复用本测试文件的显式提交后端：验证真实属性卡的动作选择、时钟、求值和提交，
// 不将 stub 的填充像素称为 GPU 或游戏画面。真实模型只从环境变量取得并复制到临时目录。
namespace {
std::int32_t WAR3_RENDER_CALL PreviewSkinStatus(War3RendererHandle handle, War3RenderSkinStatusV2* status) {
    const auto bytes = status->structSize;
    const auto result = SkinStatus(handle, status);
    if (bytes >= sizeof(War3RenderSkinColorCapabilitiesV2))
        reinterpret_cast<War3RenderSkinColorCapabilitiesV2*>(status)->colorFormat = WAR3_RENDER_STATIC_MESH_RGB_V1;
    if (bytes >= sizeof(War3RenderSkinDynamicColorCapabilitiesV2))
        reinterpret_cast<War3RenderSkinDynamicColorCapabilitiesV2*>(status)->dynamicColorFormat = WAR3_RENDER_DYNAMIC_MESH_RGB_V1;
    if (bytes >= sizeof(War3RenderSkinMaterialCapabilitiesV2))
        reinterpret_cast<War3RenderSkinMaterialCapabilitiesV2*>(status)->layerAlphaFormat = WAR3_RENDER_DYNAMIC_LAYER_ALPHA_V1;
    if (bytes >= sizeof(War3RenderSkinMaterialStateCapabilitiesV2))
        reinterpret_cast<War3RenderSkinMaterialStateCapabilitiesV2*>(status)->materialStateFormat = WAR3_RENDER_DYNAMIC_MATERIAL_STATE_V1;
    return result;
}
War3RenderPluginApiV2 PreviewBackend(bool particles) {
    auto api = backend;
    api.submitModelScene = ModelScene;
    api.submitSkinnedModelScene = SkinScene;
    api.updateSkinMatrices = SkinUpdate;
    api.getSkinStatus = PreviewSkinStatus;
    api.captureSkinnedFrameRgba = SkinCapture;
    if (particles) {
        api.submitParticleModelScene = ParticleScene;
        api.updateParticleModelFrame = ParticleUpdate;
        api.captureParticleFrameRgba = ParticleCapture;
    }
    return api;
}
WorldTerrainSources PreviewFixtureArchive(const std::filesystem::path& path, const std::vector<std::byte>& model) {
    const auto parsed = Render::ParseMdx(model);
    if (!parsed.Succeeded()) throw std::runtime_error("original preview model parse failed");
    std::vector<Mpq::MpqBuildFile> files{{"preview-model.mdx", model}};
    for (const auto& texture : parsed.value->textures) if (!texture.path.empty())
        files.push_back({texture.path, AnimationParticleContinuityTest::Texture()});
    const auto written = Mpq::WriteMpqArchive(path, files);
    if (!written.Succeeded()) throw std::runtime_error("original preview archive write failed");
    const auto digest = Sha256File(path);
    if (!digest.Succeeded()) throw std::runtime_error("original preview archive digest failed");
    WorldTerrainSources sources; sources.archivePath = path; sources.archiveSha = *digest.value;
    for (const auto& file : files) sources.entries.push_back({file.name, file.name, {}});
    return sources;
}
std::vector<std::byte> PreviewReadBytes(const std::filesystem::path& path) {
    QFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 32 * 1024 * 1024)
        throw std::runtime_error("preview model must be a readable file within 32 MiB");
    const auto data = file.readAll();
    if (data.size() != file.size()) throw std::runtime_error("preview model short read");
    const auto* begin = reinterpret_cast<const std::byte*>(data.constData());
    return {begin, begin + data.size()};
}
}

void WorldViewportTests::ModelPreviewUsesGameOrderAndSubmitsMotion() {
    using namespace Render;
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto model = AnimationPreviewFixture::InverseKeys();
    const auto sources = PreviewFixtureArchive(std::filesystem::path(directory.path().toStdWString()) / L"fixture.w3x", model);
    // 严格默认合同仍拒绝原创逆序轨迹；不能靠改默认或放宽旧测试来让 UI 通过。
    AnimationInstanceSpec spec; spec.identity = {1, 1}; spec.hostKey = "strict-source-order";
    spec.model.modelPath = "preview-model.mdx"; spec.elapsedMs = 500;
    auto strict = ModelAnimationService::Create({1, 0, sources.archiveSha}, std::span(&spec, 1),
        [&](auto path, auto) { return Result<AnimationResource>::Success({path == "preview-model.mdx" ? model :
            AnimationParticleContinuityTest::Texture(), "original-fixture", std::string(64, 'a')}); });
    bool rejected = !strict.Succeeded();
    if (strict.Succeeded()) rejected = !(*strict.value)->Prepare(nullptr).Succeeded();
    QVERIFY(rejected);
    QCOMPARE(AnimationLimits{}.playbackPolicy, AnimationPlaybackPolicy::StrictReplay);

    BackendState state; active = &state;
    auto api = PreviewBackend(false); // 即使没有粒子插件，切换动作也必须从 0 开始。
    WorldModelPreview preview(nullptr, &api); preview.resize(400, 280);
    bool current = true;
    ModelInstanceRequest request; request.modelPath = "preview-model.mdx";
    preview.ShowSelection(QStringLiteral("original-preview"), 1, 0, {}, "original-instance", request,
        Result<WorldTerrainSources>::Success(sources), [&] { return current; });
    auto* player = preview.findChild<WorldAnimationPlayer*>(); QVERIFY(player);
    auto* actions = preview.findChild<QComboBox*>(QStringLiteral("worldUnitPreviewAction")); QVERIFY(actions);
    auto* play = preview.findChild<QPushButton*>(QStringLiteral("worldUnitPreviewPlayPause")); QVERIFY(play);
    auto* step = preview.findChild<QPushButton*>(QStringLiteral("worldUnitPreviewStep")); QVERIFY(step);
    auto* viewport = preview.findChild<WorldViewport*>(); QVERIFY(viewport);
    auto* message = preview.findChild<QLabel*>(QStringLiteral("worldUnitPreviewMessage")); QVERIFY(message);
    QTRY_VERIFY_WITH_TIMEOUT(player->Ready(), 5000);
    player->SetPlaying(false); QTRY_VERIFY(!player->Busy());
    QCOMPARE(actions->count(), 2);
    actions->setCurrentIndex(1); QTRY_VERIFY(player->Frame()->states.front().sequenceIndex == 1 && !player->Busy());
    QCOMPARE(player->Frame()->states.front().elapsedMs, std::uint64_t{0});
    actions->setCurrentIndex(0); QTRY_VERIFY(player->Frame()->states.front().sequenceIndex == 0 && !player->Busy());
    QCOMPARE(player->Frame()->states.front().elapsedMs, std::uint64_t{0});
    const auto start = player->Frame(); const auto uploads = state.matrixUploads;
    QVERIFY(step->isEnabled()); step->click();
    QTRY_VERIFY(player->Frame()->contentVersion > start->contentVersion && !player->Busy());
    QCOMPARE(player->Frame()->states.front().elapsedMs, std::uint64_t{33});
    QVERIFY(!player->Frame()->poses.front().hidden);
    QVERIFY(player->Frame()->poses.front().matrixHash != start->poses.front().matrixHash);
    QVERIFY(state.matrixUploads > uploads);
    QCOMPARE(viewport->AnimationTarget(), player->Frame());
    play->click(); QTRY_VERIFY(player->Frame()->states.front().elapsedMs > 66);
    QVERIFY(player->Playing()); play->click(); QTRY_VERIFY(!player->Busy());
    QVERIFY(!player->Playing());

    actions->setCurrentIndex(1); QTRY_VERIFY(player->Frame()->states.front().sequenceIndex == 1 && !player->Busy());
    AnimationEdit nearEnd; nearEnd.identity = player->Frame()->states.front().identity; nearEnd.elapsedMs = 999;
    player->Edit(nearEnd); QTRY_VERIFY(player->Frame()->states.front().elapsedMs == 999 && !player->Busy());
    step->click(); QTRY_VERIFY(player->Frame()->states.front().elapsedMs == 1000 && !player->Busy());
    QVERIFY(!player->Frame()->states.front().playing);
    play->click(); QTRY_VERIFY(player->Frame()->states.front().elapsedMs < 1000);
    QVERIFY(player->Playing()); play->click(); QTRY_VERIFY(!player->Busy());
    current = false; play->click(); QVERIFY(!player->Ready());
    QVERIFY(!message->text().isEmpty());
    QCOMPARE(Sha256File(sources.archivePath).value, std::optional{sources.archiveSha});
}

void WorldViewportTests::ModelPreviewRealSequences() {
    using namespace Render;
    const auto input = qEnvironmentVariable("FORGE_PREVIEW_REAL_MODEL");
    if (input.isEmpty()) QSKIP("FORGE_PREVIEW_REAL_MODEL is unset; real preview input is opt-in");
    QTemporaryDir directory; QVERIFY(directory.isValid());
    const auto root = std::filesystem::path(directory.path().toStdWString());
    const auto original = std::filesystem::path(input.toStdWString());
    const auto originalSha = Sha256File(original); QVERIFY(originalSha.Succeeded());
    const auto copied = root / L"preview-model.mdx";
    QVERIFY(std::filesystem::copy_file(original, copied));
    const auto model = PreviewReadBytes(copied);
    QCOMPARE(Sha256(model).value, originalSha.value);
    const auto parsed = ParseMdx(model); QVERIFY(parsed.Succeeded());
    QVERIFY(!parsed.value->sequences.empty());
    bool expectedOk = false;
    const auto expected = qEnvironmentVariable("FORGE_PREVIEW_EXPECT_SEQUENCES").toUInt(&expectedOk);
    if (expectedOk) QCOMPARE(parsed.value->sequences.size(), static_cast<std::size_t>(expected));
    WorldTerrainSources sources;
    const auto mapInput = qEnvironmentVariable("FORGE_PREVIEW_REAL_MAP");
    std::optional<std::string> mapSha;
    if (!mapInput.isEmpty()) {
        const auto originalMap = std::filesystem::path(mapInput.toStdWString());
        const auto digest = Sha256File(originalMap); QVERIFY(digest.Succeeded()); mapSha = *digest.value;
        sources.archivePath = root / L"resource-copy.w3x";
        QVERIFY(std::filesystem::copy_file(originalMap, sources.archivePath));
        auto archive = Mpq::MpqArchive::OpenReadOnly(sources.archivePath); QVERIFY(archive.Succeeded());
        for (const auto& name : archive.value->ListFiles()) sources.entries.push_back({name, name, {}});
        if (archive.value->HasFile("war3map.w3e")) {
            const auto bytes = archive.value->ReadFileBounded("war3map.w3e", 64ull * 1024 * 1024); QVERIFY(bytes.Succeeded());
            const auto terrain = MapData::ParseMapTerrain(*bytes.value); QVERIFY(terrain.Succeeded());
            sources.mainTileset = terrain.value->Snapshot()->header.mainTileset;
        }
        sources.archiveSha = *mapSha;
        QCOMPARE(Sha256File(sources.archivePath).value, mapSha);
    } else {
        sources.archivePath = root / L"resources.w3x";
        const std::vector<Mpq::MpqBuildFile> files{{"preview-model.mdx", model}};
        QVERIFY(Mpq::WriteMpqArchive(sources.archivePath, files).Succeeded());
        const auto digest = Sha256File(sources.archivePath); QVERIFY(digest.Succeeded()); sources.archiveSha = *digest.value;
    }
    sources.entries.push_back({"preview-model.mdx", {}, copied});
    sources.gameRoot = std::filesystem::path(qEnvironmentVariable("FORGE_WORLD_GAME_ROOT").toStdWString());
    BackendState state; active = &state;
    auto api = PreviewBackend(true);
    WorldModelPreview preview(nullptr, &api); preview.resize(420, 300);
    ModelInstanceRequest request; request.modelPath = "preview-model.mdx";
    preview.ShowSelection(QStringLiteral("local-model-copy"), 1, 0, {}, "real-model-copy", request,
        Result<WorldTerrainSources>::Success(sources), [] { return true; });
    auto* player = preview.findChild<WorldAnimationPlayer*>(); QVERIFY(player);
    auto* actions = preview.findChild<QComboBox*>(QStringLiteral("worldUnitPreviewAction")); QVERIFY(actions);
    auto* play = preview.findChild<QPushButton*>(QStringLiteral("worldUnitPreviewPlayPause")); QVERIFY(play);
    auto* viewport = preview.findChild<WorldViewport*>(); QVERIFY(viewport);
    auto* message = preview.findChild<QLabel*>(QStringLiteral("worldUnitPreviewMessage")); QVERIFY(message);
    QTRY_VERIFY_WITH_TIMEOUT(player->Ready(), 15000);
    player->SetPlaying(false); QTRY_VERIFY(!player->Busy());
    QCOMPARE(actions->count(), static_cast<int>(parsed.value->sequences.size()));
    std::size_t movedSequences = 0;
    for (std::uint32_t index = 0; index < parsed.value->sequences.size(); ++index) {
        const auto& sequence = parsed.value->sequences[index];
        QVERIFY2(sequence.Usable(), "declared real sequence is invalid");
        const auto item = actions->findData(QVariant::fromValue(index)); QVERIFY(item >= 0);
        if (actions->currentIndex() == item && actions->count() > 1) {
            actions->setCurrentIndex((item + 1) % actions->count());
            QTRY_VERIFY(!player->Busy());
        }
        actions->setCurrentIndex(item);
        QTRY_VERIFY(player->Frame()->states.front().sequenceIndex == index && !player->Busy());
        const auto first = player->Frame(); const auto submitted = state.matrixUploads;
        play->click();
        QTRY_VERIFY_WITH_TIMEOUT(player->Frame()->contentVersion > first->contentVersion &&
            player->Frame()->states.front().globalMs > first->states.front().globalMs, 5000);
        player->SetPlaying(false); QTRY_VERIFY(!player->Busy());
        QVERIFY2(player->Ready(), qPrintable(message->text()));
        QVERIFY(state.matrixUploads > submitted);
        QCOMPARE(viewport->AnimationTarget(), player->Frame());
        bool moved = player->Frame()->poses.front().matrixHash != first->poses.front().matrixHash;
        for (const auto elapsed : {static_cast<std::uint64_t>(sequence.end - sequence.start) / 2,
            static_cast<std::uint64_t>(sequence.end - sequence.start)}) {
            AnimationEdit seek; seek.identity = first->states.front().identity; seek.elapsedMs = elapsed; seek.globalMs = elapsed;
            player->Edit(seek);
            QTRY_VERIFY(player->Frame()->states.front().elapsedMs == elapsed && !player->Busy());
            QVERIFY2(player->Ready(), qPrintable(message->text()));
            const auto& pose = player->Frame()->poses.front();
            QVERIFY2(!pose.hidden && pose.animated && !pose.staticForBudget, qPrintable(message->text()));
            moved |= pose.matrixHash != first->poses.front().matrixHash;
            QCOMPARE(state.skinContent, player->Frame()->contentVersion);
        }
        movedSequences += moved;
        qInfo().noquote() << "preview_sequence" << index << QString::fromStdString(sequence.name)
            << "clock_advanced=1 pose_samples=3 submitted=1 matrix_changed=" << moved
            << "nonloop=" << (sequence.flags == 1) << "gpu_evidence=0";
    }
    QVERIFY2(movedSequences != 0, "all sampled matrices stayed static; clock-only success is insufficient");
    qInfo() << "preview_sequences=" << actions->count() << "moving_sequences=" << movedSequences
        << "untextured_layers=" << player->Scene()->geometry.untexturedLayers;
    preview.ClearSelection(); QTRY_VERIFY(!player->Busy());
    QCOMPARE(Sha256File(original).value, originalSha.value);
    if (mapSha) QCOMPARE(Sha256File(std::filesystem::path(mapInput.toStdWString())).value, mapSha);
}
