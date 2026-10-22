#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Script {

// 输出片段的来源：属于哪个 GUI 触发器的哪条语句（eca 为 0 表示该触发器自身的生成代码）。
struct ScriptOwner {
    std::uint64_t trigger = 0;
    std::uint64_t eca = 0;
    friend bool operator==(const ScriptOwner&, const ScriptOwner&) = default;
};
// 从 offset 起的输出属于 owner，直到下一个标记。
struct ScriptOwnerMark {
    std::size_t offset = 0;
    ScriptOwner owner;
};

// 逐行脚本输出：与 WE 输出器一致，每行开头按当前缩进级别补 4 个空格；
// Put(text, false) 续写同一行，Put(text, true) 结束一行。
class ScriptText {
public:
    // 之后写出的文本记到 owner 名下；Cut / Insert 同步维护标记，供检查结果定位回语句。
    void SetOwner(ScriptOwner owner) {
        if (owner_ == owner) return;
        owner_ = owner;
        Mark();
    }
    [[nodiscard]] ScriptOwner Owner() const noexcept { return owner_; }
    [[nodiscard]] const std::vector<ScriptOwnerMark>& OwnerMarks() const noexcept { return marks_; }

    void Begin() { ++indent_; }
    void End() { indent_ = indent_ > 0 ? indent_ - 1 : 0; }

    void Put(std::string_view text, bool endLine = false) {
        if (lineStart_ && endLine && text.empty()) {
            text_ += '\n'; // 空行不带缩进
            return;
        }
        if (lineStart_) {
            text_.append(static_cast<std::size_t>(indent_) * 4, ' ');
            lineStart_ = false;
        }
        text_ += text;
        if (endLine) {
            text_ += '\n';
            lineStart_ = true;
        }
    }
    void Line(std::string_view text) { Put(text, true); }
    // 不带缩进的整行（预处理指令、分隔注释）。
    void Raw(std::string_view text) {
        if (!lineStart_) {
            text_ += '\n';
        }
        text_ += text;
        text_ += '\n';
        lineStart_ = true;
    }

    // 已输出的字节数；与 Cut / Insert 配合，把刚写出的片段取回改写（如展开“任意玩家”循环）。
    [[nodiscard]] std::size_t Size() const noexcept { return text_.size(); }
    // 取回并删除 offset 之后的全部输出；之后从行首继续。
    [[nodiscard]] std::string Cut(std::size_t offset) {
        std::string tail = text_.substr(offset);
        text_.erase(offset);
        lineStart_ = text_.empty() || text_.back() == '\n';
        // 取回的片段由调用方以当前来源重新写出。
        while (!marks_.empty() && marks_.back().offset >= offset) marks_.pop_back();
        Mark();
        return tail;
    }
    // 在已输出的位置插入整行文本（text 需自带换行），不影响当前缩进与行状态。
    void Insert(std::size_t offset, std::string_view text) {
        text_.insert(offset, text);
        for (auto& mark : marks_) {
            if (mark.offset >= offset) mark.offset += text.size();
        }
    }

    [[nodiscard]] const std::string& Text() const noexcept { return text_; }
    [[nodiscard]] std::string Take() { return std::move(text_); }
    [[nodiscard]] int Indent() const noexcept { return indent_; }
    void SetIndent(int indent) { indent_ = indent < 0 ? 0 : indent; }

private:
    void Mark() {
        const ScriptOwner current = marks_.empty() ? ScriptOwner{} : marks_.back().owner;
        if (current == owner_) return;
        if (!marks_.empty() && marks_.back().offset == text_.size()) {
            marks_.pop_back();
            if ((marks_.empty() ? ScriptOwner{} : marks_.back().owner) == owner_) return;
        }
        marks_.push_back({text_.size(), owner_});
    }

    std::string text_;
    int indent_ = 0;
    bool lineStart_ = true;
    ScriptOwner owner_;
    std::vector<ScriptOwnerMark> marks_;
};

} // namespace War3Map::Script
