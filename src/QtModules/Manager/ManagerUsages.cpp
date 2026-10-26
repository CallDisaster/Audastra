#include "ManagerRows.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "ProjectQuerySources.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QCoreApplication>

#include <algorithm>
#include <map>

namespace War3Map::Modules::Manager {
namespace {

using ObjectData::ObjectType;
using Services::Usage;
using Services::UsageModule;
using Services::UsagePlace;

QString Tr(const char* text) {
    return QCoreApplication::translate("ObjectManager", text);
}

QString U8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString PlaceLabel(UsagePlace place) {
    switch (place) {
    case UsagePlace::Event: return Tr("事件");
    case UsagePlace::Condition: return Tr("条件");
    case UsagePlace::Action: return Tr("动作");
    case UsagePlace::ObjectParent: return Tr("父对象");
    case UsagePlace::ObjectField: return Tr("字段");
    case UsagePlace::Placement: return Tr("放置");
    case UsagePlace::VariableInitial: return Tr("初始值");
    }
    return {};
}

// 名称取 N1；缺定义时不为不存在的对象生成可跳转结果。
QString ObjectName(const QtProject::EditorProject& project, ObjectType type, const std::string& rawcode) {
    const auto shown = ResolveProjectObjectPresentation(project, {type, rawcode});
    const auto sources = QtProject::CaptureProjectObjectSources(project);
    const auto& source = sources[static_cast<std::size_t>(type)];
    const bool recorded = (source.state == Services::ProjectSearchState::Ready || source.state == Services::ProjectSearchState::Partial)
        && std::any_of(source.records.begin(), source.records.end(), [&](const auto& record) {
            return record.key == ObjectData::ObjectKey{type, rawcode};
        });
    const bool standard = project.standardCatalog_ && project.standardCatalog_->Find(type, rawcode);
    return recorded || standard ? shown.name : QString{};
}

} // namespace

QString UsageOwnerLabel(const QtProject::EditorProject& project, const Usage& usage) {
    if (usage.module == UsageModule::Objects && usage.ownerObject) {
        const QString name = ObjectName(project, usage.ownerObject->type, usage.ownerObject->rawcode);
        const QString rawcode = U8(usage.ownerObject->rawcode);
        return name.isEmpty() ? rawcode : QStringLiteral("%1 (%2)").arg(name, rawcode);
    }
    if (usage.module == UsageModule::Variables) return Tr("变量 %1").arg(U8(usage.owner));
    return U8(usage.owner);
}

QString UsageText(const QtProject::EditorProject& project, const Usage& usage, bool rawValues) {
    QString text;
    switch (usage.module) {
    case UsageModule::Triggers:
        if (usage.ecaIndex && project.triggerDocument_ && project.uiCatalog_) {
            Trigger::PresentationOptions options;
            options.referenceResolver = [&project](std::string_view raw, std::string_view type) {
                return ResolveProjectReferencePresentation(project, raw, type);
            };
            options.showRawValues = rawValues;
            text = PlaceLabel(usage.place) + QStringLiteral(" · ") +
                U8(Trigger::PresentEca(*project.triggerDocument_, *usage.ecaIndex, *project.uiCatalog_, options).text);
        }
        break;
    case UsageModule::Objects:
        text = usage.place == UsagePlace::ObjectParent ? PlaceLabel(usage.place)
            : PlaceLabel(usage.place) + QStringLiteral(" ") + U8(usage.detail);
        break;
    case UsageModule::Variables: {
        std::string type;
        if (project.triggerDocument_) {
            const Trigger::VariableRecord* variable = nullptr;
            for (const auto& entry : project.triggerDocument_->variables) {
                if (entry.name.text != usage.owner) continue;
                if (variable) { variable = nullptr; break; } // 同名定义不猜类型。
                variable = &entry;
            }
            if (variable) type = variable->typeName.text;
        }
        text = PlaceLabel(usage.place) + QStringLiteral(" = ") + U8(rawValues ? usage.detail : ResolveProjectName(project, usage.detail, type).displayName);
        break;
    }
    case UsageModule::World:
        text = U8(usage.detail) + (usage.placementIndex ? QStringLiteral(" #%1").arg(*usage.placementIndex) : QString{});
        break;
    }
    if (usage.disabled) text += Tr("（已禁用，不生成代码）");
    if (usage.possible) text += Tr("（可能引用：目标类型不唯一）");
    return text;
}

std::optional<Shell::NavigationTarget> UsageTarget(const QtProject::EditorProject& project, const Usage& usage) {
    switch (usage.module) {
    case UsageModule::Triggers:
        if (usage.ecaIndex && project.triggerDocument_ && *usage.ecaIndex < project.triggerDocument_->ecaNodes.size()) {
            return Shell::NavigationTarget{"triggers", "eca",
                QString::number(project.triggerDocument_->ecaNodes[*usage.ecaIndex].id), {}};
        }
        return std::nullopt;
    case UsageModule::Objects:
        if (!usage.ownerObject) return std::nullopt;
        return Shell::NavigationTarget{"objects", "object",
            U8(ObjectData::ToString(usage.ownerObject->type)) + ':' + U8(usage.ownerObject->rawcode), {}};
    case UsageModule::Variables:
        return Shell::NavigationTarget{"variables", "variable", U8(usage.owner), {}};
    case UsageModule::World:
        return std::nullopt;   // 世界编辑器尚未接通
    }
    return std::nullopt;
}

std::optional<ResolvedToken> ResolveToken(const QtProject::EditorProject& project, const std::string& token) {
    const auto resolved = ResolveProjectName(project, token);
    if (resolved.state == Services::NameResolutionState::Ambiguous) return std::nullopt;
    if (const auto* document = project.triggerDocument_.get()) {
        if (const auto* trigger = Trigger::TriggerDirectoryEditor::FindTriggerBySymbol(*document, token)) {
            return ResolvedToken{Tr("触发器"), U8(trigger->name.text),
                {"triggers", "trigger", QString::number(trigger->id), {}}};
        }
        for (const auto& variable : document->variables) {
            if (variable.name.text == token || "udg_" + variable.name.text == token) {
                return ResolvedToken{Tr("变量"), U8(variable.name.text), {"variables", "variable", U8(variable.name.text), {}}};
            }
        }
    }
    if (token.size() == 4) {
        static const char* const kTypes[] = {"单位", "技能", "物品", "魔法效果", "科技", "装饰物", "可破坏物"};
        const auto sources = QtProject::CaptureProjectObjectSources(project);
        std::optional<ObjectType> candidate;
        for (int type = 0; type < 7; ++type) {
            const auto objectType = static_cast<ObjectType>(type);
            const auto& source = sources[static_cast<std::size_t>(type)];
            const auto records = std::count_if(source.records.begin(), source.records.end(), [&](const auto& record) {
                return record.key == ObjectData::ObjectKey{objectType, token};
            });
            // 没有参数类型的四字码只能跳到唯一真实定义；未知或部分来源未命中不能证明不存在。
            if (source.state != Services::ProjectSearchState::Ready && source.state != Services::ProjectSearchState::Missing
                && !(source.state == Services::ProjectSearchState::Partial && records == 1)) return std::nullopt;
            if (records > 1) return std::nullopt;
            const bool standard = project.standardCatalog_ && project.standardCatalog_->Find(objectType, token);
            if (records == 0 && !standard) continue;
            if (candidate) return std::nullopt;
            candidate = objectType;
        }
        if (!candidate) return std::nullopt;
        const auto shown = ResolveProjectObjectPresentation(project, {*candidate, token});
        QString label = shown.name == U8(token) ? U8(token) : QStringLiteral("%1 (%2)").arg(shown.name, U8(token));
        if (!shown.issue.isEmpty()) label += QStringLiteral(" · ") + shown.issue;
        return ResolvedToken{Tr(kTypes[static_cast<std::size_t>(*candidate)]), label,
            {"objects", "object", U8(ObjectData::ToString(*candidate)) + ':' + U8(token), {}}};
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, std::size_t>> FunctionsCalledBy(
    const QtProject::EditorProject& project, const std::string& ownerId) {
    std::vector<std::pair<std::string, std::size_t>> result;
    const auto& index = project.References();
    for (const std::string& name : index.CalledFunctions()) {
        std::size_t count = 0;
        for (const Usage* usage : index.CallsOf(name)) count += usage->ownerId == ownerId ? 1 : 0;
        if (count != 0) result.emplace_back(name, count);
    }
    return result;
}

std::vector<OverviewLine> BuildOverview(const QtProject::EditorProject& project) {
    std::vector<OverviewLine> lines;
    const QString summary = Tr("数量");
    static const std::pair<Page, ObjectType> kObjectPages[] = {{Page::Unit, ObjectType::Unit}, {Page::Item, ObjectType::Item},
        {Page::Destructable, ObjectType::Destructable}, {Page::Doodad, ObjectType::Doodad}, {Page::Ability, ObjectType::Ability},
        {Page::Buff, ObjectType::Buff}, {Page::Upgrade, ObjectType::Upgrade}};
    std::size_t unusedObjects = 0;
    std::optional<Page> firstUnusedPage;
    const auto sources = QtProject::CaptureProjectObjectSources(project);
    for (const auto& [page, type] : kObjectPages) {
        std::size_t custom = 0;
        std::size_t modified = 0;
        const auto& source = sources[static_cast<std::size_t>(type)];
        if (source.state == Services::ProjectSearchState::Ready || source.state == Services::ProjectSearchState::Partial)
            for (const auto& record : source.records) (record.parentRawcode ? custom : modified) += 1;
        for (const Row& row : BuildRows(project, page, false)) {
            if (!row.unused) continue;
            ++unusedObjects;
            if (!firstUnusedPage) firstUnusedPage = page;
        }
        QString count = Tr("自定义 %1 · 已修改 %2").arg(custom).arg(modified);
        if (source.state != Services::ProjectSearchState::Ready && source.state != Services::ProjectSearchState::Missing)
            count += Tr("（来源未完整）");
        lines.push_back({summary, PageLabel(page), count, page, {}});
    }
    std::size_t disabled = 0;
    std::size_t closed = 0;
    std::size_t triggers = 0;
    std::size_t unusedVariables = 0;
    if (const auto* document = project.triggerDocument_.get()) {
        triggers = document->triggers.size();
        for (const auto& trigger : document->triggers) {
            disabled += trigger.enabled == 0 ? 1 : 0;
            closed += trigger.enabled != 0 && trigger.initiallyClosed != 0 ? 1 : 0;
        }
        for (const Row& row : BuildRows(project, Page::Variable, false)) unusedVariables += row.unused ? 1 : 0;
        lines.push_back({summary, PageLabel(Page::Trigger), Tr("%1 个 · 初始关闭 %2").arg(triggers).arg(closed), Page::Trigger, {}});
        lines.push_back({summary, PageLabel(Page::Variable), QString::number(document->variables.size()), Page::Variable, {}});
    }
    lines.push_back({summary, PageLabel(Page::Function), Tr("已使用 %1").arg(project.References().CalledFunctions().size()),
        Page::Function, {}});
    const QString cleanup = Tr("静态引用检查");
    lines.push_back({cleanup, Tr("未发现静态引用的变量"), QString::number(unusedVariables),
        unusedVariables ? std::optional<Page>(Page::Variable) : std::nullopt, QStringLiteral("unused")});
    lines.push_back({cleanup, Tr("未发现静态引用的自定义物体"), QString::number(unusedObjects), firstUnusedPage, QStringLiteral("unused")});
    lines.push_back({cleanup, Tr("已禁用的触发器"), QString::number(disabled),
        disabled ? std::optional<Page>(Page::Trigger) : std::nullopt, QStringLiteral("off")});
    return lines;
}

} // namespace War3Map::Modules::Manager
