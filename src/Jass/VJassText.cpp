#include "VJassText.h"

#include <algorithm>

namespace War3Map::Jass::detail {

SourceTrace::SourceTrace(std::string_view sourceText, std::string sourceName, TranslationBudget& sourceBudget)
    : source(sourceText), name(std::move(sourceName)), budget(sourceBudget) {
    lines_.push_back(0);
    for (std::size_t i = 0; i < source.size(); ++i) {
        budget.check.PollOccasionally(i);
        if (source[i] != '\r' && source[i] != '\n') continue;
        if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n') ++i;
        budget.check.ConsumeAst();
        ReserveBounded(lines_, lines_.size() + 1, budget.check.AstLimit(), budget.check);
        lines_.push_back(static_cast<std::uint32_t>(i + 1));
    }
}

std::uint32_t SourceTrace::Line(std::uint32_t offset) const {
    return static_cast<std::uint32_t>(std::upper_bound(lines_.begin(), lines_.end(), offset) - lines_.begin());
}

std::uint32_t SourceTrace::Offset(std::uint32_t line) const {
    return line == 0 ? 0 : line <= lines_.size() ? lines_[line - 1] : static_cast<std::uint32_t>(source.size());
}

void MappedOutput::AddOrigin(std::uint32_t output, std::uint32_t bytes, std::uint32_t source, bool exact) {
    if (bytes == 0) return;
    auto& budget = source_.budget.check;
    if (output > data_.text.size() || bytes > data_.text.size() - output || source > source_.source.size() ||
        (exact && bytes > source_.source.size() - source)) budget.Fail(CheckFailure::InvalidTree);
    budget.ConsumeAst();
    const auto line = source_.Line(source);
    const auto column = source - source_.Offset(line) + 1;
    if (!data_.byteOrigins.empty()) {
        auto& previous = data_.byteOrigins.back();
        if (previous.outputOffset + previous.byteLength == output && previous.exact == exact &&
            previous.sourceLine == line && (exact ? previous.sourceOffset + previous.byteLength == source : previous.sourceOffset == source)) {
            previous.byteLength += bytes;
            return;
        }
    }
    ReserveBounded(data_.byteOrigins, data_.byteOrigins.size() + 1, budget.AstLimit(), budget);
    data_.byteOrigins.push_back({output, bytes, source, line, column, exact});
}

void MappedOutput::CopyOrigins(const std::vector<VJassByteOrigin>& origins, std::uint32_t base) {
    std::size_t expected = 0;
    for (const auto& origin : origins) {
        source_.budget.check.Poll();
        if (origin.byteLength == 0 || origin.outputOffset != expected || base > data_.text.size() ||
            origin.outputOffset > data_.text.size() - base || origin.byteLength > data_.text.size() - base - origin.outputOffset ||
            origin.sourceOffset > source_.source.size() || origin.sourceLine != source_.Line(origin.sourceOffset) ||
            origin.sourceColumn != origin.sourceOffset - source_.Offset(origin.sourceLine) + 1) {
            source_.budget.check.Fail(CheckFailure::InvalidTree);
        }
        AddOrigin(base + origin.outputOffset, origin.byteLength, origin.sourceOffset, origin.exact);
        expected += origin.byteLength;
    }
    if (expected != data_.text.size() - base) source_.budget.check.Fail(CheckFailure::InvalidTree);
}

void MappedOutput::AppendAt(std::string_view text, std::uint32_t offset, bool original, bool track) {
    if (text.empty()) return;
    if (data_.text.size() > UINT32_MAX || text.size() > UINT32_MAX - data_.text.size() ||
        offset > source_.source.size() || (original && text.size() > source_.source.size() - offset)) {
        source_.budget.check.Fail(CheckFailure::InvalidTree);
    }
    const auto output = static_cast<std::uint32_t>(data_.text.size());
    source_.budget.Append(data_.text, text);
    std::size_t originBegin = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        source_.budget.check.PollOccasionally(i);
        const char c = text[i];
        if (c == '\n' && previousCr_) {
            if (track && original && originBegin <= i) {
                AddOrigin(output + static_cast<std::uint32_t>(originBegin), static_cast<std::uint32_t>(i + 1 - originBegin),
                    offset + static_cast<std::uint32_t>(originBegin), true);
                originBegin = i + 1;
            }
            previousCr_ = false;
            continue;
        }
        previousCr_ = false;
        if (!lineStarted_) {
            source_.budget.check.ConsumeAst();
            data_.sourceMap.Map(outputLine_, {0, source_.Line(offset + static_cast<std::uint32_t>(original ? i : 0))});
            lineStarted_ = true;
        }
        if (c == '\n' || c == '\r') {
            if (track && original) {
                auto originEnd = i + 1;
                if (c == '\r' && originEnd < text.size() && text[originEnd] == '\n') ++originEnd;
                AddOrigin(output + static_cast<std::uint32_t>(originBegin), static_cast<std::uint32_t>(originEnd - originBegin),
                    offset + static_cast<std::uint32_t>(originBegin), true);
                originBegin = originEnd;
            }
            ++outputLine_;
            lineStarted_ = false;
            previousCr_ = c == '\r';
        }
    }
    if (track && original && originBegin < text.size()) {
        AddOrigin(output + static_cast<std::uint32_t>(originBegin), static_cast<std::uint32_t>(text.size() - originBegin),
            offset + static_cast<std::uint32_t>(originBegin), true);
    } else if (track && !original) {
        AddOrigin(output, static_cast<std::uint32_t>(text.size()), offset, false);
    }
}

