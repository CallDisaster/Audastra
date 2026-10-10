#pragma once

#include "ScriptText.h"
#include "War3MapCore/Script/LuaTriggerUnit.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace War3Map::Script::lua {

struct SourceMapLimit {};

// Lua 只在完整行边界拼接。保留 ScriptText 的换行/缩进，附带平移来源区间。
class MappedText {
public:
    void Begin() { text_.Begin(); }
    void End() { text_.End(); }
    void Line(std::string_view text) { text_.Line(text); }
    void Raw(std::string_view text) { text_.Raw(text); }
    [[nodiscard]] std::size_t Size() const noexcept { return text_.Size(); }
    [[nodiscard]] const std::string& Text() const noexcept { return text_.Text(); }
    [[nodiscard]] std::string Take() { return text_.Take(); }

    void Record(std::size_t start, StableId trigger, StableId node, LuaSourceSpanKind kind) {
        if (start > Size()) throw std::logic_error("Lua source range starts past output");
        if (!node || start == Size()) return;
        if (spans_.size() >= kMaxSpans) throw SourceMapLimit{};
        spans_.push_back({start, Size() - start, trigger, node, kind});
    }

    // 作者正文的范围不包含 Raw 补出的换行；全局 header 有显式类型且身份为零。
    void RecordAuthor(std::size_t start, std::size_t length, StableId trigger, LuaSourceSpanKind kind) {
        if (start > Size() || length > Size() - start
            || (kind != LuaSourceSpanKind::AuthorHeader && kind != LuaSourceSpanKind::AuthorCustom)
            || (kind == LuaSourceSpanKind::AuthorHeader ? trigger != 0 : trigger == 0))
            throw std::logic_error("Lua author range or source identity is invalid");
        if (!length) return;
        if (spans_.size() >= kMaxSpans) throw SourceMapLimit{};
        spans_.push_back({start, length, trigger, trigger, kind});
    }

    void Append(MappedText& other) {
        if (other.spans_.size() > kMaxSpans - spans_.size()) throw SourceMapLimit{};
        // Raw 在未结束的当前行之后会先补换行，再追加传入文本。
        const auto base = Size() + (!Text().empty() && Text().back() != '\n' ? 1u : 0u);
        text_.Raw(other.Take());
        for (auto span : other.spans_) {
            span.byteOffset += base;
            spans_.push_back(span);
        }
        other.spans_.clear();
    }

    [[nodiscard]] std::vector<LuaSourceSpan> TakeSpans() {
        std::sort(spans_.begin(), spans_.end(), [](const auto& left, const auto& right) {
            if (left.byteOffset != right.byteOffset) return left.byteOffset < right.byteOffset;
            if (left.byteLength != right.byteLength) return left.byteLength > right.byteLength;
            return std::tie(left.kind, left.triggerId, left.nodeId) < std::tie(right.kind, right.triggerId, right.nodeId);
        });
        return std::move(spans_);
    }

private:
    static constexpr std::size_t kMaxSpans = 262144;
    ScriptText text_;
    std::vector<LuaSourceSpan> spans_;
};

} // namespace War3Map::Script::lua
