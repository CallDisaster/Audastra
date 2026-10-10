#pragma once

#include "WorldPlacementPreview.h"
#include "WorldFrameRecorder.h"
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <functional>
#include <map>
#include <set>

// 显式后台实图探针。只向本测试创建的控件投递Qt事件，不移动系统鼠标、
// 不激活桌面窗口、不读屏；PNG均来自生产renderer的同次完成帧RGBA。
void NativeWorkbenchTests::NativeWorldPlacementBackgroundEvidence() {
    using namespace War3Map;
    using namespace War3Map::Modules;
    const auto evidence = qEnvironmentVariable("FORGE_WORLD_PLACEMENT_EVIDENCE_DIR");
    if (evidence.isEmpty()) QSKIP("Explicit production placement evidence probe is not enabled");
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("windows"));
    const auto map = qEnvironmentVariable("FORGE_R08_WORLD_MAP");
    const auto ydwe = qEnvironmentVariable("FORGE_YDWE_ROOT"), game = qEnvironmentVariable("FORGE_WAR3_ROOT");
    QVERIFY2(!map.isEmpty() && !ydwe.isEmpty() && !game.isEmpty(), "Supply an independent map copy and resource roots");
    QVERIFY(QDir().mkpath(evidence));
    const auto receiptPath = QDir(evidence).filePath(QStringLiteral("world-placement-receipt.json"));
    const auto eventsPath = QDir(evidence).filePath(QStringLiteral("world-placement-events.jsonl"));
    QVERIFY2(!QFileInfo::exists(receiptPath) && !QFileInfo::exists(eventsPath), "Preserving previous evidence");
    QFile events(eventsPath);
    QVERIFY(events.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QElapsedTimer probeClock; probeClock.start();
    QString stage = QStringLiteral("source-hash"), failure;
    bool completed = false, sourceUnchanged = false;
    QJsonArray samples;
    QJsonObject receipt{{"scope", "windows-production-world-placement-background"}, {"source_copy", map},
        {"desktop_input_used", false}, {"screen_capture_used", false}, {"archive_save_requested", false},
        {"visible_ten_minute_acceptance", false}, {"game_runtime_verified", false},
        {"quality", "PreviewApproximate"}, {"model_pixel_semantic_review", "pending PNG review"}};
    MainWindow window(MainWindow::StartupMode::HeadlessProbe);
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    window.resize(1600, 900);
    QPointer<WorldModule> observedWorld;
    QPointer<WorldViewport> observedViewport;
    QPointer<WorldPlacementPreview> observedPreview;
    std::unique_ptr<WorldFrameRecorder> recorder;
    std::string sourceSha, sourceAfterSha;
    const auto number = [](std::uint64_t value) { return QString::number(value); };
    const auto writeEvent = [&](QJsonObject value) {
        value.insert("stage", stage); value.insert("steady_ns", number(WorldInstanceNowNs()));
        value.insert("probe_ms", probeClock.elapsed());
        const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact) + '\n';
        if (events.write(bytes) != bytes.size() || !events.flush()) {
            failure = QStringLiteral("Cannot preserve placement evidence events"); return false;
        }
        return true;
    };
    struct FinalizeEvidence {
        std::function<void()> finish;
        ~FinalizeEvidence() { finish(); }
    } finalize{[&] {
        if (recorder && recorder->Running()) recorder->Stop(completed ? QStringLiteral("后台摆放探针结束") : stage + QStringLiteral("失败"));
        if (sourceAfterSha.empty()) {
            const auto after = Sha256File(NativePath(map));
            if (after.Succeeded()) sourceAfterSha = *after.value;
        }
        sourceUnchanged = !sourceSha.empty() && sourceAfterSha == sourceSha;
        if (!completed && failure.isEmpty()) failure = QStringLiteral("Assertion failed in stage: ") + stage;
        receipt.insert("source_sha256", QString::fromStdString(sourceSha));
        receipt.insert("source_after_sha256", QString::fromStdString(sourceAfterSha));
        receipt.insert("source_unchanged", sourceUnchanged);
        receipt.insert("functional_sequence_completed", completed);
        receipt.insert("completed", completed && !QTest::currentTestFailed() && failure.isEmpty() && sourceUnchanged);
        receipt.insert("last_stage", stage); receipt.insert("failure", failure);
        receipt.insert("elapsed_ms", probeClock.elapsed()); receipt.insert("samples", samples);
        receipt.insert("qt_test_failed", QTest::currentTestFailed());
        if (observedWorld) receipt.insert("playback_diagnostics", observedWorld->PlaybackDiagnosticSnapshot());
        if (observedViewport) {
            receipt.insert("renderer", observedViewport->BackendDiagnosticSnapshot());
            receipt.insert("viewport_status", observedViewport->StatusText());
            receipt.insert("hover_status", observedViewport->HoverText());
        }
        if (observedPreview) receipt.insert("preview_final", QJsonObject{{"busy", observedPreview->Busy()},
            {"playback_active", observedPreview->PlaybackActive()}, {"model_submitted", observedPreview->ModelSubmitted()}});
        QJsonArray diagnostics;
        if (window.project_ && window.project_->diagnosticsModel_) {
            const auto* model = window.project_->diagnosticsModel_;
            for (int row = std::max(0, model->rowCount() - 64); row < model->rowCount(); ++row) {
                QJsonArray cells;
                for (int column = 0; column < std::min(4, model->columnCount()); ++column)
                    cells.append(model->index(row, column).data().toString().left(4096));
                diagnostics.append(cells);
            }
            receipt.insert("diagnostic_rows_omitted", std::max(0, model->rowCount() - 64));
        }
        receipt.insert("diagnostics_tail", diagnostics);
        if (!WriteText(receiptPath, QJsonDocument(receipt).toJson(QJsonDocument::Indented)))
            qWarning() << "Could not write placement failure/final receipt" << receiptPath;
        (void)writeEvent({{"event", "finish"}, {"completed", completed}, {"source_unchanged", sourceUnchanged}, {"failure", failure}});
        window.hide(); // 只清理本函数创建且不接焦点的窗口，不发送关闭/保存给其他实例。
    }};
    const auto require = [&](bool condition, const QString& detail) {
        if (!condition) { failure = detail; (void)writeEvent({{"event", "failure"}, {"detail", detail}}); }
        return condition;
    };
    const auto phase = [&](const QString& value) {
        stage = value; if (recorder) recorder->SetPhase(value);
        return writeEvent({{"event", "phase"}});
    };
    const auto waitUntil = [&](const std::function<bool()>& predicate, int milliseconds) {
        QElapsedTimer wait; wait.start();
        for (;;) {
            if (predicate()) return true;
            if (wait.elapsed() >= milliseconds || probeClock.elapsed() >= 240000) return false;
            QTest::qWait(10);
        }
    };
    const auto sourceHash = Sha256File(NativePath(map));
    QVERIFY2(require(sourceHash.Succeeded(), QStringLiteral("Cannot hash input copy")), "Input hash failed");
    sourceSha = *sourceHash.value;
    QVERIFY(phase(QStringLiteral("load-map-and-world")));
    QVERIFY2(require(window.UseUiDataRoot(NativePath(ydwe)), QStringLiteral("UseUiDataRoot failed")), "UI root failed");
    QVERIFY2(require(window.OpenGameResources(NativePath(game)), QStringLiteral("OpenGameResources failed")), "Game root failed");
    QVERIFY2(require(window.OpenMapArchivePath(NativePath(map)), QStringLiteral("OpenMapArchivePath failed")), "Map open failed");
    QVERIFY(window.project_->mapEdits_ && window.project_->mapDataUndoStack_);
    auto* world = dynamic_cast<WorldModule*>(window.modules_.Find("world")); QVERIFY(world); observedWorld = world;
    auto* page = world->Widget(); auto* viewport = world->Viewport(); QVERIFY(page && viewport); observedViewport = viewport;
    auto* player = world->AnimationPlayer(); QVERIFY(player);
    // 摆放服务由首次真正的资源选择惰性创建；不能为了测试预先开启摆放工具。
    WorldPlacementPreview* preview = nullptr;
    QRect desktop;
    QJsonArray screens;
    for (const auto* screen : QGuiApplication::screens()) {
        desktop = desktop.united(screen->geometry());
        const auto r = screen->geometry(); screens.append(QJsonObject{{"x", r.x()}, {"y", r.y()}, {"w", r.width()}, {"h", r.height()}});
    }
    QVERIFY2(require(desktop.isValid() && desktop.right() < 1000000, QStringLiteral("Cannot establish screen bounds")), "Invalid screen geometry");
    window.move(desktop.right() + window.width() + 128, desktop.top());
    const auto outsideDesktop = [&] {
        for (const auto* screen : QGuiApplication::screens()) if (window.frameGeometry().intersects(screen->geometry())) return false;
        return !window.frameGeometry().intersects(desktop);
    };
    QVERIFY2(require(outsideDesktop(), QStringLiteral("Owned window was not positioned outside screen union before show")), "Unsafe probe position");
    window.ExecuteCommand("module.world"); window.show();
    QTest::qWait(30);
    if (!outsideDesktop() || window.isActiveWindow()) window.hide();
    QVERIFY2(require(outsideDesktop(), QStringLiteral("Platform moved owned probe into desktop; immediately hidden")), "Probe entered screen geometry");
    QVERIFY2(require(!window.isActiveWindow(), QStringLiteral("Non-activating owned window unexpectedly became active")), "Probe unexpectedly activated");
    receipt.insert("screens", screens);
    receipt.insert("owned_window_geometry", QJsonObject{{"x", window.frameGeometry().x()}, {"y", window.frameGeometry().y()},
        {"w", window.frameGeometry().width()}, {"h", window.frameGeometry().height()}});
    receipt.insert("owned_window_outside_all_screens", true);
    const auto ready = [&] { return viewport->isVisible() && viewport->RendererReady() && player->Ready()
        && player->Playing() && player->Frame() && viewport->VertexCount() != 0; };
    QVERIFY2(require(waitUntil(ready, 60000), QStringLiteral("World/player not ready within 60 seconds")), "World load failed or timed out");
    recorder = std::make_unique<WorldFrameRecorder>(*world);
    QVERIFY2(require(recorder->Start(QDir(evidence).filePath(QStringLiteral("measurement")), map, true),
        QStringLiteral("Common background frame recorder could not start")), "Frame recorder failed");
    const auto baselineFrame = player->Frame(); const auto baselineScene = player->Scene();
    QVERIFY(baselineFrame && baselineScene);
    std::set<std::pair<std::uint64_t, std::uint64_t>> originalIdentities;
    for (const auto& spec : baselineScene->specs) originalIdentities.emplace(spec.identity.value, spec.identity.generation);
    const auto originalDocument = window.project_->mapEdits_->Snapshot();
    QVERIFY(originalDocument.Units()); const auto initialUnits = originalDocument.Units()->units.size();
    const auto initialUndo = window.project_->mapDataUndoStack_->index();
    const auto sendMouse = [](QWidget* widget, QEvent::Type type, QPointF point,
        Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
        QMouseEvent event(type, point, QPointF(widget->mapToGlobal(point.toPoint())), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    };
    const auto click = [&](QWidget* widget, QPointF point, Qt::MouseButton button = Qt::LeftButton) {
        sendMouse(widget, QEvent::MouseButtonPress, point, button, button);
        sendMouse(widget, QEvent::MouseButtonRelease, point, button, Qt::NoButton);
    };
    auto* select = page->findChild<QPushButton*>(QStringLiteral("worldSelectTool"));
    auto* place = page->findChild<QPushButton*>(QStringLiteral("worldPlaceToggle"));
    auto* search = page->findChild<QLineEdit*>(QStringLiteral("worldAssetSearch"));
    auto* rows = page->findChild<QListWidget*>(QStringLiteral("worldAssetRows"));
    auto* assetDock = page->findChild<QDockWidget*>(QStringLiteral("worldAssetDock"));
    QVERIFY(select && place && search && rows && assetDock);
    QVERIFY2(!assetDock->isFloating(), "Probe must not open a floating resource window");
    assetDock->show(); assetDock->raise(); // 激活真实资源标签；不预先开启摆放。
    click(select, select->rect().center());
    QVERIFY2(require(select->isChecked() && !viewport->Placing(), QStringLiteral("Probe must begin in Select tool")), "Selection tool did not activate");
    if (auto* collapse = page->findChild<QToolButton*>(QStringLiteral("worldAssetCollapse")); collapse && collapse->isChecked())
        click(collapse, collapse->rect().center());
    if (auto* tabs = page->findChild<QTabBar*>(QStringLiteral("worldAssetPages"))) tabs->setCurrentIndex(0);
    for (const auto* name : {"worldAssetKind", "worldAssetSource", "worldAssetGroup", "worldAssetCatalogSet", "worldAssetCategory"})
        if (auto* combo = page->findChild<QComboBox*>(QLatin1String(name))) combo->setCurrentIndex(0);
    search->setText(QStringLiteral("hars"));
    const auto sanctumItem = [&]() -> QListWidgetItem* {
        for (int i = 0; i < rows->count(); ++i) if (rows->item(i)->data(Qt::UserRole).toString() == QLatin1String("hars")) return rows->item(i);
        return nullptr;
    };
    const auto hasSanctum = waitUntil([&] { return sanctumItem() != nullptr; }, 15000);
    QVERIFY(writeEvent({{"event", "resource_rows"}, {"count", rows->count()}, {"visible", rows->isVisible()},
        {"search", search->text()}, {"hars_found", hasSanctum}}));
    QVERIFY2(require(hasSanctum, QStringLiteral("Arcane Sanctum hars absent from actual resource rows")), "No hars row");
    const auto findGround = [&]() -> std::optional<QPointF> {
        for (const auto fraction : {QPointF{.5,.5}, QPointF{.5,.65}, QPointF{.35,.5}, QPointF{.65,.5}, QPointF{.5,.35}, QPointF{.35,.65}, QPointF{.65,.65}}) {
            const QPointF point{fraction.x() * viewport->width(), fraction.y() * viewport->height()};
            sendMouse(viewport, QEvent::MouseMove, point);
            if (waitUntil([&] { return viewport->HoverCoordinate().has_value(); }, 500)) return point;
        }
        return std::nullopt;
    };
    QVERIFY(phase(QStringLiteral("camera-and-ground")));
    auto groundPoint = findGround();
    QVERIFY2(require(groundPoint.has_value(), QStringLiteral("No real completed-frame terrain hit at fixed test points")), "No ground point");
    const auto ground = viewport->HoverCoordinate()->position;
    auto camera = viewport->Camera(); camera.targetX = static_cast<float>(ground[0]); camera.targetY = static_cast<float>(ground[1]);
    camera.targetZ = static_cast<float>(ground[2]); camera.distance = 1600;
    const auto setCamera = viewport->SetCamera(camera, viewport->DocumentRevision());
    QVERIFY(setCamera.Succeeded()); QTest::qWait(100);
    groundPoint = findGround(); QVERIFY(groundPoint);
    receipt.insert("ground", QJsonObject{{"x", viewport->HoverCoordinate()->position[0]}, {"y", viewport->HoverCoordinate()->position[1]},
        {"z", viewport->HoverCoordinate()->position[2]}});
    std::uint64_t lastReadbackNs = 0;
    const auto capture = [&]() -> std::optional<Render::CapturedFrameRgba> {
        const auto began = WorldInstanceNowNs(), expectedRevision = viewport->DocumentRevision();
        // 附加预览层与主实例票据独立完成；刚提交后可以暂时没有匹配的同帧读回。
        // 仅重试明确的异步未就绪码，仍由生产接口验证每次返回的完整帧/姿势/层身份。
        // 重试与等待计入总耗时，不能把过期帧或占位图用于100/200ms出口。
        for (unsigned attempt = 1; attempt <= 12; ++attempt) {
            const auto attemptStarted = WorldInstanceNowNs();
            auto result = viewport->CaptureCompletedFrame(expectedRevision, Render::kMaximumFrameCaptureBytes);
            const auto attemptNs = WorldInstanceNowNs() - attemptStarted;
            lastReadbackNs = WorldInstanceNowNs() - began;
            if (result.Succeeded()) {
                if (attempt > 1 && !writeEvent({{"event", "capture_retry_completed"}, {"attempt", int(attempt)},
                    {"expected_revision", number(expectedRevision)}, {"elapsed_ns", number(lastReadbackNs)},
                    {"completed_frame", number(result.value->view.identity.frameCount)}})) return std::nullopt;
                return std::move(*result.value);
            }
            QJsonArray diagnostics;
            bool transient = !result.diagnostics.empty();
            for (const auto& diagnostic : result.diagnostics) {
                QJsonObject detail{{"severity", int(diagnostic.severity)}, {"subsystem", QString::fromStdString(diagnostic.subsystem)},
                    {"code", QString::fromStdString(diagnostic.code)}, {"message", QString::fromStdString(diagnostic.message)}};
                if (diagnostic.origin) {
                    const auto path = diagnostic.origin->source.generic_u8string();
                    detail.insert("origin", QJsonObject{{"source", QString::fromUtf8(reinterpret_cast<const char*>(path.data()), qsizetype(path.size()))},
                        {"offset", number(diagnostic.origin->offset)}, {"length", number(diagnostic.origin->length)},
                        {"fingerprint", QString::fromStdString(diagnostic.origin->fingerprint)}});
                }
                diagnostics.append(detail);
                transient &= diagnostic.code == "WORLD_CAPTURE_LAYER_STALE" || diagnostic.code == "WORLD_CAPTURE_PRESENTATION_PENDING";
            }
            if (!result.diagnostics.empty()) receipt.insert("last_capture_diagnostic", QString::fromStdString(result.diagnostics.front().code));
            receipt.insert("last_capture_diagnostics", diagnostics);
            const bool retry = transient && attempt < 12 && lastReadbackNs < 500000000 && probeClock.elapsed() < 240000;
            if (!writeEvent({{"event", "capture_failed_attempt"}, {"attempt", int(attempt)}, {"expected_revision", number(expectedRevision)},
                {"attempt_ns", number(attemptNs)}, {"elapsed_ns", number(lastReadbackNs)}, {"retry", retry}, {"diagnostics", diagnostics}})
                || !retry) return std::nullopt;
            QTest::qWait(8);
        }
        return std::nullopt;
    };
    const auto saveFrame = [&](const Render::CapturedFrameRgba& frame, const QString& name) {
        const auto path = QDir(evidence).filePath(name + QStringLiteral(".png"));
        if (QFileInfo::exists(path) || frame.view.physicalWidth == 0 || frame.view.physicalHeight == 0
            || frame.rgba.size() != std::uint64_t(frame.view.physicalWidth) * frame.view.physicalHeight * 4) return false;
        const QImage image(reinterpret_cast<const uchar*>(frame.rgba.data()), int(frame.view.physicalWidth), int(frame.view.physicalHeight),
            int(frame.view.physicalWidth * 4), QImage::Format_RGBA8888);
        return image.save(path);
    };
    const auto frameJson = [&](const Render::CapturedFrameRgba& frame) {
        QJsonObject result{{"frame", number(frame.view.identity.frameCount)}, {"surface_epoch", number(frame.view.identity.surfaceEpoch)},
            {"view_epoch", number(frame.view.identity.viewEpoch)}, {"render_scene_epoch", number(frame.view.identity.sceneEpoch)},
            {"viewport_document_revision", number(frame.documentRevision)}, {"width", int(frame.view.physicalWidth)}, {"height", int(frame.view.physicalHeight)},
            {"layers_revision", number(frame.modelLayersRevision)}, {"layer_count", int(frame.modelLayers.size())}};
        if (frame.animation) {
            result.insert("world_scene", number(frame.animation->sceneVersion)); result.insert("world_content", number(frame.animation->contentVersion));
            result.insert("map_revision", number(frame.animation->documentRevision));
        }
        const auto digest = Sha256(frame.rgba); if (digest.Succeeded()) result.insert("pixel_sha256", QString::fromStdString(*digest.value));
        return result;
    };
    auto before = capture();
    QVERIFY2(require(before.has_value() && saveFrame(*before, QStringLiteral("00-before-preview")), QStringLiteral("Cannot capture initial completed frame")), "Initial RGBA capture failed");
    std::uint64_t coldScene = 0;
    bool hotWithin100ms = false;
    for (int pass = 0; pass < 2; ++pass) {
        QVERIFY(phase(pass ? QStringLiteral("hot-resource-click-preview") : QStringLiteral("cold-resource-click-preview")));
        QEvent leave(QEvent::Leave); QApplication::sendEvent(viewport, &leave);
        auto* item = sanctumItem(); QVERIFY(item);
        rows->scrollToItem(item); rows->doItemsLayout();
        const auto itemRect = rows->visualItemRect(item);
        QVERIFY2(require(itemRect.isValid() && rows->viewport()->rect().intersects(itemRect), QStringLiteral("Actual hars row is not click-accessible")), "hars row not visible");
        const auto started = WorldInstanceNowNs();
        click(rows->viewport(), itemRect.center());
        preview = world->findChild<WorldPlacementPreview*>(); observedPreview = preview;
        QVERIFY2(require(preview != nullptr, QStringLiteral("Resource click did not create the placement preview service")), "No lazy preview service");
        sendMouse(viewport, QEvent::MouseMove, *groundPoint);
        QVERIFY2(require(viewport->Placing() && place->isChecked() && !select->isChecked(), QStringLiteral("Resource item click did not enter placement and synchronize toolbar")), "Placement mode mismatch");
        std::optional<Render::CapturedFrameRgba> previewCapture;
        std::shared_ptr<const Render::AnimationFrame> previewLayer;
        std::uint64_t observedNs = 0;
        const auto matched = waitUntil([&] {
            const auto scene = preview->CurrentScene();
            if (!scene || !preview->ModelSubmitted()) return false;
            auto current = capture(); if (!current) return false;
            for (std::size_t i = 0; i < current->modelLayerFrames.size() && i < current->modelLayers.size(); ++i) {
                const auto& layer = current->modelLayerFrames[i]; const auto& status = current->modelLayers[i];
                if (!layer || layer->sceneVersion != scene->sceneVersion || status.skin.sceneVersion != layer->sceneVersion
                    || status.skin.contentVersion != layer->contentVersion || status.skin.frameCount != current->view.identity.frameCount) continue;
                if (std::none_of(layer->poses.begin(), layer->poses.end(), [](const auto& pose) {
                    return !pose.hidden && !pose.outsideView && !pose.noVisibleGeometry;
                })) continue;
                observedNs = WorldInstanceNowNs(); previewLayer = layer; previewCapture = std::move(current); return true;
            }
            return false;
        }, 15000);
        QVERIFY2(require(matched, QStringLiteral("No completed RGBA frame contains the hars preview scene within 15 seconds")), "Preview model did not reach completed frame");
        const auto scene = preview->CurrentScene(); QVERIFY(scene && previewLayer && previewCapture);
        const auto elapsedNs = observedNs - started;
        const auto imageName = pass ? QStringLiteral("02-hot-preview") : QStringLiteral("01-cold-preview");
        QVERIFY(saveFrame(*previewCapture, imageName));
        auto sample = frameJson(*previewCapture); sample.insert("kind", pass ? "hot_preview" : "cold_preview");
        sample.insert("click_to_observed_completed_ns", number(elapsedNs)); sample.insert("preview_scene", number(scene->sceneVersion));
        sample.insert("preview_content", number(previewLayer->contentVersion)); sample.insert("cpu_accepted", bool(preview->CurrentFrame()));
        sample.insert("completed_layer_bound", true); sample.insert("preview_visible_pose_count", int(std::count_if(previewLayer->poses.begin(), previewLayer->poses.end(),
            [](const auto& pose) { return !pose.hidden && !pose.outsideView && !pose.noVisibleGeometry; })));
        sample.insert("model_geometry_vertices", number(scene->geometry.vertices.size())); sample.insert("image", imageName + ".png");
        sample.insert("readback_and_polling_included", true); sample.insert("playback_timer_active", preview->PlaybackActive());
        sample.insert("last_readback_ns", number(lastReadbackNs));
        QJsonArray modelPaths; for (const auto& spec : scene->specs) modelPaths.append(QString::fromStdString(spec.model.modelPath));
        sample.insert("model_paths", modelPaths);
        if (pass) { hotWithin100ms = elapsedNs <= 100000000; sample.insert("within_100ms", hotWithin100ms); sample.insert("same_resident_scene", scene->sceneVersion == coldScene); }
        else coldScene = scene->sceneVersion;
        samples.append(sample); QVERIFY(writeEvent(sample));
        if (!pass) {
            click(viewport, *groundPoint, Qt::RightButton);
            QVERIFY2(require(waitUntil([&] { return !viewport->Placing() && !preview->ModelSubmitted(); }, 5000),
                QStringLiteral("Cancel did not remove preview")), "Cold preview cancel failed");
        }
    }
    QVERIFY(phase(QStringLiteral("twenty-formal-placements")));
    bool formalWithin200ms = true;
    bool formalPrefetchRequested = false;
    std::uint64_t timingCursor = 0;
    for (const auto& old : player->InstanceTimingHistory()) timingCursor = std::max(timingCursor, old.sequence);
    std::size_t completedPlacements = 0;
    for (int index = 0; index < 20; ++index) {
        QVERIFY2(require(probeClock.elapsed() < 240000, QStringLiteral("Probe reached four-minute budget")), "Probe budget exceeded");
        const QPointF point{viewport->width() * (0.28 + 0.11 * (index % 5)), viewport->height() * (0.36 + 0.10 * (index / 5))};
        sendMouse(viewport, QEvent::MouseMove, point);
        QVERIFY2(require(waitUntil([&] { return viewport->HoverCoordinate().has_value(); }, 1500), QStringLiteral("Formal placement point has no valid terrain hit")), "Placement ground unavailable");
        const auto previousRevision = window.project_->mapEdits_->Revision();
        const auto clickNs = WorldInstanceNowNs();
        sendMouse(viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
        const auto pressReturnedNs = WorldInstanceNowNs();
        sendMouse(viewport, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        const auto clickReturnedNs = WorldInstanceNowNs();
        QVERIFY(writeEvent({{"event", "formal_mouse_processing"}, {"index", index + 1},
            {"press_started_ns", number(clickNs)}, {"press_returned_ns", number(pressReturnedNs)},
            {"release_returned_ns", number(clickReturnedNs)}, {"press_processing_ns", number(pressReturnedNs - clickNs)},
            {"release_processing_ns", number(clickReturnedNs - pressReturnedNs)},
            {"mouse_processing_ns", number(clickReturnedNs - clickNs)}}));
        QVERIFY2(require(window.project_->mapEdits_->Revision() > previousRevision, QStringLiteral("Actual world click did not create a placement transaction")), "No placement transaction");
        const auto targetRevision = window.project_->mapEdits_->Revision();
        std::optional<WorldInstanceTiming> finished;
        const bool finishedInTime = waitUntil([&] {
            for (const auto& timing : player->InstanceTimingHistory(timingCursor)) {
                if (timing.documentRevision >= targetRevision && timing.additions != 0) { finished = timing; return true; }
            }
            return false;
        }, 15000);
        QVERIFY2(require(finishedInTime && finished.has_value(), QStringLiteral("Formal placement has no terminal roster receipt within 15 seconds")), "Placement roster timed out");
        timingCursor = finished->sequence;
        const auto elapsedNs = finished->finishedNs > clickNs ? finished->finishedNs - clickNs : 0;
        const bool bound = finished->accepted && finished->completed.structSize == sizeof(War3RenderEditorCompletedFrameV2)
            && finished->completed.view.frameCount != 0;
        const bool within = bound && elapsedNs != 0 && elapsedNs <= 200000000;
        formalWithin200ms &= within;
        formalPrefetchRequested |= finished->prefetchRequestedNs != 0;
        QJsonObject sample{{"kind", "formal_placement"}, {"index", index + 1}, {"target_map_revision", number(targetRevision)},
            {"press_started_ns", number(clickNs)}, {"press_returned_ns", number(pressReturnedNs)},
            {"release_returned_ns", number(clickReturnedNs)}, {"mouse_processing_ns", number(clickReturnedNs - clickNs)},
            {"click_to_roster_completion_observed_ns", number(elapsedNs)}, {"roster_completed", bound}, {"within_200ms", within},
            {"source_hash_reused", finished->verifiedArchiveReused}, {"source_hash_ns", number(finished->sourceHashNs)},
            {"source_open_ns", number(finished->sourceOpenNs)}, {"capture_ns", number(finished->captureNs)},
            {"catalog_prepare_ns", number(finished->catalogPrepareNs)}, {"rebase_ns", number(finished->rebaseNs)},
            {"formal_prefetch_state", QString::fromStdString(finished->prefetchState)},
            {"formal_prefetch_asset_hit", finished->catalogDetail.preparedAssetHit},
            {"roster_ticket", number(finished->rosterTicket)}, {"completed_frame", number(finished->completed.view.frameCount)},
            {"completed_scene", number(finished->completed.skin.sceneVersion)}, {"completed_content", number(finished->completed.skin.contentVersion)},
            {"diagnostic", QString::fromStdString(finished->diagnosticCode)}};
        samples.append(sample); QVERIFY(writeEvent(sample));
        QVERIFY2(require(bound, QStringLiteral("Placement roster rejected; see exact timing diagnostic")), "Roster did not complete");
        ++completedPlacements;
        if (index == 0 || index == 9 || index == 19) {
            auto current = capture();
            QVERIFY2(require(current.has_value(), QStringLiteral("Completed placed-model RGBA capture failed")), "Placed capture failed");
            const auto name = QStringLiteral("formal-%1").arg(index + 1, 2, 10, QLatin1Char('0'));
            QVERIFY(saveFrame(*current, name));
            auto pixels = frameJson(*current); pixels.insert("kind", "formal_rgba"); pixels.insert("image", name + ".png");
            const auto scene = player->Scene();
            const bool sceneBound = scene && current->animation && scene->sceneVersion == current->animation->sceneVersion;
            int newVisible = 0;
            if (sceneBound) for (const auto& pose : current->animation->poses)
                if (!originalIdentities.contains({pose.identity.value, pose.identity.generation})
                    && !pose.hidden && !pose.outsideView && !pose.noVisibleGeometry) ++newVisible;
            pixels.insert("world_scene_bound", sceneBound); pixels.insert("new_visible_geometry_poses", newVisible);
            pixels.insert("model_pixel_semantic_confirmation", "pending image review; no placeholder counted");
            samples.append(pixels); QVERIFY(writeEvent(pixels));
            QVERIFY2(require(sceneBound && newVisible > 0, QStringLiteral("Roster completed but no newly placed visible model pose is bound to RGBA")), "Only roster/placeholder evidence is insufficient");
        }
    }
    receipt.insert("completed_placement_transactions", int(completedPlacements));
    const auto afterTwenty = window.project_->mapEdits_->Snapshot(); QVERIFY(afterTwenty.Units());
    QCOMPARE(afterTwenty.Units()->units.size(), initialUnits + 20);
    QCOMPARE(window.project_->mapDataUndoStack_->index(), initialUndo + 20);
    QVERIFY(std::all_of(afterTwenty.Units()->units.end() - 20, afterTwenty.Units()->units.end(), [](const auto& unit) { return unit.rawcode == "hars"; }));
    const auto afterFrame = player->Frame(); QVERIFY(afterFrame);
    std::map<std::pair<std::uint64_t, std::uint64_t>, const Render::AnimationInstanceState*> currentStates;
    for (const auto& state : afterFrame->states) currentStates.emplace(std::pair{state.identity.value, state.identity.generation}, &state);
    std::size_t retainedOriginal = 0, advancedOriginal = 0, regressedOriginal = 0;
    for (const auto& old : baselineFrame->states) {
        const auto at = currentStates.find({old.identity.value, old.identity.generation});
        if (at == currentStates.end()) continue;
        ++retainedOriginal; advancedOriginal += at->second->globalMs > old.globalMs;
        regressedOriginal += at->second->globalMs < old.globalMs;
    }
    receipt.insert("old_instance_clock_continuity", QJsonObject{{"before", number(baselineFrame->states.size())},
        {"retained", number(retainedOriginal)}, {"advanced", number(advancedOriginal)}, {"regressed", number(regressedOriginal)},
        {"scope", "owned CPU states across completed rosters; not visible FPS or particle appearance"}});
    QJsonArray particleContinuity;
    for (const auto& old : baselineFrame->particleLinks) {
        if (!old.simulated || particleContinuity.size() >= 8) continue;
        const auto current = std::find_if(afterFrame->particleLinks.begin(), afterFrame->particleLinks.end(),
            [&](const auto& link) { return link.identity == old.identity; });
        QJsonObject row{{"instance", number(old.identity.value)}, {"generation", number(old.identity.generation)},
            {"before_simulation_ms", number(old.simulationTimeMs)}, {"retained_link", current != afterFrame->particleLinks.end()}};
        if (current != afterFrame->particleLinks.end()) {
            row.insert("after_simulation_ms", number(current->simulationTimeMs)); row.insert("outside_view", current->outsideView);
            row.insert("frozen", current->frozen); row.insert("after_particle_count", int(current->particleCount));
        }
        particleContinuity.append(row);
    }
    receipt.insert("old_particle_timeline_samples", particleContinuity);
    QVERIFY2(require(retainedOriginal == baselineFrame->states.size() && regressedOriginal == 0 && advancedOriginal != 0,
        QStringLiteral("Old world clocks were lost, reset or did not advance during placements")), "Old-world continuity failed");
    QVERIFY(phase(QStringLiteral("undo-redo-cancel")));
    const auto twentyRevision = window.project_->mapEdits_->Revision();
    window.ExecuteCommand("edit.undo");
    QCOMPARE(window.project_->mapDataUndoStack_->index(), initialUndo + 19);
    QVERIFY(waitUntil([&] { return !player->InstancesBusy() && player->Scene()
        && player->Scene()->documentRevision == window.project_->mapEdits_->Revision(); }, 15000));
    const auto undone = window.project_->mapEdits_->Snapshot(); QVERIFY(undone.Units()); QCOMPARE(undone.Units()->units.size(), initialUnits + 19);
    auto undoImage = capture(); QVERIFY(undoImage && saveFrame(*undoImage, QStringLiteral("undo-last-placement")));
    window.ExecuteCommand("edit.redo");
    QCOMPARE(window.project_->mapDataUndoStack_->index(), initialUndo + 20);
    QVERIFY(waitUntil([&] { return !player->InstancesBusy() && player->Scene()
        && player->Scene()->documentRevision == window.project_->mapEdits_->Revision(); }, 15000));
    const auto redone = window.project_->mapEdits_->Snapshot(); QVERIFY(redone.Units()); QCOMPARE(redone.Units()->units.size(), initialUnits + 20);
    auto redoImage = capture(); QVERIFY(redoImage && saveFrame(*redoImage, QStringLiteral("redo-last-placement")));
    const auto previewSceneBeforeCancel = preview->CurrentScene();
    click(viewport, *groundPoint, Qt::RightButton);
    QVERIFY(waitUntil([&] { return !viewport->Placing() && !preview->ModelSubmitted(); }, 5000));
    auto cancelled = capture(); QVERIFY(cancelled && saveFrame(*cancelled, QStringLiteral("cancel-preview")));
    const bool ghostLayerAbsent = !previewSceneBeforeCancel || std::none_of(cancelled->modelLayerFrames.begin(), cancelled->modelLayerFrames.end(),
        [&](const auto& frame) { return frame && frame->sceneVersion == previewSceneBeforeCancel->sceneVersion; });
    QVERIFY2(require(ghostLayerAbsent, QStringLiteral("Cancelled preview scene remains in completed RGBA layers")), "Ghost preview after cancel");
    receipt.insert("undo_redo_cancel", QJsonObject{{"twenty_revision", number(twentyRevision)},
        {"final_revision", number(window.project_->mapEdits_->Revision())}, {"undo_unit_count", number(undone.Units()->units.size())},
        {"redo_unit_count", number(redone.Units()->units.size())}, {"cancelled_preview_layer_absent", ghostLayerAbsent}});
    receipt.insert("hot_preview_100ms_with_readback", hotWithin100ms);
    receipt.insert("all_formal_rosters_200ms", formalWithin200ms);
    receipt.insert("formal_prefetch_requested", formalPrefetchRequested);
    QVERIFY(phase(QStringLiteral("final-source-check")));
    const auto finalSha = Sha256File(NativePath(map));
    if (finalSha.Succeeded()) sourceAfterSha = *finalSha.value;
    QVERIFY2(require(finalSha.Succeeded() && *finalSha.value == sourceSha, QStringLiteral("Input archive SHA changed")), "Input archive changed");
    completed = true;
    QVERIFY2(require(formalPrefetchRequested, QStringLiteral("Resource selection never armed formal static asset preparation")),
        "Formal prefetch wiring was not exercised by the actual resource selection");
    QVERIFY2(require(hotWithin100ms && formalWithin200ms,
        QStringLiteral("Functional sequence finished, but latency gates failed; retained exact completed-frame measurements")),
        "Placement latency gate failed; see completed-frame timings rather than treating a placeholder as success");
}
