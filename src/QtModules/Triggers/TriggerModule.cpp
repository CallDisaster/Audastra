#include "TriggerModule.h"
#include "TriggerLuaScripts.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "ObjectNames.h"
#include "TriggerCommands.h"
#include "TriggerModels.h"
#include "TriggerBlueprint.h"
#include "TriggerValuePanel.h"
#include "TriggerReplaceDialog.h"
#include "Icons/SvgIcons.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "Widgets/ColumnLayout.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QTimer>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QTreeView>
#include <QUndoStack>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <vector>

namespace War3Map::Modules {
namespace {
std::optional<StableId> SearchIdentity(const QVariant& value) {
    if (value.typeId() != QMetaType::QString) return std::nullopt;
    const auto text = value.toString();
    if (text.isEmpty() || text.size() > 20 || !std::all_of(text.begin(), text.end(), [](QChar c) {
        return c >= QLatin1Char('0') && c <= QLatin1Char('9');
    })) return std::nullopt;
    bool valid = false; const auto id = text.toULongLong(&valid);
    return valid && id && QString::number(id) == text ? std::optional<StableId>(id) : std::nullopt;
}

// 搜索只导航现有图，不用编辑 API 的类型 / 可替换能力推断参数归属。
// 逐边有界遍历，函数参数映射到实际可见的包含语句；不自动打开编辑对话框。
std::optional<StableId> SearchArgumentAnchor(const Trigger::TriggerDocument& document,
    StableId ownerId, StableId ecaId, StableId argumentId) {
    constexpr std::size_t maxRecords = 250000, maxVisits = 65536;
    if (document.triggers.size() > maxRecords || document.ecaNodes.size() > maxRecords
        || document.argumentNodes.size() > maxRecords) return std::nullopt;
    const auto unique = [&](const auto& rows, StableId id) {
        return std::count_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; }) == 1;
    };
    if (!unique(document.triggers, ownerId) || !unique(document.ecaNodes, ecaId)
        || !unique(document.argumentNodes, argumentId)) return std::nullopt;
    const auto owner = std::find_if(document.triggers.begin(), document.triggers.end(),
        [&](const auto& row) { return row.id == ownerId; });
    if (owner->type != 0 || owner->customTextFlag != 0) return std::nullopt;
    struct Frame {
        bool argument = false, hidden = false, entered = false;
        std::uint32_t index = 0;
        StableId ownerEca = 0, visibleEca = 0;
        std::size_t next = 0;
    };
    std::vector<Frame> stack;
    std::unordered_set<std::uint64_t> seen;
    std::unordered_set<StableId> identities;
    std::optional<StableId> anchor;
    std::size_t visits = 0;
    bool ecaFound = false;
    for (const auto root : owner->ecas) {
        stack.push_back({false, false, false, root});
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (!frame.entered) {
                if (++visits > maxVisits || (frame.argument ? frame.index >= document.argumentNodes.size()
                    : frame.index >= document.ecaNodes.size())) return std::nullopt;
                const auto key = (static_cast<std::uint64_t>(frame.argument) << 32) | frame.index;
                const auto id = frame.argument ? document.argumentNodes[frame.index].id : document.ecaNodes[frame.index].id;
                if (!id || !seen.insert(key).second || !identities.insert(id).second) return std::nullopt;
                frame.entered = true;
                if (frame.argument) {
                    const auto& node = document.argumentNodes[frame.index];
                    if ((node.insertCall != 0) != node.callEca.has_value()
                        || (node.insertIndex != 0) != node.indexArgument.has_value()) return std::nullopt;
                    if (node.id == argumentId) {
                        if (frame.ownerEca != ecaId) return std::nullopt;
                        anchor = frame.visibleEca;
                    }
                } else {
                    const auto& node = document.ecaNodes[frame.index];
                    if (node.sourceTriggerId != ownerId) return std::nullopt;
                    if (!frame.hidden) frame.visibleEca = node.id;
                    if (node.id == ecaId) ecaFound = true;
                }
            }
            if (frame.argument) {
                const auto& node = document.argumentNodes[frame.index];
                if (frame.next == 0) {
                    frame.next = 1;
                    if (node.callEca) {
                        stack.push_back({false, true, false, *node.callEca, 0, frame.visibleEca}); continue;
                    }
                }
                if (frame.next == 1) {
                    frame.next = 2;
                    if (node.indexArgument) {
                        stack.push_back({true, frame.hidden, false, *node.indexArgument, frame.ownerEca, frame.visibleEca}); continue;
                    }
                }
                stack.pop_back();
            } else {
                const auto& node = document.ecaNodes[frame.index];
                if (frame.next < node.arguments.size()) {
                    const auto index = node.arguments[frame.next++];
                    stack.push_back({true, frame.hidden, false, index, node.id, frame.visibleEca}); continue;
                }
                const auto child = frame.next - node.arguments.size();
                if (child < node.children.size()) {
                    ++frame.next; stack.push_back({false, frame.hidden, false, node.children[child], 0, frame.visibleEca}); continue;
                }
                stack.pop_back();
            }
        }
    }
    return ecaFound && anchor && *anchor ? anchor : std::nullopt;
}
} // namespace

