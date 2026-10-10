#include "SlkTableText.h"

#include "War3MapCore/Text/TextUtil.h"

#include <charconv>

namespace War3Map::ObjectData::SlkText {
namespace {

using War3Map::Text::LowerAscii;
using War3Map::Text::TrimCWhitespace;

std::string DetectNewline(std::string_view text) {
    return text.find("\r\n") != std::string_view::npos || text.find('\n') == std::string_view::npos ? "\r\n" : "\n";
}

std::string_view WithoutBom(std::string_view text) {
    return text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text;
}

// 与 ParseSlkCells 严格模式相同：引号内分号不分隔，"" 为转义引号。
std::vector<std::string_view> SplitRecord(std::string_view text) {
    std::vector<std::string_view> fields;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '"') {
            if (quoted && at + 1 < text.size() && text[at + 1] == '"') ++at;
            else quoted = !quoted;
        } else if (!quoted && text[at] == ';') {
            fields.push_back(text.substr(start, at - start));
            start = at + 1;
        }
    }
    fields.push_back(text.substr(start));
    return fields;
}

Status Fail(std::string code, std::string message) { return Status::Failure("Object.Slk", std::move(code), std::move(message)); }

} // namespace

std::string DecodeToken(std::string_view token) {
    token = TrimCWhitespace(token);
    if (token.size() < 2 || token.front() != '"' || token.back() != '"') return std::string(token);
    token.remove_prefix(1);
    token.remove_suffix(1);
    std::string decoded;
    decoded.reserve(token.size());
    for (std::size_t at = 0; at < token.size(); ++at) {
        decoded.push_back(token[at]);
        if (token[at] == '"' && at + 1 < token.size() && token[at + 1] == '"') ++at;
    }
    return decoded;
}

std::optional<std::string> StringToken(std::string_view text) {
    if (text.find_first_of("\";\r\n") != std::string_view::npos) return std::nullopt;
    return "\"" + std::string(text) + "\"";
}

Result<RawSlkTable> ParseRawSlk(std::string text) {
    RawSlkTable table;
    const std::string_view view = WithoutBom(text);
    if (!view.starts_with("ID;")) return Result<RawSlkTable>::Failure("Object.Slk", "slk-header", "SLK 文件头无效。");
    table.newline = DetectNewline(view);
    int x = 1, y = 1;
    bool ended = false;
    for (std::size_t start = 0; start < view.size();) {
        const auto end = view.find('\n', start);
        auto line = TrimCWhitespace(view.substr(start, end == std::string_view::npos ? view.size() - start : end - start));
        start = end == std::string_view::npos ? view.size() : end + 1;
        if (line == "E") { ended = true; break; }
        if (!line.starts_with("C;")) continue;
        std::optional<std::string_view> value;
        for (const auto field : SplitRecord(line.substr(2))) {
            if (field.empty()) continue;
            if (field.front() == 'X' || field.front() == 'Y') {
                int coordinate = 0;
                const auto parsed = std::from_chars(field.data() + 1, field.data() + field.size(), coordinate);
                if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || coordinate < 1 || coordinate > 100000)
                    return Result<RawSlkTable>::Failure("Object.Slk", "slk-coordinate", "SLK 单元格坐标无效或超出读取范围。");
                (field.front() == 'X' ? x : y) = coordinate;
            } else if (field.front() == 'K') value = TrimCWhitespace(field.substr(1));
        }
        if (value) table.cells[y][x] = std::string(*value);
    }
    if (!ended || !table.cells.contains(1) || !table.cells.at(1).contains(1))
        return Result<RawSlkTable>::Failure("Object.Slk", "slk-incomplete", "SLK 缺少列标题或结束记录。");
    for (const auto& [column, token] : table.cells.at(1)) table.columns.emplace(LowerAscii(DecodeToken(token)), column);
    for (const auto& [row, values] : table.cells) {
        table.maxRow = std::max(table.maxRow, row);
        if (row == 1) continue;
        const auto id = values.find(1);
        if (id != values.end()) table.rows.emplace(DecodeToken(id->second), row);
    }
    table.text = std::move(text);
    return Result<RawSlkTable>::Success(std::move(table));
}

