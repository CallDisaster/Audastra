#include "TriggerCommands.h"

#include "EditorProject.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include <type_traits>

#include <QDebug>

#include <algorithm>
#include <utility>

AppendEcaUndoCommand::AppendEcaUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::Trigger::EcaAppendReceipt receipt,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      receipt_(std::move(receipt)) {
}

void AppendEcaUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::UndoAppend(*document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "触发器新增撤销失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

void AppendEcaUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::RedoAppend(*document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "触发器新增重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

EditTriggerArgumentsUndoCommand::EditTriggerArgumentsUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    std::vector<TriggerArgumentChange> changes,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      changes_(std::move(changes)) {
}

void EditTriggerArgumentsUndoCommand::undo() {
    Apply(false);
}

void EditTriggerArgumentsUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    Apply(true);
}

void EditTriggerArgumentsUndoCommand::Apply(bool after) {
    if (document_ == nullptr) {
        return;
    }
    for (const TriggerArgumentChange& change : changes_) {
        auto* argument = War3Map::Trigger::TriggerDocumentEditor::FindArgument(
            *document_, change.argumentId);
        if (argument == nullptr) {
            qCritical() << "触发器参数撤销/重做失败：找不到参数"
                        << static_cast<qulonglong>(change.argumentId);
            continue;
        }
        *argument = after ? change.after : change.before;
    }
}

EditTriggerPropertiesUndoCommand::EditTriggerPropertiesUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::StableId triggerId,
    TriggerPropertyState before,
    TriggerPropertyState after,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      triggerId_(triggerId),
      before_(std::move(before)),
      after_(std::move(after)) {
}

void EditTriggerPropertiesUndoCommand::undo() {
    Apply(before_);
}

void EditTriggerPropertiesUndoCommand::redo() {
    // 不预先应用：压入时 QUndoStack 会立即调用 redo() 应用 after 状态。
    Apply(after_);
}

void EditTriggerPropertiesUndoCommand::Apply(const TriggerPropertyState& state) {
    if (document_ == nullptr) {
        return;
    }
    using Editor = War3Map::Trigger::TriggerDocumentEditor;
    War3Map::Status status = Editor::SetTriggerEnabled(
        *document_, triggerId_, state.enabled != 0);
    if (status.Succeeded()) {
        status = Editor::SetTriggerInitiallyOn(
            *document_, triggerId_, state.initiallyClosed == 0);
    }
    if (status.Succeeded()) {
        status = Editor::SetTriggerRunOnInit(
            *document_, triggerId_, state.runOnInit != 0);
    }
    if (status.Succeeded()) {
        status = Editor::SetTriggerComment(*document_, triggerId_, state.comment);
    }
    if (!status.Succeeded()) {
        qCritical() << "触发器属性撤销/重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

SetEcaEnabledUndoCommand::SetEcaEnabledUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::StableId ecaId,
    bool before,
    bool after,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      ecaId_(ecaId),
      before_(before),
      after_(after) {
}

void SetEcaEnabledUndoCommand::undo() {
    Apply(before_);
}

void SetEcaEnabledUndoCommand::redo() {
    Apply(after_);
}

void SetEcaEnabledUndoCommand::Apply(bool enabled) {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::SetEcaEnabled(
            *document_, ecaId_, enabled);
    if (!status.Succeeded()) {
        // 保护门禁拒绝后，这条命令没有产生任何状态变化。标记为 obsolete，
        // 让 QUndoStack 立即移除它，避免仅因一次被拒操作把项目误标为 dirty。
        setObsolete(true);
        qCritical() << "ECA 启用/禁用撤销/重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

RemoveEcaUndoCommand::RemoveEcaUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    EcaRemoveReceipt receipt,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      receipt_(std::move(receipt)) {
}

void RemoveEcaUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::UndoRemoveEcaSubtree(
            *document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "ECA 删除撤销失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

void RemoveEcaUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ == nullptr) {
        return;
    }
    auto result = War3Map::Trigger::TriggerDocumentEditor::RemoveEcaSubtree(
        *document_, receipt_.ecaId);
    if (!result.Succeeded()) {
        qCritical() << "ECA 删除重做失败"
                    << (result.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(result.diagnostics.front().message));
    }
}

AddTriggerUndoCommand::AddTriggerUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::StableId triggerId,
    QString text,
    QUndoCommand* parent,
    War3Map::Trigger::GuiConversionSnapshots* snapshots)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      triggerId_(triggerId), snapshots_(snapshots),
      snapshot_(War3Map::Trigger::GuiConversionSnapshots::PrepareRemoval(triggerId)) {
}

void AddTriggerUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    auto removed = War3Map::Trigger::TriggerDocumentEditor::RemoveTrigger(
        *document_, triggerId_);
    if (!removed.Succeeded()) {
        qCritical() << "新建触发器撤销失败"
                    << (removed.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(removed.diagnostics.front().message));
        return;
    }
    removal_ = std::move(*removed.value);
    if (snapshots_) snapshots_->Swap(snapshot_);
}

void AddTriggerUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        if (snapshots_) snapshots_->Swap(snapshot_);
        return;
    }
    if (document_ == nullptr || !removal_.has_value()) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::UndoRemoveTrigger(
            *document_, *removal_);
    if (!status.Succeeded()) {
        qCritical() << "新建触发器重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
        return;
    }
    if (snapshots_) snapshots_->Swap(snapshot_);
}

