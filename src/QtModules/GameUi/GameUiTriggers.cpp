#include "GameUiModule.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "TriggerCommands.h"
#include "War3MapCore/GameUi/CastTriggers.h"
#include "War3MapCore/GameUi/PartyTriggers.h"
#include "War3MapCore/GameUi/TalentTriggers.h"
#include "War3MapCore/GameUi/TargetTriggers.h"
#include "War3MapCore/GameUi/UiSkin.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <cstring>

// 界面描述、读条系统、团队框架与目标框架 → 自然 T：生成文本由 Core（UiTriggers、CastTriggers、PartyTriggers、TargetTriggers）给出，这里补上工作区里才有的输入
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

bool HasTrigger(const std::vector<Trigger::TriggerRecord>& triggers, const std::string& name) {
    return std::any_of(triggers.begin(), triggers.end(), [&](const Trigger::TriggerRecord& row) { return row.name.text == name; });
}

// 登记的增减益与头像的图标取自物编（地图覆盖与原版目录）；取不到时返回原因。
std::optional<QString> ResolveBuffIcons(const QtProject::EditorProject& project, const QStringList& codes, std::vector<UnitBuff>& out) {
    for (const auto& code : codes) {
        const auto icon = ResolveProjectObjectPresentation(project, ObjectData::ObjectKey{ObjectData::ObjectType::Buff, code.toStdString()}).iconPath;
        if (icon.isEmpty()) return GameUiModule::tr("增减益 %1 在物编里没有图标").arg(code);
        out.push_back(UnitBuff{code.toStdString(), icon.toStdString()});
    }
    return std::nullopt;
}

std::optional<QString> ResolvePortraitIcons(const QtProject::EditorProject& project, const QStringList& codes, std::vector<UnitPortrait>& out) {
    for (const auto& code : codes) {
        const auto icon = ResolveProjectObjectPresentation(project, ObjectData::ObjectKey{ObjectData::ObjectType::Unit, code.toStdString()}).iconPath;
        if (icon.isEmpty()) return GameUiModule::tr("单位 %1 在物编里没有图标").arg(code);
        out.push_back(UnitPortrait{code.toStdString(), icon.toStdString()});
    }
    return std::nullopt;
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
    if (!request.select.isEmpty() && !project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Call, "AudastraUIClickUnit"))
        return fail(tr("函数库里的 AudastraUI 是旧版，缺少“点击单位”，团队框架的点选要用它；在函数库里重新安装 AudastraUI"));
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
        request.name.toStdString(), request.dead.toStdString(), request.outOfRange.toStdString(), request.select.toStdString(), {}};
    for (const auto& icon : request.buffIcons) options.frame.buffIcons.push_back(icon.toStdString());
    options.frame.healthNow = request.healthNow.toStdString();
    options.frame.healthMax = request.healthMax.toStdString();
    options.frame.manaNow = request.manaNow.toStdString();
    options.frame.manaMax = request.manaMax.toStdString();
    options.frame.portrait = request.portrait.toStdString();
    options.nameSource = request.nameSource;
    options.farRange = request.farRange;
    options.autoHeroes = request.autoHeroes;
    const bool kept = !request.buffIcons.isEmpty() && HasTrigger(triggers, PartyTriggerName(options.name, "登记", "Register"));
    options.includeRegistration = !kept;
    if (!kept) {
        if (const auto problem = ResolveBuffIcons(project_, request.buffs, options.buffs)) return fail(*problem);
    }
    const bool portraitKept = !request.portrait.isEmpty() && HasTrigger(triggers, PartyTriggerName(options.name, "头像", "Portrait"));
    options.includePortraitRegistration = !portraitKept;
    if (!portraitKept) {
        if (const auto problem = ResolvePortraitIcons(project_, request.portraits, options.portraits)) return fail(*problem);
    }
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
    result.registrationKept = kept;
    result.portraitKept = portraitKept;
    if (preview) return result;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kPartyTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("生成团队框架"));
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    return result;
}

