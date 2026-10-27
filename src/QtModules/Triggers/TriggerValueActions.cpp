#include "TriggerModule.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "ProjectQuerySources.h"
#include "TriggerArgumentCommands.h"
#include "TriggerStringCommands.h"
#include "TriggerCommands.h"
#include "TriggerBlueprint.h"
#include "TriggerDialogs.h"
#include "TriggerModels.h"
#include "TriggerSentenceDelegate.h"
#include "TriggerValuePanel.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerVariableReference.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QLineEdit>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>
#include <QUndoStack>

// 参数编辑的公共管线（参数对话框与取值面板共用）与取值面板的打开 / 应用。
namespace War3Map::Modules {
namespace {

using Editor = Trigger::TriggerDocumentEditor;

QString FirstMessage(const std::vector<Diagnostic>& diagnostics, const QString& fallback) {
    return diagnostics.empty() ? fallback : QString::fromUtf8(diagnostics.front().message);
}

// 编辑器使用 WTS 原正文（保留换行与颜色码）；显示服务的单行名字不能回写为正文。
std::optional<QString> CurrentStringBody(const QtProject::EditorProject& project, std::string_view raw) {
    const auto id = Services::StringReferenceId(raw);
    if (!id || !project.mapDocument_) return std::nullopt;
    const auto map = project.mapEdits_ ? project.mapEdits_->Snapshot() : *project.mapDocument_;
    if (!map.Strings()) return std::nullopt;
    const auto resolved = map.Strings()->Resolve("TRIGSTR_" + std::to_string(*id));
    return resolved.Succeeded() && resolved.value ? std::optional<QString>(QString::fromStdString(
        Trigger::DecodeSourceText(std::as_bytes(std::span(resolved.value->data(), resolved.value->size()))).text)) : std::nullopt;
}

} // namespace

bool TriggerModule::ApplyArgumentEdits(StableId ecaId, const std::vector<TriggerArgumentEditRequest>& requests,
    const QString& label, std::unique_ptr<QUndoCommand> prefix, std::optional<StableId>* newNestedEcaId) {
    auto& document = *project_.triggerDocument_;
    // 共享文本必须通过 WTG + WTS 的共同事务，不能混入先改一半的批量标量命令。
    const TriggerArgumentEditRequest* stringRequest = nullptr;
    for (const auto& request : requests) {
        const auto* argument = Editor::FindArgument(document, request.argumentId);
        if (argument && argument->type == 3 && !argument->callEca && !argument->indexArgument &&
            Services::StringReferenceId(argument->value.text) && request.valueKind == 3 &&
            request.value != argument->value.text) {
            stringRequest = &request;
            break;
        }
    }
    if (stringRequest) {
        if (requests.size() != 1 || prefix || stringRequest->callFunction || !stringRequest->callArguments.empty()) {
            ShowStatus(tr("共享字符串请逐个编辑：点击对应参数后修改正文；这次批量修改没有应用。"), 8000);
            return false;
        }
        if (!project_.mapEdits_ || !project_.triggerUndoStack_) {
            ShowStatus(tr("当前地图的字符串表不可编辑，参数保持原值。"), 8000);
            return false;
        }
        const auto map = project_.mapEdits_->Snapshot();
        const auto objects = QtProject::CaptureProjectObjectSources(project_);
        Services::NameResolutionContext context;
        context.objects = objects;
        context.standardObjects = project_.standardCatalog_.get();
        context.mapDocument = &map;
        context.triggers = &document;
        const auto* before = Editor::FindArgument(document, stringRequest->argumentId);
        if (map.Strings()) {
            const auto existing = map.Strings()->Resolve("TRIGSTR_" + std::to_string(*Services::StringReferenceId(before->value.text)));
            if (existing.Succeeded() && existing.value && Trigger::DecodeSourceText(
                    std::as_bytes(std::span(existing.value->data(), existing.value->size()))).text == stringRequest->value)
                return true;
        }
        const auto usage = Services::CountTriggerStringUsage(context, before->value.text);
        auto scope = Services::TriggerStringEditScope::AllReferences;
        if (usage.count > 1 || !usage.complete) {
            QString message = tr("这段文字在已读取的数据中被 %1 处共用。请选择修改范围。")
                .arg(static_cast<qulonglong>(usage.count));
            if (!usage.complete) {
                message += tr("\n引用统计尚不完整，以下来源没有确定：");
                for (const auto& limitation : usage.limitations) message += QStringLiteral("\n• ") + QString::fromStdString(limitation);
            }
            QMessageBox dialog(QMessageBox::Question, tr("编辑共享字符串"), message, QMessageBox::NoButton, page_);
            dialog.setObjectName("triggerStringEditScope");
            auto* all = dialog.addButton(tr("全部一起改"), QMessageBox::AcceptRole);
            auto* only = dialog.addButton(tr("只改这一处"), QMessageBox::ActionRole);
            auto* cancel = dialog.addButton(QMessageBox::Cancel);
            dialog.setDefaultButton(cancel);
            dialog.exec();
            if (dialog.clickedButton() == only) scope = Services::TriggerStringEditScope::OnlyThisArgument;
            else if (dialog.clickedButton() != all) return false;
        }
        const auto plan = Services::PrepareTriggerStringEdit(context, stringRequest->argumentId, stringRequest->value, scope);
        if (!plan.Succeeded() || !plan.value) {
            project_.AddDiagnostics(plan.diagnostics);
            ShowStatus(FirstMessage(plan.diagnostics, tr("共享字符串修改失败，参数保持原值。")), 8000);
            return false;
        }
        auto applied = Services::ApplyTriggerStringEdit(document, *project_.mapEdits_, *plan.value);
        if (!applied.Succeeded() || !applied.value) {
            project_.AddDiagnostics(applied.diagnostics);
            ShowStatus(FirstMessage(applied.diagnostics, tr("共享字符串事务没有应用。")), 8000);
            return false;
        }
        (void)project_.ObserveMapDataEdit("编辑触发器共享字符串");
        project_.triggerUndoStack_->push(new QtProject::TriggerStringEditCommand(project_, std::move(*applied.value), label));
        emit project_.MapDocumentChanged();
        RefreshEditor(ecaId);
        return true;
    }
    std::vector<Editor::ArgumentCallDetachReceipt> detachReceipts;
    std::vector<Trigger::EcaAppendReceipt> callReceipts;
    std::vector<TriggerArgumentChange> scalarChanges;
    bool failed = false;
    QString failureMessage;
    const auto currentCall = [&document](const Trigger::ArgumentNode& argument) -> const Trigger::EcaNode* {
        return argument.callEca.has_value() && *argument.callEca < document.ecaNodes.size()
            ? &document.ecaNodes[*argument.callEca] : nullptr;
    };
    // 保持原函数：仍选函数、函数名相同且不需要重写它的参数。
    const auto keepsCall = [&](const TriggerArgumentEditRequest& request, const Trigger::ArgumentNode& argument) {
        const auto* call = currentCall(argument);
        return call != nullptr && request.valueKind == 2 && request.callArguments.empty() &&
            (!request.callFunction.has_value() || *request.callFunction == call->name.text);
    };
    const auto setScalar = [&](StableId argumentId, std::int32_t kind, const std::string& value) {
        const auto* argument = Editor::FindArgument(document, argumentId);
        if (argument == nullptr || (argument->type == kind && argument->value.text == value)) return true;
        TriggerArgumentChange change;
        change.argumentId = argumentId;
        change.before = *argument;
        const Status status = Editor::SetScalarArgument(document, argumentId, kind, value);
        if (!status.Succeeded()) {
            failureMessage = FirstMessage(status.diagnostics, tr("参数修改失败。"));
            return false;
        }
        change.after = *Editor::FindArgument(document, argumentId);
        scalarChanges.push_back(std::move(change));
        return true;
    };

    // 1. 换来源的函数参数先摘除原函数（压实节点表）；2. 挂接新函数（扩展尾部）；3. 标量。
    //    失败时按相反顺序回滚，撤销栈也按同一顺序推入。
    for (const TriggerArgumentEditRequest& request : requests) {
        const auto* argument = Editor::FindArgument(document, request.argumentId);
        if (argument == nullptr || currentCall(*argument) == nullptr || keepsCall(request, *argument)) continue;
        auto detached = Editor::DetachArgumentCall(document, request.argumentId);
        if (!detached.Succeeded()) {
            failed = true;
            failureMessage = FirstMessage(detached.diagnostics, tr("无法替换原来的函数。"));
            break;
        }
        detachReceipts.push_back(std::move(*detached.value));
    }
    for (const TriggerArgumentEditRequest& request : requests) {
        if (failed) break;
        const auto* argument = Editor::FindArgument(document, request.argumentId);
        if (argument == nullptr || request.valueKind != 2 || currentCall(*argument) != nullptr ||
            !request.callFunction.has_value()) {
            continue;
        }
        auto appended = Editor::AppendArgumentCall(document, *project_.uiCatalog_, request.argumentId,
            request.expectedType, *request.callFunction);
        if (!appended.Succeeded()) {
            failed = true;
            failureMessage = FirstMessage(appended.diagnostics, tr("无法挂接所选函数。"));
            break;
        }
        const StableId nested = appended.value->rootEcaId;
        callReceipts.push_back(std::move(*appended.value));
        // 新函数自带的参数值（如局部变量读取的变量名）。
        const auto* call = Editor::FindEca(document, nested);
        for (std::size_t position = 0; call != nullptr && position < request.callArguments.size(); ++position) {
            if (position >= call->arguments.size() || call->arguments[position] >= document.argumentNodes.size()) break;
            const auto& [kind, value] = request.callArguments[position];
            if (!setScalar(document.argumentNodes[call->arguments[position]].id, kind, value)) {
                failed = true;
                break;
            }
        }
        if (!failed && request.callArguments.empty() && newNestedEcaId != nullptr && !newNestedEcaId->has_value()) {
            *newNestedEcaId = nested;
        }
    }
    for (const TriggerArgumentEditRequest& request : requests) {
        if (failed) break;
        if (request.valueKind == 2) continue;
        if (!setScalar(request.argumentId, request.valueKind, request.value)) failed = true;
    }

    if (failed || !ValidateOrReport()) {
        for (auto it = scalarChanges.rbegin(); it != scalarChanges.rend(); ++it) {
            if (auto* argument = Editor::FindArgument(document, it->argumentId)) {
                *argument = it->before;
            }
        }
        for (auto it = callReceipts.rbegin(); it != callReceipts.rend(); ++it) {
            (void)Editor::UndoAppend(document, *it);
        }
        for (auto it = detachReceipts.rbegin(); it != detachReceipts.rend(); ++it) {
            (void)Editor::UndoDetachArgumentCall(document, *it);
        }
        if (failed) {
            project_.AddDiagnostic(tr("错误"), QStringLiteral("Trigger.Edit"),
                QStringLiteral("TRIGGER_PARAMETER_EDIT_FAILED"), failureMessage);
        }
        ShowStatus(failed ? failureMessage : tr("修改没有通过 WTG/WCT 图验证，已经回滚。"), 8000);
        if (newNestedEcaId != nullptr) newNestedEcaId->reset();
        return false;
    }

    if (!detachReceipts.empty() || !callReceipts.empty() || !scalarChanges.empty() || prefix) {
        auto* stack = project_.triggerUndoStack_;
        stack->beginMacro(label);
        if (prefix) stack->push(prefix.release());
        for (auto& receipt : detachReceipts) {
            stack->push(new DetachArgumentCallUndoCommand(&document, std::move(receipt), tr("替换函数")));
        }
        for (auto& receipt : callReceipts) {
            stack->push(new AppendEcaUndoCommand(&document, std::move(receipt), tr("选择函数")));
        }
        if (!scalarChanges.empty()) {
            stack->push(new EditTriggerArgumentsUndoCommand(&document, std::move(scalarChanges), tr("修改参数值")));
        }
        stack->endMacro();
        RefreshEditor(ecaId);
    }
    return true;
}

bool TriggerModule::ApplyValueChoice(StableId ecaId, std::size_t position, const TriggerValueChoice& choice) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || project_.triggerUndoStack_ == nullptr) {
        return false;
    }
    const auto* eca = Editor::FindEca(*document, ecaId);
    if (eca == nullptr || position >= eca->arguments.size() || eca->arguments[position] >= document->argumentNodes.size()) {
        return false;
    }
    const auto rejectReference = [this](const QString& code, const QString& message) {
        project_.AddDiagnostic(tr("错误"), QStringLiteral("Trigger.MapObjects"), code, message);
        ShowStatus(message, 8000);
        return false;
    };
    const bool sealedChoice = !choice.mapObjectSnapshotIdentity.empty();
    if (sealedChoice && (choice.valueKind != 1 || choice.callFunction || !choice.callArguments.empty() || choice.newVariableType)) {
        return rejectReference(QStringLiteral("TRIGGER_MAP_OBJECT_CHOICE_INVALID"),
            tr("预放对象只允许作为只读标量引用，不能同时新建变量或函数。"));
    }
    // 弹层可能在地图发生编辑后才回调。只有重新捕获实际成员与物编来源后才能使用旧选择。
    // 捕获失败不禁止真实全局变量；没有来源资格的外部符号则由同一 resolver 拒绝。
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjects;
    if (choice.valueKind == 1 && !choice.newVariableType) {
        const auto captured = project_.CaptureTriggerMapObjectReferences();
        if (captured.Succeeded() && captured.value) mapObjects = *captured.value;
        if (sealedChoice && (!mapObjects || mapObjects->Identity() != choice.mapObjectSnapshotIdentity)) {
            return rejectReference(QStringLiteral("TRIGGER_MAP_OBJECT_SOURCE_CHANGED"),
                tr("预放对象的地图或分类来源已变化，请重新打开参数选择器。%1")
                    .arg(mapObjects ? QString{} : FirstMessage(captured.diagnostics, tr("当前来源不可用。"))));
        }
        const auto resolved = Trigger::ResolveVariableReference(*document, choice.value, mapObjects.get());
        if (!resolved.Succeeded() || !resolved.value) {
            return rejectReference(QStringLiteral("TRIGGER_PARAMETER_REFERENCE_INVALID"),
                FirstMessage(resolved.diagnostics, tr("变量引用没有可验证的来源。")));
        }
        const bool preplaced = resolved.value->kind == Trigger::VariableReferenceKind::PreplacedObject;
        if (preplaced != sealedChoice) {
            return rejectReference(QStringLiteral("TRIGGER_MAP_OBJECT_SNAPSHOT_REQUIRED"),
                tr("预放对象引用必须使用当前选择器捕获的来源身份。"));
        }
        const Trigger::TriggerTypeSystem types(*project_.uiCatalog_, mapObjects.get());
        const auto* trigger = Editor::FindTrigger(*document, eca->sourceTriggerId);
        const auto locals = trigger ? types.CollectLocals(*document, *trigger) : std::vector<Trigger::LocalVariable>{};
        const auto expected = types.ExpectedType(*document, *eca, position, locals);
        if (!types.Accepts(expected, resolved.value->type)) {
            return rejectReference(QStringLiteral("TRIGGER_PARAMETER_REFERENCE_TYPE"),
                tr("所选引用的类型“%1”不能用于“%2”参数。")
                    .arg(QString::fromStdString(resolved.value->type), QString::fromStdString(expected)));
        }
        const auto* definition = project_.uiCatalog_->Find(eca->type, eca->name.text);
        const bool writable = (eca->name.text == "SetVariable" && position == 0) ||
            (definition && Trigger::TriggerTypeSystem::DeclaredType(*definition, position) == "AnyGlobal");
        auto replacement = document->argumentNodes[eca->arguments[position]];
        replacement.type = 1;
        replacement.value.text = choice.value;
        replacement.callEca.reset();
        replacement.insertCall = 0;
        if (writable) {
            const auto checked = types.ValidateWritableVariable(*document, replacement, resolved.value->type);
            if (!checked.Succeeded()) return rejectReference(QStringLiteral("TRIGGER_PARAMETER_NOT_WRITABLE"),
                FirstMessage(checked.diagnostics, tr("所选引用不能作为赋值目标。")));
        }
        if (preplaced && (replacement.insertIndex != 0 || replacement.indexArgument || resolved.value->isArray)) {
            return rejectReference(QStringLiteral("TRIGGER_MAP_OBJECT_ARRAY_FORBIDDEN"),
                tr("预放单位或物品是只读标量，不能带数组下标。"));
        }
    }
    TriggerArgumentEditRequest request;
    request.argumentId = document->argumentNodes[eca->arguments[position]].id;
    request.valueKind = choice.valueKind;
    request.value = choice.value;
    request.callFunction = choice.callFunction;
    request.callArguments = choice.callArguments;
    request.expectedType = ResolvedArgumentType(*eca, position);
    // “新建并使用”：变量与参数绑定作为同一次修改、一次撤销（原型 R04）。
    std::unique_ptr<QUndoCommand> prefix;
    if (choice.newVariableType) {
        const Status added = Editor::AddVariable(*document, choice.value, *choice.newVariableType);
        if (!added.Succeeded()) {
            ShowStatus(tr("无法新建变量：%1").arg(FirstMessage(added.diagnostics, tr("未知错误"))), 8000);
            return false;
        }
        prefix = std::make_unique<AddVariableUndoCommand>(document, choice.value, *choice.newVariableType, false, tr("新建变量"));
    }
    std::optional<StableId> nested;
    if (!ApplyArgumentEdits(ecaId, {request}, tr("设置参数"), std::move(prefix), &nested)) {
        if (choice.newVariableType) (void)Editor::RemoveVariable(*document, choice.value);
        return false;
    }
    // 新挂接的函数有自己的参数：继续在参数对话框里填写。
    if (nested) {
        const auto* call = Editor::FindEca(*document, *nested);
        if (call != nullptr && !call->arguments.empty() && page_ && page_->isVisible()) {
            EditEcaParameters(*nested);
        }
    }
    return true;
}


