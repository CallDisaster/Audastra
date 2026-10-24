#pragma once

#include "War3MapCore/GameUi/UiDescription.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// 生成器（读条系统、团队框架）绑定界面描述的数值、状态与事件时共用的查找与检查。
namespace War3Map::GameUi::detail {

[[nodiscard]] inline const UiValue* FindValue(const UiDescription& d, std::string_view id) {
    const auto found = std::find_if(d.values.begin(), d.values.end(), [&](const UiValue& v) { return v.id == id; });
    return found == d.values.end() ? nullptr : &*found;
}

[[nodiscard]] inline const UiState* FindState(const UiDescription& d, std::string_view id) {
    const auto found = std::find_if(d.states.begin(), d.states.end(), [&](const UiState& s) { return s.id == id; });
    return found == d.states.end() ? nullptr : &*found;
}

[[nodiscard]] inline const UiEvent* FindEvent(const UiDescription& d, std::string_view id) {
    const auto found = std::find_if(d.events.begin(), d.events.end(), [&](const UiEvent& e) { return e.id == id; });
    return found == d.events.end() ? nullptr : &*found;
}

// 绑定的数值须存在、类型一致、格数一致（size 为 0 是单值）。不通过时返回原因；what 为类型的中文名。
[[nodiscard]] inline std::optional<std::string> CheckValueBinding(const UiDescription& d, std::string_view id, UiValueType type,
    std::uint32_t size, std::string_view what) {
    const UiValue* value = FindValue(d, id);
    if (!value) return "界面 " + d.id + " 没有数值 " + std::string(id);
    if (value->type == type && value->size == size) return std::nullopt;
    return "界面 " + d.id + " 的数值 " + std::string(id) + " 应为" + std::string(what)
        + (size == 0 ? std::string("单值") : "数组（" + std::to_string(size) + " 格）");
}

} // namespace War3Map::GameUi::detail
