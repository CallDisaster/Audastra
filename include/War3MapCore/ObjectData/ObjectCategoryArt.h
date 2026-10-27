#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData {

// 原版编辑器给装饰物/可破坏物定义的类别（UI/WorldEditData.txt 的 [DoodadCategories]、
// [DestructibleCategories]）：类别码 → 显示名（WESTRING 键）与目录里用的类别图标。
struct ObjectCategoryArt {
    ObjectType type = ObjectType::Doodad;
    std::string code;    // 大写类别码，如 D、P、B
    std::string nameKey; // 如 WESTRING_DTYPE_DESTRUCTABLE；未写时为空
    std::string texture; // 如 ReplaceableTextures\WorldEditUI\Doodad-Destructible；未写时为空
};

// 按文件中的顺序返回两段的条目；其他段和格式不符的行忽略。
[[nodiscard]] std::vector<ObjectCategoryArt> ParseObjectCategoryArt(std::string_view worldEditData);

// 物体 category 字段的首字母（不分大小写）对应的类别；没有对应类别时返回空。
[[nodiscard]] const ObjectCategoryArt* FindObjectCategoryArt(
    const std::vector<ObjectCategoryArt>& categories, ObjectType type, std::string_view category);

} // namespace War3Map::ObjectData