TriggerModule::TriggerModule(QtProject::EditorProject& project)
    : project_(project) {
    // 与现有 indexChanged 连接同一已存活 stack；不从回调后可被替换的 public raw 指针重建弱引用。
    scalarUiStack_ = project_.triggerUndoStack_;
    connect(&project_, &QtProject::EditorProject::TriggerDocumentAboutToChange, this, &TriggerModule::ReleaseDocument);
    connect(&project_, &QtProject::EditorProject::TriggerDocumentChanged, this, &TriggerModule::OnTriggerDocumentChanged);
    connect(&project_, &QtProject::EditorProject::EditTrackingBaselineChanged, this, &TriggerModule::OnEditTrackingBaselineChanged);
    // 外部来源变化不能沿用 scalar 瞬时提示；先失效再执行原有刷新订阅。
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsAboutToReplace, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsReplaced, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, &TriggerModule::InvalidateScalarUiBinding);
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &TriggerModule::RefreshAfterUndo);
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsReplaced, this, &TriggerModule::RefreshAfterUndo);
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, &TriggerModule::RefreshCodeSourceState);
    connect(&project_, &QtProject::EditorProject::SourceStateChanged, this, [this] { InvalidateTriggerReplace(true); });
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, [this] { InvalidateTriggerReplace(true); });
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsAboutToReplace, this, [this] { InvalidateTriggerReplace(true); });
    connect(&project_, &QtProject::EditorProject::ObjectDocumentsReplaced, this, [this] { InvalidateTriggerReplace(true); });
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, [this] { InvalidateTriggerReplace(true); });
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, [this] { InvalidateTriggerReplace(); });
    for (auto* stack : project_.objectUndoStacks_) {
        if (stack) connect(stack, &QUndoStack::indexChanged, this, &TriggerModule::InvalidateScalarUiBinding);
        if (stack) connect(stack, &QUndoStack::indexChanged, this, &TriggerModule::RefreshAfterUndo);
        if (stack) connect(stack, &QUndoStack::indexChanged, this, [this] { InvalidateTriggerReplace(); });
    }
    // 字符串表换了只影响显示文本：保留选中项重绘。
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, [this] {
        if (project_.triggerDocument_ == nullptr || !page_) return;
        const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
        RefreshEditor(eca == nullptr ? 0 : eca->id);
    });
    connect(&project_, &QtProject::EditorProject::UiLibrariesChanged, this, [this](const QStringList& packages, bool reloaded) {
        if (page_ && codeTarget_) ConfigureCodeChecking();
        ShowStatus(reloaded
            ? tr("函数库 %1 已变化，已按新定义重新载入触发器").arg(packages.join(QStringLiteral("、")))
            : tr("函数库 %1 已变化；触发器有未导出的修改，未自动重新载入（可在“函数库…”中重新载入）").arg(packages.join(QStringLiteral("、"))),
            10000);
    });
    // 撤销 / 重做 / 新命令：目录与逻辑区一起刷新（增删触发器、改名、改标志都会改变目录）。
    if (project_.triggerUndoStack_ != nullptr) {
        connect(project_.triggerUndoStack_, &QUndoStack::indexChanged, this, &TriggerModule::RefreshAfterUndo);
        connect(project_.triggerUndoStack_, &QUndoStack::indexChanged, this, [this] { InvalidateTriggerReplace(); });
    }
}

TriggerModule::~TriggerModule() {
    if (triggerReplaceStop_) triggerReplaceStop_->request_stop();
    delete page_.data();
}

Shell::ModuleInfo TriggerModule::Info() const {
    return {"triggers", tr("触发器"), "lucide/git-branch", QKeySequence(Qt::ALT | Qt::Key_1)};
}

QWidget* TriggerModule::Widget() {
    if (!page_) {
        BuildPage();
        BuildShortcuts();
        BindDocument();
    }
    return page_;
}

void TriggerModule::AttachHost(Shell::ModuleHost* host) {
    if (host_ != host) CancelNavigationRestoration();
    host_ = host;
}

QUndoStack* TriggerModule::UndoStack() {
    if (workspaceTabs_ && workspaceTabs_->currentIndex() == 1) return project_.AssetUndoStack();
    return project_.triggerUndoStack_;
}

QPlainTextEdit* TriggerModule::CommentEditor() const {
    return commentEdit_;
}

QPlainTextEdit* TriggerModule::CodeEditor() const { return scriptView_; }

void TriggerModule::ShowStatus(const QString& text, int timeoutMs) {
    if (host_ != nullptr) {
        host_->ShowStatusMessage(text, timeoutMs);
    }
}

void TriggerModule::ReleaseDocument() {
    InvalidateScalarUiBinding();
    InvalidateTriggerReplace(true);
    CancelNavigationRestoration();
    ++navigationDocumentGeneration_;
    if (!page_) {
        return;
    }
    // 旧文档 / 目录析构前解除模型引用；剪贴板里的子树属于旧文档的语义，一并清空。
    explorerModel_->SetDocument(nullptr);
    ecaModel_->SetDocument(nullptr, nullptr);
    LeaveMapScript();
    PopulateProperties(nullptr);
    const QScopedValueRollback suppressCode(suppressCodeSignals_, true);
    codeTarget_.reset(); codeBaselinePlan_.reset(); codeBaselineText_.clear();
    codeSourceSettingsSha256_.clear(); codeNativeLua_ = false;
    codeDraftPending_ = false; showingGlobalCode_ = false;
    scriptView_->SetPrepareCheck({});
    scriptView_->SetText({});
    scriptView_->hide();
    codeActions_->hide();
    convertCodeButton_->hide(); recoverCodeButton_->hide();
    codeTools_->hide(); codeProblems_->hide();
    ecaView_->setVisible(!BlueprintVisible());
    blueprint_->SetSource(nullptr, nullptr, {});
    clipboard_.reset();
    if (valuePanel_) valuePanel_->close(); // 面板引用旧文档
    selectedTrigger_ = 0;
    selectedCategory_.reset();
    displayedTrigger_ = 0;
    selectionCategory_.reset();
    for (QPushButton* button : {addEventButton_, addConditionButton_, addActionButton_, editEcaButton_}) {
        button->setEnabled(false);
    }
}

