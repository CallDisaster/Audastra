#include "MainWindow.h"

#include "WorldModule.h"
#include "WorldSelectionPresenter.h"
#include "WorldAnimationPlayer.h"
#include "WorldViewport.h"
#include "WorldScreenshotEncoder.h"

#include <QDir>
#include <QJsonArray>
#include <QSaveFile>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QScreen>
#include <QToolButton>
#include <QTimer>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <map>

// 无界面世界交互探针：窗口放在桌面可见范围之外、不抢焦点，只向本进程控件发送 Qt 合成事件，
// 不产生系统级鼠标键盘输入。用于在真实大地图副本上复现播放、点选、框选与镜头旋转。

namespace {

void Pump(int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void SendMouse(QWidget& widget, QEvent::Type type, QPointF point, Qt::MouseButton button,
    Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(type, point, widget.mapToGlobal(point), button, buttons, modifiers);
    QApplication::sendEvent(&widget, &event);
}

QString ButtonState(QWidget& page, const char* name) {
    auto* button = page.findChild<QAbstractButton*>(QString::fromLatin1(name));
    return button ? QStringLiteral("%1=%2").arg(QString::fromLatin1(name), button->isEnabled() ? "on" : "off")
                  : QStringLiteral("%1=missing").arg(QString::fromLatin1(name));
}

} // namespace

bool MainWindow::ProbeWorldInteraction(int seconds, QStringList& log, bool passive, int readyTimeoutSeconds,
    const std::filesystem::path& captureOutput, bool frameOnly, bool modelListOnly, const QString& focusHostKey,
    std::optional<std::uint32_t> sequenceIndex, std::optional<std::uint64_t> elapsedMs, std::optional<std::uint64_t> globalMs) {
    log << QStringLiteral("world-probe-actual-qt-platform=%1").arg(QApplication::platformName());
    if (frameOnly && (passive || captureOutput.empty() || seconds != 0)) {
        log << QStringLiteral("world-probe-frame-only=refused-conflicting-or-missing-output");
        return false;
    }
    if (modelListOnly && (passive || frameOnly || captureOutput.empty() || seconds != 0)) {
        log << QStringLiteral("world-probe-model-list-only=refused-conflicting-or-missing-output");
        return false;
    }
    if (!focusHostKey.isEmpty() && (!frameOnly || passive || modelListOnly || captureOutput.empty())) return false;
    const bool clockRequested = sequenceIndex || elapsedMs || globalMs;
    if (clockRequested && (!sequenceIndex || !elapsedMs || !globalMs || focusHostKey.isEmpty()
        || !frameOnly || passive || modelListOnly || captureOutput.empty())) return false;
    QString captureDirectory;
    bool captureWritten = false;
    if (!captureOutput.empty()) {
        std::error_code error;
        // 每次验收一个新目录；拒绝覆盖任何先前证据，也不为父路径创建目录。
        if (passive || !std::filesystem::create_directory(captureOutput, error) || error) {
            log << QStringLiteral("world-capture-output=refused-existing-or-invalid-directory");
            return false;
        }
        captureDirectory = QString::fromStdWString(captureOutput.wstring());
    }
    if (!worldModule_) { log << QStringLiteral("world-module=missing"); return false; }
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    resize(1600, 1000);
    QRect screens;
    for (const auto* screen : QApplication::screens()) screens = screens.united(screen->geometry());
    if (screens.isEmpty()) { log << QStringLiteral("world-probe-outcome=no-screen-geometry"); return false; }
    // 使用当前所有显示器的外接矩形，固定负坐标未必真的在多显示器桌面之外。
    move(screens.left() - width() - 256, screens.top() - height() - 256);
    show();
    ActivateModule("world");
    auto* page = worldModule_->Widget();
    auto* viewport = worldModule_->Viewport();
    if (!page || !viewport) { log << QStringLiteral("world-viewport=missing"); return false; }
    const auto modelStatus = [&] {
        auto* label = page->findChild<QLabel*>(QStringLiteral("worldModelStatus"));
        return label ? label->text() : QStringLiteral("model-status-missing");
    };
    const auto writeCapture = [&](War3Map::Render::CapturedFrameRgba frame, QJsonObject probe) {
        auto input = War3Map::Modules::MakeWorldScreenshotEncodeInput(std::move(frame));
        input.metadata.insert("probe", std::move(probe));
        War3Map::Modules::WorldScreenshotEncoder encoder;
        if (!encoder.Submit(std::move(input))) {
            log << QStringLiteral("world-capture-encoder=refused"); return false;
        }
        auto encoded = encoder.Finish();
        if (!encoded || !encoded->Succeeded()) {
            log << QStringLiteral("world-capture-encoder=%1")
                .arg(encoded ? encoded->diagnostic : QStringLiteral("no-result"));
            return false;
        }
        const auto write = [&](const char* name, const QByteArray& bytes) {
            QSaveFile file(QDir(captureDirectory).filePath(QString::fromLatin1(name)));
            file.setDirectWriteFallback(false);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        };
        if (!write("world.png", encoded->png)
            || !write("world.json", QJsonDocument(encoded->metadata).toJson(QJsonDocument::Indented))) {
            log << QStringLiteral("world-capture-write=failed; incomplete-evidence-retained"); return false;
        }
        log << QStringLiteral("world-capture-written=%1; rgba-source=renderer-completed-frame").arg(captureDirectory);
        return true;
    };
    const auto recentMetrics = [&]() {
        QJsonArray rows;
        const auto index = viewport->ReadFrameMetrics(0);
        if (!index.Succeeded() || !index.value) return rows;
        const auto latest = index.value->page.latestFrame;
        const auto page = viewport->ReadFrameMetrics(latest > 8 ? latest - 8 : 0);
        if (!page.Succeeded() || !page.value) return rows;
        for (const auto& metric : page.value->records) {
            if (rows.size() == 8) break;
            const auto& frame = metric.completed;
            rows.append(QJsonObject{{"flags", static_cast<int>(metric.flags)},
                {"result", metric.result}, {"present_result", metric.presentResult},
                {"frame_number", QString::number(frame.view.frameCount)},
                {"surface_epoch", QString::number(frame.view.surfaceEpoch)},
                {"scene_epoch", QString::number(frame.view.sceneEpoch)},
                {"view_epoch", QString::number(frame.view.viewEpoch)},
                {"animation_scene_version", QString::number(frame.skin.sceneVersion)},
                {"animation_content_version", QString::number(frame.skin.contentVersion)}});
        }
        return rows;
    };
    const auto snapshot = [&](const QString& tag) {
        const auto frame = viewport->AnimationTarget();
        log << QStringLiteral("[%1] renderer=%2 failed=%3 anim=%4 content=%5 timer=%6 %7 %8 | status=%9 | model=%10 | hover=%11 | selection=%12")
            .arg(tag).arg(viewport->RendererReady()).arg(viewport->RendererFailed()).arg(viewport->HasAnimationScene())
            .arg(frame ? QString::number(frame->contentVersion) : QStringLiteral("-")).arg(viewport->FrameTimerActive())
            .arg(ButtonState(*page, "worldAnimationPlay"), ButtonState(*page, "worldAnimationPause"),
                viewport->StatusText(), modelStatus(), viewport->HoverText(), viewport->SelectionText());
        QElapsedTimer diagnosticTimer; diagnosticTimer.start();
        auto diagnostic = worldModule_->PlaybackDiagnosticSnapshot();
        diagnostic.insert("modelDiagnostics", worldModule_->ModelDiagnosticSnapshot());
        diagnostic.insert("diagnosticCaptureElapsedMs", diagnosticTimer.nsecsElapsed() / 1000000.0);
        diagnostic.insert("tag", tag);
        diagnostic.insert("passive", passive);
        diagnostic.insert("visible_acceptance", false);
        diagnostic.insert("viewport_physical_width", viewport->MeasurementPhysicalSize().width());
        diagnostic.insert("viewport_physical_height", viewport->MeasurementPhysicalSize().height());
        diagnostic.insert("viewport_dpr", viewport->devicePixelRatioF());
        diagnostic.insert("camera", viewport->SaveCamera());
        diagnostic.insert("frame_metrics", recentMetrics());
        const auto stateLine = QStringLiteral("world-playback-state=")
            + QString::fromUtf8(QJsonDocument(diagnostic).toJson(QJsonDocument::Compact));
        // 长加载/失败也即时落到调用者stdout，进程异常退出不会丢掉全部现场。
        QTextStream progress(stdout);
        progress.setEncoding(QStringConverter::Utf8);
        progress << stateLine << '\n';
        progress.flush();
    };

    QElapsedTimer clock; clock.start();
    const auto serviceReady = [&] {
        const auto* player = worldModule_->AnimationPlayer();
        return viewport->RendererReady() && player && player->Ready()
            && (!modelListOnly || worldModule_->ModelDiagnosticsReady());
    };
    snapshot(QStringLiteral("loading-start"));
    qint64 nextStatusMs = 5000;
    while (clock.elapsed() < static_cast<qint64>(readyTimeoutSeconds) * 1000 && !serviceReady()) {
        Pump(250);
        if (clock.elapsed() >= nextStatusMs) {
            snapshot(QStringLiteral("loading@%1ms").arg(clock.elapsed()));
            nextStatusMs = clock.elapsed() + 5000;
        }
        if (viewport->RendererFailed()) break;
    }
    snapshot(QStringLiteral("ready@%1ms").arg(clock.elapsed()));
    if (!serviceReady()) {
        log << QStringLiteral("world-probe-outcome=player-not-ready; retained-picture-is-not-playback");
        return false;
    }

    QJsonObject clockReceipt;
    if (!focusHostKey.isEmpty()) {
        const auto scene = worldModule_->AnimationPlayer()->Scene();
        if (!scene) return false;
        const War3Map::Render::AnimationInstanceSpec* target = nullptr;
        std::size_t targetIndex = 0;
        for (std::size_t i = 0; i < scene->specs.size(); ++i) if (QString::fromStdString(scene->specs[i].hostKey) == focusHostKey) {
            if (target) { log << QStringLiteral("world-focus=refused-ambiguous-host-key"); return false; }
            target = &scene->specs[i]; targetIndex = i;
        }
        if (!target) { log << QStringLiteral("world-focus=refused-missing-host-key"); return false; }
        auto camera = viewport->SaveCamera();
        camera["x"] = target->model.position[0]; camera["y"] = target->model.position[1];
        camera["z"] = target->model.position[2]; camera["distance"] = 900;
        if (!viewport->RestoreCamera(camera)) return false;
        Pump(250);
        log << QStringLiteral("world-focus-host-key=%1; selection=false").arg(focusHostKey);
        if (clockRequested) {
            auto* player = worldModule_->AnimationPlayer();
            player->SetPlaying(false);
            QElapsedTimer pause; pause.start();
            while (pause.elapsed() < 2000 && (player->Busy() || player->InstancesBusy() || player->Playing())) Pump(20);
            if (player->Scene() != scene || player->Busy() || player->InstancesBusy() || player->Playing()
                || targetIndex >= scene->sequences.size() || *sequenceIndex >= scene->sequences[targetIndex].size()) {
                log << QStringLiteral("world-clock=refused-stale-owner-or-sequence"); return false;
            }
            War3Map::Render::AnimationEdit edit;
            edit.identity = target->identity; edit.sequenceIndex = *sequenceIndex;
            edit.elapsedMs = *elapsedMs; edit.globalMs = *globalMs; edit.playing = false;
            player->EditMany({edit});
            const auto accepted = [&] {
                const auto frame = player->Frame();
                return player->Scene() == scene && frame && frame == viewport->AnimationTarget()
                    && !player->Busy() && !player->InstancesBusy() && !player->Playing()
                    && frame->sceneVersion == scene->sceneVersion && frame->documentRevision == scene->documentRevision
                    && targetIndex < frame->states.size() && frame->states[targetIndex].identity == target->identity
                    && frame->states[targetIndex].sequenceIndex == *sequenceIndex
                    && frame->states[targetIndex].elapsedMs == *elapsedMs && frame->states[targetIndex].globalMs == *globalMs
                    && !frame->states[targetIndex].playing;
            };
            QElapsedTimer editWait; editWait.start();
            while (editWait.elapsed() < 5000 && !accepted()) Pump(20);
            if (!accepted()) { log << QStringLiteral("world-clock=refused-target-clock-not-accepted"); return false; }
            clockReceipt = {{"hostKey", focusHostKey}, {"identity", QString::number(target->identity.value)},
                {"identityGeneration", QString::number(target->identity.generation)},
                {"requestIndex", static_cast<double>(targetIndex)},
                {"sequenceIndex", static_cast<double>(*sequenceIndex)}, {"elapsedMs", QString::number(*elapsedMs)},
                {"globalMs", QString::number(*globalMs)}, {"playing", false}, {"accepted", true},
                {"acceptedContentVersion", QString::number(player->Frame()->contentVersion)}};
            log << QStringLiteral("world-clock=accepted; sequence=%1; elapsed=%2; global=%3")
                .arg(*sequenceIndex).arg(*elapsedMs).arg(*globalMs);
        }
    }

    if (modelListOnly) {
        // 独立显式模式：生产诊断窗口的实际绑定视图；不产生选择/定位或GPU签收。
        auto* player = worldModule_->AnimationPlayer();
        player->SetPlaying(false);
        QElapsedTimer settle; settle.start();
        while (settle.elapsed() < 2000 && (player->Busy() || player->InstancesBusy() || player->Playing())) Pump(20);
        if (player->Busy() || player->InstancesBusy() || player->Playing()) {
            log << QStringLiteral("world-model-list-only=refused-unsettled-animation"); return false;
        }
        QElapsedTimer captureTimer; captureTimer.start();
        auto capture = worldModule_->CaptureModelDiagnosticsView();
        if (!capture.Succeeded() || !capture.value) {
            log << QStringLiteral("world-model-list-only=refused-stale-or-unready-binding"); return false;
        }
        capture.value->binding.insert("widgetCaptureElapsedMs", captureTimer.nsecsElapsed() / 1000000.0);
        const auto imagePath = QDir(captureDirectory).filePath(QStringLiteral("model-list.png"));
        QSaveFile image(imagePath); image.setDirectWriteFallback(false);
        if (!image.open(QIODevice::WriteOnly) || !capture.value->image.save(&image, "PNG") || !image.commit()) {
            log << QStringLiteral("world-model-list-only=image-publication-failed"); return false;
        }
        const auto write = [&](const char* name, const QByteArray& bytes) {
            QSaveFile file(QDir(captureDirectory).filePath(QString::fromLatin1(name)));
            file.setDirectWriteFallback(false);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        };
        if (!write("model-list.json", QJsonDocument(capture.value->binding).toJson(QJsonDocument::Indented))
            || !write("model-list.txt", capture.value->exportText.toUtf8())) {
            log << QStringLiteral("world-model-list-only=metadata-publication-failed"); return false;
        }
        snapshot(QStringLiteral("model-list-only-end"));
        log << QStringLiteral("world-probe-outcome=model-list-widget-captured; interaction=false; gpu-proof=false; author-visible-acceptance=false");
        return true;
    }

    if (frameOnly) {
        // 独立GPU读回证据；不执行点击、不授予选择圈或前台可见签收。
        auto* player = worldModule_->AnimationPlayer();
        player->SetPlaying(false);
        QElapsedTimer pauseWait; pauseWait.start();
        while (pauseWait.elapsed() < 2000 && (player->Busy() || player->InstancesBusy() || player->Playing())) Pump(20);
        const auto stableFrame = viewport->AnimationTarget();
        const auto stableScene = player->Scene();
        const auto revision = viewport->DocumentRevision();
        Pump(50);
        if (!stableFrame || !stableScene || player->Busy() || player->InstancesBusy() || player->Playing()
            || player->Frame() != stableFrame || viewport->AnimationTarget() != stableFrame
            || player->Scene() != stableScene || viewport->DocumentRevision() != revision
            || stableFrame->sceneVersion != stableScene->sceneVersion
            || stableFrame->documentRevision != stableScene->documentRevision) {
            log << QStringLiteral("world-frame-only=refused-unstable-animation"); return false;
        }
        if (clockRequested) {
            const auto request = clockReceipt["requestIndex"].toInt(-1);
            if (request < 0 || static_cast<std::size_t>(request) >= stableFrame->states.size()) return false;
            const auto& state = stableFrame->states[static_cast<std::size_t>(request)];
            if (QString::number(state.identity.value) != clockReceipt["identity"].toString()
                || QString::number(state.identity.generation) != clockReceipt["identityGeneration"].toString()
                || state.sequenceIndex != *sequenceIndex || state.elapsedMs != *elapsedMs
                || state.globalMs != *globalMs || state.playing) {
                log << QStringLiteral("world-clock=refused-capture-clock-changed"); return false;
            }
            clockReceipt["capturedContentVersion"] = QString::number(stableFrame->contentVersion);
            clockReceipt["capturedSceneVersion"] = QString::number(stableFrame->sceneVersion);
            clockReceipt["capturedDocumentRevision"] = QString::number(stableFrame->documentRevision);
        }
        const auto sourceStable = [&] {
            return viewport->AnimationTarget() == stableFrame && player->Frame() == stableFrame && player->Scene() == stableScene
                && !player->Busy() && !player->InstancesBusy() && !player->Playing() && viewport->DocumentRevision() == revision;
        };
        auto frame = viewport->CaptureCompletedFrame(revision, War3Map::Render::kMaximumFrameCaptureBytes);
        War3Map::Result<QJsonObject> premCapture;
        if (frame.Succeeded() && frame.value) premCapture = worldModule_->ModelPremCaptureSnapshot(*frame.value);
        QElapsedTimer completionWait; completionWait.start();
        while (sourceStable() && completionWait.elapsed() < 5000 && (!frame.Succeeded() || !frame.value
            || !premCapture.Succeeded() || !premCapture.value)) {
            Pump(250); // 有界等待实际层完成，保持同一父包；不把CPU提交即视为已绘制。
            if (!sourceStable()) break;
            frame = viewport->CaptureCompletedFrame(revision, War3Map::Render::kMaximumFrameCaptureBytes);
            if (frame.Succeeded() && frame.value) premCapture = worldModule_->ModelPremCaptureSnapshot(*frame.value);
        }
        if (!sourceStable()) { log << QStringLiteral("world-frame-only=refused-completion-wait-source-changed"); return false; }
        if (!frame.Succeeded() || !frame.value) {
            for (const auto& diagnostic : frame.diagnostics)
                log << QStringLiteral("world-frame-only-refused=%1; %2")
                    .arg(QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
            return false;
        }
        if (!premCapture.Succeeded() || !premCapture.value) {
            for (const auto& diagnostic : premCapture.diagnostics)
                log << QStringLiteral("world-prem-capture-refused=%1; %2")
                    .arg(QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
            return false;
        }
        if (frame.value->animation != stableFrame || viewport->AnimationTarget() != stableFrame
            || player->Frame() != stableFrame || player->Scene() != stableScene
            || player->Busy() || player->InstancesBusy() || player->Playing()
            || viewport->DocumentRevision() != revision) {
            log << QStringLiteral("world-frame-only=refused-capture-source-changed"); return false;
        }
        const auto metrics = recentMetrics();
        QJsonObject capturedMetric;
        for (const auto& row : metrics) {
            const auto metric = row.toObject();
            const auto flags = static_cast<std::uint32_t>(metric["flags"].toInt());
            if (metric["frame_number"].toString().toULongLong() == frame.value->view.identity.frameCount
                && metric["result"].toInt() == 0
                && (flags & (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID
                    | WAR3_RENDER_FRAME_METRICS_READBACK)) == (WAR3_RENDER_FRAME_METRICS_DRAW_COMPLETED
                    | WAR3_RENDER_FRAME_METRICS_UPLOAD_VALID | WAR3_RENDER_FRAME_METRICS_READBACK)
                && metric["animation_scene_version"].toString().toULongLong() == stableFrame->sceneVersion
                && metric["animation_content_version"].toString().toULongLong() == stableFrame->contentVersion
                && metric["surface_epoch"].toString().toULongLong() == frame.value->view.identity.surfaceEpoch
                && metric["scene_epoch"].toString().toULongLong() == frame.value->view.identity.sceneEpoch
                && metric["view_epoch"].toString().toULongLong() == frame.value->view.identity.viewEpoch) capturedMetric = metric;
        }
        if (capturedMetric.isEmpty()) {
            log << QStringLiteral("world-frame-only=refused-no-same-readback-metric"); return false;
        }
        QJsonObject context{{"mode", "frame-only"}, {"selection_acceptance", false},
            {"author_visible_acceptance", false}, {"input_kind", "owned-widget-no-interaction"},
            {"stable_animation_content_version", QString::number(stableFrame->contentVersion)},
            {"frame_metrics", metrics}, {"capture_frame_metrics", capturedMetric}};
        const auto bugCapture = worldModule_->ModelBugPlaceholderCaptureSnapshot(*frame.value);
        if (!bugCapture.Succeeded() || !bugCapture.value) {
            for (const auto& diagnostic : bugCapture.diagnostics)
                log << QStringLiteral("world-placeholder-capture-refused=%1; %2")
                    .arg(QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
            return false;
        }
        context["bug_placeholders"] = *bugCapture.value;
        context["prem_preview"] = *premCapture.value;
        if (!focusHostKey.isEmpty()) context["focused_host_key"] = focusHostKey;
        if (clockRequested) context["requested_accepted_animation_clock"] = clockReceipt;
        if (!writeCapture(std::move(*frame.value), std::move(context))) return false;
        snapshot(QStringLiteral("frame-only-end"));
        log << QStringLiteral("world-probe-outcome=frame-only-completed; interaction=false; selection-acceptance=false; author-visible-acceptance=false");
        return true;
    }

    // 持续播放：每 5 秒记录一次，观察是否停住。
    clock.restart();
    std::uint64_t lastContent = 0; int stalls = 0;
    while (clock.elapsed() < static_cast<qint64>(seconds) * 1000) {
        const auto remaining = static_cast<qint64>(seconds) * 1000 - clock.elapsed();
        if (remaining <= 0) break;
        Pump(static_cast<int>(std::min<qint64>(5000, remaining)));
        const auto frame = viewport->AnimationTarget();
        const auto content = frame ? frame->contentVersion : 0;
        if (content == lastContent) ++stalls;
        lastContent = content;
        snapshot(QStringLiteral("play@%1s").arg(clock.elapsed() / 1000));
    }
    log << QStringLiteral("play-stalled-samples=%1").arg(stalls);
    if (passive) {
        snapshot(QStringLiteral("passive-end"));
        log << QStringLiteral("world-probe-outcome=diagnostic-completed; interaction=false; visible-acceptance=false");
        return serviceReady();
    }

    // 镜头拉近到第一个可选实例附近，再逐个单击其投影中心。
    int clickHits = 0, clickTried = 0;
    std::optional<std::uint32_t> clickedRequest;
    int clickedInstance = -1;
    QObject::connect(viewport, &War3Map::Modules::WorldViewport::InstanceClicked, page, [&](int instance, int) {
        log << QStringLiteral("  signal InstanceClicked %1").arg(instance);
        clickedInstance = instance;
        clickedRequest.reset();
        const auto* currentPlayer = worldModule_->AnimationPlayer();
        const auto picking = currentPlayer ? currentPlayer->Picking() : nullptr;
        const auto scene = currentPlayer ? currentPlayer->Scene() : nullptr;
        if (instance >= 0 && picking && scene && static_cast<std::size_t>(instance) < picking->instanceRequests.size()) {
            const auto request = picking->instanceRequests[instance];
            if (request < scene->specs.size()) {
                clickedRequest = request;
                log << QStringLiteral("  click-source instance=%1 request=%2 host=%3")
                    .arg(instance).arg(request).arg(QString::fromStdString(scene->specs[request].hostKey));
            }
        }
        if (instance >= 0) ++clickHits;
    });
    // 选取圈附加层的准备/提交结果（失败原因只在这里报出）。
    auto* selectionPresenter = worldModule_->findChild<War3Map::Modules::WorldSelectionPresenter*>();
    if (auto* presenter = selectionPresenter) {
        QObject::connect(presenter, &War3Map::Modules::WorldSelectionPresenter::Message, page, [&](const QString& text) {
            log << QStringLiteral("  selection-circle %1").arg(text);
        });
    } else log << QStringLiteral("selection-circle presenter=missing");
    int boxSignals = 0, boxCount = 0;
    QObject::connect(viewport, &War3Map::Modules::WorldViewport::InstancesBoxSelected, page, [&](const QVector<int>& instances, int) {
        ++boxSignals; boxCount = instances.size();
    });
    // 渲染实例下标会随裁剪改变；保留来源请求，再从每次接受的Picking重新定位。
    auto* player = worldModule_->AnimationPlayer();
    const auto targetScene = player ? player->Scene() : nullptr;
    if (!targetScene) { log << QStringLiteral("world-probe-outcome=no-target-scene"); return false; }
    const auto describe = [&](std::uint32_t request) {
        const auto& spec = targetScene->specs[request];
        const auto& circle = spec.model.selection;
        return QStringLiteral("request=%1 host=%2 model=%3 circle-known=%4 circle-scale=%5")
            .arg(request).arg(QString::fromStdString(spec.hostKey), QString::fromStdString(spec.model.modelPath))
            .arg(circle && circle->circleScaleKnown)
            .arg(circle ? QString::number(circle->circleScale, 'g', 9) : QStringLiteral("absent"));
    };
    // 保留旧抽样110的元数据回执；不根据没有圈层的结果反推dummy或零尺寸。
    if (const auto picking = player->Picking(); picking && picking->instanceRequests.size() > 110) {
        const auto request = picking->instanceRequests[110];
        if (request < targetScene->specs.size())
            log << QStringLiteral("world-probe-legacy-target=110; %1").arg(describe(request));
    }
    std::vector<std::uint32_t> eligible;
    for (std::uint32_t request = 0; request < targetScene->specs.size(); ++request) {
        const auto& circle = targetScene->specs[request].model.selection;
        if (circle && circle->circleScaleKnown && std::isfinite(circle->circleScale)
            && circle->circleScale > 0 && std::isfinite(circle->circleHeight)) eligible.push_back(request);
    }
    std::vector<std::uint32_t> targets;
    const auto total = viewport->ModelInstanceCount();
    for (std::size_t index = 0; index < eligible.size() && targets.size() < 10;
        index += std::max<std::size_t>(1, eligible.size() / 10)) targets.push_back(eligible[index]);
    log << QStringLiteral("instances=%1 click-targets=%2 positive-circle-requests=%3")
        .arg(total).arg(targets.size()).arg(eligible.size());
    const auto resolve = [&](std::uint32_t request) -> std::optional<std::uint32_t> {
        const auto scene = player->Scene();
        const auto picking = player->Picking();
        if (!scene || !picking || scene->generation != targetScene->generation
            || scene->sceneVersion != targetScene->sceneVersion || request >= scene->specs.size()
            || scene->specs[request].hostKey != targetScene->specs[request].hostKey) return std::nullopt;
        for (std::uint32_t index = 0; index < picking->instanceRequests.size(); ++index)
            if (picking->instanceRequests[index] == request && viewport->SelectableInstancePosition(index)) return index;
        return std::nullopt;
    };
    for (const auto request : targets) {
        const auto position = targetScene->specs[request].model.position;
        log << QStringLiteral("world-probe-target %1").arg(describe(request));
        auto camera = viewport->SaveCamera();
        camera["x"] = position[0]; camera["y"] = position[1]; camera["z"] = position[2]; camera["distance"] = 900;
        (void)viewport->RestoreCamera(camera);
        Pump(1200);
        const auto resolved = resolve(request);
        if (!resolved) { log << QStringLiteral("request %1 not selectable after camera").arg(request); continue; }
        const auto target = *resolved;
        const auto projected = viewport->ProjectedSelectableInstances(100000);
        const auto found = std::find_if(projected.begin(), projected.end(), [&](const auto& item) { return item.first == target; });
        if (found == projected.end()) { log << QStringLiteral("target %1 not projected").arg(target); continue; }
        ++clickTried;
        const auto point = found->second + QPointF(0, -12); // 稍高于脚底中心，落在模型身体上
        SendMouse(*viewport, QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        Pump(150);
        if (resolve(request) != target) { log << QStringLiteral("request %1 remapped before click").arg(request); continue; }
        clickedRequest.reset();
        SendMouse(*viewport, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        log << QStringLiteral("  on-press hover=%1").arg(viewport->HoverText());
        SendMouse(*viewport, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        log << QStringLiteral("  on-release hover=%1").arg(viewport->HoverText());
        Pump(100);
        log << QStringLiteral("  after-100ms hover=%1").arg(viewport->HoverText());
        Pump(1700);
        log << QStringLiteral("click target=%1 at=(%2,%3) selected=%4 | hover=%5 | selection=%6")
            .arg(target).arg(point.x(), 0, 'f', 1).arg(point.y(), 0, 'f', 1)
            .arg(viewport->SelectedInstance() ? QString::number(*viewport->SelectedInstance()) : QStringLiteral("none"))
            .arg(viewport->HoverText(), viewport->SelectionText());
        const auto selected = viewport->SelectedInstance();
        const auto currentTarget = resolve(request);
        const auto selectedFrame = viewport->AnimationTarget();
        if (const auto picking = player->Picking(); selected && picking && *selected < picking->instanceRequests.size()) {
            const auto selectedRequest = picking->instanceRequests[*selected];
            const auto scene = player->Scene();
            log << QStringLiteral("world-probe-selected instance=%1 request=%2 host=%3 target-request=%4 same-target=%5")
                .arg(*selected).arg(selectedRequest)
                .arg(scene && selectedRequest < scene->specs.size()
                    ? QString::fromStdString(scene->specs[selectedRequest].hostKey) : QStringLiteral("invalid"))
                .arg(request).arg(currentTarget && selected == currentTarget);
        }
        // 动画裁剪会正常重排渲染下标；真实点击与当前选择必须仍绑定同一来源请求。
        if (!captureDirectory.isEmpty() && !captureWritten && selectionPresenter
            && clickedRequest == request && currentTarget && selected == currentTarget && selectedFrame) {
            auto frame = viewport->CaptureCompletedFrame(viewport->DocumentRevision(),
                War3Map::Render::kMaximumFrameCaptureBytes);
            if (!frame.Succeeded() || !frame.value) {
                for (const auto& diagnostic : frame.diagnostics)
                    log << QStringLiteral("world-capture-refused=%1; %2")
                        .arg(QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
                continue;
            }
            if (frame.value->animation != selectedFrame || viewport->SelectedInstance() != selected
                || resolve(request) != selected) {
                log << QStringLiteral("world-capture-refused=selection-source-or-frame-changed");
                continue;
            }
            const auto circleId = selectionPresenter->LayerId();
            log << QStringLiteral("world-capture-context circle-id=%1 layers=%2 host-frames=%3")
                .arg(circleId).arg(frame.value->modelLayers.size()).arg(frame.value->modelLayerFrames.size());
            bool hasCircle = false;
            for (std::size_t index = 0; index < frame.value->modelLayers.size(); ++index) {
                const auto hostFrame = index < frame.value->modelLayerFrames.size()
                    ? frame.value->modelLayerFrames[index] : nullptr;
                log << QStringLiteral("world-capture-layer id=%1 instances=%2 states=%3")
                    .arg(frame.value->modelLayers[index].layerId)
                    .arg(hostFrame ? hostFrame->instances.size() : 0)
                    .arg(hostFrame ? hostFrame->states.size() : 0);
                // 选取圈是静态实例帧，没有播放 states；实际层实例须非空且已通过同帧GPU身份核验。
                if (frame.value->modelLayers[index].layerId == circleId
                    && index < frame.value->modelLayerFrames.size() && frame.value->modelLayerFrames[index]
                    && !frame.value->modelLayerFrames[index]->instances.empty()) hasCircle = true;
            }
            if (!hasCircle) {
                log << QStringLiteral("world-capture-target=%1; selection-circle-layer=absent").arg(target);
                continue;
            }
            auto context = QJsonObject{{"mode", "strict"}, {"selection_acceptance", true}, {"selected_instance", QString::number(*selected)},
                {"projected_instance_before_click", QString::number(target)},
                {"clicked_instance_at_signal", QString::number(clickedInstance)},
                {"selected_source_request", QString::number(request)},
                {"selected_host_key", QString::fromStdString(targetScene->specs[request].hostKey)},
                {"selected_circle_scale", targetScene->specs[request].model.selection->circleScale},
                {"selection_circle_layer_id", QString::number(circleId)},
                {"selected_world_position", QJsonArray{position[0], position[1], position[2]}},
                {"input_kind", "owned-widget-qt-events"}, {"author_visible_acceptance", false}};
            if (!writeCapture(std::move(*frame.value), std::move(context))) return false;
            captureWritten = true;
        }
    }
    log << QStringLiteral("click-hits=%1/%2").arg(clickHits).arg(clickTried);

    // 普通框选：覆盖视口中央大部分区域。
    const QPointF from(viewport->width() * 0.15, viewport->height() * 0.15), to(viewport->width() * 0.85, viewport->height() * 0.85);
    SendMouse(*viewport, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    for (int step = 1; step <= 8; ++step)
        SendMouse(*viewport, QEvent::MouseMove, from + (to - from) * (step / 8.0), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    SendMouse(*viewport, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    Pump(1500);
    log << QStringLiteral("box-signals=%1 box-count=%2 selection=%3").arg(boxSignals).arg(boxCount).arg(viewport->SelectionText());

    // Ctrl+右键拖动旋转镜头。
    const auto before = QJsonDocument(viewport->SaveCamera()).toJson(QJsonDocument::Compact);
    const QPointF center(viewport->width() / 2.0, viewport->height() / 2.0);
    SendMouse(*viewport, QEvent::MouseButtonPress, center, Qt::RightButton, Qt::RightButton, Qt::ControlModifier);
    for (int step = 1; step <= 10; ++step)
        SendMouse(*viewport, QEvent::MouseMove, center + QPointF(step * 12.0, step * 4.0), Qt::NoButton, Qt::RightButton, Qt::ControlModifier);
    SendMouse(*viewport, QEvent::MouseButtonRelease, center + QPointF(120, 40), Qt::RightButton, Qt::NoButton, Qt::ControlModifier);
    Pump(500);
    const auto after = QJsonDocument(viewport->SaveCamera()).toJson(QJsonDocument::Compact);
    log << QStringLiteral("camera-before=%1").arg(QString::fromUtf8(before));
    log << QStringLiteral("camera-after=%1").arg(QString::fromUtf8(after));
    snapshot(QStringLiteral("end"));
    if (auto* label = page->findChild<QLabel*>(QStringLiteral("worldModelStatus")))
        log << QStringLiteral("model-details=%1").arg(label->toolTip().replace(QLatin1Char('\n'), QStringLiteral(" / ")));
    if (const auto frame = viewport->AnimationTarget()) {
        std::map<std::string, int> codes; int simulated = 0;
        for (const auto& link : frame->particleLinks) {
            if (link.simulated) ++simulated;
            ++codes[link.rejectionCode.empty() ? std::string("-") : link.rejectionCode];
        }
        log << QStringLiteral("particle-links=%1 simulated=%2 budget-instances=%3").arg(frame->particleLinks.size()).arg(simulated).arg(frame->budgetParticleInstances);
        for (const auto& [code, count] : codes) log << QStringLiteral("  link %1 = %2").arg(QString::fromStdString(code)).arg(count);
        // 镜头附近（3000 内）的粒子实例覆盖情况。
        const auto camera = viewport->SaveCamera();
        const double cx = camera["x"].toDouble(), cy = camera["y"].toDouble();
        int near = 0, nearSimulated = 0;
        for (const auto& link : frame->particleLinks) {
            if (link.requestIndex >= frame->states.size()) continue;
            const auto& t = frame->states[link.requestIndex].transform;
            if (std::hypot(t[3] - cx, t[7] - cy) > 3000) continue;
            ++near; if (link.simulated) ++nearSimulated;
        }
        log << QStringLiteral("near-camera particle instances=%1 simulated=%2").arg(near).arg(nearSimulated);
    }
    if (!captureDirectory.isEmpty() && !captureWritten) {
        log << QStringLiteral("world-capture-outcome=no-selected-unit-circle-completed-frame");
        return false;
    }
    return true;
}
