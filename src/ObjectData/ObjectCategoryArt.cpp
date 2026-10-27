#include "War3MapCore/ObjectData/ObjectCategoryArt.h"

#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>

namespace War3Map::ObjectData {

std::vector<ObjectCategoryArt> ParseObjectCategoryArt(std::string_view text) {
    std::vector<ObjectCategoryArt> categories;
    ObjectType section = ObjectType::Doodad;
    bool inside = false;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = Text::TrimCWhitespace(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.starts_with("//") || line.starts_with(";")) continue;
        if (line.front() == '[') {
            const auto name = Text::LowerAscii(line);
            inside = name == "[doodadcategories]" || name == "[destructiblecategories]";
            section = name == "[destructiblecategories]" ? ObjectType::Destructable : ObjectType::Doodad;
            continue;
        }
        const auto equals = line.find('=');
        if (!inside || equals == std::string_view::npos) continue;
        const auto code = Text::TrimCWhitespace(line.substr(0, equals));
        if (code.empty()) continue;
        const auto value = line.substr(equals + 1);
        const auto comma = value.find(',');
        ObjectCategoryArt category;
        category.type = section;
        category.code = Text::UpperAscii(code);
        category.nameKey = Text::TrimCWhitespaceCopy(value.substr(0, comma));
        if (comma != std::string_view::npos) category.texture = Text::TrimCWhitespaceCopy(value.substr(comma + 1));
        // 同一段里重复的类别码以后写的为准，与原版读配置的覆盖方式一致。
        const auto existing = std::find_if(categories.begin(), categories.end(), [&](const auto& item) {
            return item.type == category.type && item.code == category.code;
        });
        if (existing != categories.end()) *existing = std::move(category);
        else categories.push_back(std::move(category));
    }
    return categories;
}

const ObjectCategoryArt* FindObjectCategoryArt(
    const std::vector<ObjectCategoryArt>& categories, ObjectType type, std::string_view category) {
    const auto value = Text::TrimCWhitespace(category);
    if (value.empty()) return nullptr;
    const auto code = Text::UpperAscii(value.substr(0, 1));
    const auto found = std::find_if(categories.begin(), categories.end(), [&](const auto& item) {
        return item.type == type && item.code == code;
    });
    return found == categories.end() ? nullptr : &*found;
}

} // namespace War3Map::ObjectData