std::optional<GameUiModule::TargetSystemGeneration> GameUiModule::GenerateTargetSystem(const TargetSystemRequest& request, bool preview,
    QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<TargetSystemGeneration> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Call, "GetLocalPlayer"))
        return fail(tr("触发器函数库里没有 YDWE 的“本地玩家”，目标框架的本机界面要用它"));
    if (!request.select.isEmpty() && !project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Call, "AudastraUIClickUnit"))
        return fail(tr("函数库里的 AudastraUI 是旧版，缺少“点击单位”，目标框架的点选要用它；在函数库里重新安装 AudastraUI"));
    if (!definitions_) return fail(tr("界面定义仍在读取"));
    const UiDescription* description = Description(request.frame);
    if (!description) return fail(tr("地图里没有界面描述 %1").arg(request.frame));
    const auto& triggers = project_.triggerDocument_->triggers;
    if (!HasTrigger(triggers, UiTriggerName(*description, "更新", "Update"))) return fail(tr("先为界面 %1 生成触发器").arg(request.frame));

    TargetSystemOptions options;
    options.name = request.systemName.toStdString();
    options.key = request.key.toStdString();
    options.source = request.source;
    options.nameSource = request.nameSource;
    options.players = request.players < 0 ? 0u : static_cast<std::uint32_t>(request.players);
    TargetFrameBinding& b = options.frame;
    b.description = description;
    b.shown = request.shown.toStdString();
    b.health = request.health.toStdString();
    b.mana = request.mana.toStdString();
    b.healthNow = request.healthNow.toStdString();
    b.healthMax = request.healthMax.toStdString();
    b.manaNow = request.manaNow.toStdString();
    b.manaMax = request.manaMax.toStdString();
    b.name = request.name.toStdString();
    b.level = request.level.toStdString();
    b.dead = request.dead.toStdString();
    b.portrait = request.portrait.toStdString();
    b.select = request.select.toStdString();
    for (const auto& icon : request.buffIcons) b.buffIcons.push_back(icon.toStdString());
    const bool kept = !request.buffIcons.isEmpty() && HasTrigger(triggers, TargetTriggerName(options.name, options.key, "登记", "Register"));
    options.includeRegistration = !kept;
    if (!kept) {
        if (const auto problem = ResolveBuffIcons(project_, request.buffs, options.buffs)) return fail(*problem);
    }
    const bool portraitKept = !request.portrait.isEmpty() && HasTrigger(triggers, TargetTriggerName(options.name, options.key, "头像", "Portrait"));
    options.includePortraitRegistration = !portraitKept;
    if (!portraitKept) {
        if (const auto problem = ResolvePortraitIcons(project_, request.portraits, options.portraits)) return fail(*problem);
    }
    auto plan = GameUi::GenerateTargetSystem(options);
    if (!plan.Succeeded()) return fail(FirstMessage(plan.diagnostics));

    TargetSystemGeneration result;
    result.featureText = Text(plan.value->featureText);
    for (const auto& name : plan.value->triggers) result.triggers.append(Text(name));
    for (const auto& name : plan.value->variables) result.variables.append(Text(name));
    result.setTrigger = Text(plan.value->setTrigger);
    result.memberVariable = Text(plan.value->memberVariable);
    result.playerVariable = Text(plan.value->playerVariable);
    result.registrationKept = kept;
    result.portraitKept = portraitKept;
    if (preview) return result;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kTargetTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("生成目标框架"));
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    return result;
}

