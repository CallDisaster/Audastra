#include "GameUiModule.h"

#include "EditorProject.h"
#include "Modules/ModuleHost.h"
#include "TriggerCommands.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QInputDialog>
#include <QMessageBox>

#include <algorithm>
#include <cstring>

// 界面描述 → 自然 T：生成文本由 Core（UiTriggers）给出，这里补上工作区里才有的输入
// （界面目录、皮肤字体、地图界面目录），经共用的触发器事务写进 AudastraUI 类别，界面触发器的基础模板放进地图。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }

QString FirstMessage(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return Text(diagnostic.message);
    }
    return diagnostics.empty() ? QString() : Text(diagnostics.front().message);
}

// 描述里的模板框架是否用到地图界面目录里的模板（这时生成的触发器要先加载地图的界面目录）。
bool UsesMapTemplate(const std::vector<UiFrame>& frames, const FrameLibrary& library) {
    return std::any_of(frames.begin(), frames.end(), [&](const UiFrame& frame) {
        if (frame.type == UiFrameType::Template) {
            const auto found = library.FindTemplate(frame.templateName);
            if (found && found->fileIndex < library.Files().size() && library.Files()[found->fileIndex]->origin == "map") return true;
        }
        return UsesMapTemplate(frame.children, library);
    });
}

std::shared_ptr<const std::vector<std::byte>> Bytes(const std::string& text) {
    auto bytes = std::make_shared<std::vector<std::byte>>(text.size());
    std::memcpy(bytes->data(), text.data(), text.size());
    return bytes;
}

} // namespace

std::optional<GameUiModule::TriggerGeneration> GameUiModule::GenerateDescriptionTriggers(const QString& id, bool preview, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<TriggerGeneration> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!definitions_ || !definitions_->library) return fail(tr("界面定义仍在读取"));
    const UiDescription* description = Description(id);
    if (!description) return fail(tr("地图里没有界面描述 %1").arg(id));
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "AudastraUISetPoint"))
        return fail(tr("触发器函数库里没有 AudastraUI，先在函数库里安装并启用它"));

    const UiSkin skin = MakeSkin();
    UiTriggerOptions options;
    options.library = definitions_->library.get();
    if (const auto font = skin.Lookup("MasterFont", race_.toStdString(), expansion_)) options.defaultFont = font->value;
    if (UsesMapTemplate(description->frames, *definitions_->library)) options.tocFiles = definitions_->mapTocs;
    auto plan = GenerateUiTriggers(*description, options);
    if (!plan.Succeeded()) return fail(FirstMessage(plan.diagnostics));

    TriggerGeneration result;
    result.featureText = Text(plan.value->featureText);
    for (const auto& name : plan.value->triggers) result.triggers.append(Text(name));
    for (const auto& name : plan.value->variables) result.variables.append(Text(name));
    result.warnings = std::move(plan.diagnostics);
    if (preview) return result;

    // 基础模板：待保存修改里已是同样内容时不再放一次。
    const auto assets = project_.CaptureAssets();
    std::vector<QtProject::EditorProject::ImportPayload> payloads;
    for (const auto& [path, text] : {std::pair{kUiTemplateTocMember, UiTemplateTocText()}, std::pair{kUiTemplateFdfMember, UiTemplateFdfText()}}) {
        const QString archivePath = Text(path);
        const auto bytes = Bytes(text);
        const bool staged = std::any_of(assets.imports.begin(), assets.imports.end(), [&](const QtProject::PendingImport& entry) {
            return QtProject::EditorProject::AssetKey(entry.archivePath) == QtProject::EditorProject::AssetKey(archivePath)
                && entry.ownedPayload && *entry.ownedPayload == *bytes;
        });
        if (!staged) payloads.push_back({archivePath, bytes, false});
    }
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kUiTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations),
        tr("生成界面 %1 的触发器").arg(id), [&]() -> Status {
            if (payloads.empty()) return Status::Ok();
            stagingOwnImport_ = true;  // 描述预览不用这两份模板，不整体重读
            const QString problem = project_.AddImportPayloads(payloads, true, tr("界面触发器的基础模板"));
            stagingOwnImport_ = false;
            if (!problem.isEmpty()) return Status::Failure("GameUi", "AUI_TEMPLATE_IMPORT", problem.toStdString());
            return Status::Ok();
        });
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    for (const auto& payload : payloads) result.templateFiles.append(payload.archivePath);
    return result;
}

// 由界面事件驱动的触发器：event 为写好的事件一行，放进以界面显示名命名的类别；已有同名触发器时不覆盖作者写的动作，直接用它。
std::optional<StableId> GameUiModule::CreateHandlerTrigger(const UiDescription& description, const std::string& role,
    const std::string& key, const std::string& event, const std::string& comment, const QString& undoLabel, QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<StableId> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Event, "AudastraUITriggerRegisterEvent"))
        return fail(tr("触发器函数库里没有 AudastraUI，先在函数库里安装并启用它"));
    const std::string name = UiTriggerName(description, role, key);
    const auto find = [&]() -> std::optional<StableId> {
        const auto& rows = project_.triggerDocument_->triggers;
        const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.name.text == name; });
        return found == rows.end() ? std::nullopt : std::optional<StableId>(found->id);
    };
    if (const auto existing = find()) return existing;
    const std::string text = "触发器 " + name + ":\n    events:\n        " + event + "\n    actions:\n        CommentString(\"" + comment + "\")\n";
    const std::string category = description.label.empty() ? description.id : description.label;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{category, text, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), undoLabel);
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    const auto created = find();
    if (!created) return fail(tr("没有找到新建的触发器 %1").arg(Text(name)));
    return created;
}

