#pragma once

#include "Modules/EditorModule.h"

#include "War3MapCore/Services/ProjectReferenceIndex.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

namespace War3Map::QtProject { class EditorProject; }

// 物体管理的行数据（对应 Web 原型 ObjectManager.tsx 的各页）。只读投影：工程文档与引用索引
// 都在工程层，这里只把它们整理成表格行与详情所需的引用列表。
namespace War3Map::Modules::Manager {

enum class Page : int {
    Overview,
    Unit,
    Item,
    Destructable,
    Doodad,
    Ability,
    Buff,
    Upgrade,
    Trigger,
    Function,
    Variable,
    Count,
};

struct Row {
    QString key;          // 选中项在本页的稳定键
    QStringList cells;    // 表格列
    QString title;
    QString subtitle;
    QString tooltip;
    // 被引用（函数页为调用位置）；放置在地图上的实例单独列出。
    std::vector<const Services::Usage*> usages;
    std::vector<const Services::Usage*> placed;
    std::string outgoingOwner;   // “使用了”列表的引用者 ID
    std::optional<Shell::NavigationTarget> open;
    bool unused = false;
    QString tag;                 // 过滤标签：custom / modified / standard / off / closed / 函数种类
};

struct Column {
    QString label;
    bool numeric = false;
};

struct Filter {
    QString id;
    QString label;
};

[[nodiscard]] QString PageLabel(Page page);
[[nodiscard]] std::vector<Column> PageColumns(Page page);
[[nodiscard]] std::vector<Filter> PageFilters(Page page);
[[nodiscard]] bool RowPasses(Page page, const Row& row, const QString& filter);

// 生成一页的行。project 只读；索引须来自 project.References()。
[[nodiscard]] std::vector<Row> BuildRows(const QtProject::EditorProject& project, Page page, bool allFunctions, bool rawValues = false);

// 概况页的数量行：标签、值、点击后跳转的页与过滤（可为空）。
struct OverviewLine {
    QString section;
    QString label;
    QString value;
    std::optional<Page> page;
    QString filter;
};
[[nodiscard]] std::vector<OverviewLine> BuildOverview(const QtProject::EditorProject& project);

// 引用的显示文字与跳转目标（触发器语句按需渲染自然语言）。
[[nodiscard]] QString UsageText(const QtProject::EditorProject& project, const Services::Usage& usage, bool rawValues = false);
[[nodiscard]] std::optional<Shell::NavigationTarget> UsageTarget(
    const QtProject::EditorProject& project, const Services::Usage& usage);
[[nodiscard]] QString UsageOwnerLabel(const QtProject::EditorProject& project, const Services::Usage& usage);

// “使用了”：把记号解析为工程中的触发器 / 变量 / 物体；普通字符串不会命中。
struct ResolvedToken {
    QString type;
    QString label;
    Shell::NavigationTarget target;
};
[[nodiscard]] std::optional<ResolvedToken> ResolveToken(const QtProject::EditorProject& project, const std::string& token);

// 触发器调用的函数：函数名与调用次数，按名称排序。
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> FunctionsCalledBy(
    const QtProject::EditorProject& project, const std::string& ownerId);

} // namespace War3Map::Modules::Manager
