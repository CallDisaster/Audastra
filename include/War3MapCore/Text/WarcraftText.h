#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Text {

// 只解释已解码 UTF-8 的 Warcraft 文本标记；不读取文件、不解析 WTS、不改源。
// ARGB 原值保留用于来源说明，RGB 预览不宣称游戏 alpha/字体/热键等价。
enum class WarcraftTextState { Complete, Truncated, InvalidUtf8, InvalidLimits };
enum class WarcraftTextRunKind { Text, Newline };
enum class WarcraftTextIssue {
    MalformedColour, UnknownEscape, InvalidUtf8, InputBudget, RunBudget, LineBudget, InvalidLimits
};

struct WarcraftTextLimits {
    std::uint32_t maxInputBytes = 1024 * 1024;
    std::uint32_t maxRuns = 8192;
    std::uint32_t maxLines = 4096;
    std::uint32_t maxDiagnostics = 16;
};

struct WarcraftTextDiagnostic {
    WarcraftTextIssue issue = WarcraftTextIssue::UnknownEscape;
    std::uint32_t sourceOffset = 0;
    std::uint32_t sourceBytes = 0;
    [[nodiscard]] bool operator==(const WarcraftTextDiagnostic&) const = default;
};

struct WarcraftTextRun {
    WarcraftTextRunKind kind = WarcraftTextRunKind::Text;
    // 来源范围包围此段实际输出的原字节；合并段的范围可包含无输出的格式标记。
    std::uint32_t sourceOffset = 0;
    std::uint32_t sourceBytes = 0;
    std::uint32_t textOffset = 0;
    std::uint32_t textBytes = 0;
    std::optional<std::uint32_t> argb;
    [[nodiscard]] bool operator==(const WarcraftTextRun&) const = default;
};

struct WarcraftTextResult {
    WarcraftTextState state = WarcraftTextState::Complete;
    std::string plainText;
    std::vector<WarcraftTextRun> runs;
    std::vector<WarcraftTextDiagnostic> diagnostics;
    std::uint32_t consumedSourceBytes = 0;
    std::uint32_t lineCount = 1;
    std::uint32_t omittedDiagnostics = 0;
    [[nodiscard]] bool Complete() const { return state == WarcraftTextState::Complete; }
};

// 支持 |cAARRGGBB（大小写）、|r、|n、||，以及原 CR/LF/CRLF。
// 未知/坏标记按字面显示；无效 UTF-8/超帽只返回有界前缀，源输入保持不变。
// 非零预算须不超过硬帽：输入 16MiB、段 32768、行 16384、诊断 64。
[[nodiscard]] WarcraftTextResult ParseWarcraftText(
    std::string_view source, const WarcraftTextLimits& limits = {});

// 取色插入固定不透明标记；只返回字符串，不更改任何文档或现有标记。
[[nodiscard]] std::string WarcraftColourCode(std::uint32_t rgb);

} // namespace War3Map::Text