bool TriggerModule::EditScalarArgument(StableId ecaId, std::size_t position, const QRect& anchor) {
    auto* document = project_.triggerDocument_.get();
    if (document == nullptr || project_.uiCatalog_ == nullptr || blueprint_ == nullptr) return false;
    const auto* eca = Editor::FindEca(*document, ecaId);
    if (eca == nullptr || position >= eca->arguments.size() || eca->arguments[position] >= document->argumentNodes.size()) return false;
    const auto& argument = document->argumentNodes[eca->arguments[position]];
    if (argument.type != 3) return false;
    const std::string type = ResolvedArgumentType(*eca, position);
    const Trigger::TriggerTypeSystem types(*project_.uiCatalog_);
    const auto kind = types.LiteralKind(type);
    const bool integer = kind == Trigger::TriggerTypeSystem::Literal::Integer;
    const bool real = kind == Trigger::TriggerTypeSystem::Literal::Real;
    const bool text = kind == Trigger::TriggerTypeSystem::Literal::String;
    if (!integer && !real && !text) return false;
    const auto body = text ? CurrentStringBody(project_, argument.value.text) : std::nullopt;
    if (body && (body->contains(QLatin1Char('\n')) || body->contains(QLatin1Char('\r')))) return false;

    auto* editor = new QLineEdit(blueprint_->viewport());
    editor->setObjectName("blueprintScalarEditor");
    const auto displayed = ResolveProjectName(project_, argument.value.text, type);
    editor->setText(body.value_or(QString::fromStdString(argument.value.text)));
    editor->setToolTip(QString::fromStdString(displayed.reason));
    const QPoint origin = blueprint_->viewport()->mapFromGlobal(anchor.topLeft());
    editor->setGeometry(QRect(origin, QSize(160, 24)));
    editor->selectAll();
    editor->setFocus();
    auto submitted = std::make_shared<bool>(false);
    auto submit = [this, editor, submitted, originalInput = editor->text(), ecaId, argumentId = argument.id, type, integer, real]() {
        if (*submitted) return;
        *submitted = true;
        if (editor->text() == originalInput) { editor->deleteLater(); return; }
        if (project_.uiCatalog_ == nullptr) { editor->deleteLater(); return; }
        const QString text = integer || real ? editor->text().trimmed() : editor->text();
        const Trigger::TriggerTypeSystem types(*project_.uiCatalog_);
        const Status checked = types.ValidateLiteral(type, text.toStdString());
        if (!checked.Succeeded()) {
            ShowStatus(FirstMessage(checked.diagnostics, tr("固定值无效，未修改参数。")), 5000);
            editor->setFocus();
            editor->selectAll();
            // Return 同时触发 returnPressed/editingFinished；本轮只报一次，下一轮允许作者改正。
            QTimer::singleShot(0, editor, [submitted] { *submitted = false; });
            return;
        }
        TriggerArgumentEditRequest request;
        request.argumentId = argumentId;
        request.valueKind = 3;
        request.value = text.toStdString();
        editor->deleteLater();
        ApplyArgumentEdits(ecaId, {request}, tr("编辑参数"), nullptr, nullptr);
    };
    connect(editor, &QLineEdit::editingFinished, this, submit);
    connect(editor, &QLineEdit::returnPressed, this, submit);
    editor->show();
    return true;
}

