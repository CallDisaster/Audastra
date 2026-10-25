#include "WorldModule.h"
#include "WorldPanelWidgets.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "TerrainResizeEditPreparation.h"

#include <QElapsedTimer>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QThread>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <limits>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kSourceBytes = 64u * 1024u * 1024u;
using Request = QtProject::TerrainWaterEditRequest;
using Rectangle = MapData::TerrainCanvasRectangle;

Services::MapDataQueryBinding ResizeDocument(const Request& request) {
    return {"world-resize-" + std::to_string(request.sessionIdentity),
        std::max<std::uint64_t>(1, request.projectGeneration), request.revision, request.sourceSha256};
}
Status ResizeFailure(const char* code, const char* message) {
    return Status::Failure("World.TerrainResize", code, message);
}
}

QWidget* WorldModule::BuildTerrainResizeControls(QWidget* parent) {
    auto* section = WorldUi::Surface(parent);
    section->setObjectName(QStringLiteral("worldTerrainResize"));
    auto* column = new QVBoxLayout(section);
    column->setContentsMargins(0, 12, 0, 0); column->setSpacing(8);
    column->addWidget(WorldUi::SectionLabel(tr("地图尺寸"), section));
    auto* help = new QLabel(tr("四边相对当前地图西南角，单位为地格；扩西、扩南填负数。保留原可玩区和所有内容的世界位置。"), section);
    help->setWordWrap(true); column->addWidget(help);
    auto* grid = new QGridLayout;
    const std::array names{"West", "South", "East", "North"};
    const std::array labels{tr("西界"), tr("南界"), tr("东界"), tr("北界")};
    for (std::size_t i = 0; i < terrainResizeFields_.size(); ++i) {
        auto* field = new QSpinBox(section); field->setKeyboardTracking(false);
        field->setObjectName(QStringLiteral("worldTerrainResize") + QLatin1String(names[i]));
        field->setRange(0, 0); field->setEnabled(false); terrainResizeFields_[i] = field;
        auto* label = new QLabel(labels[i], section); label->setBuddy(field);
        grid->addWidget(label, static_cast<int>(i / 2), static_cast<int>((i % 2) * 2));
        grid->addWidget(field, static_cast<int>(i / 2), static_cast<int>((i % 2) * 2 + 1));
        connect(field, &QSpinBox::valueChanged, this, [this] {
            terrainResizeChecked_.reset(); UpdateTerrainResizeControls();
        });
    }
    column->addLayout(grid);
    terrainResizeRead_ = new QPushButton(tr("读取当前尺寸"), section);
    terrainResizeRead_->setObjectName(QStringLiteral("worldTerrainResizeRead"));
    terrainResizePreview_ = new QPushButton(tr("预览检查"), section);
    terrainResizePreview_->setObjectName(QStringLiteral("worldTerrainResizePreview"));
    terrainResizeApply_ = new QPushButton(tr("应用尺寸"), section);
    terrainResizeApply_->setObjectName(QStringLiteral("worldTerrainResizeApply"));
    column->addWidget(terrainResizeRead_); column->addWidget(terrainResizePreview_); column->addWidget(terrainResizeApply_);
    terrainResizeStatus_ = new QLabel(tr("先读取当前尺寸，再检查新外矩形。"), section);
    terrainResizeStatus_->setObjectName(QStringLiteral("worldTerrainResizeStatus"));
    terrainResizeStatus_->setWordWrap(true); column->addWidget(terrainResizeStatus_);
    connect(terrainResizeRead_, &QPushButton::clicked, this, [this] { StartTerrainResizeTask(TerrainResizeAction::Read); });
    connect(terrainResizePreview_, &QPushButton::clicked, this, [this] { StartTerrainResizeTask(TerrainResizeAction::Preview); });
    connect(terrainResizeApply_, &QPushButton::clicked, this, [this] { StartTerrainResizeTask(TerrainResizeAction::Apply); });
    UpdateTerrainResizeControls(); return section;
}

void WorldModule::InvalidateTerrainResizeControls() {
    ++terrainResizeSerial_; terrainResizeReadRequest_.reset(); terrainResizeChecked_.reset(); terrainResizeRulesSha_.clear();
    terrainResizeCommitToken_.reset();
    if (terrainResizeStatus_) {
        terrainResizeStatus_->setProperty("outcome", "stale");
        terrainResizeStatus_->setText(tr("地图或修订已变化，请重新读取当前尺寸。"));
    }
    UpdateTerrainResizeControls();
}