std::optional<GameUiModule::TalentSystemGeneration> GameUiModule::GenerateTalentSystem(const TalentSystemRequest& request, bool preview,
    QString* error) {
    const auto fail = [error](const QString& message) -> std::optional<TalentSystemGeneration> {
        if (error) *error = message;
        return std::nullopt;
    };
    if (!project_.uiCatalog_ || !project_.triggerDocument_) return fail(tr("当前地图没有可编辑的触发器"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Call, "GetLocalPlayer"))
        return fail(tr("触发器函数库里没有 YDWE 的“本地玩家”，天赋树的本机界面要用它"));
    if (!project_.uiCatalog_->Find(Trigger::UiDefinitionKind::Action, "RemoveLocation"))
        return fail(tr("触发器函数库里没有 YDWE 的“清除点”，天赋树建同步单位要用它"));
    if (!definitions_) return fail(tr("界面定义仍在读取"));
    const UiDescription* description = Description(request.frame);
    if (!description) return fail(tr("地图里没有界面描述 %1").arg(request.frame));
    const auto& triggers = project_.triggerDocument_->triggers;
    const auto exists = [&](const std::string& name) {
        return std::any_of(triggers.begin(), triggers.end(), [&](const Trigger::TriggerRecord& row) { return row.name.text == name; });
    };
    if (!exists(UiTriggerName(*description, "更新", "Update"))) return fail(tr("先为界面 %1 生成触发器").arg(request.frame));
    if (!request.afterPick.isEmpty() && !exists(request.afterPick.toStdString()))
        return fail(tr("地图里没有选定后要运行的触发器 %1").arg(request.afterPick));
    using ObjectData::ObjectKey;
    using ObjectData::ObjectType;
    // 找不到时名字回退成编号本身，按有没有缺失原因判断。
    const auto known = [&](ObjectType type, const QString& code) {
        const auto presentation = ResolveProjectObjectPresentation(project_, ObjectKey{type, code.toStdString()});
        return presentation.issue.isEmpty() && !presentation.name.isEmpty();
    };
    if (!known(ObjectType::Unit, request.syncUnitType)) return fail(tr("同步单位类型 %1 在物编里没有").arg(request.syncUnitType));

    TalentSystemOptions options;
    options.name = request.name.toStdString();
    TalentFrameBinding& b = options.frame;
    b.description = description;
    b.shown = request.shown.toStdString();
    b.entryShown = request.entryShown.toStdString();
    b.entry = request.entry.toStdString();
    b.title = request.title.toStdString();
    b.points = request.points.toStdString();
    b.level = request.level.toStdString();
    b.open = request.open.toStdString();
    b.close = request.close.toStdString();
    b.tipShown = request.tipShown.toStdString();
    b.tipTitle = request.tipTitle.toStdString();
    b.tipText = request.tipText.toStdString();
    for (const auto& column : request.columns) {
        b.columns.push_back(TalentColumnBinding{column.icon.toStdString(), column.name.toStdString(), column.picked.toStdString(),
            column.pick.toStdString(), column.enter.toStdString(), column.leave.toStdString()});
    }
    for (const int level : request.unlockLevels) options.unlockLevels.push_back(static_cast<std::uint32_t>(std::max(level, 0)));
    options.syncUnitType = request.syncUnitType.toStdString();
    options.players = static_cast<std::uint32_t>(std::max(request.players, 0));
    options.afterPick = request.afterPick.toStdString();
    const bool kept = !request.replaceRegistration && exists(TalentTriggerName(options.name, "登记", "Register"));
    options.includeRegistration = !kept;
    if (!kept) {
        for (const auto& hero : request.heroes) {
            if (!known(ObjectType::Unit, hero.unitType)) return fail(tr("英雄类型 %1 在物编里没有").arg(hero.unitType));
            TalentHero row{hero.unitType.toStdString(), hero.label.toStdString(), {}};
            for (const auto& entry : hero.entries) {
                QString icon = entry.icon;
                // 选项的图标为空时取学会技能的图标（地图覆盖与原版目录）。
                if (!entry.learn.isEmpty()) {
                    const auto ability = ResolveProjectObjectPresentation(project_, ObjectKey{ObjectType::Ability, entry.learn.toStdString()});
                    if (!ability.issue.isEmpty() || ability.name.isEmpty()) return fail(tr("技能 %1 在物编里没有").arg(entry.learn));
                    if (icon.isEmpty()) icon = ability.iconPath;
                }
                row.entries.push_back(TalentEntry{static_cast<std::uint32_t>(std::max(entry.tier, 0)),
                    static_cast<std::uint32_t>(std::max(entry.option, 0)), entry.name.toStdString(), icon.toStdString(),
                    entry.disabledIcon.toStdString(), entry.text.toStdString(), entry.learn.toStdString()});
            }
            options.heroes.push_back(std::move(row));
        }
    }
    auto plan = GameUi::GenerateTalentSystem(options);
    if (!plan.Succeeded()) return fail(FirstMessage(plan.diagnostics));

    TalentSystemGeneration result;
    result.featureText = Text(plan.value->featureText);
    for (const auto& name : plan.value->triggers) result.triggers.append(Text(name));
    for (const auto& name : plan.value->variables) result.variables.append(Text(name));
    result.tiers = plan.value->tiers;
    result.options = plan.value->options;
    result.pickedVariable = Text(plan.value->pickedVariable);
    result.bindTrigger = Text(plan.value->bindTrigger);
    result.registrationTrigger = Text(plan.value->registrationTrigger);
    result.registrationKept = kept;
    if (preview) return result;
    std::vector<Trigger::GraphEditOperation> operations;
    operations.emplace_back(Trigger::GraphApplyFeatureText{std::string(kTalentTriggerCategory), plan.value->featureText, {}, std::nullopt});
    const auto applied = QtProject::PublishTriggerGraphOperations(project_, std::move(operations), tr("生成天赋系统"));
    if (!applied.Succeeded()) return fail(FirstMessage(applied.diagnostics));
    return result;
}

} // namespace War3Map::Modules
