#pragma once

#include <string>
#include <string_view>

namespace War3Map::Script {

// 逐行脚本输出：与 WE 输出器一致，每行开头按当前缩进级别补 4 个空格；
// Put(text, false) 续写同一行，Put(text, true) 结束一行。
class ScriptText {
public:
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
        return tail;
    }
    // 在已输出的位置插入整行文本（text 需自带换行），不影响当前缩进与行状态。
    void Insert(std::size_t offset, std::string_view text) { text_.insert(offset, text); }

    [[nodiscard]] const std::string& Text() const noexcept { return text_; }
    [[nodiscard]] std::string Take() { return std::move(text_); }
    [[nodiscard]] int Indent() const noexcept { return indent_; }
    void SetIndent(int indent) { indent_ = indent < 0 ? 0 : indent; }

private:
    std::string text_;
    int indent_ = 0;
    bool lineStart_ = true;
};

} // namespace War3Map::Script