void TriggerModule::OpenValuePanel(StableId ecaId, std::size_t position, const QRect& anchor) {
    if (project_.triggerDocument_ == nullptr || project_.uiCatalog_ == nullptr) {
        return;
    }
    if (valuePanel_) valuePanel_->close();
    std::shared_ptr<const Trigger::MapObjectReferenceSnapshot> mapObjects;
    QString captureFailure;
    if (const auto* eca = Editor::FindEca(*project_.triggerDocument_, ecaId)) {
        const auto* definition = project_.uiCatalog_->Find(eca->type, eca->name.text);
        const bool writable = (eca->name.text == "SetVariable" && position == 0) ||
            (definition && Trigger::TriggerTypeSystem::DeclaredType(*definition, position) == "AnyGlobal");
        const Trigger::TriggerTypeSystem types(*project_.uiCatalog_);
        const auto expected = ResolvedArgumentType(*eca, position);
        // 普通数字、文本及赋值目标不枚举全图实例；预放来源只在相应只读参数中捕获。
        if (!writable && (types.Accepts(expected, "unit") || types.Accepts(expected, "item"))) {
            const auto captured = project_.CaptureTriggerMapObjectReferences();
            if (captured.Succeeded() && captured.value) mapObjects = *captured.value;
            else captureFailure = FirstMessage(captured.diagnostics, tr("当前地图来源不可用。"));
        }
    }
    auto* panel = new TriggerValuePanel(*project_.triggerDocument_, *project_.uiCatalog_, ecaId, position,
        [this](const std::string& type) { return ObjectsFor(type); }, CategoryIcons(), page_, mapObjects, captureFailure);
    if (const auto* eca = Editor::FindEca(*project_.triggerDocument_, ecaId);
        eca && position < eca->arguments.size() && eca->arguments[position] < project_.triggerDocument_->argumentNodes.size()) {
        const auto& argument = project_.triggerDocument_->argumentNodes[eca->arguments[position]];
        if (!argument.callEca) {
            auto shown = ResolveProjectReferencePresentation(project_, argument.value.text, panel->ExpectedType());
            if (showRawValues_ && showRawValues_->isChecked()) { shown.displayName = shown.rawValue; shown.iconPath.clear(); }
            panel->SetReferencePresentation(shown,
                project_.iconProvider_->IconForTexture(QString::fromStdString(shown.iconPath)),
                CurrentStringBody(project_, argument.value.text));
        }
    }
    panel->SetChosen([this, ecaId, position](const TriggerValueChoice& choice) {
        QTimer::singleShot(0, this, [this, ecaId, position, choice]() { ApplyValueChoice(ecaId, position, choice); });
    });
    panel->SetEditCall([this](StableId callId) {
        QTimer::singleShot(0, this, [this, callId]() { EditEcaParameters(callId); });
    });
    valuePanel_ = panel;
    panel->ShowAt(anchor);
}

