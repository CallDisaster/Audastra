#include "WorldModule.h"
#include "EditorProject.h"
#include "ProjectPlacementImport.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementManifest.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <filesystem>
#include <optional>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kListedTypes = 20;

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
    for (const auto& warning : report.warnings) lines << QObject::tr("提醒：%1").arg(QString::fromStdString(warning.message));
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
    auto* report = new QLabel(&dialog);
    report->setObjectName(QStringLiteral("worldPlacementImportReport"));
    report->setWordWrap(true); report->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* accept = buttons->button(QDialogButtonBox::Ok);
    accept->setText(tr("导入")); accept->setEnabled(false);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form); layout->addWidget(report); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    std::optional<MapData::PlacementManifest> manifest;
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
        const auto planned = QtProject::PlanProjectPlacementImport(project_, *read.value);
        if (!planned.Succeeded() || !planned.value) {
            report->setText(tr("无法导入：%1").arg(WorldDiagnosticText(planned.diagnostics))); return;
        }
        report->setText(ReportText(*read.value, planned.value->report));
        manifest = std::move(*read.value);
        accept->setEnabled(true);
    });
    if (dialog.exec() != QDialog::Accepted || !manifest) return;
    // 提交时按当前工程重新规划；模态期间的修改会反映在实际分配的类型编号里。
    const auto committed = QtProject::CommitProjectPlacementImport(project_, std::move(*manifest));
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
