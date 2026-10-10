#pragma once

#include "JassCheckBudget.h"
#include "War3MapCore/Jass/SourceMap.h"
#include "War3MapCore/Jass/VJass.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// vJass 输出的文本工具：按源文本范围做替换，未改动的部分原样保留（含注释与缩进）。
namespace War3Map::Jass::detail {

// 文本生成另计累计工作量，避免名称展开/中间表达式复制放大；输入仍由 CheckBudget 计数。
struct TranslationBudget {
    CheckBudget& check;
    std::size_t limit;
    std::size_t generated = 0;
    void Consume(std::size_t bytes) {
        check.Poll();
        if (generated > limit || bytes > limit - generated) check.Fail(CheckFailure::TotalBytes);
        generated += bytes;
    }
    void Append(std::string& out, std::string_view text) {
        if (out.size() > limit || text.size() > limit - out.size()) check.Fail(CheckFailure::TotalBytes);
        Consume(text.size());
        out.append(text);
    }
};

struct MappedText {
    std::string text;
    SourceMap sourceMap;
    std::vector<VJassByteOrigin> byteOrigins;
};

// 源行表只建立一次；来自 IR 的偏移是唯一映射依据，不匹配最终输出的文本。
class SourceTrace {
public:
    SourceTrace(std::string_view source, std::string name, TranslationBudget& budget);
    [[nodiscard]] std::uint32_t Line(std::uint32_t offset) const;
    [[nodiscard]] std::uint32_t Offset(std::uint32_t line) const;
    std::string_view source;
    std::string name;
    TranslationBudget& budget;
private:
    std::vector<std::uint32_t> lines_;
};

class MappedOutput {
public:
    explicit MappedOutput(SourceTrace& source) : source_(source) { data_.sourceMap.AddFile(source.name); }
    void Anchor(std::uint32_t offset) { anchor_ = offset; }
    void AnchorLine(std::uint32_t line) { Anchor(source_.Offset(line)); }
    MappedOutput& operator+=(std::string_view text) { AppendAt(text, anchor_, false); return *this; }
    MappedOutput& operator+=(char c) { return *this += std::string_view(&c, 1); }
    MappedOutput& operator+=(const MappedText& text);
    void Original(std::uint32_t begin, std::uint32_t end);
    void Tracked(std::string_view text, const std::vector<VJassByteOrigin>& origins, std::uint32_t anchor);
    void reserve(std::size_t bytes) { data_.text.reserve((std::min)(bytes, source_.budget.limit)); }
    [[nodiscard]] bool empty() const { return data_.text.empty(); }
    [[nodiscard]] char back() const { return data_.text.back(); }
    [[nodiscard]] MappedText Take() { return std::move(data_); }
private:
    void AppendAt(std::string_view text, std::uint32_t offset, bool original, bool track = true);
    void AppendPosition(std::string_view text, SourcePosition position);
    void AddOrigin(std::uint32_t output, std::uint32_t bytes, std::uint32_t source, bool exact);
    void CopyOrigins(const std::vector<VJassByteOrigin>& origins, std::uint32_t base);
    SourceTrace& source_;
    MappedText data_;
    std::uint32_t anchor_ = 0, outputLine_ = 1;
    std::size_t ticks_ = 0;
    bool lineStarted_ = false, previousCr_ = false;
};

struct TextEdit {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    std::string text;
    std::vector<VJassByteOrigin> byteOrigins;
};

// 去掉 /* */ 注释（字符串、rawcode、// 注释中的除外）；注释占多行时整体换成一个空格，使表达式回到一行。
[[nodiscard]] std::string StripBlockComments(std::string_view text, CheckBudget* budget = nullptr);

// 源文本 [begin, end) 应用 edits（须按 begin 排序、互不重叠且落在范围内），未改动部分去掉块注释后原样输出。
void AppendWithEdits(std::string& out, std::string_view source, std::uint32_t begin, std::uint32_t end, std::vector<TextEdit>& edits);
void AppendWithEdits(MappedOutput& out, std::string_view source, std::uint32_t begin, std::uint32_t end, std::vector<TextEdit>& edits, CheckBudget& budget);

// 所在行行首 / 所在行换行之后的位置。
[[nodiscard]] std::uint32_t LineBegin(std::string_view text, std::uint32_t offset) noexcept;
[[nodiscard]] std::uint32_t LineEnd(std::string_view text, std::uint32_t offset) noexcept;

} // namespace War3Map::Jass::detail