void TriggerModule::BindDocument() {
    const QPointer<TriggerModule> self(this);
    const QPointer<QtProject::EditorProject> project(&project_);
    const auto generation = project_.editRevision_.Generation(), revision = project_.editRevision_.Revision();
    const auto navigationGeneration = navigationDocumentGeneration_;
    const auto* boundDocument = project_.triggerDocument_.get(); const auto* boundCatalog = project_.uiCatalog_.get();
    const auto catalogSha = boundCatalog ? boundCatalog->Sha256() : std::string{};
    InvalidateScalarUiBinding();
    if (!page_) {
        return;
    }
    UpdateSummary();
    collapsedCategories_.clear();
    triggerClipboard_.reset();
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        if (!pendingViewState_.isEmpty()) {
            const QJsonObject restore = pendingViewState_;
            RestoreViewState(restore);
        }
        return;
    }
    explorerModel_->SetDocument(document);
    ecaModel_->SetDocument(document, project_.uiCatalog_.get());
    ApplyExplorerExpansion();
    // 默认选中第一个非空类别的第一个触发器。
    for (int categoryRow = 0; categoryRow < filter_->rowCount(); ++categoryRow) {
        const QModelIndex category = filter_->index(categoryRow, 0);
        if (filter_->rowCount(category) == 0) {
            continue;
        }
        explorerView_->setCurrentIndex(filter_->index(0, 0, category));
        break;
    }
    if (!pendingViewState_.isEmpty()) {
        const QJsonObject restore = pendingViewState_;
        RestoreViewState(restore);
    }
    if (!self || !project) return;
    if (project_.triggerDocument_.get() == boundDocument && project_.uiCatalog_.get() == boundCatalog
        && (!boundCatalog || boundCatalog->Sha256() == catalogSha)
        && project_.editRevision_.Generation() == generation && project_.editRevision_.Revision() == revision
        && navigationDocumentGeneration_ == navigationGeneration) RememberScalarUiBinding();
    else InvalidateScalarUiBinding();
}

void TriggerModule::InvalidateScalarUiBinding() {
    pendingScalarUiNotice_.reset();
    scalarUiDocument_ = nullptr; scalarUiCatalog_ = nullptr;
}

bool TriggerModule::CanRefreshScalarUi() const {
    return scalarUiStack_ && scalarUiStack_.data() == project_.triggerUndoStack_
        && page_ && ecaModel_ && !publishingCode_ && !codeTarget_ && !showingGlobalCode_ && !showingMapScript_
        && !HasPendingCodeDraft() && !BlueprintVisible() && (!valuePanel_ || !valuePanel_->isVisible())
        && scalarUiNavigationGeneration_ == navigationDocumentGeneration_
        && scalarUiDocument_ == project_.triggerDocument_.get() && scalarUiCatalog_ == project_.uiCatalog_.get()
        && ecaModel_->IsBoundTo(scalarUiDocument_, scalarUiCatalog_) && ecaModel_->BoundTriggerId() == displayedTrigger_;
}

void TriggerModule::RememberScalarUiBinding() {
    pendingScalarUiNotice_.reset();
    if (!scalarUiStack_ || scalarUiStack_.data() != project_.triggerUndoStack_ || !page_ || !ecaModel_
        || !ecaModel_->IsBoundTo(project_.triggerDocument_.get(), project_.uiCatalog_.get())
        || ecaModel_->BoundTriggerId() != displayedTrigger_) {
        scalarUiDocument_ = nullptr; scalarUiCatalog_ = nullptr; return;
    }
    scalarUiDocument_ = project_.triggerDocument_.get(); scalarUiCatalog_ = project_.uiCatalog_.get();
    scalarUiGeneration_ = project_.editRevision_.Generation(); scalarUiRevision_ = project_.editRevision_.Revision();
    scalarUiNavigationGeneration_ = navigationDocumentGeneration_;
}

void TriggerModule::OnTriggerDocumentChanged() {
    pendingScalarUiNotice_.reset();
    const bool eligible = CanRefreshScalarUi();
    if (eligible) {
        const auto modelEpoch = ecaModel_->PresentationEpoch();
        const auto boundTrigger = ecaModel_->BoundTriggerId();
        pendingScalarUiNotice_ = project_.ActiveScalarUiNotice(scalarUiDocument_, scalarUiCatalog_,
            scalarUiGeneration_, scalarUiRevision_, this);
        if (pendingScalarUiNotice_ && ecaModel_->PresentationEpoch() == modelEpoch
            && ecaModel_->BoundTriggerId() == boundTrigger && boundTrigger == displayedTrigger_) {
            pendingScalarUiModelEpoch_ = modelEpoch; return;
        }
    }
    BindDocument();
}