RenameTriggerUndoCommand::RenameTriggerUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::StableId triggerId,
    std::string before,
    std::string after,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      triggerId_(triggerId),
      before_(std::move(before)),
      after_(std::move(after)) {
}

void RenameTriggerUndoCommand::undo() {
    Apply(before_);
}

void RenameTriggerUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    Apply(after_);
}

void RenameTriggerUndoCommand::Apply(const std::string& name) {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::RenameTrigger(
            *document_, triggerId_, name);
    if (!status.Succeeded()) {
        qCritical() << "重命名触发器撤销/重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

RemoveTriggerUndoCommand::RemoveTriggerUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    TriggerRemoveReceipt receipt,
    QString text,
    QUndoCommand* parent,
    War3Map::Trigger::GuiConversionSnapshots* snapshots)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      receipt_(std::move(receipt)), snapshots_(snapshots),
      snapshot_(War3Map::Trigger::GuiConversionSnapshots::PrepareRemoval(receipt_.trigger.id)) {
}

void RemoveTriggerUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::UndoRemoveTrigger(
            *document_, receipt_);
    if (!status.Succeeded()) {
        qCritical() << "删除触发器撤销失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
        return;
    }
    if (snapshots_) snapshots_->Swap(snapshot_);
}

void RemoveTriggerUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        if (snapshots_) snapshots_->Swap(snapshot_);
        return;
    }
    if (document_ == nullptr) {
        return;
    }
    auto removed = War3Map::Trigger::TriggerDocumentEditor::RemoveTrigger(
        *document_, receipt_.trigger.id);
    if (!removed.Succeeded()) {
        qCritical() << "删除触发器重做失败"
                    << (removed.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(removed.diagnostics.front().message));
        return;
    }
    receipt_ = std::move(*removed.value);
    if (snapshots_) snapshots_->Swap(snapshot_);
}

AddVariableUndoCommand::AddVariableUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    std::string name,
    std::string typeName,
    bool isArray,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      name_(std::move(name)),
      typeName_(std::move(typeName)),
      isArray_(isArray) {
}

void AddVariableUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::RemoveVariable(*document_, name_);
    if (!status.Succeeded()) {
        qCritical() << "新建变量撤销失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

void AddVariableUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::AddVariable(
            *document_, name_, typeName_, isArray_);
    if (!status.Succeeded()) {
        qCritical() << "新建变量重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

RenameVariableUndoCommand::RenameVariableUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    std::string before,
    std::string after,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      before_(std::move(before)),
      after_(std::move(after)) {
}

void RenameVariableUndoCommand::undo() {
    Apply(after_, before_);
}

void RenameVariableUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    Apply(before_, after_);
}

void RenameVariableUndoCommand::Apply(const std::string& from, const std::string& to) {
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::RenameVariable(*document_, from, to);
    if (!status.Succeeded()) {
        qCritical() << "重命名变量撤销/重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

RemoveVariableUndoCommand::RemoveVariableUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    War3Map::Trigger::VariableRecord record,
    std::size_t row,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      record_(std::move(record)),
      row_(row) {
}

void RemoveVariableUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    const auto existing = std::find_if(
        document_->variables.begin(), document_->variables.end(),
        [this](const War3Map::Trigger::VariableRecord& value) {
            return value.name.text == record_.name.text;
        });
    if (existing != document_->variables.end()) {
        return; // 幂等：已存在则视为已撤销
    }
    const std::size_t row = std::min(row_, document_->variables.size());
    document_->variables.insert(
        document_->variables.begin() + static_cast<std::ptrdiff_t>(row), record_);
}

void RemoveVariableUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ == nullptr) {
        return;
    }
    const War3Map::Status status =
        War3Map::Trigger::TriggerDocumentEditor::RemoveVariable(
            *document_, record_.name.text);
    if (!status.Succeeded()) {
        qCritical() << "删除变量重做失败"
                    << (status.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(status.diagnostics.front().message));
    }
}

PasteEcaUndoCommand::PasteEcaUndoCommand(
    War3Map::Trigger::TriggerDocument* document,
    const War3Map::Trigger::UiCatalog* catalog,
    EcaSubtreeSnapshot snapshot,
    War3Map::StableId parentTriggerId,
    War3Map::StableId parentEcaId,
    std::size_t row,
    std::optional<std::int32_t> rootChildId,
    War3Map::StableId rootEcaId,
    QString text,
    QUndoCommand* parent)
    : QUndoCommand(std::move(text), parent),
      document_(document),
      catalog_(catalog),
      snapshot_(std::move(snapshot)),
      parentTriggerId_(parentTriggerId),
      parentEcaId_(parentEcaId),
      row_(row),
      rootChildId_(rootChildId),
      rootEcaId_(rootEcaId) {
}

void PasteEcaUndoCommand::undo() {
    if (document_ == nullptr) {
        return;
    }
    auto removed = War3Map::Trigger::TriggerDocumentEditor::RemoveEcaSubtree(
        *document_, rootEcaId_);
    if (!removed.Succeeded()) {
        qCritical() << "粘贴撤销失败"
                    << (removed.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(removed.diagnostics.front().message));
    }
}

void PasteEcaUndoCommand::redo() {
    if (firstRedo_) {
        firstRedo_ = false;
        return;
    }
    if (document_ == nullptr || catalog_ == nullptr) {
        return;
    }
    auto paste = War3Map::Trigger::TriggerDocumentEditor::PasteEcaSubtree(
        *document_, *catalog_, snapshot_, parentTriggerId_, parentEcaId_, row_, rootChildId_);
    if (!paste.Succeeded()) {
        qCritical() << "粘贴重做失败"
                    << (paste.diagnostics.empty()
                        ? QStringLiteral("未知错误")
                        : QString::fromUtf8(paste.diagnostics.front().message));
        return;
    }
    rootEcaId_ = paste.value->rootEcaId;
}

