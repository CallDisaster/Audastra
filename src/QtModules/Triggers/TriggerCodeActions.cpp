#include "TriggerModule.h"
#include "TriggerLuaScripts.h"

#include "EditorProject.h"
#include "TriggerCodeCommands.h"
#include "TriggerModels.h"
#include "TriggerBlueprint.h"
#include "Widgets/CodeView.h"
#include "Widgets/CodeProblemsView.h"
#include "TriggerCodeChecking.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/LuaAuthorCode.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"

#include <QInputDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QTreeView>
#include <QToolButton>
#include <QUndoStack>

namespace War3Map::Modules {
namespace {
QString CodeMessage(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? QObject::tr("代码修改未应用。") : QString::fromUtf8(diagnostics.front().message);
}
QString CodeUnicodeReadonlyNotice() {
    return QObject::tr("只读：含特殊空白字符");
}
// 只读原来源；编辑态 language、目标配置和脚本成员的存在均不能授予原生 Lua 作者权限。
Result<std::vector<std::byte>> NativeLuaSettings(const QtProject::EditorProject& project) {
    const auto fail = [](const char* code, const char* message) {
        return Result<std::vector<std::byte>>::Failure("LuaAuthor", code, message);
    };
    if (project.sourceChangeSuspected_ || project.sourceChangeConfirmed_)
        return fail("LUA_AUTHOR_SOURCE_CHANGED", "地图来源已变化；原生 Lua 草稿保留，请重新载入来源。");
    auto source = project.ReadMapMember("war3map.w3i");
    if (!source.Succeeded() || !source.value) return source;
    if (source.value->size() > 4 * 1024 * 1024)
        return fail("LUA_AUTHOR_SETTINGS_BUDGET", "原来源地图信息超过 4 MiB 上限。");
    const auto settings = MapData::MapSettings::Parse(*source.value);
    if (!settings.Succeeded() || !settings.value || !settings.value->FullyParsed()
        || settings.value->SourceFormatVersion() != 31 || settings.value->formatVersion != 31
        || settings.value->scriptLanguage != 1)
        return fail("LUA_AUTHOR_SOURCE_LANGUAGE", "原来源须完整解析为 W3I v31 / Lua 地图。");
    return source;
}
}

bool TriggerModule::ClassicCodeAllowed() const {
    if (project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) return false;
    if (NativeLuaSettings(project_).Succeeded()) return false;
    if (project_.luaMapTarget_) return false;
    if (project_.mapEdits_) {
        const auto snapshot = project_.mapEdits_->Snapshot();
        if (const auto& settings = snapshot.Settings(); settings && settings->scriptLanguage != 0) return false;
    }
    return !project_.archiveSession_ || (!project_.archiveSession_->HasMember("war3map.lua")
        && !project_.archiveSession_->HasMember("Scripts/war3map.lua"));
}

bool TriggerModule::CodeAuthoringAllowed() const {
    if (!sourceDisplayLossless_) return false;
    return codeNativeLua_ ? !codeSourceSettingsSha256_.empty()
        && !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_ : ClassicCodeAllowed();
}

void TriggerModule::RefreshCodeSourceState() {
    if (!page_ || !codeTarget_) return;
    if (!codeDraftPending_) { LoadCodeTarget(*codeTarget_); return; }
    // 来源事件不重置草稿或基线，提交时仍核对原 W3I 身份。
    scriptView_->SetEditable(CodeAuthoringAllowed() && codeBaselinePlan_.has_value());
    applyCodeButton_->setEnabled(CodeAuthoringAllowed());
    if (!CodeAuthoringAllowed()) codeDraftNote_->setText(!sourceDisplayLossless_ ? CodeUnicodeReadonlyNotice()
        : tr("地图来源已变化"));
    ConfigureCodeChecking();
}

bool TriggerModule::HasPendingCodeDraft() const { return codeDraftPending_ || (luaScripts_ && luaScripts_->HasPendingDraft()); }

void TriggerModule::UpdateCodeDraftState() {
    if (suppressCodeSignals_ || !scriptView_ || !codeTarget_ || scriptView_->IsLoading() || codeDisplayLoadGeneration_) return;
    codeDraftPending_ = scriptView_->toPlainText() != codeDisplayBaselineText_;
    applyCodeButton_->setEnabled(codeDraftPending_ && CodeAuthoringAllowed());
    discardCodeButton_->setEnabled(codeDraftPending_);
    codeDraftNote_->setText(!sourceDisplayLossless_ ? CodeUnicodeReadonlyNotice()
        : codeDraftPending_ ? tr("未应用") : tr("已应用"));
    emit DraftEdited();
}

void TriggerModule::LoadCodeTarget(Trigger::CodeBlockTarget target) {
    if (!project_.triggerDocument_ || !project_.uiCatalog_) return;
    const Trigger::RawText* text = nullptr;
    if (target.kind == Trigger::CodeBlockKind::GlobalHeader) text = &project_.triggerDocument_->wct.customCode;
    else if (const auto* trigger = Trigger::TriggerDocumentEditor::FindTrigger(*project_.triggerDocument_, target.triggerId);
        trigger && trigger->customTextFlag != 0) text = &trigger->customText;
    if (!text) return;
    const QScopedValueRollback suppress(suppressCodeSignals_, true);
    codeTarget_ = target;
    codeDisplayLoadGeneration_.reset();
    codeBaselineText_ = QString::fromUtf8(Trigger::TriggerCodeEditor::NormalizedText(*text));
    sourceDisplayLossless_ = !codeBaselineText_.contains(QChar(0x00a0)) && !codeBaselineText_.contains(QChar(0x2028))
        && !codeBaselineText_.contains(QChar(0x2029));
    scriptView_->SetText(codeBaselineText_);
    codeDisplayBaselineText_ = scriptView_->IsLoading() ? QString{} : scriptView_->toPlainText();
    codeDraftPending_ = false;
    codeBaselinePlan_.reset();
    codeSourceSettingsSha256_.clear();
    const auto native = NativeLuaSettings(project_);
    codeNativeLua_ = native.Succeeded() && native.value.has_value();
    if (codeNativeLua_) {
        const auto hash = Sha256(*native.value);
        if (hash.Succeeded() && hash.value) codeSourceSettingsSha256_ = *hash.value;
    }
    auto baseline = codeNativeLua_
        ? Script::LuaAuthorCodeEditor::PrepareText(*native.value, *project_.triggerDocument_, *project_.uiCatalog_, target,
            codeBaselineText_.toStdString())
        : Trigger::TriggerCodeEditor::PrepareText(*project_.triggerDocument_, *project_.uiCatalog_, target,
            codeBaselineText_.toStdString());
    if (baseline.Succeeded() && baseline.value && !baseline.value->Changed()) codeBaselinePlan_ = std::move(*baseline.value);
    {
        const QSignalBlocker block(codeLanguage_);
        codeLanguage_->setCurrentIndex(codeNativeLua_ ? 2 : codeLanguage_->currentIndex() == 2 ? 1 : codeLanguage_->currentIndex());
        codeLanguage_->setEnabled(!codeNativeLua_);
    }
    scriptView_->SetLanguage(codeNativeLua_ ? UiKit::CodeLanguage::Lua
        : codeLanguage_->currentIndex() == 0 ? UiKit::CodeLanguage::Jass : UiKit::CodeLanguage::VJass);
    scriptView_->SetEditable(CodeAuthoringAllowed() && codeBaselinePlan_.has_value() && !scriptView_->IsLoading());
    scriptView_->SetSourceName(QString::fromUtf8(Services::CodeCheckTargetSourceName(target,
        codeNativeLua_ ? Services::CodeLanguage::Lua53 : Services::CodeLanguage::Jass)));
    ConfigureCodeChecking();
    applyCodeButton_->setEnabled(false);
    discardCodeButton_->setEnabled(false);
    codeDraftNote_->setText(!sourceDisplayLossless_ ? CodeUnicodeReadonlyNotice()
        : !CodeAuthoringAllowed() ? tr("只读")
        : codeBaselinePlan_ ? QString{} : CodeMessage(baseline.diagnostics));
}

void TriggerModule::ConfigureCodeChecking() {
    if (!scriptView_ || !codeTarget_) return;
    if (project_.uiCatalog_ && codeSymbolNotesCatalog_ != project_.uiCatalog_->Sha256()) {
        codeSymbolNotes_ = UiFunctionNotes(*project_.uiCatalog_);
        codeSymbolNotesCatalog_ = project_.uiCatalog_->Sha256();
    } else if (!project_.uiCatalog_) {
        codeSymbolNotes_.clear(); codeSymbolNotesCatalog_.clear();
    }
    scriptView_->SetSymbolNotes(codeSymbolNotes_);
    if (!sourceDisplayLossless_) {
        const auto reason = CodeUnicodeReadonlyNotice() + tr(" 未检查；保存仍使用原字节。");
        // Qt 显示已规范化，不能把显示文本当成作者原正文检查或生成项目索引。
        scriptView_->SetPrepareCheck([reason](const QString&, std::uint64_t generation) -> UiKit::CodeCheckTask {
            return [reason, generation](std::stop_token stop) {
                UiKit::CodeCheckResult result; result.generation = generation;
                result.reason = reason; result.cancelled = stop.stop_requested();
                return result;
            };
        });
    } else {
        if (!codeCheckCache_) codeCheckCache_ = std::make_shared<Services::CodeCheckProjectCache>();
        scriptView_->SetCheckBindingValidator([this](const QString& binding) {
            return project_.IsCodeCheckBindingCurrent(binding.toStdString());
        });
        // UI 只冻结共同工程快照；后台函数拥有全部来源，绝不借用 project_ 或控件。
        scriptView_->SetPrepareCheck([this](const QString& text, std::uint64_t generation) {
            return PrepareProjectCodeCheck(project_.CaptureCodeCheckSnapshot(), codeCheckCache_, *codeTarget_, text, generation, scriptView_->Language());
        });
    }
    if (scriptView_->IsLoading()) codeDisplayLoadGeneration_ = scriptView_->Generation();
}

void TriggerModule::RefreshCodeFromDocument() {
    if (codeTarget_ && !codeDraftPending_ && !publishingCode_) LoadCodeTarget(*codeTarget_);
}

void TriggerModule::DiscardPendingCodeDraft() {
    if (luaScripts_) luaScripts_->DiscardPendingDraft();
    codeDraftPending_ = false;
    if (showingGlobalCode_) LoadCodeTarget({Trigger::CodeBlockKind::GlobalHeader, 0});
    else ShowSelectedTrigger();
}

bool TriggerModule::ApplyCodePlan(const Trigger::TriggerCodeEditPlan& plan, const QString& label,
    Trigger::GuiConversionSnapshots::PreparedSnapshot snapshot) {
    if (!project_.triggerDocument_ || !project_.uiCatalog_ || !project_.triggerUndoStack_) return false;
    // 先分配 Qt 命令；共用服务发布成功后只移动已签发的收据。
    auto command = std::make_unique<QtProject::TriggerCodeEditCommand>(project_, Trigger::TriggerCodeEditReceipt{}, label);
    if (plan.Target().kind == Trigger::CodeBlockKind::CustomTrigger
        && !Trigger::TriggerDocumentEditor::FindTrigger(*project_.triggerDocument_, plan.Target().triggerId))
        snapshot = Trigger::GuiConversionSnapshots::PrepareRemoval(plan.Target().triggerId);
    command->SetSnapshot(std::move(snapshot));
    if (const auto before = project_.ObservedTriggerFingerprint()) command->SetBeforeFingerprint(*before);
    auto applied = Trigger::TriggerCodeEditor::Apply(*project_.triggerDocument_, *project_.uiCatalog_, plan);
    if (!applied.Succeeded() || !applied.value) {
        project_.AddDiagnostics(applied.diagnostics);
        if (codeDraftNote_) codeDraftNote_->setText(CodeMessage(applied.diagnostics));
        ShowStatus(CodeMessage(applied.diagnostics), 10000);
        return false;
    }
    if (!applied.value->Changed()) return true;
    command->SetReceipt(std::move(*applied.value));
    command->PublishSnapshot();
    const QScopedValueRollback publishing(publishingCode_, true);
    project_.triggerUndoStack_->push(command.release());
    return true;
}

bool TriggerModule::CommitCodeDraft() {
    if (!codeDraftPending_) return true;
    if (!codeTarget_ || !codeBaselinePlan_ || codeBaselinePlan_->Changed() || !project_.triggerDocument_ || !project_.uiCatalog_ || !CodeAuthoringAllowed()) {
        codeDraftNote_->setText(!sourceDisplayLossless_ ? CodeUnicodeReadonlyNotice()
            : tr("代码来源或语言已改变，草稿保留；请复制正文或放弃草稿后重新载入。"));
        ShowStatus(codeDraftNote_->text(), 10000);
        return false;
    }
    // 未改基线计划只核对身份，不发布正文；旧草稿不能绑定到新文档或新元数据。
    auto checked = Trigger::TriggerCodeEditor::Apply(*project_.triggerDocument_, *project_.uiCatalog_, *codeBaselinePlan_);
    if (!checked.Succeeded()) {
        project_.AddDiagnostics(checked.diagnostics);
        codeDraftNote_->setText(CodeMessage(checked.diagnostics));
        ShowStatus(codeDraftNote_->text(), 10000);
        return false;
    }
    const auto native = codeNativeLua_ ? NativeLuaSettings(project_) : Result<std::vector<std::byte>>{};
    if (codeNativeLua_) {
        const auto hash = native.Succeeded() && native.value ? Sha256(*native.value) : Result<std::string>{};
        if (!hash.Succeeded() || !hash.value || *hash.value != codeSourceSettingsSha256_) {
            project_.AddDiagnostics(native.diagnostics);
            codeDraftNote_->setText(tr("Lua 地图信息已改变"));
            ShowStatus(codeDraftNote_->text(), 10000); return false;
        }
    }
    auto prepared = codeNativeLua_
        ? Script::LuaAuthorCodeEditor::PrepareText(*native.value, *project_.triggerDocument_, *project_.uiCatalog_, *codeTarget_,
            scriptView_->toPlainText().toStdString())
        : Trigger::TriggerCodeEditor::PrepareText(*project_.triggerDocument_, *project_.uiCatalog_, *codeTarget_,
            scriptView_->toPlainText().toStdString());
    if (!prepared.Succeeded() || !prepared.value) {
        project_.AddDiagnostics(prepared.diagnostics);
        codeDraftNote_->setText(CodeMessage(prepared.diagnostics));
        ShowStatus(codeDraftNote_->text(), 10000);
        return false;
    }
    if (!ApplyCodePlan(*prepared.value, showingGlobalCode_ ? tr("编辑全局自定义代码") : tr("编辑自定义代码"))) return false;
    codeDraftPending_ = false;
    RefreshCodeFromDocument();
    RefreshExplorer();
    return true;
}

bool TriggerModule::ShowGlobalCustomCode() {
    (void)Widget();
    if (!project_.triggerDocument_ || !project_.uiCatalog_ || !CommitPendingEdits()) return false;
    LeaveMapScript();
    showingGlobalCode_ = true;
    LoadCodeTarget({Trigger::CodeBlockKind::GlobalHeader, 0});
    displayedTrigger_ = selectedTrigger_;
    logicTitle_->setText(tr("全局自定义代码"));
    logicHint_->setText(codeNativeLua_ ? tr("Lua 5.3 · WCT 地图头部") : tr("JASS / vJass · WCT 地图头部"));
    propertiesBox_->hide(); ecaView_->hide(); blueprint_->hide(); mapScriptView_->hide();
    for (QPushButton* button : {addEventButton_, addConditionButton_, addActionButton_, editEcaButton_}) button->setEnabled(false);
    traditionalButton_->setEnabled(false); blueprintButton_->setEnabled(false); showRawValues_->hide();
    scriptView_->show(); codeTools_->show(); codeProblems_->show(); codeActions_->show(); convertCodeButton_->hide(); recoverCodeButton_->hide();
    applyCodeButton_->show(); discardCodeButton_->show(); codeDraftNote_->show();
    scriptView_->setFocus();
    return true;
}

void TriggerModule::NewCustomTrigger() {
    if (!CommitPendingEdits() || !project_.triggerDocument_) return;
    if (!NativeLuaSettings(project_).Succeeded() && !ClassicCodeAllowed()) {
        ShowStatus(tr("原地图来源或语言不允许创建自定义代码。"), 10000); return;
    }
    const auto suggested = Trigger::TriggerDirectoryEditor::UniqueTriggerName(*project_.triggerDocument_, tr("自定义代码").toStdString());
    bool accepted = false;
    const auto name = QInputDialog::getText(page_, tr("新建自定义代码"), tr("触发器名称（生成 InitTrig 初始化函数）："),
        QLineEdit::Normal, QString::fromUtf8(suggested), &accepted);
    if (accepted) (void)CreateCustomTrigger(name);
}

bool TriggerModule::CreateCustomTrigger(const QString& name) {
    (void)Widget();
    if (!CommitPendingEdits() || !project_.triggerDocument_ || !project_.uiCatalog_ || !project_.triggerUndoStack_) return false;
    const auto native = NativeLuaSettings(project_);
    const bool nativeLua = native.Succeeded() && native.value.has_value();
    if (!nativeLua && !ClassicCodeAllowed()) { ShowStatus(tr("原地图来源或语言不允许创建自定义代码。"), 10000); return false; }
    const auto text = name.trimmed().toStdString();
    const auto valid = Trigger::TriggerDirectoryEditor::ValidateTriggerName(*project_.triggerDocument_, text);
    if (!valid.Succeeded()) { project_.AddDiagnostics(valid.diagnostics); ShowStatus(CodeMessage(valid.diagnostics), 10000); return false; }
    const auto category = CurrentCategoryId();
    auto plan = nativeLua
        ? Script::LuaAuthorCodeEditor::PrepareNewCustom(*native.value, *project_.triggerDocument_, *project_.uiCatalog_,
            category.value_or(-1), text, {}, Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes,
            category ? std::string{} : tr("触发器").toStdString())
        : Trigger::TriggerCodeEditor::PrepareNewCustom(*project_.triggerDocument_, *project_.uiCatalog_,
            category.value_or(-1), text, {}, Trigger::TriggerCodeEditor::kDefaultMaxEditedBytes,
            category ? std::string{} : tr("触发器").toStdString());
    if (!plan.Succeeded() || !plan.value) { project_.AddDiagnostics(plan.diagnostics); ShowStatus(CodeMessage(plan.diagnostics), 10000); return false; }
    if (!ApplyCodePlan(*plan.value, tr("新建自定义代码"))) return false;
    const auto id = plan.value->Target().triggerId;
    const auto* created = Trigger::TriggerDocumentEditor::FindTrigger(*project_.triggerDocument_, id);
    if (!created) return false;
    SelectDirectoryItem({false, id, created->categoryId});
    RefreshEditor();
    scriptView_->setFocus();
    return true;
}

bool TriggerModule::ConvertCurrentTriggerToCustomCode() {
    if (!CommitPendingEdits() || !project_.triggerDocument_ || !project_.uiCatalog_ || !project_.triggerUndoStack_) return false;
    if (!ClassicCodeAllowed()) { ShowStatus(tr("当前地图使用 Lua，不能转换为 JASS 代码。"), 10000); return false; }
    const auto* trigger = CurrentTrigger();
    if (!trigger || trigger->type != 0 || trigger->customTextFlag != 0) return false;
    const auto id = trigger->id;
    const auto name = QString::fromUtf8(trigger->name.text);
    const auto inputs = CodeConversionInputs();
    auto generated = Script::GenerateGuiTriggerSource(inputs, id);
    if (!generated.Succeeded() || !generated.value) { project_.AddDiagnostics(generated.diagnostics); ShowStatus(CodeMessage(generated.diagnostics), 10000); return false; }
    auto plan = Trigger::TriggerCodeEditor::PrepareConversion(*project_.triggerDocument_, *project_.uiCatalog_, id,
        generated.value->text, generated.value->effectiveRunOnInit);
    if (!plan.Succeeded() || !plan.value) { project_.AddDiagnostics(plan.diagnostics); ShowStatus(CodeMessage(plan.diagnostics), 10000); return false; }
    // 先生成并密封计划，确认对话框之后只能发布同一计划，不能重新绑定已变化的图。
    auto snapshot = Trigger::GuiConversionSnapshots::Prepare(*project_.triggerDocument_, *project_.uiCatalog_, id, generated.value->text);
    if (!snapshot.Succeeded() || !snapshot.value) { project_.AddDiagnostics(snapshot.diagnostics); ShowStatus(CodeMessage(snapshot.diagnostics), 10000); return false; }
    const auto title = tr("转换为自定义代码");
    const auto body = tr("将“%1”转换为 JASS / vJass 代码。此操作可撤销；应用后不能直接切回 GUI，后续在代码中编辑。继续转换？").arg(name);
    const bool confirmed = confirm_ ? confirm_(title, body, {})
        : QMessageBox::question(page_, title, body, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
    if (!confirmed) return false;
    // 快照和同一转换收据一起进入撤销栈；取消或过期拒绝不会更改现有快照。
    if (!ApplyCodePlan(*plan.value, title, std::move(*snapshot.value))) return false;
    RefreshExplorer(id);
    RefreshEditor();
    return true;
}

Script::MapScriptInputs TriggerModule::CodeConversionInputs() {
    Script::MapScriptInputs inputs; inputs.triggers = project_.triggerDocument_.get(); inputs.catalog = project_.uiCatalog_.get();
    const auto& toolRoot = project_.compilerDataRoot_.empty() ? project_.activeUiDataRoot_ : project_.compilerDataRoot_;
    if (!toolRoot.empty()) {
        for (const auto& directory : ScriptSystemIncludes(toolRoot)) {
            std::error_code error;
            if (std::filesystem::exists(directory / "YDTrigger" / "SaveLoadSystem" / "HTSystem.j", error)) inputs.nullSafeHandleSet = true;
        }
    }
    return inputs;
}

bool TriggerModule::ConvertCurrentCodeToTrigger() {
    if (!CommitPendingEdits() || !project_.triggerDocument_ || !project_.uiCatalog_ || !project_.triggerUndoStack_) return false;
    if (!ClassicCodeAllowed()) { ShowStatus(tr("当前地图使用 Lua，不能把 JASS 代码转回 T。"), 10000); return false; }
    const auto* trigger = CurrentTrigger();
    if (!trigger || trigger->type != 0 || trigger->customTextFlag == 0) return false;
    const auto id = trigger->id;
    const auto name = QString::fromUtf8(trigger->name.text);
    const auto reject = [this](const std::vector<Diagnostic>& diagnostics) {
        project_.AddDiagnostics(diagnostics);
        const auto message = tr("不能转回 T：") + CodeMessage(diagnostics);
        if (codeDraftNote_) codeDraftNote_->setText(message);
        ShowStatus(message, 15000);
        return false;
    };
    const auto* snapshot = project_.guiConversionSnapshots_.Find(*project_.triggerDocument_, *project_.uiCatalog_, id);
    auto recovered = Script::RecoverGuiTrigger(CodeConversionInputs(), id, snapshot);
    if (!recovered.Succeeded() || !recovered.value) return reject(recovered.diagnostics);
    auto plan = Trigger::TriggerCodeEditor::PrepareRecovery(*project_.triggerDocument_, *project_.uiCatalog_, id,
        recovered.value->fragment);
    if (!plan.Succeeded() || !plan.value) return reject(plan.diagnostics);
    const auto title = tr("转回 T");
    const auto retained = recovered.value->preservedCodeLines || recovered.value->preservedCodeExpressions
        ? tr("其中 %1 行、%2 个表达式保留为自定义代码（哈希键原名无法从编译后的整数恢复）。")
            .arg(recovered.value->preservedCodeLines).arg(recovered.value->preservedCodeExpressions) : QString{};
    const auto body = tr("将“%1”转回传统事件 / 条件 / 动作。%2此操作可撤销。继续？").arg(name,
        recovered.value->fromSnapshot ? tr("代码未改动，按转换前原样恢复。")
            : tr("按代码重建，已与按 T 重新生成的代码逐词核对一致；代码里看不到的禁用行不会出现。") + retained);
    const bool confirmed = confirm_ ? confirm_(title, body, {})
        : QMessageBox::question(page_, title, body, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
    if (!confirmed) return false;
    if (!ApplyCodePlan(*plan.value, title)) return false;
    // 保留绑定的工程快照；撤销“转回”时正文恢复，原快照仍能继续使用。
    RefreshExplorer(id);
    RefreshEditor();
    return true;
}

} // namespace War3Map::Modules