void TriggerModule::OnEditTrackingBaselineChanged(quint64 generation, quint64 revision) {
    InvalidateScalarUiBinding();
    if (!scalarUiStack_ || scalarUiStack_.data() != project_.triggerUndoStack_
        || !project_.IsEditTrackingUiBaselineCurrent(generation, revision)) return;
    // 来源的冷基线变动统一原完整绑定；同 owner 的未通知拓扑变化也不能保留旧 tree。
    BindDocument();
}

void TriggerModule::UpdateSummary() {
    const auto* document = project_.triggerDocument_.get();
    const auto* catalog = project_.uiCatalog_.get();
    if (document == nullptr || catalog == nullptr) {
        summary_->setText(project_.triggerStatus_.isEmpty()
            ? tr("打开项目后载入 WTG/WCT；第三方 UI 库按当前数据根动态识别。")
            : project_.triggerStatus_);
        return;
    }
    summary_->setText(tr("%1 个类别 · %2 个触发器 · %3 个 ECA · 定义 %4/%5")
        .arg(document->categories.size())
        .arg(document->triggers.size())
        .arg(document->ecaNodes.size())
        .arg(catalog->Documents().size())
        .arg(catalog->PresentSourceCount() > 0 ? catalog->PresentSourceCount() - 1 : 0));
}

const Trigger::TriggerRecord* TriggerModule::CurrentTrigger() const {
    const auto* document = project_.triggerDocument_.get();
    if (!page_ || document == nullptr || selectedTrigger_ == 0) {
        return nullptr;
    }
    for (const auto& trigger : document->triggers) {
        if (trigger.id == selectedTrigger_) return &trigger;
    }
    return nullptr;
}

void TriggerModule::SelectTrigger(StableId triggerId) {
    const QModelIndex source = explorerModel_->IndexForTriggerId(triggerId);
    if (!source.isValid()) {
        return;
    }
    const QModelIndex proxy = filter_->mapFromSource(source);
    explorerView_->setCurrentIndex(proxy);
    explorerView_->scrollTo(proxy);
}

void TriggerModule::ShowSelectedTrigger() {
    if (refreshingExplorer_ || HasPendingCodeDraft()) {
        return;
    }
    LeaveMapScript();
    showingGlobalCode_ = false;
    propertiesBox_->show();
    const auto* trigger = CurrentTrigger();
    displayedTrigger_ = trigger == nullptr ? 0 : trigger->id;
    ecaModel_->SetTrigger(displayedTrigger_);
    // 注释条目（WTG isComment）只有说明文字：不显示逻辑区，只显示注释编辑框。
    const bool comment = trigger != nullptr && trigger->type != 0;
    const bool isScript = !comment && trigger != nullptr && trigger->customTextFlag != 0;
    if (isScript) LoadCodeTarget({Trigger::CodeBlockKind::CustomTrigger, trigger->id});
    else {
        const QScopedValueRollback suppress(suppressCodeSignals_, true);
        codeTarget_.reset(); codeBaselinePlan_.reset(); codeBaselineText_.clear(); scriptView_->SetPrepareCheck({}); scriptView_->SetText({});
        codeSourceSettingsSha256_.clear(); codeNativeLua_ = false;
    }
    scriptView_->setVisible(isScript);
    codeTools_->setVisible(isScript); codeProblems_->setVisible(isScript);
    traditionalButton_->setEnabled(!isScript);
    blueprintButton_->setEnabled(!isScript);
    showRawValues_->setVisible(!isScript);
    codeActions_->setVisible(trigger && !comment);
    applyCodeButton_->setVisible(isScript);
    discardCodeButton_->setVisible(isScript);
    codeDraftNote_->setVisible(isScript);
    ecaView_->setVisible(!comment && !isScript && !BlueprintVisible());
    blueprint_->setVisible(!comment && !isScript && BlueprintVisible());
    const bool canAdd = trigger != nullptr && !comment && trigger->customTextFlag == 0;
    convertCodeButton_->setVisible(canAdd);
    convertCodeButton_->setEnabled(canAdd && ClassicCodeAllowed());
    recoverCodeButton_->setVisible(isScript && !codeNativeLua_);
    recoverCodeButton_->setEnabled(isScript && !codeNativeLua_ && ClassicCodeAllowed());
    for (QPushButton* button : {addEventButton_, addConditionButton_, addActionButton_}) {
        button->setEnabled(canAdd);
    }
    editEcaButton_->setEnabled(false);
    PopulateProperties(trigger);
    if (trigger != nullptr && ecaModel_->rowCount() > 0) {
        ecaView_->expandAll();
        ecaView_->setCurrentIndex(ecaModel_->index(0, 0));
    }
    SyncBlueprint();
}

void TriggerModule::ShowSelectedEca() {
    const QModelIndex index = ecaView_->currentIndex();
    const auto* eca = ecaModel_->EcaAt(index);
    editEcaButton_->setEnabled(eca != nullptr && !eca->uiSource.empty());
    if (eca != nullptr && !syncingSelection_ && BlueprintVisible()) {
        blueprint_->Select(eca->id);
    }
}

