#pragma once

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::Text {

// 只处理 ASCII 范围的字节；返回 view 保留输入的生命周期约束。
[[nodiscard]] std::string_view TrimHorizontal(std::string_view text) noexcept;
[[nodiscard]] std::string_view TrimLineWhitespace(std::string_view text) noexcept;
[[nodiscard]] std::string TrimLineWhitespaceCopy(std::string_view text);
[[nodiscard]] std::string_view TrimCWhitespace(std::string_view text) noexcept;
[[nodiscard]] std::string TrimCWhitespaceCopy(std::string_view text);
// YDWE 注入配置历史行为：两端所有 0x00..0x20 字节均视作空白。
[[nodiscard]] std::string TrimAsciiControlSpaceCopy(std::string_view text);
[[nodiscard]] std::string LowerAscii(std::string_view text);
[[nodiscard]] std::string UpperAscii(std::string_view text);
// 保留旧调用点的 C locale 字节大小写语义；先转 unsigned char 再调用 ctype。
[[nodiscard]] std::string LowerCType(std::string_view text);
[[nodiscard]] std::string UpperCType(std::string_view text);

// WE / YDTrigger 对象脚本名规则；触发器、整图生成与地图数据身份检查共用。
[[nodiscard]] std::string ObjectScriptName(std::string_view name);

// 不识别引号；保留空字段（包括末尾字段）。
[[nodiscard]] std::vector<std::string_view> SplitPlain(std::string_view text, char separator);

// WE/游戏 profile 文本（[节] 与 键=值 行）：节名与键转小写、值去两端空白；
// “//” 或 “;” 开头的行是注释，节外的键归入空节名，同节同键后者覆盖。调用方负责去 BOM。
using ProfileSections = std::map<std::string, std::unordered_map<std::string, std::string>>;
[[nodiscard]] ProfileSections ParseProfile(std::string_view text);

} // namespace War3Map::Text
