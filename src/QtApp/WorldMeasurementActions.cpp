#include "MainWindow.h"
#include "WorldModule.h"
#include "WorldViewport.h"
#include "WorldAnimationPlayer.h"
#include "WorldFrameRecorder.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

bool MainWindow::EnableWorldMeasurement(const QString& directory, int backgroundSeconds, bool cameraTour) {
    if (!worldModule_ || directory.isEmpty() || worldFrameRecorder_) return false;
    ActivateModule("world");
    auto* viewport = worldModule_->Viewport();
    if (!viewport) return false;
    worldFrameRecorder_ = new War3Map::Modules::WorldFrameRecorder(*worldModule_, this);
    auto* recorder = worldFrameRecorder_;
    auto* dock = new QDockWidget(tr("完成帧测量"), this);
    dock->setObjectName(QStringLiteral("worldMeasurementDock"));
    auto* panel = new QWidget(dock);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 6, 8, 6);
    auto* row = new QHBoxLayout;
    auto* start = new QPushButton(tr("开始记录"), panel);
    start->setObjectName(QStringLiteral("worldMeasurementStart"));
    auto* stop = new QPushButton(tr("停止并导出"), panel);
    stop->setObjectName(QStringLiteral("worldMeasurementStop"));
    stop->setEnabled(false);
    auto* phase = new QComboBox(panel);
    phase->addItems({tr("固定视区"), tr("平移和缩放"), tr("连续操作"), tr("暂停检查")});
    auto* witness = new QCheckBox(tr("本视区有应播放动画的单位或粒子"), panel);
    witness->setObjectName(QStringLiteral("worldMeasurementWitness"));
    auto* visible = new QCheckBox(tr("窗口可见且未被遮挡"), panel);
    visible->setObjectName(QStringLiteral("worldMeasurementUnobscured"));
    row->addWidget(start); row->addWidget(stop); row->addWidget(phase);
    row->addWidget(witness); row->addWidget(visible); row->addStretch();
    layout->addLayout(row);
    auto* status = new QLabel(tr("等待世界加载"), panel);
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setObjectName(QStringLiteral("worldMeasurementStatus"));
    layout->addWidget(status);
    dock->setWidget(panel);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    connect(witness, &QCheckBox::toggled, recorder, &War3Map::Modules::WorldFrameRecorder::SetWitnessConfirmed);
    connect(visible, &QCheckBox::toggled, recorder, &War3Map::Modules::WorldFrameRecorder::SetUnobscured);
    connect(phase, &QComboBox::currentTextChanged, recorder, &War3Map::Modules::WorldFrameRecorder::SetPhase);
    recorder->SetPhase(phase->currentText());
    connect(recorder, &War3Map::Modules::WorldFrameRecorder::StatusChanged, status, &QLabel::setText);
    connect(start, &QPushButton::clicked, this, [this, recorder, directory, start, stop, backgroundSeconds] {
        // 每次采集绑定当前实际打开的地图，工具不会保存或修改该文件。
        const auto path = QString::fromStdWString(project_->sourceArchivePath_.wstring());
        if (recorder->Start(directory, path, backgroundSeconds > 0)) {
            start->setEnabled(false); stop->setEnabled(true);
        }
    });
    connect(stop, &QPushButton::clicked, recorder, [recorder] { recorder->Stop(QObject::tr("作者停止并导出")); });
    connect(recorder, &War3Map::Modules::WorldFrameRecorder::Finished, this, [start, stop, status](const QString& path) {
        start->setEnabled(false); stop->setEnabled(false);
        status->setText(QObject::tr("记录已导出：%1").arg(path));
    });
    connect(qApp, &QCoreApplication::aboutToQuit, recorder, [recorder] { recorder->Stop(QObject::tr("进程退出")); });
    if (backgroundSeconds > 0) {
        // 后台仅使用本进程的桌面外窗口，不发送合成鼠标/键盘事件，不声明可见验收。
        witness->setEnabled(false); visible->setEnabled(false);
        auto* readyTimer = new QTimer(recorder);
        readyTimer->setInterval(100);
        auto deadline = std::make_shared<QElapsedTimer>(); deadline->start();
        connect(readyTimer, &QTimer::timeout, this, [this, readyTimer, deadline, recorder, start, backgroundSeconds, cameraTour] {
            if (deadline->elapsed() > 180000 || worldModule_->Viewport()->RendererFailed()) {
                readyTimer->stop(); QCoreApplication::exit(4); return;
            }
            auto* player = worldModule_->AnimationPlayer();
            if (worldModule_->Viewport()->RendererReady() && player && player->Ready()) {
                readyTimer->stop(); start->click();
                if (!recorder->Running()) { QCoreApplication::exit(4); return; }
                if (cameraTour) {
                    // 只改变本进程自有诊断视口；不发送键鼠，不读取或操作作者窗口。
                    // 完成帧记录器照常绑定真实相机/姿势，并明确标为后台证据。
                    auto* view = worldModule_->Viewport();
                    const auto base = view->Camera();
                    auto* tour = new QTimer(recorder);
                    auto clock = std::make_shared<QElapsedTimer>(); clock->start();
                    tour->setInterval(16);
                    recorder->SetPhase(QStringLiteral("后台连续平移旋转，非可见验收"));
                    connect(tour, &QTimer::timeout, recorder, [view, base, clock, recorder, tour] {
                        if (!recorder->Running()) { tour->stop(); return; }
                        const double seconds = clock->elapsed() / 1000.0;
                        auto camera = base;
                        camera.targetX += static_cast<float>(384 * std::sin(seconds * 0.7));
                        camera.targetY += static_cast<float>(256 * std::sin(seconds * 0.9));
                        camera.yawRadians += static_cast<float>(0.25 * std::sin(seconds * 1.1));
                        camera.pitchRadians += static_cast<float>(0.08 * std::sin(seconds * 0.6));
                        (void)view->SetCamera(camera, view->DocumentRevision());
                    });
                    tour->start();
                }
                QTimer::singleShot(std::clamp(backgroundSeconds, 1, 3600) * 1000, recorder, [recorder] {
                    recorder->Stop(QStringLiteral("后台隔离测量结束，非可见验收")); QCoreApplication::quit();
                });
            }
        });
        readyTimer->start();
    }
    return true;
}