void WorldModule::UpdateTerrainResizeControls() {
    if (!terrainResizeStatus_) return;
    const auto* edits = project_.MapEdits(); const auto terrain = edits ? edits->Terrain() : nullptr;
    const bool available = terrain && terrain->header.width >= 2 && terrain->header.height >= 2
        && edits->HasTerrainPathing() && project_.mapDataUndoStack_ && !project_.savingMap_
        && !project_.HasPendingAssetEdits() && !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_;
    const bool busy = terrainResizeWorker_ || terrainBoundsWorker_;
    const bool current = available && terrainResizeReadRequest_
        && QtProject::TerrainWaterEditRequestCurrent(project_, *terrainResizeReadRequest_, true);
    const Rectangle rectangle{terrainResizeFields_[0]->value(), terrainResizeFields_[1]->value(),
        terrainResizeFields_[2]->value(), terrainResizeFields_[3]->value()};
    const auto width = std::int64_t(rectangle.east) - rectangle.west, height = std::int64_t(rectangle.north) - rectangle.south;
    const auto& rules = terrainResizeDimensions_;
    const bool valid = current && rules.sideStep && width >= rules.minimumSide && height >= rules.minimumSide
        && width <= rules.maximumSide && height <= rules.maximumSide && width > 0 && height > 0
        && width % rules.sideStep == 0 && height % rules.sideStep == 0 && width <= rules.maximumArea / height;
    const bool checked = valid && terrainResizeChecked_ && *terrainResizeChecked_ == rectangle;
    for (auto* field : terrainResizeFields_) field->setEnabled(current && !busy);
    terrainResizeRead_->setEnabled(available && !busy);
    terrainResizePreview_->setEnabled(valid && !busy); terrainResizeApply_->setEnabled(checked && !busy);
    terrainResizeStatus_->setProperty("busy", busy); terrainResizeStatus_->setProperty("ready", current && !busy);
    if (current && !busy && !checked) terrainResizeStatus_->setText(valid
        ? tr("待检查：%1 × %2 地格。原可玩区和内容须完整保留。").arg(width).arg(height)
        : tr("实际规则：每轴 %1–%2 地格，%3 格步长，面积不超过 %4。")
            .arg(rules.minimumSide).arg(rules.maximumSide).arg(rules.sideStep).arg(rules.maximumArea));
    if (!available && !busy) terrainResizeStatus_->setText(tr("请打开地形、通行图和来源完整的地图；保存期间不能调整尺寸。"));
}

