#pragma once

#include "War3MapCore/Trigger/UiCatalog.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// TriggerData 风格文本（define.txt 的 TriggerCategories / TriggerTypes / TriggerParams）的
// 统一读取：CSV 切列、预设、分类与 WE 式“分类名 - 函数名”标题。触发器面板、参数选择、
// 类型系统、MCP 共用此处，不再各自解析。显示文本经 UiCatalog::Localize 解析 WESTRING_* 键。
namespace War3Map::Trigger {

// CSV 值切列：单 / 双引号成对括起一列（引号内可用 \ 转义）；右引号后紧跟非逗号文本时视为
// 列结束（部分函数库把 "SCRIPT"显示名 写在一起，按作者本意拆成两列）；每列去掉首尾空白。
[[nodiscard]] std::vector<std::string> SplitTriggerDataCsv(std::string_view value);

// define 定义的 value 字段切列；没有 value 时为空。
[[nodiscard]] std::vector<std::string> DefineColumns(const UiDefinition& definition);

// 函数定义的字段（title、category、returns 等）；缺失时为空。
[[nodiscard]] std::optional<std::string_view> DefinitionField(const UiDefinition& definition, std::string_view name);

// TriggerParams 预设：名称=版本,类型,脚本值,显示名。
struct TriggerPreset {
    const UiDefinition* definition = nullptr;
    std::string name;    // 写入参数值的预设名
    std::string type;    // 类型列
    std::string script;  // 脚本值列（原样，不做反引号替换）
    std::string display; // 显示名（已本地化；无法解析时依次退回脚本值、预设名）
    bool displayResolved = false; // 显示名来自显示名列（字面值或已解析的键）
};

[[nodiscard]] std::optional<TriggerPreset> FindPreset(const UiCatalog& catalog, std::string_view name);
// type 为空时返回全部预设；按预设名稳定排序。
[[nodiscard]] std::vector<TriggerPreset> PresetsOfType(const UiCatalog& catalog, std::string_view type);

// TriggerCategories：TC_X=显示名,图标[,标志]。标志为 1 时 WE 列表不加分类前缀。
struct TriggerCategory {
    std::string id;
    std::string display;   // 已本地化；无法解析时为去掉 TC_ 的 id
    std::string icon;      // 图标路径原文（可能缺 .blp、相对 YDWE 根目录或含双反斜杠）
    std::string layer;     // 提供该分类的 UI 层
    bool hidePrefix = false;
    bool displayResolved = true;
};

class TriggerCategoryTable {
public:
    TriggerCategoryTable() = default;
    explicit TriggerCategoryTable(const UiCatalog& catalog);

    // 分类 id 不区分大小写（与 WE 读取 INI 键一致）。
    [[nodiscard]] const TriggerCategory* Find(std::string_view id) const;
    // 已声明分类的原始写法（按声明处大小写）；未声明时原样返回。
    [[nodiscard]] std::string CanonicalId(std::string_view id) const;
    // 分类显示名；未声明的分类返回去掉 TC_ 的 id，空 id 返回空串。
    [[nodiscard]] std::string DisplayName(std::string_view id) const;
    // 函数所属分类 id（category 字段，缺失为空）。
    [[nodiscard]] static std::string CategoryOf(const UiDefinition& definition);
    // WE 列表中函数标题前的分类前缀（不含分隔符）；分类标志为 1、未声明或无分类时为空。
    // 显示名是找不到的 WESTRING 键时，前缀退回去掉 TC_ 的 id。
    [[nodiscard]] std::string Prefix(const UiDefinition& definition) const;
    [[nodiscard]] std::span<const TriggerCategory> Categories() const noexcept { return categories_; }

private:
    std::vector<TriggerCategory> categories_;
    std::unordered_map<std::string, std::size_t> index_;
};

// 函数标题（title 字段；缺失时为函数名），已本地化。
[[nodiscard]] std::string DefinitionTitle(const UiCatalog& catalog, const UiDefinition& definition);
// WE 式列表标题：“分类名 - 标题”；没有前缀时就是标题。
[[nodiscard]] std::string PrefixedTitle(const UiCatalog& catalog, const TriggerCategoryTable& categories,
    const UiDefinition& definition);

} // namespace War3Map::Trigger