void TriggerModule::PopulateProperties(const Trigger::TriggerRecord* trigger) {
    suppressPropertySignals_ = true;
    const bool has = trigger != nullptr;
    propertiesBox_->setEnabled(has);
    propertyTriggerId_ = has ? trigger->id : 0;
    enabledCheck_->setChecked(has && trigger->enabled != 0);
    initiallyOnCheck_->setChecked(has && trigger->initiallyClosed == 0);
    runOnInitCheck_->setChecked(has && trigger->runOnInit != 0);
    if (has) {
        commentEdit_->setPlainText(QString::fromUtf8(trigger->description.text));
    } else {
        commentEdit_->clear();
    }
    // 文本控件会规范化 CRLF 等字符；以装载后的显示文本判断用户是否编辑过。
    propertyCommentBaseline_ = commentEdit_->toPlainText();
    UpdateMetaBar(trigger);
    suppressPropertySignals_ = false;
}

bool TriggerModule::CommitPendingEdits() {
    if (page_) {
        if (!CommitCodeDraft()) return false;
        if (luaScripts_ && !luaScripts_->CommitPendingDraft()) return false;
        CommitProperties();
    }
    return true;
}

void TriggerModule::CommitProperties() {
    auto* document = project_.triggerDocument_.get();
    if (suppressPropertySignals_ || document == nullptr || project_.triggerUndoStack_ == nullptr ||
        propertyTriggerId_ == 0) {
        return;
    }
    const bool desiredEnabled = enabledCheck_->isChecked(), desiredOpen = initiallyOnCheck_->isChecked();
    const bool desiredRun = runOnInitCheck_->isChecked();
    const QString comment = commentEdit_->toPlainText();
    if (!CommitCodeDraft()) {
        if (const auto* actual = Trigger::TriggerDocumentEditor::FindTrigger(*document, propertyTriggerId_)) {
            const QScopedValueRollback suppress(suppressPropertySignals_, true);
            enabledCheck_->setChecked(actual->enabled != 0);
            initiallyOnCheck_->setChecked(actual->initiallyClosed == 0);
            runOnInitCheck_->setChecked(actual->runOnInit != 0);
        }
        return;
    }
    const auto* trigger = Trigger::TriggerDocumentEditor::FindTrigger(*document, propertyTriggerId_);
    if (trigger == nullptr) {
        return;
    }
    TriggerPropertyState before;
    before.enabled = trigger->enabled;
    before.initiallyClosed = trigger->initiallyClosed;
    before.runOnInit = trigger->runOnInit;
    before.comment = trigger->description.text;
    TriggerPropertyState after;
    after.enabled = desiredEnabled ? 1 : 0;
    after.initiallyClosed = desiredOpen ? 0 : 1;
    after.runOnInit = desiredRun ? 1 : 0;
    after.comment = comment == propertyCommentBaseline_ ? before.comment : comment.toStdString();
    if (before.enabled == after.enabled && before.initiallyClosed == after.initiallyClosed &&
        before.runOnInit == after.runOnInit && before.comment == after.comment) {
        return; // 无变化，不入栈
    }
    project_.triggerUndoStack_->push(new EditTriggerPropertiesUndoCommand(
        document, propertyTriggerId_, std::move(before), std::move(after), tr("编辑触发器属性")));
}

void TriggerModule::RefreshAfterUndo() {
    const auto pending = std::exchange(pendingScalarUiNotice_, {});
    const QPointer<QUndoStack> actualStack(scalarUiStack_);
    const auto modelEpoch = pendingScalarUiModelEpoch_;
    if (!page_ || publishingCode_) {
        return;
    }
    if (pending && actualStack && sender() == actualStack.data() && CanRefreshScalarUi()
        && ecaModel_->PresentationEpoch() == modelEpoch
        && project_.ScalarUiNoticeMatchesIndex(pending, actualStack.data(), actualStack->index(), this)) {
        const QPointer<TriggerModule> self(this);
        const QPointer<QtProject::EditorProject> project(&project_);
        const QPointer<TriggerEcaModel> model(ecaModel_);
        const bool refreshed = model->InvalidateScalarPresentations();
        if (!self || !project || !model || !page_) return;
        if (refreshed && actualStack && actualStack.data() == project_.triggerUndoStack_ && CanRefreshScalarUi()
            && model->PresentationEpoch() == modelEpoch
            && project_.ScalarUiNoticeMatchesIndex(pending, actualStack.data(), actualStack->index(), this)) {
            RememberScalarUiBinding();
            return;
        }
    }
    if (HasPendingCodeDraft()) ConfigureCodeChecking();
    // ECA 模型可能仍指向改动前的节点表：按稳定 ID 记住选中项，刷新后再定位。
    const QVariant ecaId = ecaView_->currentIndex().data(TriggerEcaModel::StableIdRole);
    const StableId shown = displayedTrigger_;
    RefreshExplorer();
    if (displayedTrigger_ != shown) {
        return; // 原触发器已不存在（例如撤销了新建），RefreshExplorer 已改显示其他项
    }
    RefreshEditor(ecaId.isValid() ? ecaId.toULongLong() : 0);
}

