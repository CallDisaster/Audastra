#include "WorldModule.h"
#include "WorldPanelWidgets.h"
#include "EditorProject.h"
#include "Commands/MapDataCommands.h"
#include "War3MapCore/Services/TerrainBoundsEditService.h"
#include "War3MapCore/Services/TerrainPathingSave.h"
#include "War3MapCore/VerifiedFile.h"

#include <QDir>
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
#include <cmath>
#include <exception>

namespace War3Map::Modules {
namespace {
constexpr std::size_t kSourceBytes = 64u * 1024u * 1024u;
constexpr std::size_t kCandidateBytes = 512u * 1024u * 1024u;
constexpr std::size_t kWork = 64u * 1024u * 1024u;
using Request = QtProject::TerrainWaterEditRequest;
using Rectangle = MapData::TerrainPlayableRectangle;

Status BoundsFailure(const char* code, const char* message) {
    return Status::Failure("World.TerrainBounds", code, message);
}

Services::MapDataQueryBinding BoundsDocument(const Request& request) {
    // 文档ID来自真实会话，代次与SHA来自同次工程冻结；不是界面自报的来源许可。
    return {"world-bounds-" + std::to_string(request.sessionIdentity),
        std::max<std::uint64_t>(1, request.projectGeneration), request.revision, request.sourceSha256};
}

Status VerifyBoundsSource(const Request& request, const MapData::TerrainPathingResources* owner = nullptr) {
    // 此处复核原始来源；草稿已有范围修改时，不能按保存输出的After地形验来源。
    const auto& opened = request.archivePath.empty() ? request.projectRoot : request.archivePath;
    std::error_code error;
    if (!std::filesystem::equivalent(opened, request.sourcePath, error) || error)
        return BoundsFailure("WORLD_BOUNDS_SOURCE_CHANGED", "地图打开路径已变化，请重新打开地图。");
    if (request.archive) {
        const auto source = request.archive->VerifySourceUnchanged();
        if (!source.Succeeded()) return source;
        return owner ? owner->VerifySourceInputs(*request.archive, request.maximumBytes) : Status::Ok();
    }
    if (!request.directory)
        return BoundsFailure("WORLD_BOUNDS_SOURCE_REQUIRED", "地图范围缺少可复核的真实来源。");
    return owner ? owner->VerifySourceInputs(*request.directory, request.maximumBytes) : Status::Ok();
}

Result<std::vector<std::byte>> ReadBoundsSourceTerrain(const Request& request) {
    using Output = Result<std::vector<std::byte>>;
    if (request.archive) {
        const std::array<std::string, 1> names{"war3map.w3e"};
        ArchiveExactMemberLimits limits;
        limits.memberBytes = limits.totalBytes = request.maximumBytes;
        auto members = request.archive->CaptureExactMembers(names, limits);
        if (!members.Succeeded()) return {{}, std::move(members.diagnostics)};
        if (members.value->size() != 1 || !members.value->front().present)
            return Output::Failure("World.TerrainBounds", "WORLD_BOUNDS_TERRAIN_MISSING", "地图缺少原始地形。");
        return Output::Success(std::move(members.value->front().bytes));
    }
    const WorkspaceFile* selected = nullptr;
    if (request.directory) for (const auto& file : request.directory->files) {
        const auto path = QDir::fromNativeSeparators(QString::fromStdWString(file.relativePath.wstring())).toCaseFolded();
        if (path != QStringLiteral("war3map.w3e") && path != QStringLiteral("map/war3map.w3e")) continue;
        if (selected) return Output::Failure("World.TerrainBounds", "WORLD_BOUNDS_TERRAIN_AMBIGUOUS", "目录地形来源有冲突。");
        selected = &file;
    }
    if (!selected || selected->size > request.maximumBytes)
        return Output::Failure("World.TerrainBounds", "WORLD_BOUNDS_TERRAIN_UNAVAILABLE", "目录地形缺失或超过读取预算。");
    auto bytes = ReadVerifiedFile(request.sourcePath / selected->relativePath, selected->sha256, request.maximumBytes);
    if (bytes.Succeeded() && bytes.value->size() != selected->size)
        return Output::Failure("World.TerrainBounds", "WORLD_BOUNDS_SOURCE_CHANGED", "目录地形长度已经变化。");
    return bytes;
}

struct BoundsResult {
    Status status;
    Rectangle current;
    std::array<float, 4> margins{};
    std::optional<MapData::TerrainPathingResources> owner;
    std::optional<Services::TerrainSessionResources> structureResources;
    std::optional<Services::PreparedTerrainBoundsEdit> prepared;
    std::string rulesSha;
    qint64 milliseconds = 0;
};

// 完整来源、当前DOO、实际CameraMargins和密封候选全部只在worker读取/准备。
void PrepareBoundsTask(const Request& request, const MapData::MapDataEditSession& frozen,
    const std::optional<Rectangle>& intent, const std::string& expectedRulesSha, BoundsResult& result) {
    const auto source = VerifyBoundsSource(request);
    if (!source.Succeeded()) { result.status = source; return; }
    Result<MapData::TerrainPathingResources> owner;
    if (frozen.HasTerrainResizeChanges()) {
        auto view = request.archive
            ? Services::TerrainSessionResources::Capture(request.archive, request.structureSnapshot, request.gameRoot,
                request.maximumBytes, request.structureRetainedBytes)
            : Services::TerrainSessionResources::Capture(*request.directory, request.structureSnapshot, request.gameRoot,
                request.maximumBytes, request.structureRetainedBytes);
        if (!view.Succeeded() || !view.value) { result.status.diagnostics = std::move(view.diagnostics); return; }
        owner = view.value->CaptureNative(*frozen.Terrain(), nullptr, request.maximumBytes);
        result.structureResources.emplace(std::move(*view.value));
    } else {
        auto bytes = ReadBoundsSourceTerrain(request);
        if (!bytes.Succeeded()) { result.status.diagnostics = std::move(bytes.diagnostics); return; }
        auto terrain = MapData::ParseMapTerrain(*bytes.value);
        if (!terrain.Succeeded()) { result.status.diagnostics = std::move(terrain.diagnostics); return; }
        auto doodads = frozen.Snapshot(MapDataKind::Doodads, request.maximumBytes);
        if (!doodads.Succeeded()) { result.status.diagnostics = std::move(doodads.diagnostics); return; }
        const auto& member = doodads.value->Member(MapDataKind::Doodads);
        const std::vector<std::byte>* currentDoodads = nullptr;
        if (member.state == MapDataState::Ready && member.sources.size() == 1) currentDoodads = &member.sources.front().data;
        else if (member.state != MapDataState::Missing || !member.sources.empty()) {
            result.status = BoundsFailure("WORLD_BOUNDS_DOODADS_UNAVAILABLE", "装饰物来源不完整，范围未修改。"); return;
        }
        owner = request.archive
            ? Services::TerrainPathingSave::CaptureNative(*request.archive, *terrain.value->Snapshot(), *request.terrain,
                request.gameRoot, request.maximumBytes, currentDoodads)
            : Services::TerrainPathingSave::CaptureNative(*request.directory, *terrain.value->Snapshot(), *request.terrain,
                request.gameRoot, request.maximumBytes, currentDoodads);
    }
    if (!owner.Succeeded()) { result.status.diagnostics = std::move(owner.diagnostics); return; }
    if (!owner.value->NativeCameraMargins()) {
        result.status = BoundsFailure("WORLD_BOUNDS_RULES_UNAVAILABLE", "无法读取游戏镜头边距，范围未修改。"); return;
    }
    result.margins = *owner.value->NativeCameraMargins();
    auto fingerprint = owner.value->TerrainBoundsDependencyFingerprint();
    if (!fingerprint.Succeeded()) { result.status.diagnostics = std::move(fingerprint.diagnostics); return; }
    result.rulesSha = std::move(*fingerprint.value);
    if (intent && result.rulesSha != expectedRulesSha) {
        result.status = BoundsFailure("WORLD_BOUNDS_RULES_CHANGED", "地图或游戏规则已经变化，请重新读取当前范围。"); return;
    }
    if (intent) {
        const auto document = BoundsDocument(request);
        const Services::TerrainEditBinding binding{document, request.sessionIdentity, request.revision, request.terrainSourceRevision};
        auto prepared = Services::PrepareTerrainBoundsEdit(frozen, document, binding, *owner.value,
            MapData::TerrainBoundsEditIntent{*intent, 1}, kCandidateBytes, kWork);
        if (!prepared.Succeeded()) { result.status.diagnostics = std::move(prepared.diagnostics); return; }
        result.prepared.emplace(std::move(*prepared.value));
    } else {
        auto settings = frozen.Snapshot(MapDataKind::Settings, 1024u * 1024u);
        if (!settings.Succeeded()) { result.status.diagnostics = std::move(settings.diagnostics); return; }
        if (!settings.value->Settings()) {
            result.status = BoundsFailure("WORLD_BOUNDS_SETTINGS_UNAVAILABLE", "当前地图说明无法完整读取。"); return;
        }
        auto bounds = MapData::ComputeTerrainNativePlayableBounds(request.terrain->header, *settings.value->Settings(), result.margins);
        if (!bounds.Succeeded()) { result.status.diagnostics = std::move(bounds.diagnostics); return; }
        const auto& header = request.terrain->header;
        const std::array<double, 4> values{(double(bounds.value->left) - header.offset[0]) / 128,
            (double(bounds.value->bottom) - header.offset[1]) / 128,
            (double(bounds.value->right) - header.offset[0]) / 128,
            (double(bounds.value->top) - header.offset[1]) / 128};
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (!std::isfinite(values[i]) || std::trunc(values[i]) != values[i] || values[i] < 0
                || values[i] > double(i % 2 ? header.height - 1 : header.width - 1)) {
                result.status = BoundsFailure("WORLD_BOUNDS_INTEGER_DOMAIN", "当前可玩范围不是地图内的整数地格，暂不支持调整。"); return;
            }
        }
        result.current = {static_cast<std::uint32_t>(values[0]), static_cast<std::uint32_t>(values[1]),
            static_cast<std::uint32_t>(values[2]), static_cast<std::uint32_t>(values[3])};
    }
    result.status = result.structureResources ? result.structureResources->Verify(*owner.value, request.maximumBytes)
        : VerifyBoundsSource(request, &*owner.value);
    if (result.status.Succeeded()) result.owner.emplace(std::move(*owner.value));
}
} // namespace

