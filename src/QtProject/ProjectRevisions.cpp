#include "EditorProject.h"
#include "ProjectQuerySources.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Workspace.h"

#include <QUndoStack>
#include <limits>
#include <utility>

namespace War3Map::QtProject {
namespace detail {
// 私有调用事实，仅用于 UI；没有 mutation/Observe API 接受此类型。
struct TriggerScalarUiNotice {
    QPointer<const EditorProject> owner;
    QPointer<const QUndoStack> stack;
    QPointer<const QObject> consumer;
    const Trigger::TriggerDocument* document = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const QUndoCommand* command = nullptr;
    std::string catalogSha, sourceWtgSha, sourceWctSha;
    std::uint64_t beforeGeneration = 0, beforeRevision = 0, afterGeneration = 0, afterRevision = 0;
    int expectedIndex = -1;
    bool forward = false, active = false, borrowed = false;
};
struct TriggerUiBaseline {
    QPointer<const EditorProject> owner;
    const Trigger::TriggerDocument* document = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const QUndoStack* stack = nullptr; // 只比较；不从公开 raw 指针构造/解引用新的弱 owner。
    std::string catalogSha, sourceWtgSha, sourceWctSha;
    std::uint64_t generation = 0, revision = 0;
    bool trustedSourceSeal = false;
};
}
namespace {
std::string ObjectScope(int index) {
    return "objects/" + std::string(ObjectData::LniFileName(static_cast<ObjectData::ObjectType>(index)));
}
bool TriggerSourceMissing(const EditorProject& project) {
    if (project.archiveSession_) {
        if (project.archiveSession_->HasOpaqueMembers() || project.archiveSession_->HasMember("war3map.wtg")
            || project.archiveSession_->HasMember("war3map.wct")) return false;
    } else if (!project.sourceArchivePath_.empty() || !project.directorySnapshot_) return false;
    if (!project.directorySnapshot_) return project.archiveSession_ != nullptr;
    std::error_code error;
    if (project.projectRoot_.empty() || !std::filesystem::equivalent(project.projectRoot_,
        project.directorySnapshot_->Snapshot().root, error) || error) return false;
    for (const auto& file : project.directorySnapshot_->Snapshot().files) {
        const auto path = QString::fromStdWString(file.relativePath.generic_wstring()).toLower();
        if (path == "war3map.wtg" || path == "war3map.wct" || path == "map/war3map.wtg" || path == "map/war3map.wct") return false;
    }
    return true;
}
} // namespace

void EditorProject::CaptureContentChangesBaseline() {
    const auto sources = CaptureProjectObjectSources(*this);
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        changeObjectBaselines_[i] = objectDocuments_[i] ? std::make_unique<ObjectData::LniObjectDocument>(*objectDocuments_[i]) : nullptr;
        changeObjectBaselineKnown_[i] = objectDocuments_[i] != nullptr || sources[i].state == Services::ProjectSearchState::Missing;
    }
    changeTriggerBaseline_ = triggerDocument_ ? std::make_unique<Trigger::TriggerDocument>(*triggerDocument_) : nullptr;
    changeTriggerBaselineKnown_ = triggerDocument_ != nullptr || TriggerSourceMissing(*this);
    changeBaselineGeneration_ = editRevision_.Generation();
    const auto saved = editRevision_.Baseline(EditSaveTarget::Map);
    changeContentBaselineScopes_ = saved ? saved->scopes : EditScopeSnapshot{};
}