void MappedOutput::Tracked(std::string_view text, const std::vector<VJassByteOrigin>& origins, std::uint32_t anchor) {
    if (origins.empty()) { AppendAt(text, anchor, false); return; }
    const auto base = static_cast<std::uint32_t>(data_.text.size());
    AppendAt(text, anchor, false, false);
    CopyOrigins(origins, base);
}

void MappedOutput::AppendPosition(std::string_view text, SourcePosition position) {
    if (text.empty()) return;
    if (data_.text.size() > UINT32_MAX || text.size() > UINT32_MAX - data_.text.size())
        source_.budget.check.Fail(CheckFailure::InvalidTree);
    source_.budget.Append(data_.text, text);
    for (std::size_t i = 0; i < text.size(); ++i) {
        source_.budget.check.PollOccasionally(i);
        const char c = text[i];
        if (c == '\n' && previousCr_) { previousCr_ = false; continue; }
        previousCr_ = false;
        if (!lineStarted_) { source_.budget.check.ConsumeAst(); data_.sourceMap.Map(outputLine_, position); lineStarted_ = true; }
        if (c == '\n' || c == '\r') { ++outputLine_; lineStarted_ = false; previousCr_ = c == '\r'; }
    }
}

MappedOutput& MappedOutput::operator+=(const MappedText& text) {
    const auto base = static_cast<std::uint32_t>(data_.text.size());
    std::size_t begin = 0;
    std::uint32_t line = 1;
    while (begin < text.text.size()) {
        source_.budget.check.Poll();
        auto end = begin;
        while (end < text.text.size() && text.text[end] != '\n' && text.text[end] != '\r') {
            source_.budget.check.PollOccasionally(end);
            ++end;
        }
        if (end < text.text.size()) {
            if (text.text[end++] == '\r' && end < text.text.size() && text.text[end] == '\n') ++end;
        }
        auto position = text.sourceMap.Find(line);
        if (!position || position->file >= text.sourceMap.Files().size()) source_.budget.check.Fail(CheckFailure::InvalidTree);
        position->file = data_.sourceMap.AddFile(text.sourceMap.Files()[position->file]);
        AppendPosition(std::string_view(text.text).substr(begin, end - begin), *position);
        if (text.byteOrigins.empty()) AddOrigin(base + static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(end - begin), source_.Offset(position->line), false);
        begin = end;
        ++line;
    }
    if (!text.byteOrigins.empty()) CopyOrigins(text.byteOrigins, base);
    return *this;
}

void MappedOutput::Original(std::uint32_t begin, std::uint32_t end) {
    if (begin > end || end > source_.source.size()) source_.budget.check.Fail(CheckFailure::InvalidTree);
    const auto source = source_.source;
    std::size_t at = begin, copied = begin;
    while (at < end) {
        source_.budget.check.PollOccasionally(ticks_++);
        const auto c = source[at];
        if (c == '"' || c == '\'') {
            ++at;
            while (at < end && source[at] != c && (c == '"' || (source[at] != '\n' && source[at] != '\r'))) {
                source_.budget.check.PollOccasionally(ticks_++);
                at = (std::min)(std::size_t{end}, at + (source[at] == '\\' ? 2 : 1));
            }
            at = (std::min)(std::size_t{end}, at + 1);
        } else if (c == '/' && at + 1 < end && source[at + 1] == '/') {
            while (at < end && source[at] != '\n' && source[at] != '\r') { source_.budget.check.PollOccasionally(ticks_++); ++at; }
        } else if (c == '/' && at + 1 < end && source[at + 1] == '*') {
            AppendAt(source.substr(copied, at - copied), static_cast<std::uint32_t>(copied), true);
            const auto anchor = static_cast<std::uint32_t>(at);
            at += 2;
            while (at < end && !(source[at] == '*' && at + 1 < end && source[at + 1] == '/')) {
                source_.budget.check.PollOccasionally(ticks_++);
                ++at;
            }
            at = (std::min)(std::size_t{end}, at + 2);
            if (data_.text.empty() || data_.text.back() != ' ') AppendAt(" ", anchor, false);
            copied = at;
        } else ++at;
    }
    AppendAt(source.substr(copied, end - copied), static_cast<std::uint32_t>(copied), true);
}

