#include "War3MapCore/Text/SlkCells.h"

#include "War3MapCore/Text/TextUtil.h"

#include <charconv>
#include <cstdlib>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

namespace War3Map::Text {
namespace {

std::vector<std::string_view> SplitFields(std::string_view text, bool strict) {
    std::vector<std::string_view> fields;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (text[at] == '"') {
            if (strict && quoted && at + 1 < text.size() && text[at + 1] == '"') ++at;
            else quoted = !quoted;
        } else if (!quoted && text[at] == ';') {
            fields.push_back(text.substr(start, at - start));
            start = at + 1;
        }
    }
    fields.push_back(text.substr(start));
    return fields;
}

std::string CellValue(std::string_view text, bool strict) {
    if (strict) text = TrimCWhitespace(text);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    if (!strict) return std::string(text);
    std::string decoded;
    decoded.reserve(text.size());
    for (std::size_t at = 0; at < text.size(); ++at) {
        decoded.push_back(text[at]);
        if (text[at] == '"' && at + 1 < text.size() && text[at + 1] == '"') ++at;
    }
    return decoded;
}

} // namespace

Result<SlkCells> ParseSlkCells(std::string_view text, SlkCellMode mode) {
    const bool strict = mode == SlkCellMode::CatalogStrict;
    if (strict && !text.starts_with("ID;")) return Result<SlkCells>::Failure(
        "Text.Slk", "slk-header", "SLK 文件头无效。");
    try {
        SlkCells cells;
        int x = 1, y = 1;
        bool ended = false;
        for (std::size_t start = 0; start < text.size();) {
            const auto end = text.find('\n', start);
            auto line = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
            start = end == std::string_view::npos ? text.size() : end + 1;
            if (strict) line = TrimCWhitespace(line);
            else if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (strict && line == "E") { ended = true; break; }
            if (!line.starts_with("C;")) continue;
            std::optional<std::string> value;
            for (const auto field : SplitFields(line.substr(2), strict)) {
                if (field.empty()) continue;
                if (field.front() == 'X' || field.front() == 'Y') {
                    int coordinate = 0;
                    if (strict) {
                        const auto parsed = std::from_chars(field.data() + 1,
                            field.data() + field.size(), coordinate);
                        if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size()
                            || coordinate < 1 || coordinate > 100000)
                            return Result<SlkCells>::Failure("Text.Slk", "slk-coordinate",
                                "SLK 单元格坐标无效或超出目录读取范围。");
                    } else coordinate = std::atoi(std::string(field.substr(1)).c_str());
                    (field.front() == 'X' ? x : y) = coordinate;
                } else if (field.front() == 'K') value = CellValue(field.substr(1), strict);
            }
            if (value) cells[y][x] = std::move(*value);
        }
        if (strict && (!ended || !cells.contains(1) || !cells.at(1).contains(1)))
            return Result<SlkCells>::Failure("Text.Slk", "slk-incomplete",
                "SLK 缺少列标题或结束记录，未接受部分表格。");
        return Result<SlkCells>::Success(std::move(cells));
    } catch (const std::bad_alloc&) {
        return Result<SlkCells>::Failure("Text.Slk", "slk-allocation", "SLK 单元格超出内存预算。");
    } catch (const std::length_error&) {
        return Result<SlkCells>::Failure("Text.Slk", "slk-container-limit", "SLK 单元格超出容器容量。");
    }
}

} // namespace War3Map::Text