Result<Services::ProjectContentChangesContext> EditorProject::CaptureContentChangesContext(
    const Services::MapDataQueryBinding& binding) const {
    if (!editTrackingReady_ || editTrackingFailed_ || !changeBaselineGeneration_)
        return Result<Services::ProjectContentChangesContext>::Failure("Project.Changes", "CONTENT_BASELINE_UNAVAILABLE",
            "内容保存基线或当前修订未能可靠捕获，不能声明零项修改");
    const auto saved = editRevision_.Baseline(EditSaveTarget::Map);
    if (!saved) return Result<Services::ProjectContentChangesContext>::Failure("Project.Changes", "CONTENT_BASELINE_MISSING", "没有地图内容保存基线");
    const auto current = editRevision_.Current();
    const auto fingerprint = [](const auto& scopes, const std::string& key) {
        const auto found = scopes.find(key); return found == scopes.end() ? std::string() : found->second;
    };
    // .forge 恢复会提升修订代次，但仍借用打开源图时捕获的基线；逐项核对，
    // 不把恢复后的 overlay 再 clone 为地图保存基线。
    const auto sameBaseline = [&](const std::string& key) {
        return fingerprint(saved->scopes, key) == fingerprint(changeContentBaselineScopes_, key);
    };
    bool baselineMatches = sameBaseline("source") && sameBaseline("triggers");
    for (int i = 0; i < static_cast<int>(kObjectTypeCount); ++i) baselineMatches = baselineMatches && sameBaseline(ObjectScope(i));
    if (!baselineMatches) return Result<Services::ProjectContentChangesContext>::Failure("Project.Changes",
        "CONTENT_BASELINE_IDENTITY_CHANGED", "已保存内容基线的来源或指纹已变化，请重新打开可靠来源");
    const auto sources = CaptureProjectObjectSources(*this);
    Services::ProjectContentChangesContext context;
    context.binding = binding; context.editorRevision = editRevision_.Revision(); context.schema = objectFieldSchema_.get();
    for (std::size_t i = 0; i < kObjectTypeCount; ++i) {
        auto& source = context.objects[i]; const auto key = ObjectScope(static_cast<int>(i));
        source.baseline = changeObjectBaselines_[i].get(); source.baselineKnown = changeObjectBaselineKnown_[i];
        source.current = objectDocuments_[i].get();
        source.currentKnown = objectDocuments_[i] != nullptr || sources[i].state == Services::ProjectSearchState::Missing;
        source.baselineFingerprint = fingerprint(saved->scopes, key); source.currentFingerprint = fingerprint(current, key);
        // 在原本缺席类别里创建后全部撤销，现有修订服务已把空覆盖移出 scope。
        if (source.current && source.current->Objects().empty() && source.currentFingerprint.empty()
            && source.baselineKnown && !source.baseline) source.current = nullptr;
    }
    context.baselineTriggers = changeTriggerBaseline_.get(); context.baselineTriggersKnown = changeTriggerBaselineKnown_;
    context.currentTriggers = triggerDocument_.get(); context.currentTriggersKnown = triggerDocument_ != nullptr || TriggerSourceMissing(*this);
    context.baselineTriggerFingerprint = fingerprint(saved->scopes, "triggers"); context.currentTriggerFingerprint = fingerprint(current, "triggers");
    return Result<Services::ProjectContentChangesContext>::Success(std::move(context));
}

void EditorProject::ResetEditTracking() {
    terrainJournalWarning_.clear();
    scalarUiNotice_.reset(); editTrackingUiBaseline_.reset();
    changeBaselineGeneration_ = 0;
    editTrackingReady_ = false;
    const auto current = CaptureEditScopes();
    editTrackingFailed_ = !current.has_value();
    if (!current) return;
    editTrackingReady_ = editRevision_.Reset(*current, *current, *current).Succeeded();
    if (editTrackingReady_) { CaptureContentChangesBaseline(); UpdateEditDirtyFlags(); }
    if (!editTrackingReady_ || editTrackingFailed_) return;
    try {
        auto baseline = std::make_shared<detail::TriggerUiBaseline>();
        baseline->owner = this; baseline->document = triggerDocument_.get(); baseline->catalog = uiCatalog_.get();
        baseline->stack = triggerUndoStack_; baseline->generation = editRevision_.Generation(); baseline->revision = editRevision_.Revision();
        if (uiCatalog_) baseline->catalogSha = uiCatalog_->Sha256();
        if (triggerDocument_) {
            baseline->trustedSourceSeal = triggerDocument_->HasTrustedSourceSeal();
            baseline->sourceWtgSha = triggerDocument_->SourceWtgSha256(); baseline->sourceWctSha = triggerDocument_->SourceWctSha256();
        }
        editTrackingUiBaseline_ = std::move(baseline);
    } catch (...) { editTrackingUiBaseline_.reset(); } // UI 失去优化，不修改实际 Reset 结果。
    // 冷路径：Module 必须完整重绑，不能从新 generation 推断此前模型拓扑未变化。
    // emit 是尾操作，接收器可同步删除 Project；不在返回后访问 this。
    emit EditTrackingBaselineChanged(editRevision_.Generation(), editRevision_.Revision());
}

bool EditorProject::IsEditTrackingUiBaselineCurrent(std::uint64_t generation, std::uint64_t revision) const {
    const auto baseline = editTrackingUiBaseline_;
    if (!baseline || baseline->owner != this || !editTrackingReady_ || editTrackingFailed_
        || sourceChangeSuspected_ || sourceChangeConfirmed_ || baseline->generation != generation || baseline->revision != revision
        || generation != editRevision_.Generation() || revision != editRevision_.Revision()
        || baseline->document != triggerDocument_.get() || baseline->catalog != uiCatalog_.get()
        || baseline->stack != triggerUndoStack_) return false;
    if (uiCatalog_ && baseline->catalogSha != uiCatalog_->Sha256()) return false;
    return !triggerDocument_ || (baseline->trustedSourceSeal == triggerDocument_->HasTrustedSourceSeal()
        && baseline->sourceWtgSha == triggerDocument_->SourceWtgSha256()
        && baseline->sourceWctSha == triggerDocument_->SourceWctSha256());
}

