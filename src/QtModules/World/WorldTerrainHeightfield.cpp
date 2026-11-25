#include "WorldModule.h"
#include "EditorProject.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/MapData/TerrainHeightfield.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <filesystem>
#include <memory>
#include <optional>

namespace War3Map::Modules {
namespace {
QString HeightText(float value) { return QString::number(double(value), 'f', 2); }

QString ReportText(const MapData::TerrainHeightfieldReport& report) {
    QStringList lines;
    lines << QObject::tr("导入 %1 个顶点 · 填充 %2 · 地图外 %3")
        .arg(report.importedVertices).arg(report.filledVertices).arg(report.outsideVertices);
    lines << QObject::tr("高度 %1 … %2 · 本层可存 %3 … %4")
        .arg(HeightText(report.minimumHeight), HeightText(report.maximumHeight),
            HeightText(report.storableMinimum), HeightText(report.storableMaximum));
    if (report.clampedLow || report.clampedHigh) {
        const auto& b = report.clampedBounds;
        lines << QObject::tr("截断：偏低 %1 · 偏高 %2 · 顶点 (%3, %4) – (%5, %6)")
            .arg(report.clampedLow).arg(report.clampedHigh).arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3]);
    } else lines << QObject::tr("截断：无");
    return lines.join(QLatin1Char('\n'));
}
} // namespace

void WorldModule::ImportTerrainHeightfield() {
    if (!page_ || !project_.mapEdits_ || !rawTerrain_ || project_.mapEdits_->Terrain() != rawTerrain_
        || worker_ || waterCommit_ || project_.savingMap_) {
        terrainBlockStatus_->setText(tr("请等待当前地形显示完成后再导入高度场")); return;
    }
    const auto terrain = rawTerrain_;
    const auto header = terrain->header;
    QDialog dialog(page_);
    dialog.setObjectName(QStringLiteral("worldTerrainHeightfieldDialog"));
    dialog.setWindowTitle(tr("导入高度场"));
    auto* form = new QFormLayout;
    auto* fileRow = new QHBoxLayout;
    auto* file = new QLineEdit(&dialog); file->setReadOnly(true); file->setMinimumWidth(360);
    file->setObjectName(QStringLiteral("worldTerrainHeightfieldFile"));
    auto* browse = new QPushButton(tr("浏览…"), &dialog);
    fileRow->addWidget(file, 1); fileRow->addWidget(browse);
    form->addRow(tr("清单文件"), fileRow);
    auto* level = new QSpinBox(&dialog); level->setRange(0, 14); level->setValue(2);
    level->setObjectName(QStringLiteral("worldTerrainHeightfieldCliff"));
    form->addRow(tr("悬崖层"), level);
    auto* sink = new QDoubleSpinBox(&dialog); sink->setRange(-65536.0, 65536.0); sink->setDecimals(2); sink->setValue(0.0);
    sink->setObjectName(QStringLiteral("worldTerrainHeightfieldSink"));
    form->addRow(tr("下沉"), sink);
    auto* follow = new QCheckBox(tr("对象随地面移动"), &dialog);
    follow->setObjectName(QStringLiteral("worldTerrainHeightfieldFollow"));
    form->addRow(QString(), follow);
    auto* report = new QLabel(&dialog);
    report->setObjectName(QStringLiteral("worldTerrainHeightfieldReport"));
    report->setWordWrap(true); report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    report->setText(tr("地图 %1×%2 顶点，西南角 (%3, %4)")
        .arg(header.width).arg(header.height).arg(HeightText(header.offset[0]), HeightText(header.offset[1])));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* accept = buttons->button(QDialogButtonBox::Ok);
    accept->setText(tr("导入")); accept->setEnabled(false);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form); layout->addWidget(report); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::optional<MapData::TerrainHeightfieldSource> source;
    std::optional<MapData::TerrainHeightfieldConversion> conversion;
    QString mapLine = report->text();
    const auto refresh = [&] {
        conversion.reset(); accept->setEnabled(false);
        if (!source) return;
        auto converted = MapData::ConvertTerrainHeightfield(header, *source,
            {static_cast<std::uint8_t>(level->value()), static_cast<float>(sink->value())});
        if (!converted.Succeeded() || !converted.value) {
            report->setText(mapLine + QLatin1Char('\n') + tr("无法导入：%1").arg(WorldDiagnosticText(converted.diagnostics)));
            return;
        }
        conversion = std::move(*converted.value);
        report->setText(mapLine + QLatin1Char('\n') + ReportText(conversion->report));
        accept->setEnabled(true);
    };
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("选择高度场清单"),
            heightfieldDirectory_, tr("高度场清单 (*.json)"));
        if (path.isEmpty()) return;
        heightfieldDirectory_ = QFileInfo(path).absolutePath();
        file->setText(QDir::toNativeSeparators(path));
        source.reset();
        auto read = MapData::ReadTerrainHeightfield(std::filesystem::path(path.toStdWString()));
        if (!read.Succeeded() || !read.value) {
            conversion.reset(); accept->setEnabled(false);
            report->setText(mapLine + QLatin1Char('\n') + tr("无法读取：%1").arg(WorldDiagnosticText(read.diagnostics)));
            return;
        }
        source = std::move(*read.value);
        mapLine = tr("地图 %1×%2 顶点，西南角 (%3, %4) · 高度场 %5×%6，原点 (%7, %8)")
            .arg(header.width).arg(header.height).arg(HeightText(header.offset[0]), HeightText(header.offset[1]))
            .arg(source->columns).arg(source->rows).arg(source->originWorld[0]).arg(source->originWorld[1]);
        refresh();
    });
    connect(level, &QSpinBox::valueChanged, &dialog, [&] { refresh(); });
    connect(sink, &QDoubleSpinBox::valueChanged, &dialog, [&] { refresh(); });
    if (dialog.exec() != QDialog::Accepted || !conversion) return;
    // 模态期间实时工具仍可能改图；换算只依赖网格尺寸和原点，地形对象变化则要求重新导入。
    if (!page_ || !project_.mapEdits_ || project_.mapEdits_->Terrain() != terrain || worker_ || waterCommit_
        || project_.savingMap_) {
        terrainBlockStatus_->setText(tr("地形已变化或正忙，高度场未导入，请重新导入")); return;
    }
    StartTerrainHeightfieldCommit(std::move(conversion->intent), follow->isChecked());
}

