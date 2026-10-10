#pragma once

#include "War3MapCore/Trigger/TriggerDataText.h"

#include <QHash>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

namespace War3Map::Trigger {
class UiCatalog;
struct UiDefinition;
}

// 触发器函数列表的共用呈现：WE 式“分类 - 标题”、分类名与图标、跨字段搜索和排序。
// 函数选择对话框与取值面板共用，不各自拼标题或解析分类。仅模块内部使用。
class TriggerFunctionList {
public:
    // 分类 id → 图标（通常由工程的 IconProvider 提供）；为空时列表不显示图标。
    using CategoryIcon = std::function<QIcon(const QString& categoryId)>;

    explicit TriggerFunctionList(const War3Map::Trigger::UiCatalog& catalog, CategoryIcon icon = {});

    // 列表标题：“分类名 - 标题”；分类标志为 1、未声明或无分类时只有标题。
    [[nodiscard]] QString Title(const War3Map::Trigger::UiDefinition& definition) const;
    [[nodiscard]] QString CategoryId(const War3Map::Trigger::UiDefinition& definition) const;
    // 分类显示名；空 id 为“未分类”。
    [[nodiscard]] QString CategoryName(const QString& categoryId) const;
    [[nodiscard]] QIcon Icon(const QString& categoryId) const;
    [[nodiscard]] QIcon Icon(const War3Map::Trigger::UiDefinition& definition) const;
    // 搜索匹配带前缀的标题、原始函数名、自然语言模板、分类 id / 名称与来源层（不区分大小写）。
    [[nodiscard]] bool Matches(const War3Map::Trigger::UiDefinition& definition, const QString& query) const;
    // 按列表标题排序（与 WE 列表一致，同名再按原始函数名），稳定。
    void Sort(std::vector<const War3Map::Trigger::UiDefinition*>& definitions) const;
    [[nodiscard]] const War3Map::Trigger::TriggerCategoryTable& Categories() const noexcept { return categories_; }

private:
    const War3Map::Trigger::UiCatalog& catalog_;
    War3Map::Trigger::TriggerCategoryTable categories_;
    CategoryIcon icon_;
    mutable QHash<const War3Map::Trigger::UiDefinition*, QString> titles_;
    mutable QHash<QString, QIcon> icons_;
};