void TriggerModule::RefreshEditor(StableId preferredEcaId) {
    if (!page_) {
        return;
    }
    UpdateSummary();
    RefreshCodeFromDocument();
    if (showingGlobalCode_) return;
    if (!HasPendingCodeDraft()) ShowSelectedTrigger();
    const auto* trigger = CurrentTrigger();
    ecaModel_->SetDocument(project_.triggerDocument_.get(), project_.uiCatalog_.get());
    ecaModel_->SetTrigger(trigger == nullptr ? 0 : trigger->id);
    PopulateProperties(trigger);
    ecaView_->expandAll(); // 全部展开：三段分组头 + 各级嵌套函数体（If/Loop/ForEach）
    QModelIndex selection = preferredEcaId == 0 ? QModelIndex{} : ecaModel_->IndexForStableId(preferredEcaId);
    if (!selection.isValid() && ecaModel_->rowCount() > 0) {
        selection = ecaModel_->index(0, 0);
    }
    if (selection.isValid()) {
        ecaView_->setCurrentIndex(selection);
        ecaView_->scrollTo(selection, QAbstractItemView::PositionAtCenter);
    }
    const QPointer<TriggerModule> self(this);
    const QPointer<QtProject::EditorProject> project(&project_);
    const QPointer<TriggerEcaModel> model(ecaModel_);
    const auto modelEpoch = model->PresentationEpoch();
    const auto generation = project_.editRevision_.Generation(), revision = project_.editRevision_.Revision();
    const auto navigationGeneration = navigationDocumentGeneration_;
    const auto* document = project_.triggerDocument_.get(); const auto* catalog = project_.uiCatalog_.get();
    SyncBlueprint();
    if (!self || !project || !model) return;
    if (ecaModel_ == model.data() && model->PresentationEpoch() == modelEpoch
        && project_.triggerDocument_.get() == document && project_.uiCatalog_.get() == catalog
        && project_.editRevision_.Generation() == generation && project_.editRevision_.Revision() == revision
        && navigationDocumentGeneration_ == navigationGeneration) RememberScalarUiBinding();
    else InvalidateScalarUiBinding();
}

void TriggerModule::SetSearchText(const QString& text) {
    if (!page_) {
        return;
    }
    if (workspaceTabs_ && workspaceTabs_->currentIndex() == 1) {
        if (!luaScripts_->Editor()->FindNext(text) && !text.isEmpty()) ShowStatus(tr("Lua 正文中未找到：%1").arg(text), 4000);
        return;
    }
    if (showingMapScript_) {
        // 脚本模式下搜索框在脚本内查找，不过滤触发器目录。
        if (!mapScriptView_->FindNext(text) && !text.isEmpty()) ShowStatus(tr("脚本中未找到：%1").arg(text), 4000);
        return;
    }
    if (codeTarget_) {
        if (!scriptView_->FindNext(text) && !text.isEmpty()) ShowStatus(tr("代码中未找到：%1").arg(text), 4000);
        return;
    }
    explorerSearch_->setText(text);
}

bool TriggerModule::HasLoadedContent() const {
    const auto* document = project_.triggerDocument_.get();
    if (!page_ || document == nullptr || project_.uiCatalog_ == nullptr) {
        return false;
    }
    if (codeTarget_) {
        if (scriptView_->isHidden() || scriptView_->IsLoading() || codeDisplayLoadGeneration_
            || codeDraftPending_ || !codeBaselinePlan_ || codeBaselinePlan_->Changed()) return false;
        const auto baselineTarget = codeBaselinePlan_->Target();
        if (baselineTarget.kind != codeTarget_->kind || baselineTarget.triggerId != codeTarget_->triggerId) return false;
        const Trigger::RawText* live = nullptr;
        if (codeTarget_->kind == Trigger::CodeBlockKind::GlobalHeader && codeTarget_->triggerId == 0) live = &document->wct.customCode;
        else if (const auto* selected = CurrentTrigger(); selected && selected->customTextFlag != 0
            && codeTarget_->kind == Trigger::CodeBlockKind::CustomTrigger && codeTarget_->triggerId == selected->id) live = &selected->customText;
        // 此处只核对已载入的可见正文；提交仍须通过原编码/字节绑定的密封计划。
        return live && !live->text.empty()
            && QString::fromUtf8(Trigger::TriggerCodeEditor::NormalizedText(*live)) == codeBaselineText_
            && scriptView_->toPlainText() == codeDisplayBaselineText_;
    }
    if (document->triggers.empty() || explorerModel_->rowCount() == 0) return false;
    for (int group = 0; group < ecaModel_->rowCount(); ++group) {
        const QModelIndex groupIndex = ecaModel_->index(group, 0);
        if (groupIndex.isValid() && ecaModel_->rowCount(groupIndex) > 0) {
            return true;
        }
    }
    return false;
}

bool TriggerModule::eventFilter(QObject* watched, QEvent* event) {
    if (watched == logicViewport_ && !manualLogicColumns_ && !logicColumnsUpdateQueued_ &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        // 等视口及滚动条完成本轮布局再分配，避免列宽变化在 resize 中重入。
        logicColumnsUpdateQueued_ = true;
        QTimer::singleShot(0, this, [this] {
            logicColumnsUpdateQueued_ = false;
            UpdateLogicColumns();
        });
    }
    if (watched == explorerView_ && HandleExplorerKey(event)) {
        return true;
    }
    // 注释编辑框失焦时提交触发器属性变更（避免逐字入栈）。
    if (watched == commentEdit_ && event->type() == QEvent::FocusOut) {
        CommitProperties();
    }
    return QObject::eventFilter(watched, event);
}