std::function<QIcon(const QString&)> TriggerModule::CategoryIcons() const {
    return [this](const QString& categoryId) {
        return project_.iconProvider_ != nullptr ? project_.iconProvider_->IconForCategory(categoryId) : QIcon{};
    };
}

std::string TriggerModule::ResolvedArgumentType(const Trigger::EcaNode& eca, std::size_t position) const {
    const auto& document = *project_.triggerDocument_;
    const Trigger::TriggerTypeSystem types(*project_.uiCatalog_);
    const auto* trigger = Editor::FindTrigger(*project_.triggerDocument_, eca.sourceTriggerId);
    const auto locals = trigger == nullptr ? std::vector<Trigger::LocalVariable>{} : types.CollectLocals(document, *trigger);
    return types.ExpectedType(document, eca, position, locals);
}

TriggerValuePanel* TriggerModule::ValuePanel() const {
    return valuePanel_.data();
}

void TriggerModule::EditFirstArgument() {
    const QModelIndex index = ecaView_->currentIndex().siblingAtColumn(0);
    const auto* eca = ecaModel_->EcaAt(index);
    if (eca == nullptr) {
        return;
    }
    QStyleOptionViewItem option;
    option.rect = ecaView_->visualRect(index);
    option.widget = ecaView_;
    option.font = ecaView_->font();
    option.decorationSize = ecaView_->iconSize();
    option.features = QStyleOptionViewItem::HasDisplay | QStyleOptionViewItem::HasDecoration;
    const auto token = sentenceDelegate_->TokenByOrder(option, index, 0);
    if (!token) {
        EditSelectedEca(); // 没有参数：打开对话框（显示语句说明）
        return;
    }
    ecaView_->scrollTo(index);
    OpenValuePanel(eca->id, token->position,
        QRect(ecaView_->viewport()->mapToGlobal(token->rect.topLeft()), token->rect.size()));
}

std::vector<TriggerValueObject> TriggerModule::ObjectsFor(const std::string& uiType) const {
    std::vector<TriggerValueObject> objects;
    for (auto& choice : ProjectObjectChoices(project_, uiType)) objects.push_back({std::move(choice.rawcode), std::move(choice.name)});
    return objects;
}

} // namespace War3Map::Modules
