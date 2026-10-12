#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData::SlkText {
// 原版 Profile 的逗号列表：带引号的分量保留逗号，分隔/解码与 SLK 优化保存链共用。
[[nodiscard]] std::vector<std::string> SplitProfileList(std::string_view value);
[[nodiscard]] std::string DecodeProfileItem(std::string_view item);
[[nodiscard]] std::optional<std::string> ProfileItemToken(std::string_view text);
[[nodiscard]] std::string JoinProfileList(const std::vector<std::string>& items);
} // namespace War3Map::ObjectData::SlkText
