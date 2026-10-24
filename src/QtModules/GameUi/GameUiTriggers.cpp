#include "GameUiModule.h"

#include "EditorProject.h"
#include "TriggerCommands.h"
#include "War3MapCore/GameUi/CastTriggers.h"
#include "War3MapCore/GameUi/PartyTriggers.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <cstring>

// 界面描述、读条系统与团队框架 → 自然 T：生成文本由 Core（UiTriggers、CastTriggers、PartyTriggers）给出，这里补上工作区里才有的输入
// （界面目录、皮肤字体、地图界面目录、已有的触发器），经共用的触发器事务写进各自的类别，界面触发器的基础模板放进地图。
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

    UiSkin skin;
    skin.SetGameSkins(definitions_->gameSkins);
    if (!definitions_->mapSkin.empty()) skin.SetMapSkin(definitions_->mapSkin);
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

std::optional<GameUiModule::CastSystemGeneration> GameUiModule::GenerateCastSystem(const CastSystemRequest& request, bool preview,
    QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<CastSystemGeneration> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "YDWEExecuteTriggerMultiple"))
        return fail(tr("触发器函数库里没有 YDWE 的“运行触发器（带参数）”，读条系统用它把参数传给效果触发器"));
    const auto& triggers = project_.triggerDocument_->triggers;
    const auto exists = [&](const std::string& name) {
        return std::any_of(triggers.begin(), triggers.end(), [&](const Trigger::TriggerRecord& row) { return row.name.text == name; });
    };
    CastSystemOptions options;
    options.abilities = request.abilities;
    const bool kept = exists(CastTriggerName(options.name, "登记", "Register"));
    options.includeRegistration = !kept;
    if (!request.bar.isEmpty()) {
        if (!definitions_) return fail(tr("界面定义仍在读取"));
        const UiDescription* description = Description(request.bar);
        if (!description) return fail(tr("地图里没有界面描述 %1").arg(request.bar));
        if (!exists(UiTriggerName(*description, "更新", "Update"))) return fail(tr("先为界面 %1 生成触发器").arg(request.bar));
        options.bar = CastBarBinding{description, request.visible.toStdString(), request.progress.toStdString(),
            request.text.toStdString(), request.remaining.toStdString(), request.channel.toStdString(), request.interrupted.toStdString()};
    }
    auto plan = GameUi::GenerateCastSystem(options);
    if (!plan.Succeeded()) return fail(FirstMessage(plan.diagnostics));

    CastSystemGeneration result;
    result.featureText = Text(plan.value->featureText);
    for (const auto& name : plan.value->triggers) result.triggers.append(Text(name));
    for (const auto& name : plan.value->variables) result.variables.append(Text(name));
    result.registrationKept = kept;
    if (preview) return result;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kCastTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("生成读条系统"));
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    return result;
}

std::optional<GameUiModule::PartySystemGeneration> GameUiModule::GeneratePartySystem(const PartySystemRequest& request, bool preview,
    QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<PartySystemGeneration> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    const bool local = !request.outOfRange.isEmpty() || !request.select.isEmpty();
    if (local && !project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Call, "GetLocalPlayer"))
        return fail(tr("触发器函数库里没有 YDWE 的“本地玩家”，团队框架的过远与点选要用它"));
    if (!request.outOfRange.isEmpty() && !project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "RemoveLocation"))
        return fail(tr("触发器函数库里没有 YDWE 的“清除点”，团队框架算过远要用它"));
    if (!definitions_) return fail(tr("界面定义仍在读取"));
    const UiDescription* description = Description(request.frame);
    if (!description) return fail(tr("地图里没有界面描述 %1").arg(request.frame));
    const auto& triggers = project_.triggerDocument_->triggers;
    if (std::none_of(triggers.begin(), triggers.end(), [&](const Trigger::TriggerRecord& row) {
            return row.name.text == UiTriggerName(*description, "更新", "Update");
        }))
        return fail(tr("先为界面 %1 生成触发器").arg(request.frame));

    PartySystemOptions options;
    options.frame = PartyFrameBinding{description, request.shown.toStdString(), request.health.toStdString(), request.mana.toStdString(),
        request.name.toStdString(), request.dead.toStdString(), request.outOfRange.toStdString(), request.select.toStdString()};
    options.nameSource = request.nameSource;
    options.farRange = request.farRange;
    options.autoHeroes = request.autoHeroes;
    auto plan = GameUi::GeneratePartySystem(options);
    if (!plan.Succeeded()) return fail(FirstMessage(plan.diagnostics));

    PartySystemGeneration result;
    result.featureText = Text(plan.value->featureText);
    for (const auto& name : plan.value->triggers) result.triggers.append(Text(name));
    for (const auto& name : plan.value->variables) result.variables.append(Text(name));
    result.slotCount = plan.value->slotCount;
    result.memberVariable = Text(plan.value->memberVariable);
    result.joinTrigger = Text(plan.value->joinTrigger);
    result.leaveTrigger = Text(plan.value->leaveTrigger);
    if (preview) return result;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kPartyTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("生成团队框架"));
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    return result;
}

} // namespace War3Map::Modules