QWidget* WorldModule::BuildTerrainBoundsControls(QWidget* parent) {
    terrainBoundsSection_ = WorldUi::Surface(parent);
    terrainBoundsSection_->setObjectName(QStringLiteral("worldTerrainBounds"));
    auto* column = new QVBoxLayout(terrainBoundsSection_);
    column->setContentsMargins(0, 12, 0, 0); column->setSpacing(8);
    column->addWidget(WorldUi::SectionLabel(tr("地图可玩范围"), terrainBoundsSection_));
    auto* help = new QLabel(tr("以西南角为 0，单位为地格；东界、北界不包含在范围内。"), terrainBoundsSection_);
    help->setWordWrap(true); column->addWidget(help);
    auto* grid = new QGridLayout;
    const std::array names{"West", "South", "East", "North"};
    const std::array labels{tr("西界"), tr("南界"), tr("东界"), tr("北界")};
    for (std::size_t i = 0; i < terrainBoundsFields_.size(); ++i) {
        auto* field = new QSpinBox(terrainBoundsSection_);
        field->setObjectName(QStringLiteral("worldTerrainBounds") + QLatin1String(names[i]));
        field->setRange(0, 0); field->setEnabled(false); field->setKeyboardTracking(false);
        terrainBoundsFields_[i] = field;
        auto* label = new QLabel(labels[i], terrainBoundsSection_); label->setBuddy(field);
        grid->addWidget(label, static_cast<int>(i / 2), static_cast<int>((i % 2) * 2));
        grid->addWidget(field, static_cast<int>(i / 2), static_cast<int>((i % 2) * 2 + 1));
        connect(field, &QSpinBox::valueChanged, this, &WorldModule::UpdateTerrainBoundsControls);
    }
    column->addLayout(grid);
    terrainBoundsRead_ = new QPushButton(tr("读取当前范围"), terrainBoundsSection_);
    terrainBoundsRead_->setObjectName(QStringLiteral("worldTerrainBoundsRead"));
    terrainBoundsApply_ = new QPushButton(tr("应用范围"), terrainBoundsSection_);
    terrainBoundsApply_->setObjectName(QStringLiteral("worldTerrainBoundsApply"));
    column->addWidget(terrainBoundsRead_); column->addWidget(terrainBoundsApply_);
    terrainBoundsStatus_ = new QLabel(tr("先读取当前范围，再调整边界。"), terrainBoundsSection_);
    terrainBoundsStatus_->setObjectName(QStringLiteral("worldTerrainBoundsStatus"));
    terrainBoundsStatus_->setWordWrap(true); column->addWidget(terrainBoundsStatus_);
    connect(terrainBoundsRead_, &QPushButton::clicked, this, [this] { StartTerrainBoundsTask(false); });
    connect(terrainBoundsApply_, &QPushButton::clicked, this, [this] { StartTerrainBoundsTask(true); });
    UpdateTerrainBoundsControls();
    return terrainBoundsSection_;
}