std::optional<StableId> GameUiModule::CreateEventTrigger(const QString& id, const QString& eventId, QString* error) {
    const UiDescription* description = Description(id);
    const auto event = description ? std::find_if(description->events.begin(), description->events.end(),
        [&](const UiEvent& row) { return Text(row.id) == eventId; }) : std::vector<UiEvent>::const_iterator{};
    if (!description || event == description->events.end()) {
        if (error) *error = description ? tr("界面 %1 没有事件 %2").arg(id, eventId) : tr("地图里没有界面描述 %1").arg(id);
        return std::nullopt;
    }
    const std::string kind = event->kind == UiEventKind::Click ? "点击" : event->kind == UiEventKind::Enter ? "鼠标进入" : "鼠标离开";
    return CreateHandlerTrigger(*description, "事件 " + event->id, "On_" + event->id,
        "AudastraUITriggerRegisterEvent(\"" + UiEventKey(description->id, event->id) + "\")",
        "界面 " + description->id + " 的事件 " + event->id + "：" + event->frame + " " + kind, tr("新建 %1 的响应触发器").arg(eventId), error);
}

std::optional<StableId> GameUiModule::CreateSimulationTrigger(const QString& id, const QString& timerId, QString* error) {
    const UiDescription* description = Description(id);
    const auto& timers = description ? description->simulation.timers : std::vector<UiSimulationTimer>{};
    const auto timer = std::find_if(timers.begin(), timers.end(), [&](const UiSimulationTimer& row) { return Text(row.id) == timerId; });
    if (!description || timer == timers.end()) {
        if (error) *error = description ? tr("界面 %1 没有模拟计时器 %2").arg(id, timerId) : tr("地图里没有界面描述 %1").arg(id);
        return std::nullopt;
    }
    return CreateHandlerTrigger(*description, "模拟 " + timer->id, "Sim_" + timer->id,
        "AudastraUIRegisterSimulationTimer(\"" + description->id + "\", \"" + timer->id + "\")",
        "界面 " + description->id + " 的模拟计时器 " + timer->id + " 到期：只在编辑器模拟里运行", tr("新建 %1 的模拟触发器").arg(timerId), error);
}

GameUiModule::RuntimeRequirement GameUiModule::QueryRuntimeRequirement() const {
    RuntimeRequirement result;
    const auto state = project_.QueryMapRuntimeState();
    if (!state.Succeeded()) {
        result.problem = FirstMessage(state.diagnostics);
        return result;
    }
    result.approved = state.value->approved;
    result.settings = state.value->settings;
    result.packages = state.value->packages;
    return result;
}

void GameUiModule::OfferMapRuntime() {
    const auto generation = project_.editRevision_.Generation();
    if (runtimeOfferDeclined_ == generation) return;
    const auto requirement = QueryRuntimeRequirement();
    if (requirement.approved) return;
    const auto report = [this](const char* code, const QString& message) {
        authoringDiagnostics_.push_back(Diagnostic{DiagnosticSeverity::Warning, "GameUi", code, message.toStdString(), std::nullopt});
        UpdateProblems();
    };
    if (!requirement.problem.isEmpty()) return report("AUI_RUNTIME_STATE", requirement.problem);
    std::vector<const Services::MapRuntimePackageInfo*> usable;
    for (const auto& package : requirement.packages) {
        if (package.available) usable.push_back(&package);
    }
    if (usable.empty()) {
        return report("AUI_RUNTIME_MISSING", tr("界面触发器要用 Audastra 运行库，本机没有安装可用的运行库包，测试运行和保存会拒绝"));
    }
    const auto label = [](const Services::MapRuntimePackageInfo& package) { return Text(package.id) + QLatin1Char(' ') + Text(package.version); };
    // 地图里已锁定的包优先；有多个可选时让作者挑。
    const auto locked = std::find_if(usable.begin(), usable.end(),
        [&](const auto* package) { return package->packageSha256 == requirement.settings.packageSha256; });
    const Services::MapRuntimePackageInfo* chosen = locked != usable.end() ? *locked : usable.front();
    if (locked == usable.end() && usable.size() > 1) {
        QStringList items;
        for (const auto* package : usable) items.append(label(*package));
        bool picked = false;
        const auto item = QInputDialog::getItem(page_, tr("启用运行库"), tr("运行库包"), items, 0, false, &picked);
        if (!picked) {
            runtimeOfferDeclined_ = generation;
            return;
        }
        chosen = usable[static_cast<std::size_t>(std::max<qsizetype>(0, items.indexOf(item)))];
    }
    QString text = tr("界面触发器要用 Audastra 运行库：地图会带上原生代码（DLL），只支持经典版 1.27a。\n\n包：%1\nDLL：%2")
                       .arg(label(*chosen), Text(chosen->payloadSha256).left(16));
    if (chosen->internalOnly) text += tr("\n\n这个包只能用于测试运行和内部验收副本，正常保存会拒绝。");
    text += tr("\n\n为本图启用？");
    if (QMessageBox::question(page_, tr("启用运行库"), text) != QMessageBox::Yes) {
        runtimeOfferDeclined_ = generation;
        return;
    }
    const auto mode = requirement.settings.mode == Services::MapRuntimeMode::Disabled ? Services::MapRuntimeMode::Automatic
                                                                                      : requirement.settings.mode;
    const auto applied = QtProject::PublishMapRuntimeSettings(project_, {mode, chosen->packageSha256}, tr("启用地图运行库"));
    if (!applied.Succeeded()) {
        return report("AUI_RUNTIME_ENABLE", applied.diagnostics.empty() ? tr("启用运行库失败") : Text(applied.diagnostics.front().message));
    }
    if (host_) host_->ShowStatusMessage(tr("已为本图启用运行库 %1").arg(label(*chosen)), 8000);
}

} // namespace War3Map::Modules
