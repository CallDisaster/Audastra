#include "WorldModule.h"
#include "EditorProject.h"
#include "ProjectPlacementImport.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementManifest.h"

#include <QApplication>
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
#include <QScrollArea>
#include <QVBoxLayout>

#include <array>
#include <filesystem>
#include <optional>
#include <utility>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kListedTypes = 20;
constexpr std::size_t kListedWarnings = 30;

QString Mebibytes(std::uint64_t bytes) { return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1); }

QString ReportText(const MapData::PlacementManifest& manifest, const Services::PlacementImportReport& report) {
    QStringList lines;
    std::uint64_t bytes = 0;
    for (const auto& file : manifest.files) bytes += file.bytes.size();
    lines << QObject::tr("文件 %1 个 · %2 MiB · 覆盖已有 %3 个").arg(report.files).arg(Mebibytes(bytes)).arg(report.replacedFiles);
    lines << QObject::tr("装饰物 %1 个（固定高度 %2）· 单位 %3 个")
        .arg(report.doodads).arg(report.fixedHeightDoodads).arg(report.units);
    lines << QObject::tr("新建类型 %1 个").arg(report.types.size());
    for (std::size_t index = 0; index < report.types.size() && index < kListedTypes; ++index) {
        const auto& type = report.types[index];
        lines << QStringLiteral("  %1 → %2 · %3 %4 · %5").arg(QString::fromStdString(type.key), QString::fromStdString(type.rawcode),
            type.kind == MapData::PlacementManifestKind::Unit ? QObject::tr("单位") : QObject::tr("装饰物"),
            QString::fromStdString(type.base)).arg(type.instances);
    }
    if (report.types.size() > kListedTypes) lines << QObject::tr("  等 %1 个").arg(report.types.size());
    const auto& bounds = report.bounds;
    lines << QObject::tr("模型写明范围：检查 %1 个摆放 · 地图外 %2 · 未包住观察点 %3 · 两者都有 %4 · 未检查 %5 · 加宽模型 %6")
        .arg(bounds.checkedInstances).arg(bounds.offMap).arg(bounds.farFromStandpoint).arg(bounds.both)
        .arg(bounds.uncheckedInstances).arg(bounds.widenedModels);
    for (std::size_t index = 0; index < report.warnings.size() && index < kListedWarnings; ++index)
        lines << QObject::tr("提醒：%1").arg(QString::fromStdString(report.warnings[index].message));
    if (report.warnings.size() > kListedWarnings) lines << QObject::tr("另有 %1 条提醒").arg(report.warnings.size() - kListedWarnings);
    return lines.join(QLatin1Char('\n'));
}
} // namespace

