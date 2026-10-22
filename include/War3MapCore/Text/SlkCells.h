#pragma once

#include "War3MapCore/Types.h"

#include <map>
#include <string>
#include <string_view>

namespace War3Map::Text {

using SlkCells = std::map<int, std::map<int, std::string>>;

enum class SlkCellMode {
    // 物编目录：验证 ID、坐标上限、首列标题和结束记录；处理双引号转义。
    CatalogStrict,
    // 历史脚本标签：保留 SoundQueries 的宽松读取、atoi 坐标与原文引号行为。
    SoundQueriesLegacy,
    // 保存资格读取：在CatalogStrict基础上拒绝重复坐标及同记录重复X/Y/K。
    // 行ID、列名和业务类型的唯一性仍由使用方检查，不改变普通目录读取。
    CatalogUnique,
};

// 只解析 SYLK C 单元格的 X/Y/K；表的业务列、声音路径和对象 rawcode 由调用者解释。
[[nodiscard]] Result<SlkCells> ParseSlkCells(std::string_view text, SlkCellMode mode);

} // namespace War3Map::Text