void TriggerModule::UpdateLogicColumns() {
    if (!page_ || !logicViewport_ || !logicViewport_->isVisible() || manualLogicColumns_ ||
        updatingLogicColumns_) return;
    auto* header = ecaView_->header();
    const int width = std::clamp(logicViewport_->width() - header->sectionSize(1),
        header->minimumSectionSize(), header->maximumSectionSize());
    if (header->sectionSize(0) == width) return;
    const QScopedValueRollback updating(updatingLogicColumns_, true);
    header->resizeSection(0, width);
}

bool TriggerModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.module != "triggers") {
        return false;
    }
    if (target.kind == QLatin1String("find-replace")) {
        // 此目标只授权Shell激活现有模块；MainWindow在导航成功后才弹查找替换面板。
        // 非空身份/扩展或无来源先拒绝，不能借未知kind创建页面、提交草稿或改选中项。
        if (!target.id.isEmpty() || !target.extra.isEmpty() || !project_.triggerDocument_ || !project_.uiCatalog_
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) return false;
        const QPointer<TriggerModule> self(this);
        const QPointer<QtProject::EditorProject> project(&project_);
        const auto* document = project_.triggerDocument_.get();
        const auto* catalog = project_.uiCatalog_.get();
        const auto documentGeneration = navigationDocumentGeneration_;
        const auto editGeneration = project_.editRevision_.Generation();
        const auto catalogSha = catalog->Sha256();
        const auto wtgSha = std::string(document->SourceWtgSha256()), wctSha = std::string(document->SourceWctSha256());
        if (!CommitPendingEdits() || !self || !project) return false;
        // 草稿可推进同一文档Revision；来源替换/新Generation/目录变化不能沿旧授权继续。
        return self->navigationDocumentGeneration_ == documentGeneration
            && project->editRevision_.Generation() == editGeneration
            && project->triggerDocument_.get() == document && project->uiCatalog_.get() == catalog
            && !project->sourceChangeSuspected_ && !project->sourceChangeConfirmed_
            && catalog->Sha256() == catalogSha && document->SourceWtgSha256() == wtgSha
            && document->SourceWctSha256() == wctSha;
    }
    std::optional<StableId> searchOwner, searchArgument, searchEca, searchAnchor;
    const bool searchParameter = target.extra.contains("triggerId") || target.extra.contains("argumentId");
    if (searchParameter) {
        searchOwner = SearchIdentity(target.extra.value("triggerId"));
        searchArgument = SearchIdentity(target.extra.value("argumentId"));
        searchEca = SearchIdentity(QVariant(target.id));
        const auto stable = target.extra.value("stableIdentity");
        if (target.kind != QLatin1String("eca") || !searchOwner || !searchArgument || !searchEca
            || stable.typeId() != QMetaType::Bool || !stable.toBool() || !project_.triggerDocument_) return false;
        searchAnchor = SearchArgumentAnchor(*project_.triggerDocument_, *searchOwner, *searchEca, *searchArgument);
        if (!searchAnchor) return false;
    }
    (void)Widget();
    if (!CommitPendingEdits()) return false;
    if (target.kind == QLatin1String("lua-file")) {
        if (!luaScripts_->SelectScript(target.id)) return false;
        workspaceTabs_->setCurrentIndex(1);
        return workspaceTabs_->currentIndex() == 1;
    }
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr) {
        return false;
    }
    workspaceTabs_->setCurrentIndex(0);
    if (target.kind == QLatin1String("code")) {
        const auto index = project_.CurrentCodeCheckIndex();
        const auto binding = target.extra.value("codeBinding").toString();
        const int line = target.extra.value("line").toInt();
        const int column = target.extra.value("column").toInt();
        if (!index || binding.toStdString() != index->contextBinding || line <= 0 || column <= 0) return false;
        if (target.id == QStringLiteral("WCT/header.j") || target.id == QStringLiteral("WCT/header.lua")) {
            if (!ShowGlobalCustomCode()) return false;
            scriptView_->GoToUtf8ByteColumn(line, column); return true;
        }
        if (target.id.startsWith(QStringLiteral("WCT/trigger-"))
            && (target.id.endsWith(QStringLiteral(".j")) || target.id.endsWith(QStringLiteral(".lua")))) {
            bool valid = false;
            const int suffixLength = target.id.endsWith(QStringLiteral(".lua")) ? 4 : 2;
            const auto codeId = target.id.mid(12, target.id.size() - 12 - suffixLength).toULongLong(&valid);
            const auto found = std::find_if(document->triggers.begin(), document->triggers.end(),
                [&](const auto& trigger) { return trigger.id == codeId && trigger.type == 0 && trigger.customTextFlag != 0; });
            if (!valid || found == document->triggers.end()) return false;
            explorerSearch_->clear(); SelectTrigger(codeId); scriptView_->GoToUtf8ByteColumn(line, column); return true;
        }
        const auto frozen = std::find_if(index->externalSources.begin(), index->externalSources.end(),
            [&](const auto& source) { return QString::fromUtf8(source.name) == target.id; });
        if (frozen == index->externalSources.end()) {
            ShowStatus(tr("本次代码索引未保存该来源的只读正文，请重新检查代码。"), 8000); return false;
        }
        const QByteArray bytes = QByteArray::fromStdString(frozen->text);
        return scriptView_->ShowFrozenSource({target.id, {}, {}, bytes}, line, column, binding, true);
    }
    // 触发器按稳定 ID 或名称定位；ECA 按稳定 ID 定位并选中其所在触发器。
    const auto stableValue = target.extra.value("stableIdentity");
    if (target.extra.contains("stableIdentity") && stableValue.typeId() != QMetaType::Bool) return false;
    const bool stableOnly = stableValue.toBool();
    if (stableOnly && (target.id.isEmpty() || target.id.size() > 20 ||
        !std::all_of(target.id.cbegin(), target.id.cend(), [](QChar c) { return c >= '0' && c <= '9'; }))) return false;
    bool numeric = false;
    const StableId id = target.id.toULongLong(&numeric);
    if (stableOnly && !numeric) return false;
    if (target.kind == QLatin1String("eca") && numeric) {
        if (searchParameter) {
            // 草稿提交可能更新文档；继续确认同一真实归属，不能沿旧指针定位。
            const auto current = SearchArgumentAnchor(*document, *searchOwner, *searchEca, *searchArgument);
            if (!current || *current != *searchAnchor) return false;
            explorerSearch_->clear(); SelectTrigger(*searchOwner); ShowSelectedTrigger(); RefreshEditor(*searchAnchor);
            logicHint_->setText(*searchAnchor == *searchEca
                ? tr("搜索参数 · ECA %1 · 参数 %2").arg(QString::number(*searchEca), QString::number(*searchArgument))
                : tr("搜索参数 · 函数 ECA %1 · 参数 %2 · 包含语句 %3（函数参数没有独立逻辑树行）")
                    .arg(QString::number(*searchEca), QString::number(*searchArgument), QString::number(*searchAnchor)));
            ShowStatus(logicHint_->text(), 8000);
            return true;
        }
        const auto placement = Trigger::TriggerDocumentEditor::LocateEcaPlacement(*document, id);
        if (!placement.found) {
            return false;
        }
        explorerSearch_->clear();
        SelectTrigger(placement.parentTriggerId);
        RefreshEditor(id);
        return true;
    }
    for (const auto& trigger : document->triggers) {
        if ((numeric && trigger.id == id) || (!stableOnly && QString::fromUtf8(trigger.name.text) == target.id)) {
            explorerSearch_->clear();
            SelectTrigger(trigger.id);
            return true;
        }
    }
    return false;
}

