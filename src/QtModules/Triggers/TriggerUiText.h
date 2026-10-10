#pragma once

#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QString>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// UI 函数库文本的小工具（参数对话框、取值面板共用；仅模块内部使用）。
namespace TriggerUiText {

inline QString Utf8(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

inline std::optional<std::string_view> Field(
    const War3Map::Trigger::UiDefinitionRevision& revision,
    std::string_view name) {
    const auto found = std::find_if(
        revision.fields.begin(), revision.fields.end(),
        [name](const auto& field) { return field.first == name; });
    return found == revision.fields.end()
        ? std::nullopt
        : std::optional<std::string_view>(found->second);
}

// description 中 ${标签} 的标签，按出现顺序。
inline std::vector<std::string> PlaceholderLabels(const War3Map::Trigger::UiDefinition& definition) {
    std::vector<std::string> result;
    const auto description = Field(definition.Active(), "description");
    if (!description.has_value()) {
        return result;
    }
    std::size_t cursor = 0;
    while (cursor < description->size()) {
        const std::size_t opening = description->find("${", cursor);
        if (opening == std::string_view::npos) {
            break;
        }
        const std::size_t closing = description->find('}', opening + 2);
        if (closing == std::string_view::npos) {
            break;
        }
        result.emplace_back(description->substr(opening + 2, closing - opening - 2));
        cursor = closing + 1;
    }
    return result;
}

inline std::string CallReturnType(const War3Map::Trigger::UiDefinition& definition) {
    const auto returns = Field(definition.Active(), "returns");
    return returns.has_value() ? std::string(*returns) : std::string{};
}

// 预设（TriggerParams）：显示名来自 Core 的统一读取（WESTRING_ 键经目录字符串表解析）。
struct Preset {
    std::string name;  // 原始预设名，写入参数值
    QString display;
};

inline std::vector<Preset> PresetsFor(const War3Map::Trigger::UiCatalog& catalog, std::string_view type) {
    std::vector<Preset> presets;
    for (const auto& preset : War3Map::Trigger::PresetsOfType(catalog, type)) {
        presets.push_back({preset.name, Utf8(preset.display)});
    }
    return presets;
}

} // namespace TriggerUiText