std::string StripBlockComments(std::string_view text, CheckBudget* budget) {
    if (budget) budget->Poll();
    if (text.find("/*") == std::string_view::npos) return std::string(text);
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    std::size_t ticks = 0;
    while (at < text.size()) {
        if (budget) budget->PollOccasionally(ticks++);
        const char c = text[at];
        if (c == '"' || c == '\'') {
            // 字符串 / rawcode：原样复制到配对的引号（rawcode 不跨行）。
            const std::size_t start = at++;
            while (at < text.size() && text[at] != c && (c == '"' || (text[at] != '\n' && text[at] != '\r'))) {
                if (budget) budget->PollOccasionally(ticks++);
                at += text[at] == '\\' ? 2 : 1;
            }
            at = std::min(at + 1, text.size());
            out.append(text.substr(start, at - start));
            continue;
        }
        if (c == '/' && at + 1 < text.size() && text[at + 1] == '/') {
            const std::size_t start = at;
            while (at < text.size() && text[at] != '\n' && text[at] != '\r') { if (budget) budget->PollOccasionally(ticks++); ++at; }
            out.append(text.substr(start, at - start));
            continue;
        }
        if (c == '/' && at + 1 < text.size() && text[at + 1] == '*') {
            at += 2;
            while (at < text.size() && !(text[at] == '*' && at + 1 < text.size() && text[at + 1] == '/')) {
                if (budget) budget->PollOccasionally(ticks++);
                ++at;
            }
            at = (std::min)(at + 2, text.size());
            if (out.empty() || out.back() != ' ') out.push_back(' ');
            continue;
        }
        out.push_back(c);
        ++at;
    }
    return out;
}

void AppendWithEdits(MappedOutput& out, std::string_view source, std::uint32_t begin, std::uint32_t end,
    std::vector<TextEdit>& edits, CheckBudget& budget) {
    budget.Poll();
    std::stable_sort(edits.begin(), edits.end(), [&budget](const TextEdit& a, const TextEdit& b) {
        budget.Poll();
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end;
    });
    auto at = begin;
    for (const auto& edit : edits) {
        budget.Poll();
        if (edit.begin < at || edit.end < edit.begin || edit.end > end) continue;
        out.Original(at, edit.begin);
        out.Tracked(edit.text, edit.byteOrigins, edit.begin);
        at = edit.end;
    }
    out.Original(at, end);
}

void AppendWithEdits(std::string& out, std::string_view source, std::uint32_t begin, std::uint32_t end, std::vector<TextEdit>& edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const TextEdit& a, const TextEdit& b) {
        return a.begin != b.begin ? a.begin < b.begin : a.end < b.end; // 同一位置的插入（零宽）排在替换之前
    });
    std::uint32_t at = begin;
    for (const auto& edit : edits) {
        if (edit.begin < at || edit.end > end) continue; // 重叠或越界的替换丢弃（调用方保证不会发生）
        out += StripBlockComments(source.substr(at, edit.begin - at));
        out += edit.text;
        at = edit.end;
    }
    out += StripBlockComments(source.substr(at, end - at));
}

std::uint32_t LineBegin(std::string_view text, std::uint32_t offset) noexcept {
    while (offset > 0 && text[offset - 1] != '\n' && text[offset - 1] != '\r') --offset;
    return offset;
}

std::uint32_t LineEnd(std::string_view text, std::uint32_t offset) noexcept {
    while (offset < text.size() && text[offset] != '\n' && text[offset] != '\r') ++offset;
    if (offset < text.size() && text[offset] == '\r') ++offset;
    if (offset < text.size() && text[offset] == '\n') ++offset;
    return static_cast<std::uint32_t>(offset);
}

} // namespace War3Map::Jass::detail