namespace War3Map::QtProject {

struct TriggerGraphSnapshotUndoCommand::AssetStorage {
    AssetState before, after;
    const MapArchiveSession* archive = nullptr;
    const DirectoryWorkspace* directory = nullptr;
    std::filesystem::path sourcePath, projectPath;
};

Status TriggerGraphSnapshotUndoCommand::PrepareAssets(const std::function<Status(AssetState&)>& prepare) {
    if (!prepare || !project_ || !snapshot_ || scalarReceipt_ || assets_ || *firstPublication_ || !Ready())
        return Status::Failure("QtProject.Trigger", "TRIGGER_ASSET_PREPARE_INVALID", "关联资源只能在完整图首次发布前准备一次。");
    try {
        auto storage = std::make_shared<AssetStorage>();
        storage->before = project_->CaptureAssets(); storage->after = storage->before;
        storage->archive = project_->archiveSession_.get(); storage->directory = project_->directorySnapshot_.get();
        storage->sourcePath = project_->sourceArchivePath_; storage->projectPath = project_->projectFilePath_;
        const auto ready = prepare(storage->after);
        if (!ready.Succeeded()) return ready;
        if (!project_ || !Ready() || project_->CaptureAssets() != storage->before)
            return Status::Failure("QtProject.Trigger", "TRIGGER_ASSET_SOURCE_STALE", "准备期间工程或资源已变化，未发布关联事务。");
        if (storage->before != storage->after) assets_ = std::move(storage);
        return Status::Ok();
    } catch (...) {
        return Status::Failure("QtProject.Trigger", "TRIGGER_ASSET_PREPARE_FAILED", "无法准备完整关联资源；未发布触发器或资源。");
    }
}

TriggerGraphSnapshotUndoCommand::TriggerGraphSnapshotUndoCommand(EditorProject& project,
    Trigger::TriggerDocument before, Trigger::TriggerDocument after,
    QString label, QUndoCommand* parent)
    : QUndoCommand(std::move(label), parent), project_(&project),
      document_(project.triggerDocument_.get()), catalog_(project.uiCatalog_.get()),
      generation_(project.editRevision_.Generation()), snapshot_(std::in_place, std::move(before), std::move(after)) {
    if (!catalog_) return;
    auto& storage = *snapshot_;
    const auto beforeIdentity = Trigger::CaptureTriggerGraphReplayIdentity(storage.before, *catalog_);
    const auto afterIdentity = Trigger::CaptureTriggerGraphReplayIdentity(storage.after, *catalog_);
    if (beforeIdentity.Succeeded() && afterIdentity.Succeeded()
        && storage.before.SourceWtgSha256() == storage.after.SourceWtgSha256()
        && storage.before.SourceWctSha256() == storage.after.SourceWctSha256()
        && storage.before.UiCatalogSha256() == storage.after.UiCatalogSha256()) {
        storage.beforeBinding = *beforeIdentity.value;
        storage.afterBinding = *afterIdentity.value;
        try {
            storage.preparedFirst.emplace(storage.after);
            std::set<StableId> changed;
            for (const auto& record : storage.before.triggers) changed.insert(record.id);
            for (const auto& record : storage.after.triggers) {
                if (!changed.erase(record.id)) changed.insert(record.id);
            }
            // 图事务在同一批次内从初始最高 ID 递增分配，不复用该批删除的 ID。
            // 跨批次增删的记录在发布前一并暂存快照，覆盖 UI 与 MCP 共用图命令。
            for (const auto id : changed) storage.changedSnapshots.push_back(Trigger::GuiConversionSnapshots::PrepareRemoval(id));
        }
        catch (...) { storage.beforeBinding.reset(); storage.afterBinding.reset(); }
    }
}

TriggerGraphSnapshotUndoCommand::TriggerGraphSnapshotUndoCommand(EditorProject& project,
    Trigger::TriggerGraphScalarEditReceipt receipt, QString label, QUndoCommand* parent)
    : QUndoCommand(std::move(label), parent), project_(&project),
      document_(project.triggerDocument_.get()), catalog_(project.uiCatalog_.get()),
      generation_(project.editRevision_.Generation()), scalarReceipt_(std::move(receipt)) {}

bool TriggerGraphSnapshotUndoCommand::Ready() const { return CanApply(true); }

bool TriggerGraphSnapshotUndoCommand::CanApply(bool forward) const {
    if (!project_ || !document_ || project_->triggerDocument_.get() != document_ || !catalog_
        || project_->uiCatalog_.get() != catalog_ || project_->editRevision_.Generation() != generation_
        || project_->sourceChangeSuspected_ || project_->sourceChangeConfirmed_) return false;
    if (assets_) {
        const auto& expected = forward ? assets_->before : assets_->after;
        if (project_->archiveSession_.get() != assets_->archive || project_->directorySnapshot_.get() != assets_->directory
            || project_->sourceArchivePath_ != assets_->sourcePath || project_->projectFilePath_ != assets_->projectPath
            || project_->CaptureAssets() != expected) return false;
    }
    // 不在 Ready→push→redo 重复全图扫描；这里不提供正文/原值资格证明。
    // ReplayTriggerGraphScalarEdit 在最终同步 Apply 中验证真实方向身份并原子重放。
    if (scalarReceipt_) {
        if (scalarReceipt_->Empty() || !document_->HasTrustedSourceSeal()) return false;
        const auto& expected = forward ? scalarReceipt_->Receipt().before : scalarReceipt_->Receipt().after;
        return document_->SourceWtgSha256() == expected.sourceWtgSha256
            && document_->SourceWctSha256() == expected.sourceWctSha256
            && document_->UiCatalogSha256() == expected.uiCatalogSha256
            && project_->uiCatalog_->Sha256() == expected.uiCatalogSha256;
    }
    if (!snapshot_ || !snapshot_->beforeBinding || !snapshot_->afterBinding) return false;
    const auto& expected = forward ? *snapshot_->beforeBinding : *snapshot_->afterBinding;
    return Trigger::VerifyTriggerGraphReplayIdentity(*document_, *project_->uiCatalog_, expected).Succeeded();
}

void TriggerGraphSnapshotUndoCommand::Apply(bool forward) {
    if (!CanApply(forward)) {
        if (!project_) { setObsolete(true); return; }
        const QPointer<EditorProject> project(project_);
        setObsolete(true);
        project->AddDiagnostic(QObject::tr("错误"), QObject::tr("触发器"),
            QStringLiteral("TRIGGER_GRAPH_UNDO_SOURCE_STALE"),
            QObject::tr("触发器来源、函数目录或当前图已变化，未应用旧事务撤销快照。"));
        return;
    }
    if (scalarReceipt_) {
        Status status;
        bool threw = false;
        const auto beforeGeneration = project_->editRevision_.Generation();
        const auto beforeRevision = project_->editRevision_.Revision();
        try {
            status = project_->ReplayScalarAndObserve(document_, catalog_, generation_, *scalarReceipt_, forward);
        } catch (...) { threw = true; }
        if (threw || !status.Succeeded()) {
            // Core 收据重放负责异常前回滚；日志回调不在 Core catch 范围内。
            // 先 obsolete，再尾调用日志，允许模型回调同步销毁 command/project。
            setObsolete(true);
            const auto code = status.diagnostics.empty() ? QStringLiteral("TRIGGER_GRAPH_SCALAR_REPLAY_FAILED")
                : QString::fromUtf8(status.diagnostics.front().code);
            const auto message = status.diagnostics.empty() ? QObject::tr("Core 标量事务重放未能完成；未发布该撤销状态。")
                : QString::fromUtf8(status.diagnostics.front().message);
            const QPointer<EditorProject> project(project_);
            if (project) project->AddDiagnostic(QObject::tr("错误"), QObject::tr("触发器"), code, message);
            return;
        }
        if (forward) *firstPublication_ = true;
        const QPointer<EditorProject> project(project_);
        if (project) project->PublishScalarTriggerChange(this, beforeGeneration, beforeRevision, forward);
        return;
    }
    auto& storage = *snapshot_;
    // 一般图路径保持先复制、再 noexcept 发布；文档地址供旧命令继续借用。
    std::optional<Trigger::TriggerDocument> replacement;
    std::optional<AssetState> assetCandidate;
    Trigger::TriggerDocument* candidate = forward && !*firstPublication_ && storage.preparedFirst ? &*storage.preparedFirst : nullptr;
    try {
        if (!candidate) { replacement.emplace(forward ? storage.after : storage.before); candidate = &*replacement; }
        if (assets_) assetCandidate.emplace(forward ? assets_->after : assets_->before);
    }
    catch (...) {
        project_->AddDiagnostic(QObject::tr("错误"), QObject::tr("触发器"),
            QStringLiteral("TRIGGER_GRAPH_UNDO_PREPARE_FAILED"), QObject::tr("无法准备完整事务快照；当前图未改动。"));
        setObsolete(true);
        return;
    }
    static_assert(std::is_nothrow_swappable_v<Trigger::TriggerDocument>);
    std::swap(*document_, *candidate);
    if (assetCandidate) {
        project_->pendingImports_.swap(assetCandidate->imports);
        project_->assetEdits_.swap(assetCandidate->edits);
        project_->runtimeApprovedPackageSha256_.swap(assetCandidate->runtimeApprovedPackageSha256);
        // 未发信号的暂存段：完整捕获并观察同一份图/资源。失败时两侧一并回滚。
        bool observed = false;
        try {
            if (project_->editTrackingReady_ && !project_->editTrackingFailed_) {
                auto scopes = project_->CaptureEditScopes();
                if (scopes) observed = project_->editRevision_.Observe(std::move(*scopes), "修改触发器及关联资源",
                    forward ? EditOperation::Redo : EditOperation::Undo).Succeeded();
            }
        } catch (...) {}
        if (!observed) {
            project_->pendingImports_.swap(assetCandidate->imports);
            project_->assetEdits_.swap(assetCandidate->edits);
            project_->runtimeApprovedPackageSha256_.swap(assetCandidate->runtimeApprovedPackageSha256);
            std::swap(*document_, *candidate);
            setObsolete(true);
            project_->AddDiagnostic(QObject::tr("错误"), QObject::tr("触发器"),
                QStringLiteral("TRIGGER_ASSET_OBSERVE_FAILED"), QObject::tr("关联事务修订核验失败；触发器和资源均保持原状。"));
            return;
        }
    }
    for (auto& snapshot : storage.changedSnapshots) project_->guiConversionSnapshots_.Swap(snapshot);
    storage.preparedFirst.reset();
    if (forward) *firstPublication_ = true;
    if (assetCandidate) {
        const QPointer<EditorProject> project(project_);
        project->knownTriggerFingerprint_.reset();
        project->UpdateEditDirtyFlags();
        emit project->TriggerDocumentChanged();
        if (project) emit project->ImportsChanged();
        if (project) emit project->ImportsEdited();
        return;
    }
    project_->ObserveTriggerEdit();
    emit project_->TriggerDocumentChanged();
}

namespace {
Result<Trigger::TriggerGraphEditReceipt> PublishGraph(EditorProject& project,
    std::vector<Trigger::GraphEditOperation> operations, const QString& undoLabel,
    const std::function<Status()>& beforePublish, const std::function<Status(AssetState&)>& prepareAssets) {
    using Receipt = Trigger::TriggerGraphEditReceipt;
    constexpr const char* kSubsystem = "QtProject.Trigger";
    const auto failed = [](std::vector<Diagnostic> diagnostics) {
        Result<Receipt> failure;
        failure.diagnostics = std::move(diagnostics);
        return failure;
    };
    if (!project.triggerDocument_ || !project.uiCatalog_ || !project.triggerUndoStack_)
        return Result<Receipt>::Failure(kSubsystem, "TRIGGER_GRAPH_UNAVAILABLE", "当前工程没有可编辑的触发器文档。");
    std::optional<Trigger::TriggerDocument> before, candidate;
    Result<Receipt> edited;
    try {
        const auto references = project.CaptureTriggerMapObjectReferences();
        const auto* objects = references.Succeeded() && references.value && !(*references.value)->References().empty()
            ? references.value->get() : nullptr;
        const auto binding = Trigger::CaptureTriggerGraphBinding(*project.triggerDocument_, *project.uiCatalog_, objects);
        if (!binding.Succeeded()) return failed(binding.diagnostics);
        Trigger::TriggerGraphEditBatch batch;
        batch.expected = *binding.value;
        batch.operations = std::move(operations);
        before.emplace(*project.triggerDocument_);
        candidate.emplace(*before);
        if (batch.operations.empty() && prepareAssets) {
            Receipt unchanged; unchanged.before = *binding.value; unchanged.after = *binding.value;
            edited = Result<Receipt>::Success(std::move(unchanged));
        } else edited = Trigger::ApplyTriggerGraphEditBatch(*candidate, *project.uiCatalog_, batch, {}, objects);
    } catch (...) {
        return Result<Receipt>::Failure(kSubsystem, "TRIGGER_GRAPH_PREPARE_FAILED", "无法准备完整触发器事务；当前文档未改动。");
    }
    if (!edited.Succeeded()) return edited;
    if (beforePublish) {
        if (const Status ready = beforePublish(); !ready.Succeeded()) return failed(ready.diagnostics);
    }
    const bool graphUnchanged = edited.value->before.currentWtgSha256 == edited.value->after.currentWtgSha256
        && edited.value->before.currentWctSha256 == edited.value->after.currentWctSha256;
    if (graphUnchanged && !prepareAssets) return edited;
    std::unique_ptr<TriggerGraphSnapshotUndoCommand> command;
    try {
        command = std::make_unique<TriggerGraphSnapshotUndoCommand>(project, std::move(*before), std::move(*candidate), undoLabel);
        if (prepareAssets) {
            const auto prepared = command->PrepareAssets(prepareAssets);
            if (!prepared.Succeeded()) return failed(prepared.diagnostics);
        }
    } catch (...) {
        return Result<Receipt>::Failure(kSubsystem, "TRIGGER_GRAPH_PREPARE_FAILED", "无法准备完整触发器事务；当前文档未改动。");
    }
    if (graphUnchanged && !command->HasAssetChanges()) return edited;
    if (!command->Ready())
        return Result<Receipt>::Failure(kSubsystem, "TRIGGER_GRAPH_UNDO_SOURCE_STALE", "来源、会话或函数目录已变化；未发布。");
    const auto published = command->FirstPublication();
    project.triggerUndoStack_->push(command.release());
    if (!*published) return Result<Receipt>::Failure(kSubsystem, "TRIGGER_GRAPH_PUBLISH_FAILED", "完整事务未能发布。");
    return edited;
}
} // namespace

Result<Trigger::TriggerGraphEditReceipt> PublishTriggerGraphOperations(EditorProject& project,
    std::vector<Trigger::GraphEditOperation> operations, const QString& undoLabel, const std::function<Status()>& beforePublish) {
    return PublishGraph(project, std::move(operations), undoLabel, beforePublish, {});
}

Result<Trigger::TriggerGraphEditReceipt> PublishTriggerGraphOperationsWithAssets(EditorProject& project,
    std::vector<Trigger::GraphEditOperation> operations, const QString& undoLabel,
    const std::function<Status(AssetState&)>& prepareAssets) {
    if (!prepareAssets) return Result<Trigger::TriggerGraphEditReceipt>::Failure("QtProject.Trigger",
        "TRIGGER_ASSET_PREPARE_INVALID", "关联资源事务缺少候选准备器。");
    return PublishGraph(project, std::move(operations), undoLabel, {}, prepareAssets);
}

} // namespace War3Map::QtProject
