#pragma once

// SLK 优化共用的文本层：按原字节读取 SYLK 表与 profile TXT，只追加新行/新节，原有内容逐字节保留。
// 解析规则与物编目录（ParseSlkCells 严格模式、ParseProfile）一致，保证读写两侧对同一文件看法相同。

#include "War3MapCore/Types.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData::SlkText {

struct RawSlkTable {
    std::string text;
    std::string newline = "\r\n";
    std::map<std::string, int> columns;              // 小写列名 → X
    std::map<std::string, int> rows;                 // 首列解码后的 ID（区分大小写） → Y
    std::map<int, std::map<int, std::string>> cells; // Y → X → K 后的原始记号（字符串含引号）
    int maxRow = 0;
};

[[nodiscard]] Result<RawSlkTable> ParseRawSlk(std::string text);
// 原始记号解码：带引号的去掉外层引号并把 "" 还原为 "，数字原样返回。
[[nodiscard]] std::string DecodeToken(std::string_view token);
// 字符串单元格记号；含引号、分号或换行时无法无歧义写出，返回空。
[[nodiscard]] std::optional<std::string> StringToken(std::string_view text);
// 在 E 记录前追加整行（每个单元格都写明 X/Y），并更新 B 记录的行数。
[[nodiscard]] Result<std::string> AppendSlkRows(const RawSlkTable& table,
    const std::vector<std::map<int, std::string>>& newRows);

struct ProfileEntry {
    std::string key;   // 原大小写
    std::string value; // 去掉两端空白后的原始值
};
struct ProfileSection {
    std::string id;    // 原大小写
    std::vector<ProfileEntry> entries;
};
struct RawProfileFile {
    std::string text;
    std::string newline = "\r\n";
    std::vector<ProfileSection> sections;
    std::map<std::string, std::vector<std::size_t>> sectionsByLowerId;
};

[[nodiscard]] RawProfileFile ParseRawProfile(std::string text);
[[nodiscard]] std::string AppendProfileSections(const RawProfileFile& file, const std::vector<ProfileSection>& sections);

// profile 逗号列表：引号内的逗号不分隔；解码时去掉每项外层引号。
[[nodiscard]] std::vector<std::string> SplitProfileList(std::string_view value);
[[nodiscard]] std::string DecodeProfileItem(std::string_view item);
// 单项记号：含逗号时加引号；含引号、换行或首尾空白时无法写出，返回空。
[[nodiscard]] std::optional<std::string> ProfileItemToken(std::string_view text);
[[nodiscard]] std::string JoinProfileList(const std::vector<std::string>& items);

} // namespace War3Map::ObjectData::SlkText