Result<std::string> AppendSlkRows(const RawSlkTable& table, const std::vector<std::map<int, std::string>>& newRows) {
    const std::string_view text = table.text;
    // 找到真正的 E 记录（与解析器同一判定）；之后的字节原样保留。
    std::size_t endRecord = std::string_view::npos;
    for (std::size_t start = 0; start < text.size();) {
        const auto end = text.find('\n', start);
        const auto line = TrimCWhitespace(text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (line == "E") { endRecord = start; break; }
        start = end == std::string_view::npos ? text.size() : end + 1;
    }
    if (endRecord == std::string_view::npos) {
        Result<std::string> failed; failed.diagnostics = Fail("slk-incomplete", "SLK 缺少结束记录，不能追加行。").diagnostics; return failed;
    }
    const int lastRow = table.maxRow + static_cast<int>(newRows.size());
    std::string head(text.substr(0, endRecord));
    // B 记录的行数随追加更新；列数不变（只写已有列）。
    for (std::size_t start = 0; start < head.size();) {
        const auto end = head.find('\n', start);
        const auto lineEnd = end == std::string::npos ? head.size() : end;
        const std::string_view line = std::string_view(head).substr(start, lineEnd - start);
        if (TrimCWhitespace(line).starts_with("B;")) {
            const auto y = line.find(";Y");
            if (y != std::string_view::npos) {
                auto digitsEnd = y + 2;
                while (digitsEnd < line.size() && line[digitsEnd] >= '0' && line[digitsEnd] <= '9') ++digitsEnd;
                head.replace(start + y + 2, digitsEnd - (y + 2), std::to_string(lastRow));
            }
            break;
        }
        start = lineEnd + 1;
    }
    if (!head.empty() && head.back() != '\n') head += table.newline;
    int row = table.maxRow;
    for (const auto& values : newRows) {
        ++row;
        for (const auto& [column, token] : values)
            head += "C;X" + std::to_string(column) + ";Y" + std::to_string(row) + ";K" + token + table.newline;
    }
    head.append(text.substr(endRecord));
    return Result<std::string>::Success(std::move(head));
}

RawProfileFile ParseRawProfile(std::string text) {
    RawProfileFile file;
    const std::string_view view = WithoutBom(text);
    file.newline = DetectNewline(view);
    std::optional<std::size_t> current;
    for (std::size_t start = 0; start < view.size();) {
        const auto end = view.find('\n', start);
        const auto line = TrimCWhitespace(view.substr(start, end == std::string_view::npos ? view.size() - start : end - start));
        start = end == std::string_view::npos ? view.size() : end + 1;
        if (line.empty() || line.starts_with("//") || line.front() == ';') continue;
        if (line.front() == '[') {
            const auto closing = line.find(']');
            const auto id = closing == std::string_view::npos ? std::string{} : std::string(TrimCWhitespace(line.substr(1, closing - 1)));
            file.sectionsByLowerId[LowerAscii(id)].push_back(file.sections.size());
            file.sections.push_back({id, {}});
            current = file.sections.size() - 1;
        } else if (const auto equals = line.find('='); equals != std::string_view::npos && current) {
            file.sections[*current].entries.push_back({std::string(TrimCWhitespace(line.substr(0, equals))),
                std::string(TrimCWhitespace(line.substr(equals + 1)))});
        }
    }
    file.text = std::move(text);
    return file;
}

std::string AppendProfileSections(const RawProfileFile& file, const std::vector<ProfileSection>& sections) {
    std::string text = file.text;
    if (!text.empty() && text.back() != '\n') text += file.newline;
    for (const auto& section : sections) {
        text += file.newline + "[" + section.id + "]" + file.newline;
        for (const auto& entry : section.entries) text += entry.key + "=" + entry.value + file.newline;
    }
    return text;
}

std::vector<std::string> SplitProfileList(std::string_view value) {
    std::vector<std::string> items;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t at = 0; at < value.size(); ++at) {
        if (value[at] == '"') quoted = !quoted;
        else if (!quoted && value[at] == ',') {
            items.emplace_back(value.substr(start, at - start));
            start = at + 1;
        }
    }
    items.emplace_back(value.substr(start));
    return items;
}

std::string DecodeProfileItem(std::string_view item) {
    item = TrimCWhitespace(item);
    if (item.size() >= 2 && item.front() == '"' && item.back() == '"') {
        item.remove_prefix(1);
        item.remove_suffix(1);
    }
    return std::string(item);
}

std::optional<std::string> ProfileItemToken(std::string_view text) {
    if (text.find_first_of("\"\r\n") != std::string_view::npos || TrimCWhitespace(text).size() != text.size()) return std::nullopt;
    if (text.find(',') != std::string_view::npos) return "\"" + std::string(text) + "\"";
    return std::string(text);
}

std::string JoinProfileList(const std::vector<std::string>& items) {
    std::string joined;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index) joined += ',';
        joined += items[index];
    }
    return joined;
}

} // namespace War3Map::ObjectData::SlkText