bool EditorProject::ObserveEditState(const std::string& description) {
    if (!editTrackingReady_) return false;
    const auto current = CaptureEditScopes();
    editTrackingFailed_ = !current.has_value();
    if (!current) return false;
    const auto observed = editRevision_.Observe(*current, description);
    editTrackingFailed_ = !observed.Succeeded();
    if (observed.Succeeded()) UpdateEditDirtyFlags();
    return observed.Succeeded();
}

bool EditorProject::ObserveMapDataEdit(const std::string& description) {
    // 局部观察不能恢复此前完整核验失败，也不能把新来源借用旧触发器/物编基线。
    if (!editTrackingReady_ || editTrackingFailed_) return false;
    auto current = editRevision_.Current();
    const auto source = current.find("source");
    const auto identity = archiveSession_ ? "archive:" + archiveSession_->SourceSha256()
        : directorySnapshot_ ? "directory:" + directorySnapshot_->Snapshot().aggregateSha256 : std::string{};
    if (source == current.end() || identity.empty() || source->second != identity) {
        editTrackingFailed_ = true; return false;
    }
    const auto mapScopes = CaptureMapEditScopes();
    if (!mapScopes) { editTrackingFailed_ = true; return false; }
    std::erase_if(current, [](const auto& entry) { return entry.first.starts_with("map/"); });
    current.insert(mapScopes->begin(), mapScopes->end());
    const auto observed = editRevision_.Observe(std::move(current), description);
    editTrackingFailed_ = !observed.Succeeded();
    if (observed.Succeeded()) UpdateEditDirtyFlags();
    return observed.Succeeded();
}

void EditorProject::ObserveObjectEdit(int typeIndex) {
    if (!editTrackingReady_ || typeIndex < 0 || typeIndex >= 7) return;
    auto current = editRevision_.Current();
    const auto key = ObjectScope(typeIndex);
    const auto& document = objectDocuments_[typeIndex];
    const auto baseline = editRevision_.Baseline(EditSaveTarget::Map);
    if (!document || (baseline && !baseline->scopes.contains(key) && document->Objects().empty())) current.erase(key);
    else current[key] = document->DocumentFingerprint();
    // 物编操作只计算当前类别，不反复序列化整个触发器目录。
    const auto status = editRevision_.Observe(std::move(current), "修改物体数据");
    if (!status.Succeeded()) editTrackingFailed_ = true;
    else UpdateEditDirtyFlags();
}

Status EditorProject::ReplayScalarAndObserve(Trigger::TriggerDocument* expectedDocument,
    const Trigger::UiCatalog* expectedCatalog, std::uint64_t expectedGeneration,
    const Trigger::TriggerGraphScalarEditReceipt& receipt, bool forward) {
    if (receipt.Empty() || !triggerDocument_ || !uiCatalog_ || triggerDocument_.get() != expectedDocument
        || uiCatalog_.get() != expectedCatalog || editRevision_.Generation() != expectedGeneration
        || sourceChangeSuspected_ || sourceChangeConfirmed_) {
        return Status::Failure("Project.Trigger", "TRIGGER_SCALAR_SESSION_STALE", "标量事务的会话、来源或所有者已变化；未发布。");
    }
    // 先准备小型 scope 更新，避免在已发布后构造字符串/复制 scope 的异常丢失 Undo。
    // 方向摘要来自不透明 Core 收据中的实际 ReplayIdentity，不是原图 source seal。
    std::optional<EditScopeSnapshot> nextScopes;
    std::string observationDescription;
    if (editTrackingReady_ && !editTrackingFailed_) {
        try {
            const auto& wanted = forward ? receipt.Receipt().after : receipt.Receipt().before;
            if (wanted.uiCatalogSha256 != uiCatalog_->Sha256() || wanted.currentWtgSha256.empty()
                || wanted.currentWctSha256.empty()) {
                return Status::Failure("Project.Trigger", "TRIGGER_SCALAR_SCOPE_UNBOUND", "标量收据没有实际配对来源；未发布。");
            }
            nextScopes = editRevision_.Current();
            (*nextScopes)["triggers"] = wanted.currentWtgSha256 + ":" + wanted.currentWctSha256 + ":" + wanted.uiCatalogSha256;
            observationDescription = "修改触发器或变量";
        } catch (...) {
            return Status::Failure("Project.Trigger", "TRIGGER_SCALAR_SCOPE_PREPARE_FAILED", "无法准备触发器修订；当前图保持。");
        }
    }
    // 每一次入口都必须实际经过完整 before、wanted 终态与回滚门；没有可重用的成功 token。
    const auto replay = Trigger::ReplayTriggerGraphScalarEdit(*expectedDocument, *expectedCatalog, receipt, forward);
    if (!replay.Succeeded()) return replay;
    if (nextScopes) {
        try {
            const auto observed = editRevision_.Observe(std::move(*nextScopes), std::move(observationDescription));
            editTrackingFailed_ = !observed.Succeeded();
            if (observed.Succeeded()) UpdateEditDirtyFlags();
        } catch (...) {
            // Core 已真实发布，保留该 Undo；修订故障沿既有未知/保存门禁处理。
            editTrackingFailed_ = true;
        }
    }
    return replay;
}

