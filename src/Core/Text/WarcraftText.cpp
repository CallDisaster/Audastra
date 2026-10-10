#include "War3MapCore/Text/WarcraftText.h"

#include "War3MapCore/Text/Utf8Scalar.h"

#include <algorithm>
#include <utility>

namespace War3Map::Text {
namespace {

int Hex(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

class Parser final {
public:
    Parser(std::string_view source, const WarcraftTextLimits& limits) : source_(source), limits_(limits) {}
    WarcraftTextResult Run() {
        if (!limits_.maxInputBytes || limits_.maxInputBytes > 16u * 1024u * 1024u
            || !limits_.maxRuns || limits_.maxRuns > 32768 || !limits_.maxLines || limits_.maxLines > 16384
            || limits_.maxDiagnostics > 64) {
            result_.state = WarcraftTextState::InvalidLimits;
            Issue(WarcraftTextIssue::InvalidLimits, 0, 0);
            return std::move(result_);
        }
        while (offset_ < source_.size()) {
            if (offset_ >= limits_.maxInputBytes) { Stop(WarcraftTextIssue::InputBudget); break; }
            const char current = source_[offset_];
            if (current == '|') {
                if (!Escape()) break;
                continue;
            }
            if (current == '\r' || current == '\n') {
                const std::uint32_t count = current == '\r' && source_.size() - offset_ >= 2 && source_[offset_ + 1] == '\n' ? 2u : 1u;
                if (!Emit("\n", count, WarcraftTextRunKind::Newline)) break;
                continue;
            }
            // 只看预算内的前缀；因预算而截断的完整标量不误报为坏 UTF-8。
            const auto prefix = source_.substr(0, std::min<std::size_t>(source_.size(), limits_.maxInputBytes));
            const auto scalar = Detail::ReadUtf8Scalar(prefix, offset_);
            if (!scalar.Valid()) {
                if (scalar.error == Detail::Utf8ScalarError::Truncated && source_.size() > prefix.size()) Stop(WarcraftTextIssue::InputBudget);
                else {
                    result_.state = WarcraftTextState::InvalidUtf8;
                    Issue(WarcraftTextIssue::InvalidUtf8, offset_, scalar.examinedBytes);
                }
                break;
            }
            if (!Emit(source_.substr(offset_, scalar.bytes), scalar.bytes, WarcraftTextRunKind::Text)) break;
        }
        result_.consumedSourceBytes = offset_;
        return std::move(result_);
    }

private:
    void Issue(WarcraftTextIssue issue, std::uint32_t offset, std::uint32_t bytes) {
        if (result_.diagnostics.size() < limits_.maxDiagnostics) result_.diagnostics.push_back({issue, offset, bytes});
        else ++result_.omittedDiagnostics;
    }
    void Stop(WarcraftTextIssue issue) {
        result_.state = WarcraftTextState::Truncated;
        Issue(issue, offset_, 0);
    }
    bool Fits(std::uint32_t bytes) {
        if (bytes > limits_.maxInputBytes - offset_) { Stop(WarcraftTextIssue::InputBudget); return false; }
        return true;
    }
    bool Emit(std::string_view text, std::uint32_t consumed, WarcraftTextRunKind kind) {
        if (!Fits(consumed)) return false;
        if (kind == WarcraftTextRunKind::Newline && result_.lineCount == limits_.maxLines) {
            Stop(WarcraftTextIssue::LineBudget); return false;
        }
        const bool merge = kind == WarcraftTextRunKind::Text && !result_.runs.empty()
            && result_.runs.back().kind == kind && result_.runs.back().argb == colour_;
        if (!merge && result_.runs.size() == limits_.maxRuns) { Stop(WarcraftTextIssue::RunBudget); return false; }
        const auto output = static_cast<std::uint32_t>(result_.plainText.size());
        result_.plainText.append(text);
        if (merge) {
            auto& last = result_.runs.back();
            last.sourceBytes = offset_ + consumed - last.sourceOffset;
            last.textBytes += static_cast<std::uint32_t>(text.size());
        } else result_.runs.push_back({kind, offset_, consumed, output, static_cast<std::uint32_t>(text.size()), colour_});
        if (kind == WarcraftTextRunKind::Newline) ++result_.lineCount;
        offset_ += consumed;
        return true;
    }
    bool Escape() {
        if (source_.size() - offset_ < 2) {
            Issue(WarcraftTextIssue::UnknownEscape, offset_, 1);
            return Emit("|", 1, WarcraftTextRunKind::Text);
        }
        const char code = source_[offset_ + 1];
        if (code == '|') return Emit("|", 2, WarcraftTextRunKind::Text);
        if (code == 'n' || code == 'N') return Emit("\n", 2, WarcraftTextRunKind::Newline);
        if (code == 'r' || code == 'R') {
            if (!Fits(2)) return false;
            colour_.reset(); offset_ += 2; return true;
        }
        if (code == 'c' || code == 'C') {
            if (!Fits(2)) return false;
            const auto available = std::min<std::size_t>(source_.size() - offset_, limits_.maxInputBytes - offset_);
            if (available < 10 && source_.size() - offset_ >= 10) { Stop(WarcraftTextIssue::InputBudget); return false; }
            std::uint32_t argb = 0;
            bool valid = available >= 10;
            for (std::uint32_t index = 2; valid && index < 10; ++index) {
                const int digit = Hex(source_[offset_ + index]);
                if (digit < 0) valid = false;
                else argb = (argb << 4u) | static_cast<std::uint32_t>(digit);
            }
            if (valid) { colour_ = argb; offset_ += 10; return true; }
            // 只消费竖线，后面的内容仍走标量读取；坏标记里也不能藏无效 UTF-8。
            Issue(WarcraftTextIssue::MalformedColour, offset_, static_cast<std::uint32_t>(std::min<std::size_t>(available, 10)));
            return Emit("|", 1, WarcraftTextRunKind::Text);
        }
        Issue(WarcraftTextIssue::UnknownEscape, offset_, 1);
        return Emit("|", 1, WarcraftTextRunKind::Text);
    }

    std::string_view source_;
    const WarcraftTextLimits& limits_;
    WarcraftTextResult result_;
    std::uint32_t offset_ = 0;
    std::optional<std::uint32_t> colour_;
};

} // namespace

WarcraftTextResult ParseWarcraftText(std::string_view source, const WarcraftTextLimits& limits) {
    return Parser(source, limits).Run();
}

std::string WarcraftColourCode(std::uint32_t rgb) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "|cff000000";
    for (std::uint32_t index = 0; index < 6; ++index)
        result[9 - index] = hex[(rgb >> (4u * index)) & 0xFu];
    return result;
}

} // namespace War3Map::Text
