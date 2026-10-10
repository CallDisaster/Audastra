#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script::detail {

// YDTrigger 局部变量名（ydul_ 前缀后的部分）的转换规则。
[[nodiscard]] std::string ConvertLocalName(std::string_view name);
// 不以数字开头、不以下划线结尾、只含字母数字下划线。
[[nodiscard]] bool IsJassIdentifier(std::string_view value);
// JASS 字符串字面量：转义反斜杠和双引号。
[[nodiscard]] std::string QuoteJassString(std::string_view value);
// TriggerData 定义值（a,b,"c,d",e）按逗号拆分；引号只用于分组，拆分后去掉。
[[nodiscard]] std::vector<std::string> SplitDefineValue(std::string_view value);

} // namespace War3Map::Script::detail