EditorProject::ScalarUiNotice EditorProject::ActiveScalarUiNotice(
    const Trigger::TriggerDocument* boundDocument, const Trigger::UiCatalog* boundCatalog,
    std::uint64_t boundGeneration, std::uint64_t boundRevision, const QObject* consumer) const {
    const auto notice = scalarUiNotice_;
    if (!consumer || !notice || !notice->active || notice->borrowed || notice->owner != this || !notice->stack
        || !editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_
        || notice->document != boundDocument || notice->catalog != boundCatalog
        || triggerDocument_.get() != boundDocument || uiCatalog_.get() != boundCatalog || !boundDocument || !boundCatalog
        || notice->beforeGeneration != boundGeneration || notice->beforeRevision != boundRevision
        || notice->afterGeneration != editRevision_.Generation() || notice->afterRevision != editRevision_.Revision()
        || notice->catalogSha != boundCatalog->Sha256() || notice->catalogSha.empty()
        || !boundDocument->HasTrustedSourceSeal() || notice->sourceWtgSha != boundDocument->SourceWtgSha256()
        || notice->sourceWctSha != boundDocument->SourceWctSha256() || notice->stack != triggerUndoStack_) return {};
    notice->consumer = consumer;
    notice->borrowed = true;
    return notice;
}

bool EditorProject::ScalarUiNoticeMatchesIndex(const ScalarUiNotice& notice,
    const QUndoStack* stack, int index, const QObject* consumer) const {
    if (!consumer || !notice || !notice->borrowed || notice->consumer != consumer || notice->active
        || scalarUiNotice_.get() != notice.get() || notice->owner != this
        || !stack || notice->stack != stack || stack != triggerUndoStack_ || index < 0 || index != stack->index()
        || index != notice->expectedIndex || !editTrackingReady_ || editTrackingFailed_
        || sourceChangeSuspected_ || sourceChangeConfirmed_ || notice->document != triggerDocument_.get()
        || notice->catalog != uiCatalog_.get() || !triggerDocument_ || !uiCatalog_
        || notice->afterGeneration != editRevision_.Generation() || notice->afterRevision != editRevision_.Revision()
        || notice->catalogSha.empty() || notice->catalogSha != uiCatalog_->Sha256()
        || !triggerDocument_->HasTrustedSourceSeal() || notice->sourceWtgSha != triggerDocument_->SourceWtgSha256()
        || notice->sourceWctSha != triggerDocument_->SourceWctSha256()) return false;
    const int commandIndex = notice->forward ? index - 1 : index;
    return commandIndex >= 0 && commandIndex < stack->count() && stack->command(commandIndex) == notice->command;
}