QJsonObject TriggerModule::SaveViewState() const {
    if (!page_) {
        return pendingViewState_;
    }
    const auto* trigger = CurrentTrigger();
    const auto* eca = ecaModel_->EcaAt(ecaView_->currentIndex());
    QJsonObject state{
        {"trigger", trigger == nullptr ? QString{} : QString::number(trigger->id)},
        {"eca", eca == nullptr ? QString{} : QString::number(eca->id)},
        {"treeScroll", explorerView_->verticalScrollBar()->value()},
        {"view", BlueprintVisible() ? QStringLiteral("blueprint") : QStringLiteral("traditional")},
        {"collapsed", [this]() {
            QJsonArray ids;
            for (const std::int32_t id : collapsedCategories_) ids.append(id);
            return ids;
        }()},
    };
    // 缺少此字段表示自动分配；只持久化用户明确调整过的两列。
    if (manualLogicColumns_) state.insert("logicColumnWidths", UiKit::SaveColumnWidths(ecaView_->header()));
    return state;
}

void TriggerModule::RestoreViewState(const QJsonObject& state) {
    pendingViewState_ = state;
    if (!page_) {
        return;
    }
    auto* header = ecaView_->header();
    // 列宽只属于本机视图偏好；缺列、非整数和超出表头上限的数据整组拒绝。
    // 无效或旧状态回到自动模式，避免只恢复一列而混用两种分配策略。
    const QJsonArray widths = state.value("logicColumnWidths").toArray();
    manualLogicColumns_ = widths.size() == 2 && std::all_of(widths.begin(), widths.end(), [header](const QJsonValue& value) {
        const int width = value.toInt(-1);
        return value.isDouble() && value.toDouble() == width && width >= header->minimumSectionSize() &&
            width <= header->maximumSectionSize();
    });
    {
        const QScopedValueRollback updating(updatingLogicColumns_, true);
        if (manualLogicColumns_) {
            // 通用恢复器跳过末列；这里两列都为 Interactive，须在验证后一起恢复。
            for (int column = 0; column < 2; ++column) header->resizeSection(column, widths.at(column).toInt());
        } else {
            header->resizeSection(1, kLogicTypeColumnWidth);
        }
    }
    // 列宽可在尚未载入地图时恢复；对象定位仍按原路径等文档就绪。
    if (project_.triggerDocument_ == nullptr) {
        UpdateLogicColumns();
        return;
    }
    pendingViewState_ = {};
    collapsedCategories_.clear();
    for (const QJsonValue& id : state["collapsed"].toArray()) collapsedCategories_.insert(id.toInt());
    const QString trigger = state["trigger"].toString();
    if (!trigger.isEmpty() && Reveal({"triggers", "trigger", trigger, {}})) {
        RefreshEditor(state["eca"].toString().toULongLong());
    }
    SetBlueprintVisible(state["view"].toString() == QLatin1String("blueprint"));
    // 定位触发器会展开它所在的类别；按保存时的折叠状态再应用一次（当前项可以在折叠的类别里）。
    ApplyExplorerExpansion();
    explorerView_->verticalScrollBar()->setValue(state["treeScroll"].toInt());
    UpdateLogicColumns();
}

} // namespace War3Map::Modules