void WorldModule::ImportPlacementManifest() {
    if (!page_ || !project_.mapEdits_ || !project_.mapEdits_->Terrain() || project_.savingMap_) {
        selectionStatus_->setText(tr("请等待地图载入或保存完成后再导入摆放清单")); return;
    }
    QDialog dialog(page_);
    dialog.setObjectName(QStringLiteral("worldPlacementImportDialog"));
    dialog.setWindowTitle(tr("导入摆放清单"));
    auto* form = new QFormLayout;
    auto* fileRow = new QHBoxLayout;
    auto* file = new QLineEdit(&dialog); file->setReadOnly(true); file->setMinimumWidth(360);
    file->setObjectName(QStringLiteral("worldPlacementImportFile"));
    auto* browse = new QPushButton(tr("浏览…"), &dialog);
    fileRow->addWidget(file, 1); fileRow->addWidget(browse);
    form->addRow(tr("清单文件"), fileRow);
    // 模型写明范围：观察点 P、P 周围立方体半边长、是否加宽导入副本的范围。
    const auto& header = project_.mapEdits_->Terrain()->header;
    const double west = header.offset[0], south = header.offset[1];
    const double east = west + 128.0 * (header.width - 1), north = south + 128.0 * (header.height - 1);
    auto* standpointRow = new QHBoxLayout;
    auto* useStandpoint = new QCheckBox(tr("观察点"), &dialog);
    useStandpoint->setObjectName(QStringLiteral("worldPlacementImportStandpoint"));
    standpointRow->addWidget(useStandpoint);
    std::array<QDoubleSpinBox*, 3> point{};
    const std::array<std::pair<double, double>, 3> ranges{{{west, east}, {south, north},
        {-Services::PlacementImportMaximumStandpointMargin, Services::PlacementImportMaximumStandpointMargin}}};
    const std::array<const char*, 3> pointNames{"worldPlacementImportStandpointX", "worldPlacementImportStandpointY",
        "worldPlacementImportStandpointZ"};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        point[axis] = new QDoubleSpinBox(&dialog);
        point[axis]->setObjectName(QString::fromLatin1(pointNames[axis]));
        point[axis]->setRange(ranges[axis].first, ranges[axis].second);
        point[axis]->setDecimals(1);
        point[axis]->setValue(axis < 2 ? (ranges[axis].first + ranges[axis].second) / 2 : 0.0);
        point[axis]->setPrefix(QStringLiteral("XYZ").mid(qsizetype(axis), 1) + QStringLiteral(" "));
        point[axis]->setEnabled(false);
        standpointRow->addWidget(point[axis], 1);
    }
    form->addRow(tr("范围检查"), standpointRow);
    auto* marginRow = new QHBoxLayout;
    auto* margin = new QDoubleSpinBox(&dialog);
    margin->setObjectName(QStringLiteral("worldPlacementImportMargin"));
    margin->setRange(1, Services::PlacementImportMaximumStandpointMargin);
    margin->setDecimals(0);
    margin->setValue(Services::PlacementImportDefaultStandpointMargin);
    margin->setPrefix(tr("余量 "));
    margin->setEnabled(false);
    auto* widen = new QCheckBox(tr("加宽模型范围"), &dialog);
    widen->setObjectName(QStringLiteral("worldPlacementImportWiden"));
    widen->setEnabled(false);
    marginRow->addWidget(margin, 1); marginRow->addWidget(widen);
    form->addRow(QString(), marginRow);
    auto* report = new QLabel(&dialog);
    report->setObjectName(QStringLiteral("worldPlacementImportReport"));
    report->setWordWrap(true); report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    report->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto* reportArea = new QScrollArea(&dialog);
    reportArea->setWidgetResizable(true); reportArea->setWidget(report); reportArea->setMinimumHeight(240);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* accept = buttons->button(QDialogButtonBox::Ok);
    accept->setText(tr("导入")); accept->setEnabled(false);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form); layout->addWidget(reportArea, 1); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const auto options = [&] {
        Services::PlacementImportBoundsOptions bounds;
        if (useStandpoint->isChecked()) bounds.standpoint = std::array<double, 3>{point[0]->value(), point[1]->value(), point[2]->value()};
        bounds.margin = margin->value();
        bounds.widen = useStandpoint->isChecked() && widen->isChecked();
        return bounds;
    };
    std::optional<MapData::PlacementManifest> manifest;
    // 读入清单或改动范围选项后按当前工程重新规划；规划失败时保留已读的清单，只禁止导入。
    const auto replan = [&] {
        accept->setEnabled(false);
        if (!manifest) return;
        const auto planned = QtProject::PlanProjectPlacementImport(project_, *manifest, options());
        if (!planned.Succeeded() || !planned.value) {
            report->setText(tr("无法导入：%1").arg(WorldDiagnosticText(planned.diagnostics))); return;
        }
        report->setText(ReportText(*manifest, planned.value->report));
        accept->setEnabled(true);
    };
    connect(useStandpoint, &QCheckBox::toggled, &dialog, [&](bool on) {
        for (auto* box : point) box->setEnabled(on);
        margin->setEnabled(on); widen->setEnabled(on);
        replan();
    });
    for (auto* box : point) connect(box, &QDoubleSpinBox::editingFinished, &dialog, replan);
    connect(margin, &QDoubleSpinBox::editingFinished, &dialog, replan);
    connect(widen, &QCheckBox::toggled, &dialog, replan);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("选择摆放清单"),
            placementManifestDirectory_, tr("摆放清单 (*.json)"));
        if (path.isEmpty()) return;
        placementManifestDirectory_ = QFileInfo(path).absolutePath();
        file->setText(QDir::toNativeSeparators(path));
        manifest.reset(); accept->setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto read = MapData::ReadPlacementManifest(std::filesystem::path(path.toStdWString()));
        QApplication::restoreOverrideCursor();
        if (!read.Succeeded() || !read.value) {
            report->setText(tr("无法读取：%1").arg(WorldDiagnosticText(read.diagnostics))); return;
        }
        if (const auto budget = QtProject::CheckPlacementImportPayloadBudget(*read.value); !budget.Succeeded()) {
            report->setText(tr("无法导入：%1").arg(WorldDiagnosticText(budget.diagnostics))); return;
        }
        manifest = std::move(*read.value);
        replan();
    });
    if (dialog.exec() != QDialog::Accepted || !manifest) return;
    // 提交时按当前工程与同一组范围选项重新规划；模态期间的修改会反映在实际分配的类型编号里。
    const auto committed = QtProject::CommitProjectPlacementImport(project_, std::move(*manifest), options());
    if (!committed.Succeeded() || !committed.value) {
        project_.AddDiagnostics(committed.diagnostics);
        selectionStatus_->setText(tr("摆放清单未导入：%1").arg(WorldDiagnosticText(committed.diagnostics)));
        return;
    }
    const auto& result = *committed.value;
    selectionStatus_->setText(tr("已导入 %1 个装饰物、%2 个单位、%3 个类型、%4 个文件")
        .arg(result.doodads).arg(result.units).arg(result.types.size()).arg(result.files));
    if (!project_.mapDataUndoStack_) RefreshPlacements();
}

} // namespace War3Map::Modules