void EditorProject::PublishScalarTriggerChange(const QUndoCommand* command,
    std::uint64_t beforeGeneration, std::uint64_t beforeRevision, bool forward) {
    std::shared_ptr<detail::TriggerScalarUiNotice> notice;
    // 失败仅失去优化；通用 signal 仍与旧路径一样恰好发出一次。
    try {
        if (command && triggerDocument_ && uiCatalog_ && triggerUndoStack_ && editTrackingReady_ && !editTrackingFailed_
            && !sourceChangeSuspected_ && !sourceChangeConfirmed_ && !uiCatalog_->Sha256().empty()
            && beforeGeneration == editRevision_.Generation()
            && triggerUndoStack_->index() >= 0 && (forward ? triggerUndoStack_->index() < (std::numeric_limits<int>::max)()
                : triggerUndoStack_->index() > 0)) {
            notice = std::make_shared<detail::TriggerScalarUiNotice>();
            notice->owner = this; notice->stack = triggerUndoStack_;
            notice->document = triggerDocument_.get(); notice->catalog = uiCatalog_.get(); notice->command = command;
            notice->catalogSha = uiCatalog_->Sha256();
            notice->sourceWtgSha = triggerDocument_->SourceWtgSha256(); notice->sourceWctSha = triggerDocument_->SourceWctSha256();
            notice->beforeGeneration = beforeGeneration; notice->beforeRevision = beforeRevision;
            notice->afterGeneration = editRevision_.Generation(); notice->afterRevision = editRevision_.Revision();
            notice->forward = forward; notice->expectedIndex = triggerUndoStack_->index() + (forward ? 1 : -1);
            notice->active = true;
        }
    } catch (...) { notice.reset(); }
    scalarUiNotice_ = notice;
    struct CloseScope {
        std::shared_ptr<detail::TriggerScalarUiNotice> notice;
        ~CloseScope() { if (notice) notice->active = false; }
    } close{notice};
    // 重入的 inner notice 不恢复 outer；scope 关闭不访问 QObject，可安全处理接收器删除 owner。
    emit TriggerDocumentChanged();
}

void EditorProject::OfferKnownTriggerFingerprint(int stackIndex, std::string fingerprint) {
    if (!triggerDocument_ || !triggerUndoStack_ || fingerprint.empty()) { knownTriggerFingerprint_.reset(); return; }
    knownTriggerFingerprint_ = KnownTriggerFingerprint{triggerDocument_.get(), triggerUndoStack_, stackIndex, std::move(fingerprint)};
}

std::optional<std::string> EditorProject::ObservedTriggerFingerprint() const {
    if (!editTrackingReady_ || editTrackingFailed_) return {};
    const auto current = editRevision_.Current();
    const auto found = current.find("triggers");
    if (found == current.end() || found->second.empty()) return {};
    return found->second;
}

void EditorProject::ObserveTriggerEdit() {
    const auto known = std::exchange(knownTriggerFingerprint_, std::nullopt);
    if (!editTrackingReady_) return;
    auto current = editRevision_.Current();
    if (triggerDocument_) {
        if (!uiCatalog_) { editTrackingFailed_ = true; return; }
        std::optional<std::string> value;
        if (known && known->document == triggerDocument_.get() && known->stack == triggerUndoStack_
            && triggerUndoStack_ && known->index == triggerUndoStack_->index()) {
            value = known->value;
            if (Trigger::TriggerCodeEditor::FullReplayVerificationEnabled()) {
                const auto actual = CaptureTriggerFingerprint();
                if (!actual || *actual != *value) {
                    editTrackingFailed_ = true;
                    AddDiagnostic(tr("错误"), tr("编辑修订"), QStringLiteral("TRIGGER_KNOWN_FINGERPRINT_MISMATCH"),
                        tr("完整核验：撤销/重做复用的触发器指纹与实际内容不一致。"));
                    return;
                }
            }
        } else value = CaptureTriggerFingerprint();
        if (!value) { editTrackingFailed_ = true; return; }
        current["triggers"] = *value;
    } else current.erase("triggers");
    const auto status = editRevision_.Observe(std::move(current), "修改触发器或变量");
    if (!status.Succeeded()) editTrackingFailed_ = true;
    else UpdateEditDirtyFlags();
}

void EditorProject::UpdateEditDirtyFlags() {
    if (!editTrackingReady_ || editTrackingFailed_) return;
    const auto current = editRevision_.Current();
    const auto saved = editRevision_.Baseline(EditSaveTarget::Map);
    if (!saved) return;
    const auto differs = [&](const std::string& key) {
        const auto a = current.find(key), b = saved->scopes.find(key);
        return a == current.end() ? b != saved->scopes.end()
            : b == saved->scopes.end() || a->second != b->second;
    };
    for (int i = 0; i < 7; ++i) objectDirty_[i] = differs(ObjectScope(i));
    triggerDirty_ = differs("triggers");
}

void EditorProject::CommitMapEditBaseline() {
    // 地图写出、回读和来源切换全部成功后才执行；只是编辑副本，不代表脚本已构建。
    if (!ObserveEditState("保存地图编辑副本")) return;
    const auto ticket = editRevision_.BeginSave(EditSaveTarget::Map);
    if (!ticket.Succeeded()) return;
    if (!editRevision_.CompleteSave(*ticket.value, true).Succeeded()) return;
    CaptureContentChangesBaseline();
    UpdateEditDirtyFlags();
}

} // namespace War3Map::QtProject