void WorldModule::InvalidateTerrainBoundsControls() {
    ++terrainBoundsSerial_;
    terrainBoundsReadRequest_.reset();
    terrainBoundsRulesSha_.clear();
    if (terrainBoundsStatus_) {
        terrainBoundsStatus_->setProperty("outcome", "stale");
        terrainBoundsStatus_->setText(tr("地图或修订已变化，请重新读取当前范围。"));
    }
    UpdateTerrainBoundsControls();
}

void WorldModule::UpdateTerrainBoundsControls() {
    if (!terrainBoundsStatus_) return;
    const auto* edits = project_.MapEdits();
    const auto terrain = edits ? edits->Terrain() : nullptr;
    const bool available = terrain && terrain->header.width >= 2 && terrain->header.width <= 2049
        && terrain->header.height >= 2 && terrain->header.height <= 2049 && edits->HasTerrainPathing()
        && project_.mapDataUndoStack_ && !project_.savingMap_ && !project_.HasPendingAssetEdits()
        && !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_;
    const bool busy = !terrainBoundsWorker_.isNull() || !terrainResizeWorker_.isNull();
    const bool current = available && terrainBoundsReadRequest_
        && QtProject::TerrainWaterEditRequestCurrent(project_, *terrainBoundsReadRequest_, true);
    bool valid = false;
    if (current) {
        const int width = terrainBoundsFields_[2]->value() - terrainBoundsFields_[0]->value();
        const int height = terrainBoundsFields_[3]->value() - terrainBoundsFields_[1]->value();
        valid = width > 0 && height > 0 && width - terrainBoundsMargins_[0] - terrainBoundsMargins_[1] >= 1
            && height - terrainBoundsMargins_[2] - terrainBoundsMargins_[3] >= 1;
        if (!busy) terrainBoundsStatus_->setText(valid
            ? tr("待应用：%1 × %2 地格。应用后可撤销。").arg(width).arg(height)
            : tr("范围须非空，且扣除游戏镜头边距后每轴至少保留一地格。"));
    }
    for (auto* field : terrainBoundsFields_) field->setEnabled(current && !busy);
    terrainBoundsRead_->setEnabled(available && !busy);
    terrainBoundsApply_->setEnabled(current && valid && !busy);
    terrainBoundsStatus_->setProperty("busy", busy);
    terrainBoundsStatus_->setProperty("ready", current && !busy);
    if (!available && !busy) terrainBoundsStatus_->setText(tr("请打开地形、通行图与来源完整的地图；保存期间不能调整。"));
}