void WorldModule::StartTerrainResizeTask(TerrainResizeAction action) {
    if (!terrainResizeStatus_ || terrainResizeWorker_ || terrainBoundsWorker_ || project_.savingMap_
        || !project_.mapDataUndoStack_ || !project_.mapEdits_ || strokeBase_ || waterCommit_) return;
    UpdateTerrainResizeControls();
    const auto* button = action == TerrainResizeAction::Read ? terrainResizeRead_
        : action == TerrainResizeAction::Preview ? terrainResizePreview_ : terrainResizeApply_;
    if (!button->isEnabled()) return;
    std::optional<MapData::TerrainResizeIntent> intent;
    if (action != TerrainResizeAction::Read) intent = MapData::TerrainResizeIntent{1,
        {terrainResizeFields_[0]->value(), terrainResizeFields_[1]->value(), terrainResizeFields_[2]->value(), terrainResizeFields_[3]->value()}};
    QElapsedTimer freezeTimer; freezeTimer.start();
    const MapData::TerrainResizeLimits limits;
    auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true, kSourceBytes, false);
    auto frozen = project_.mapEdits_->FreezeForSave(limits.maximumCandidateBytes);
    terrainResizeStatus_->setProperty("freezeMilliseconds", freezeTimer.elapsed());
    if (!request.Succeeded() || !frozen.Succeeded()) {
        const auto& diagnostics = !request.Succeeded() ? request.diagnostics : frozen.diagnostics;
        terrainResizeStatus_->setProperty("outcome", "failed");
        terrainResizeStatus_->setText(WorldDiagnosticText(diagnostics)); project_.AddDiagnostics(diagnostics); return;
    }
    const auto owned = std::make_shared<const Request>(std::move(*request.value));
    const auto result = std::make_shared<Result<QtProject::TerrainResizeEditCapture>>();
    const auto expectedRules = intent ? terrainResizeRulesSha_ : std::string{};
    const auto serial = ++terrainResizeSerial_, generation = generation_;
    auto* thread = QThread::create([owned, frozen = std::move(*frozen.value), intent, expectedRules, result, limits] {
        *result = QtProject::PrepareTerrainResizeEditCapture(owned, frozen, ResizeDocument(*owned), intent, expectedRules, limits);
    });
    terrainResizeWorker_ = thread; terrainResizeStatus_->setProperty("outcome", "pending");
    if (action == TerrainResizeAction::Apply) terrainResizeCommitToken_ = project_.BeginPendingTerrainCommit();
    UpdateTerrainResizeControls(); UpdateTerrainBoundsControls();
    terrainResizeStatus_->setText(action == TerrainResizeAction::Read ? tr("正在读取实际尺寸规则与内容…") : tr("正在检查尺寸、内容占地和派生来源…"));
    connect(thread, &QThread::finished, this, [this, thread, owned, intent, result, serial, generation, action] {
        std::unique_ptr<QObject> pendingCommit;
        if (terrainResizeWorker_ == thread) {
            terrainResizeWorker_ = nullptr;
            pendingCommit = std::move(terrainResizeCommitToken_);
        }
        UpdateTerrainBoundsControls();
        if (!page_ || serial != terrainResizeSerial_ || generation != generation_) { UpdateTerrainResizeControls(); return; }
        const auto current = [&] { return !project_.savingMap_ && project_.mapDataUndoStack_ && project_.mapEdits_
            && !strokeBase_ && !waterCommit_ && QtProject::TerrainWaterEditRequestCurrent(project_, *owned, true); };
        if (!current()) result->diagnostics = ResizeFailure("WORLD_RESIZE_EDIT_STALE", "地图或修订已变化，本次尺寸未提交。").diagnostics;
        if (!current() || !result->Succeeded() || !result->value) {
            terrainResizeChecked_.reset(); UpdateTerrainResizeControls();
            terrainResizeStatus_->setProperty("outcome", "failed");
            terrainResizeStatus_->setText(WorldDiagnosticText(result->diagnostics)); project_.AddDiagnostics(result->diagnostics); return;
        }
        const auto& capture = *result->value;
        if (action == TerrainResizeAction::Read) {
            terrainResizeReadRequest_ = owned; terrainResizeDimensions_ = capture.resources.DimensionRules();
            terrainResizeRulesSha_ = capture.resources.DependencyFingerprint(); terrainResizeChecked_.reset();
            const auto& header = owned->terrain->header;
            const std::array values{0, 0, static_cast<int>(header.width - 1), static_cast<int>(header.height - 1)};
            for (std::size_t i = 0; i < terrainResizeFields_.size(); ++i) {
                const QSignalBlocker blocked(terrainResizeFields_[i]);
                const auto maximum = std::min<std::int64_t>(std::numeric_limits<int>::max(),
                    std::int64_t(terrainResizeDimensions_.maximumSide) + (i % 2 ? header.height - 1 : header.width - 1));
                const auto minimum = std::max<std::int64_t>(std::numeric_limits<int>::min(), -std::int64_t(terrainResizeDimensions_.maximumSide));
                terrainResizeFields_[i]->setRange(static_cast<int>(minimum), static_cast<int>(maximum));
                terrainResizeFields_[i]->setValue(values[i]);
            }
            UpdateTerrainResizeControls(); terrainResizeStatus_->setProperty("outcome", "read");
            terrainResizeStatus_->setText(tr("当前 %1 × %2 地格（%3 × %4 顶点）。实际规则：每轴 %5–%6，步长 %7，面积 ≤ %8。")
                .arg(header.width - 1).arg(header.height - 1).arg(header.width).arg(header.height)
                .arg(terrainResizeDimensions_.minimumSide).arg(terrainResizeDimensions_.maximumSide)
                .arg(terrainResizeDimensions_.sideStep).arg(terrainResizeDimensions_.maximumArea)); return;
        }
        if (!capture.prepared || !intent) {
            terrainResizeStatus_->setProperty("outcome", "failed"); terrainResizeStatus_->setText(tr("尺寸候选不完整，地图未修改。")); return;
        }
        if (action == TerrainResizeAction::Preview) {
            terrainResizeChecked_ = intent->canvas; UpdateTerrainResizeControls();
            terrainResizeStatus_->setProperty("outcome", "previewed");
            terrainResizeStatus_->setText(capture.prepared->Plan().HasChanges()
                ? tr("检查通过：原可玩区和内容位置保持，可应用后撤销。") : tr("检查通过：当前尺寸相同，无需修改。")); return;
        }
        const auto before = project_.mapEdits_->Revision();
        std::unique_ptr<QtProject::MapDataEditCommand> undo;
        const auto applied = Services::ApplyTerrainResizeEdit(*project_.mapEdits_, ResizeDocument(*owned), *capture.prepared,
            [&](const auto& receipt) {
                if (!current()) return ResizeFailure("WORLD_RESIZE_EDIT_STALE", "尺寸发布前地图或修订已变化。");
                try { undo = std::make_unique<QtProject::MapDataEditCommand>(project_, receipt, tr("地图尺寸"), "调整地图尺寸"); }
                catch (const std::exception&) { return ResizeFailure("WORLD_RESIZE_UNDO_BUDGET", "无法准备撤销记录，尺寸未修改。"); }
                return Status::Ok();
            });
        if (!applied.Succeeded()) {
            terrainResizeChecked_.reset(); UpdateTerrainResizeControls(); terrainResizeStatus_->setProperty("outcome", "failed");
            terrainResizeStatus_->setText(WorldDiagnosticText(applied.diagnostics)); project_.AddDiagnostics(applied.diagnostics); return;
        }
        if (project_.mapEdits_->Revision() == before) {
            UpdateTerrainResizeControls(); terrainResizeStatus_->setProperty("outcome", "unchanged");
            terrainResizeStatus_->setText(tr("当前尺寸已经相同，无需修改。")); return;
        }
        (void)project_.ObserveMapDataEdit("调整地图尺寸"); project_.mapDataUndoStack_->push(undo.release());
        terrainResizeStatus_->setProperty("outcome", "applied");
        terrainResizeStatus_->setText(tr("地图尺寸已应用，原可玩区和内容位置保持，可撤销。保存时重建派生文件。"));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}

} // namespace War3Map::Modules
