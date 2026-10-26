#include "ManagerRows.h"

#include "EditorProject.h"
#include "ObjectNames.h"
#include "ProjectQuerySources.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QCoreApplication>

#include <algorithm>
#include <set>

namespace War3Map::Modules::Manager {
namespace {

using ObjectData::ObjectType;
using Services::Usage;
using Services::UsageModule;

QString Tr(const char* text) {
    return QCoreApplication::translate("ObjectManager", text);
}

QString U8(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

std::optional<ObjectType> TypeOf(Page page) {
    switch (page) {
    case Page::Unit: return ObjectType::Unit;
    case Page::Item: return ObjectType::Item;
    case Page::Destructable: return ObjectType::Destructable;
    case Page::Doodad: return ObjectType::Doodad;
    case Page::Ability: return ObjectType::Ability;
    case Page::Buff: return ObjectType::Buff;
    case Page::Upgrade: return ObjectType::Upgrade;
    default: return std::nullopt;
    }
}

bool Placeable(Page page) {
    return page == Page::Unit || page == Page::Item || page == Page::Destructable || page == Page::Doodad;
}

void Split(std::vector<const Usage*> all, Row& row) {
    for (const Usage* usage : all) {
        (usage->module == UsageModule::World ? row.placed : row.usages).push_back(usage);
    }
}

std::vector<Row> ObjectRows(const QtProject::EditorProject& project, ObjectType type, Page page, bool rawValues) {
    std::vector<Row> rows;
    const auto& index = project.References();
    const auto sources = QtProject::CaptureProjectObjectSources(project);
    const auto& source = sources[static_cast<std::size_t>(type)];
    const auto* catalog = project.standardCatalog_.get();
    std::set<std::string> own;
    const QString kind = U8(ObjectData::ToString(type));
    const auto finish = [&](Row row, const std::string& rawcode) {
        row.cells = {row.title, U8(rawcode), row.subtitle};
        if (Placeable(page)) row.cells << QString::number(row.placed.size());
        row.cells << QString::number(row.usages.size());
        row.open = Shell::NavigationTarget{"objects", "object", kind + ':' + U8(rawcode), {}};
        rows.push_back(std::move(row));
    };
    if (source.state == Services::ProjectSearchState::Ready || source.state == Services::ProjectSearchState::Partial) {
        for (const auto& record : source.records) {
            own.insert(record.key.rawcode);
            Row row;
            row.key = U8(record.key.rawcode);
            const bool custom = record.parentRawcode.has_value();
            row.tag = custom ? QStringLiteral("custom") : QStringLiteral("modified");
            row.subtitle = custom ? Tr("自定义") : Tr("已修改");
            const auto shown = ResolveProjectObjectPresentation(project, record.key);
            row.title = rawValues ? U8(record.key.rawcode) : shown.name;
            row.tooltip = U8(record.key.rawcode) + (shown.nameReference.isEmpty() ? QString{} : QLatin1Char('\n') + shown.nameReference)
                + (shown.issue.isEmpty() ? QString{} : QLatin1Char('\n') + shown.issue);
            if (source.state != Services::ProjectSearchState::Ready) {
                row.subtitle += Tr("（来源未完整）");
                row.tooltip += Tr("\n物编来源未完整读取，显示与引用统计仅用于排查。");
            }
            const std::vector<std::string> tokens{record.key.rawcode};
            row.outgoingOwner = Services::ObjectOwnerId(record.key);
            Split(index.UsagesOf(tokens, row.outgoingOwner), row);
            row.unused = source.state == Services::ProjectSearchState::Ready && custom && row.usages.empty() && row.placed.empty();
            finish(std::move(row), record.key.rawcode);
        }
    } else if (source.state != Services::ProjectSearchState::Missing) {
        Row row;
        row.key = QStringLiteral("@source-unavailable");
        row.title = Tr("物编来源未完整加载");
        row.subtitle = Tr("无法确认地图覆盖");
        row.tooltip = Tr("当前类别的地图物编未完整加载，不能把缺失记录解释成未修改或未使用。");
        row.cells = {row.title, QString{}, row.subtitle};
        if (Placeable(page)) row.cells << QString{};
        row.cells << QString{};
        rows.push_back(std::move(row));
    }
    if (catalog != nullptr) {
        // 地图用到但没有修改的原版物体。
        for (const auto& entry : catalog->Entries()) {
            if (entry.key.type != type || own.contains(entry.key.rawcode)) continue;
            const std::vector<std::string> tokens{entry.key.rawcode};
            auto all = index.UsagesOf(tokens);
            if (all.empty()) continue;
            Row row;
            row.key = U8(entry.key.rawcode);
            row.tag = QStringLiteral("standard");
            row.subtitle = Tr("原版");
            row.title = rawValues ? U8(entry.key.rawcode) : ResolveProjectObjectPresentation(project, entry.key).name;
            row.tooltip = U8(entry.key.rawcode);
            Split(std::move(all), row);
            finish(std::move(row), entry.key.rawcode);
        }
    }
    return rows;
}

// 与共用触发器编译器相同的脚本符号（按名字存储字节换算），以及旧版本编辑器写过的按 UTF-8 文字换算、
// 原名两种形式，都算引用。
std::vector<std::string> TriggerTokens(const Trigger::RawText& name) {
    std::vector<std::string> tokens{Trigger::TriggerDirectoryEditor::TriggerSymbol(name)};
    for (auto legacy : {"gg_trg_" + Trigger::TriggerDirectoryEditor::ScriptName(name.text), "gg_trg_" + name.text}) {
        if (std::find(tokens.begin(), tokens.end(), legacy) == tokens.end()) tokens.push_back(std::move(legacy));
    }
    return tokens;
}

std::vector<Row> TriggerRows(const QtProject::EditorProject& project) {
    std::vector<Row> rows;
    const auto* document = project.triggerDocument_.get();
    if (document == nullptr) return rows;
    const auto& index = project.References();
    for (const auto& trigger : document->triggers) {
        Row row;
        row.key = QString::number(trigger.id);
        row.title = U8(trigger.name.text);
        QString category;
        for (const auto& record : document->categories) {
            if (record.categoryId == trigger.categoryId) category = U8(record.name.text);
        }
        int events = 0;
        int actions = 0;
        for (const std::uint32_t root : trigger.ecas) {
            if (root >= document->ecaNodes.size()) continue;
            const auto type = document->ecaNodes[root].type;
            events += type == 0 ? 1 : 0;
            actions += type == 2 ? 1 : 0;
        }
        if (trigger.enabled == 0) {
            row.subtitle = Tr("禁用");
            row.tag = QStringLiteral("off");
        } else if (trigger.customTextFlag != 0) {
            row.subtitle = Tr("自定义脚本");
        } else if (trigger.initiallyClosed != 0) {
            row.subtitle = Tr("初始关闭");
            row.tag = QStringLiteral("closed");
        } else {
            row.subtitle = Tr("启用");
        }
        const auto tokens = TriggerTokens(trigger.name);
        row.outgoingOwner = Services::TriggerOwnerId(trigger.id);
        row.usages = index.UsagesOf(tokens, row.outgoingOwner);
        row.cells = {row.title, category, row.subtitle, QString::number(events), QString::number(actions),
            QString::number(row.usages.size())};
        row.open = Shell::NavigationTarget{"triggers", "trigger", row.key, {}};
        rows.push_back(std::move(row));
    }
    return rows;
}

QString KindLabel(Trigger::UiDefinitionKind kind) {
    switch (kind) {
    case Trigger::UiDefinitionKind::Event: return Tr("事件");
    case Trigger::UiDefinitionKind::Condition: return Tr("条件");
    case Trigger::UiDefinitionKind::Action: return Tr("动作");
    case Trigger::UiDefinitionKind::Call: return Tr("取值");
    default: return Tr("其他");
    }
}

QString Field(const Trigger::UiDefinition& definition, std::string_view name) {
    for (const auto& [key, value] : definition.Active().fields) {
        if (key == name) {
            QString text = U8(value);
            if (text.size() >= 2 && text.startsWith('"') && text.endsWith('"')) text = text.mid(1, text.size() - 2);
            return text;
        }
    }
    return {};
}

std::vector<Row> FunctionRows(const QtProject::EditorProject& project, bool all) {
    std::vector<Row> rows;
    const auto& index = project.References();
    const auto* catalog = project.uiCatalog_.get();
    std::set<std::string> seen;
    const auto add = [&](const std::string& name, const Trigger::UiDefinition* definition) {
        if (!seen.insert(name).second) return;
        Row row;
        row.key = U8(name);
        row.usages = index.CallsOf(name);
        std::set<std::string> triggers;
        for (const Usage* usage : row.usages) triggers.insert(usage->ownerId);
        const QString title = definition != nullptr ? Field(*definition, "title") : QString{};
        row.title = title.isEmpty() ? row.key : title;
        row.subtitle = definition != nullptr ? KindLabel(definition->kind) : Tr("缺失定义");
        row.tag = row.subtitle;
        const QString category = definition != nullptr ? Field(*definition, "category") : QString{};
        row.cells = {row.title, category, row.subtitle, QString::number(row.usages.size()), QString::number(triggers.size())};
        rows.push_back(std::move(row));
    };
    const auto find = [catalog](const std::string& name) -> const Trigger::UiDefinition* {
        if (catalog == nullptr) return nullptr;
        for (const auto kind : {Trigger::UiDefinitionKind::Event, Trigger::UiDefinitionKind::Condition,
                 Trigger::UiDefinitionKind::Action, Trigger::UiDefinitionKind::Call}) {
            if (const auto* definition = catalog->Find(kind, name)) return definition;
        }
        return nullptr;
    };
    for (const std::string& name : index.CalledFunctions()) add(name, find(name));
    if (all && catalog != nullptr) {
        for (const auto kind : {Trigger::UiDefinitionKind::Event, Trigger::UiDefinitionKind::Condition,
                 Trigger::UiDefinitionKind::Action, Trigger::UiDefinitionKind::Call}) {
            for (const auto* definition : catalog->Definitions(kind)) add(definition->name, definition);
        }
    }
    return rows;
}

std::vector<Row> VariableRows(const QtProject::EditorProject& project, bool rawValues) {
    std::vector<Row> rows;
    const auto* document = project.triggerDocument_.get();
    if (document == nullptr) return rows;
    const auto& index = project.References();
    for (const auto& variable : document->variables) {
        Row row;
        row.key = U8(variable.name.text);
        row.title = row.key;
        const std::vector<std::string> tokens{variable.name.text, "udg_" + variable.name.text};
        row.outgoingOwner = "variable:" + variable.name.text;
        row.usages = index.UsagesOf(tokens, row.outgoingOwner);
        row.unused = row.usages.empty();
        QString type = U8(variable.typeName.text);
        if (variable.isArray != 0) type += QStringLiteral("[%1]").arg(variable.size);
        row.subtitle = type;
        const auto initial = ResolveProjectName(project, variable.value.text, variable.typeName.text);
        row.tooltip = U8(variable.value.text) + (initial.reason.empty() ? QString{} : QLatin1Char('\n') + U8(initial.reason));
        row.cells = {row.title, type, variable.hasDefault != 0 ? U8(rawValues ? variable.value.text : initial.displayName) : QString{},
            QString::number(row.usages.size())};
        row.open = Shell::NavigationTarget{"variables", "variable", row.key, {}};
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

QString PageLabel(Page page) {
    static const char* const kLabels[] = {"地图概况", "单位", "物品", "可破坏物", "装饰物", "技能", "魔法效果", "科技",
        "触发器", "函数", "变量"};
    return Tr(kLabels[static_cast<int>(page)]);
}

std::vector<Column> PageColumns(Page page) {
    switch (page) {
    case Page::Trigger:
        return {{Tr("名称")}, {Tr("分类")}, {Tr("状态")}, {Tr("事件"), true}, {Tr("动作"), true}, {Tr("被引用"), true}};
    case Page::Function:
        return {{Tr("函数")}, {Tr("类别")}, {Tr("种类")}, {Tr("调用"), true}, {Tr("触发器"), true}};
    case Page::Variable:
        return {{Tr("名称")}, {Tr("类型")}, {Tr("初始值")}, {Tr("引用"), true}};
    case Page::Overview:
        return {};
    default: {
        std::vector<Column> columns{{Tr("名称")}, {Tr("ID")}, {Tr("状态")}};
        if (Placeable(page)) columns.push_back({Tr("放置"), true});
        columns.push_back({Tr("引用"), true});
        return columns;
    }
    }
}

std::vector<Filter> PageFilters(Page page) {
    switch (page) {
    case Page::Overview: return {};
    case Page::Trigger:
        return {{"all", Tr("全部")}, {"off", Tr("已禁用")}, {"closed", Tr("初始关闭")}, {"referenced", Tr("被引用")}};
    case Page::Function:
        return {{"used", Tr("已使用")}, {"all", Tr("全部函数")}};
    case Page::Variable:
        return {{"all", Tr("全部")}, {"unused", Tr("未发现静态引用")}};
    default:
        return {{"all", Tr("全部")}, {"custom", Tr("自定义")}, {"modified", Tr("已修改")},
            {"standard", Tr("引用的原版")}, {"unused", Tr("未发现静态引用")}};
    }
}

bool RowPasses(Page page, const Row& row, const QString& filter) {
    if (filter.isEmpty() || filter == QLatin1String("all")) return true;
    if (page == Page::Function) return filter != QLatin1String("used") || !row.usages.empty();
    if (filter == QLatin1String("unused")) return row.unused;
    if (filter == QLatin1String("referenced")) return !row.usages.empty();
    return row.tag == filter;
}

std::vector<Row> BuildRows(const QtProject::EditorProject& project, Page page, bool allFunctions, bool rawValues) {
    if (const auto type = TypeOf(page)) return ObjectRows(project, *type, page, rawValues);
    switch (page) {
    case Page::Trigger: return TriggerRows(project);
    case Page::Function: return FunctionRows(project, allFunctions);
    case Page::Variable: return VariableRows(project, rawValues);
    default: return {};
    }
}

} // namespace War3Map::Modules::Manager
