#pragma once

#include "War3MapCore/Text/SourceEncoding.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 预处理记号（C 翻译阶段 3）的词法分析，供预处理器内部使用。
// 与 C 的差异：'$' 可作标识符字符；"…" 字符串允许跨行（JASS 允许，WE 会写出）；
// 未配对的 ' 或 " 退化为单个标点，不吞掉后续文本。
namespace War3Map::Jass::detail {

class CheckBudget;

enum class PpKind : std::uint8_t { Identifier, Number, String, Char, Punct, Newline, End };

struct PpToken {
    std::string_view text;
    PpKind kind = PpKind::End;
    bool space = false;      // 前面有空白或注释
    bool lineStart = false;  // 逻辑行的第一个记号
    bool source = false;     // 来自当前文件的原文（可按偏移原样复制）
    std::uint32_t line = 0;  // 源行号（从 1 开始）
    std::uint32_t offset = 0;
    std::uint32_t hide = 0;  // 隐藏集编号（0 为空集）
    std::uint32_t physicalLength = 0; // 续接后拼写可能短于原字节跨度；0 使用原拼写长度。

    [[nodiscard]] bool Is(std::string_view punct) const noexcept { return kind == PpKind::Punct && text == punct; }
    [[nodiscard]] std::uint32_t End() const noexcept {
        return offset + (physicalLength ? physicalLength : static_cast<std::uint32_t>(text.size()));
    }
};

struct PpLineSplice {
    std::uint32_t logicalOffset=0,physicalOffset=0,length=0;
};
// C/Wave phase2：只删除反斜杠紧邻物理 LF/CRLF/CR，先于字符串、注释和宏分词。
// 无续接时不复制全文；有续接时持有逻辑原文和稀疏位置表，借用记号不得超出此对象寿命。
struct PpSplicedSource {
    std::string text;
    std::vector<PpLineSplice> splices;
    [[nodiscard]] std::string_view Slice(std::string_view physical, std::uint32_t from, std::uint32_t to) const;
    void MapOrigins(std::vector<PpToken>& tokens, CheckBudget* budget = nullptr) const;
};
[[nodiscard]] Result<PpSplicedSource> SplicePpLines(std::string_view text,
    std::size_t maxStorageBytes=64u*1024u*1024u, std::optional<Text::SourceEncoding> encoding={}, CheckBudget* budget = nullptr);

// 对整段文本分词；结尾总有一个 End 记号。反斜杠续行按空白处理。
[[nodiscard]] std::vector<PpToken> LexPp(std::string_view text);
// 显式编码与LexJass相同；非法/不支持标量、未结束注释或引号均拒绝，不返回部分tokens。
[[nodiscard]] Result<std::vector<PpToken>> LexPp(std::string_view text, Text::SourceEncoding encoding);
// 来源登记由 Engine 负责；该入口在每次 push 前消费共享 token 预算。
[[nodiscard]] Result<std::vector<PpToken>> LexPpBounded(std::string_view text, CheckBudget& budget,
    std::optional<Text::SourceEncoding> encoding = {});

// ## 粘贴结果是否恰为一个记号；是则返回其类别。
[[nodiscard]] bool LexSingle(std::string_view spelling, PpKind& kind, CheckBudget* budget = nullptr);
// 用同一编码检查##结果；非法编码或并非恰好一个token返回定位诊断。
[[nodiscard]] Result<PpKind> LexSingle(std::string_view spelling, Text::SourceEncoding encoding);

// 标识符字符（含 '$'）。
[[nodiscard]] constexpr bool IsIdentifierChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

// 两段文本直接相接时，a 的末字符与 b 的首字符是否会被词法分析并成一个记号（需要补空格）。
[[nodiscard]] bool WouldMerge(char a, char b) noexcept;

} // namespace War3Map::Jass::detail
