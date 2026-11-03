#pragma once

void WorldModuleTests::TerrainModelReloadKeepsCommittedEdits_data() {
    QTest::addColumn<bool>("initialLoad");
    QTest::newRow("edit-before-terrain-completion") << true;
    QTest::newRow("retained-roster-after-animation-service-clear") << false;
}

void WorldModuleTests::TerrainModelReloadKeepsCommittedEdits() {
    QFETCH(bool, initialLoad);
    QTemporaryDir directory; QVERIFY(directory.isValid());
    auto members = PlacementPreviewMembers();
    for (auto& member : members) if (member.name == "Synthetic\\Arcane.mdx")
        member.data = AnimationTest::WithoutSyntheticConstantKmtf(MdxTest::BuildTestMdx());
    const auto mapPath = directory.filePath(QStringLiteral("terrain-model-reload.w3x"));
    QVERIFY(WriteVerifiedArchive(mapPath.toStdWString(), members).Succeeded());
    auto archive = MapArchiveSession::Open(mapPath.toStdWString()); QVERIFY(archive.Succeeded());
    QtProject::EditorProject project; QVERIFY(InstallPlacementCatalog(project));
    project.sourceArchivePath_ = mapPath.toStdWString();
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    QVERIFY(AttachTrackedResourcePlacement(project));
    const auto sourceSha = project.archiveSession_->SourceSha256();
    const auto original = project.mapEdits_->Terrain(); QVERIFY(original);
    BackendState state; activeBackend = &state;
    auto api = particleLayerBackend;
    EditorPresentationFixture<BackendState>::Enable(api, state.editor);
    api.submitEditorInstanceRoster = SubmitRoster;
    api.getEditorInstanceRosterResult = RosterResult;
    api.getEditorRosterCapabilities = [](War3RendererHandle, std::uint32_t* flags) -> std::int32_t {
        if (!flags) return 1;
        *flags = WAR3_RENDER_ROSTER_APPEND_ASSETS; return 0;
    };
    WorldModule module(project, &api); auto* page = module.Widget(); state.viewport = module.Viewport();
    page->resize(900, 650); page->show();
    auto* player = module.AnimationPlayer(); QVERIFY(player);
    if (initialLoad) {
        // 不分派Qt完成通知：即使CPU已结束，旧读取结果也只能在本笔提交后到达。
        module.Refresh(); // show原本排队首次读取；本行显式开始，仍不分派其完成通知。
        QVERIFY(module.PlaybackDiagnosticSnapshot()["terrain_load"].toObject()["worker_running"].toBool());
        MapData::TerrainBrush brush; brush.kind = MapData::TerrainBrushKind::Raise;
        brush.shape = MapData::TerrainBrushShape::Square; brush.size = 1; brush.strength = 32;
        const auto& vertex = original->vertices.front();
        const std::array<MapData::TerrainStrokePoint, 1> point{{{vertex.position[0], vertex.position[1]}}};
        QVERIFY(project.mapEdits_->EditTerrain(brush, point, false).Succeeded());
    } else {
        QTRY_VERIFY_WITH_TIMEOUT(player->Ready() && module.Viewport()->HasAnimationScene(), 10000);
        player->SetPlaying(false);
        QTRY_VERIFY(!player->Busy() && !player->InstancesBusy());
        QVERIFY(module.Viewport()->HasInstanceRosterSupport());
        // 地形重建会使服务失效而保留已显示的父场景；后续共享事务必须重建，不能用旧登记解引用空服务。
        player->Clear();
        QVERIFY(!player->Scene());
        QVERIFY(module.Viewport()->HasAnimationScene());
        MapData::RegionsDocument regions{5,
            {{-16, -16, 16, 16, "Owned reload region", 7, {}, "", {0, 0, 0, 255}}}, {}};
        QVERIFY(project.mapEdits_->EditRegions(regions).Succeeded());
    }
    const auto committedRevision = project.mapEdits_->Revision();
    const auto committedTerrain = project.mapEdits_->Terrain();
    QVERIFY(project.ObserveMapDataEdit("原创场景失效回归"));
    QtProject::PushMapDataEdit(project, QStringLiteral("原创地形与模型重建"), "原创场景失效回归");
    QCOMPARE(project.mapDataUndoStack_->count(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(player->Ready() && player->Scene()
        && player->Scene()->documentRevision == committedRevision, 10000);
    QTRY_VERIFY(!player->Busy() && !player->InstancesBusy());
    auto* terrainToggle = page->findChild<QPushButton*>("worldTerrainToggle"); QVERIFY(terrainToggle);
    QTRY_VERIFY(terrainToggle->isEnabled());
    QCOMPARE(project.mapEdits_->Terrain(), committedTerrain);
    QCOMPARE(project.mapEdits_->Revision(), committedRevision);
    if (initialLoad) QVERIFY(committedTerrain != original);
    else QCOMPARE(project.mapEdits_->Snapshot().Regions()->entries.size(), std::size_t{1});
    project.mapDataUndoStack_->undo();
    QTRY_VERIFY(player->Ready() && player->Scene()
        && player->Scene()->documentRevision == project.mapEdits_->Revision());
    project.mapDataUndoStack_->redo();
    QTRY_VERIFY(player->Ready() && player->Scene()
        && player->Scene()->documentRevision == project.mapEdits_->Revision());
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
    QCOMPARE(project.archiveSession_->SourceSha256(), sourceSha);
}