void WorldModule::StartTerrainHeightfieldCommit(MapData::TerrainHeightfieldIntent intent, bool followDoodads) {
    const auto fail = [this](const std::vector<Diagnostic>& diagnostics) {
        project_.AddDiagnostics(diagnostics);
        terrainBlockStatus_->setText(tr("高度场未导入：%1").arg(WorldDiagnosticText(diagnostics)));
    };
    const auto configured = project_.ConfigureRealtimeTerrainPathing();
    if (!configured.Succeeded()) { fail(configured.diagnostics); return; }
    auto captured = QtProject::CaptureTerrainWaterEditRequest(project_, true);
    if (!captured.Succeeded() || !captured.value) { fail(captured.diagnostics); return; }
    std::size_t frozenBytes = 0;
    auto frozen = project_.mapEdits_->FreezeForSave(64u * 1024u * 1024u, &frozenBytes);
    if (!frozen.Succeeded() || !frozen.value) { fail(frozen.diagnostics); return; }
    StopPlacing(); CancelTerrainStroke(); StopTerrainBlockTool(); resumeBrushAfterRender_ = false;
    if (terrainToggle_ && terrainToggle_->isChecked()) terrainToggle_->setChecked(false);

    const auto request = std::make_shared<const QtProject::TerrainWaterEditRequest>(std::move(*captured.value));
    const std::shared_ptr<const MapData::MapDataEditSession> session = std::move(*frozen.value);
    const auto payload = std::make_shared<const MapData::TerrainHeightfieldIntent>(std::move(intent));
    auto plan = std::make_shared<Result<MapData::MapDataEditSession::PreparedTerrainEdit>>();
    // 撤销入栈完成前保持“地形提交进行中”，保存与切换地图会等待。
    auto pending = std::make_shared<std::unique_ptr<QObject>>(project_.BeginPendingTerrainCommit());
    auto* worker = QThread::create([request, session, payload, followDoodads, frozenBytes, plan] {
        auto source = Services::TerrainPathingSave::CaptureRealtimeReadSource(*session);
        if (!source.Succeeded() || !source.value) { plan->diagnostics = std::move(source.diagnostics); return; }
        auto capture = QtProject::LoadTerrainWaterEditResources(request, {}, *source.value);
        if (!capture.Succeeded() || !capture.value) { plan->diagnostics = std::move(capture.diagnostics); return; }
        *plan = session->PrepareTerrainHeightfieldEdit(capture.value->resources, *payload, followDoodads);
        if (plan->Succeeded() && plan->value)
            *plan = Services::TerrainPathingSave::CompleteRealtimeTerrainEdit(*session, *plan->value, *source.value, frozenBytes);
        if (!plan->Succeeded() || !plan->value) return;
        // 候选计算期间来源也可能变化；发布前重验实际归档，GUI只做修订CAS。
        auto verified = QtProject::VerifyTerrainWaterEditResources(*capture.value, *source.value);
        if (!verified.Succeeded()) { plan->value.reset(); plan->diagnostics = std::move(verified.diagnostics); }
    });
    worker_ = worker;
    const auto generation = generation_;
    terrainBlockStatus_->setText(tr("正在准备高度场…"));
    selectionStatus_->setText(tr("正在核验并准备高度场…"));
    connect(worker, &QThread::finished, this, [this, generation, request, plan, pending] {
        worker_ = nullptr;
        if (reloadQueued_) { Refresh(); return; }
        if (!page_ || generation != generation_) return;
        Result<MapData::TerrainStrokeResult> edited;
        if (!project_.mapEdits_ || project_.savingMap_ || !QtProject::TerrainWaterEditRequestCurrent(project_, *request, true))
            edited.diagnostics = Status::Failure("World.Terrain", "TERRAIN_HEIGHTFIELD_STALE",
                "准备高度场期间地图或资源已变化，未导入。").diagnostics;
        else if (!plan->Succeeded() || !plan->value) edited.diagnostics = plan->diagnostics;
        else edited = project_.mapEdits_->ApplyPreparedTerrainEdit(*plan->value);
        terrainBlockStatus_->setText(edited.Succeeded() ? tr("高度场已导入，可撤销")
            : tr("高度场未导入：%1").arg(WorldDiagnosticText(edited.diagnostics)));
        PublishTerrainStrokeResult(std::move(edited), request->terrain, request->revision, false, true, false);
        pending->reset();
        UpdateToolState();
        StartTerrainRender();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
    UpdateToolState();
}
} // namespace War3Map::Modules