void WorldModule::StartTerrainBoundsTask(bool apply) {
    if (!terrainBoundsStatus_ || terrainBoundsWorker_ || terrainResizeWorker_ || project_.savingMap_ || !project_.mapDataUndoStack_
        || !project_.mapEdits_ || strokeBase_ || waterCommit_) return;
    UpdateTerrainBoundsControls();
    if (apply ? !terrainBoundsApply_->isEnabled() : !terrainBoundsRead_->isEnabled()) return;
    std::optional<Rectangle> intent;
    if (apply) intent = Rectangle{static_cast<std::uint32_t>(terrainBoundsFields_[0]->value()),
        static_cast<std::uint32_t>(terrainBoundsFields_[1]->value()), static_cast<std::uint32_t>(terrainBoundsFields_[2]->value()),
        static_cast<std::uint32_t>(terrainBoundsFields_[3]->value())};
    QElapsedTimer captureTimer; captureTimer.start();
    auto request = QtProject::CaptureTerrainWaterEditRequest(project_, true, kSourceBytes);
    auto frozen = request.Succeeded() && request.value->structureSnapshot
        ? Result<std::shared_ptr<const MapData::MapDataEditSession>>::Success(request.value->structureSnapshot)
        : project_.mapEdits_->FreezeForSave(kCandidateBytes);
    terrainBoundsStatus_->setProperty("freezeMilliseconds", captureTimer.elapsed());
    if (!request.Succeeded() || !frozen.Succeeded()) {
        const auto& diagnostics = !request.Succeeded() ? request.diagnostics : frozen.diagnostics;
        terrainBoundsStatus_->setProperty("outcome", "failed");
        terrainBoundsStatus_->setText(WorldDiagnosticText(diagnostics)); return;
    }
    const auto ownedRequest = std::make_shared<const Request>(std::move(*request.value));
    const auto result = std::make_shared<BoundsResult>();
    const auto expectedRulesSha = terrainBoundsRulesSha_;
    const auto serial = ++terrainBoundsSerial_;
    const auto generation = generation_;
    auto* thread = QThread::create([ownedRequest, frozen = std::move(*frozen.value), intent, expectedRulesSha, result] {
        QElapsedTimer timer; timer.start();
        try { PrepareBoundsTask(*ownedRequest, *frozen, intent, expectedRulesSha, *result); }
        catch (const std::exception&) {
            result->status = BoundsFailure("WORLD_BOUNDS_PREPARATION_FAILED", "地图范围读取或准备失败，地图未修改。");
        }
        result->milliseconds = timer.elapsed();
    });
    terrainBoundsWorker_ = thread;
    terrainBoundsStatus_->setProperty("outcome", "pending");
    UpdateTerrainBoundsControls();
    terrainBoundsStatus_->setText(apply ? tr("正在准备并核对范围…") : tr("正在读取当前范围…"));
    connect(thread, &QThread::finished, this, [this, thread, ownedRequest, intent, result, serial, generation] {
        if (terrainBoundsWorker_ == thread) terrainBoundsWorker_ = nullptr;
        if (!page_ || serial != terrainBoundsSerial_ || generation != generation_) {
            UpdateTerrainBoundsControls(); return;
        }
        terrainBoundsStatus_->setProperty("prepareMilliseconds", result->milliseconds);
        const auto current = [&] {
            return !project_.savingMap_ && project_.mapDataUndoStack_ && project_.mapEdits_
                && !strokeBase_ && !waterCommit_
                && QtProject::TerrainWaterEditRequestCurrent(project_, *ownedRequest, true);
        };
        if (!current()) result->status = BoundsFailure("WORLD_BOUNDS_EDIT_STALE", "地图或修订已变化，本次范围未提交，请重新读取。");
        if (!result->status.Succeeded()) {
            terrainBoundsReadRequest_.reset(); UpdateTerrainBoundsControls();
            terrainBoundsStatus_->setProperty("outcome", "failed");
            terrainBoundsStatus_->setText(WorldDiagnosticText(result->status.diagnostics));
            project_.AddDiagnostics(result->status.diagnostics); return;
        }
        if (!intent) {
            terrainBoundsReadRequest_ = ownedRequest; terrainBoundsMargins_ = result->margins;
            terrainBoundsRulesSha_ = result->rulesSha;
            const std::array values{result->current.west, result->current.south, result->current.east, result->current.north};
            for (std::size_t i = 0; i < terrainBoundsFields_.size(); ++i) {
                const QSignalBlocker blocked(terrainBoundsFields_[i]);
                terrainBoundsFields_[i]->setRange(0, static_cast<int>(i % 2 ? ownedRequest->terrain->header.height - 1
                    : ownedRequest->terrain->header.width - 1));
                terrainBoundsFields_[i]->setValue(static_cast<int>(values[i]));
            }
            UpdateTerrainBoundsControls(); terrainBoundsStatus_->setProperty("outcome", "read"); return;
        }
        if (!result->prepared || !result->owner) {
            terrainBoundsStatus_->setProperty("outcome", "failed");
            terrainBoundsStatus_->setText(tr("范围候选未完整准备，地图未修改。")); UpdateTerrainBoundsControls(); return;
        }
        const auto before = project_.mapEdits_->Revision();
        std::unique_ptr<QtProject::MapDataEditCommand> undo;
        QElapsedTimer applyTimer; applyTimer.start();
        const auto applied = Services::ApplyTerrainBoundsEdit(*project_.mapEdits_, BoundsDocument(*ownedRequest), *result->prepared,
            [&](const auto& receipt) {
                // 实际来源/依赖已由worker在准备结束再次核对；这里只做同一冻结点的CAS。
                if (!current()) return BoundsFailure("WORLD_BOUNDS_EDIT_STALE", "范围发布前地图修订已变化。");
                try { undo = std::make_unique<QtProject::MapDataEditCommand>(project_, receipt, tr("地图可玩范围"), "修改地图可玩范围"); }
                catch (const std::exception&) { return BoundsFailure("WORLD_BOUNDS_UNDO_BUDGET", "无法准备撤销记录，范围未修改。"); }
                return Status::Ok();
            });
        terrainBoundsStatus_->setProperty("applyMilliseconds", applyTimer.elapsed());
        if (!applied.Succeeded()) {
            UpdateTerrainBoundsControls(); terrainBoundsStatus_->setProperty("outcome", "failed");
            terrainBoundsStatus_->setText(WorldDiagnosticText(applied.diagnostics)); project_.AddDiagnostics(applied.diagnostics); return;
        }
        if (project_.mapEdits_->Revision() == before) {
            UpdateTerrainBoundsControls(); terrainBoundsStatus_->setProperty("outcome", "unchanged");
            terrainBoundsStatus_->setText(tr("当前范围已经相同，无需修改。")); return;
        }
        QElapsedTimer publishTimer; publishTimer.start();
        (void)project_.ObserveMapDataEdit("修改地图可玩范围");
        project_.mapDataUndoStack_->push(undo.release());
        terrainBoundsStatus_->setProperty("publishMilliseconds", publishTimer.elapsed());
        // 共用撤销栈触发RefreshMapEdits，地形和地图设置按同一次事务更新。
        terrainBoundsStatus_->setProperty("outcome", "applied");
        terrainBoundsStatus_->setText(tr("可玩范围已应用：%1 × %2 地格，可撤销。")
            .arg(intent->east - intent->west).arg(intent->north - intent->south));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start(QThread::LowPriority);
}

} // namespace War3Map::Modules
