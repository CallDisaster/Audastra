#include "TriggerRenameEvidence.h"

#include <algorithm>
#include <array>
#include <vector>

namespace War3Map::Trigger::detail {
namespace {
using Evidence = TriggerDocumentEditor::RenameEvidence;

bool IdentifierStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
bool IdentifierChar(unsigned char c) { return IdentifierStart(c) || (c >= '0' && c <= '9'); }

class Scanner {
public:
    Scanner(std::string_view code, std::string_view oldName, std::span<const std::string> scripts)
        : code_(code), oldName_(oldName) {
        for (const auto& script : scripts)
            if (!script.empty() && std::find(scripts_.begin(), scripts_.end(), script) == scripts_.end()) scripts_.push_back(script);
    }

    TriggerRenameEvidence Run() {
        while (at_ < code_.size() && named_.kind == Evidence::None) {
            const unsigned char c = static_cast<unsigned char>(code_[at_]);
            if (Starts("//")) { SkipToLineEnd(); continue; }
            if (Starts("/*")) { SkipBlock("*/"); continue; }
            if (Starts("--")) {
                if (Starts("--[[")) SkipBlock("]]"); else SkipToLineEnd();
                continue;
            }
            if (c == '"' || c == '\'') {
                const auto start = at_;
                const auto text = ReadString(at_);
                if (NamesTrigger(text, true)) Record(Evidence::NameUse, start);
                continue;
            }
            if (Starts("##")) {
                // 记号拼接只有接在生成名前缀上才可能拼出这个触发器的符号。
                if (lastIdentifierEnd_ != std::string_view::npos && OnlySpaces(lastIdentifierEnd_, at_)
                    && (lastIdentifier_.starts_with("gg_trg_") || lastIdentifier_.starts_with("Trig_")
                        || lastIdentifier_.starts_with("InitTrig_")))
                    Record(Evidence::DynamicLookup, at_);
                at_ += 2;
                continue;
            }
            if (IdentifierStart(c)) { Identifier(); continue; }
            ++at_;
        }
        return named_.kind != Evidence::None ? named_ : dynamic_;
    }

private:
    bool Starts(std::string_view text) const { return code_.substr(at_).starts_with(text); }
    void SkipToLineEnd() {
        const auto end = code_.find('\n', at_);
        at_ = end == std::string_view::npos ? code_.size() : end;
    }
    void SkipBlock(std::string_view close) {
        const auto end = code_.find(close, at_ + 2);
        const auto stop = end == std::string_view::npos ? code_.size() : end + close.size();
        at_ = stop;
    }
    // 读一个带反斜杠转义的字符串或 JASS 四字符整数，返回去掉引号的原文；cursor 停在结束引号之后。
    std::string_view ReadString(std::size_t& cursor) {
        const char quote = code_[cursor];
        const auto begin = ++cursor;
        while (cursor < code_.size() && code_[cursor] != quote && code_[cursor] != '\n') {
            if (code_[cursor] == '\\' && cursor + 1 < code_.size()) ++cursor;
            ++cursor;
        }
        const auto text = code_.substr(begin, cursor - begin);
        if (cursor < code_.size() && code_[cursor] == quote) ++cursor;
        return text;
    }
    bool OnlySpaces(std::size_t from, std::size_t to) const {
        return std::all_of(code_.begin() + static_cast<std::ptrdiff_t>(from), code_.begin() + static_cast<std::ptrdiff_t>(to),
            [](char c) { return c == ' ' || c == '\t'; });
    }
    std::size_t SkipSpaces(std::size_t cursor) const {
        while (cursor < code_.size() && (code_[cursor] == ' ' || code_[cursor] == '\t' || code_[cursor] == '\r' || code_[cursor] == '\n')) ++cursor;
        return cursor;
    }
    // 生成的触发器符号与函数名：gg_trg_S、InitTrig_S、Trig_S_Actions/_Conditions/_FuncNNN…。
    bool NamesTrigger(std::string_view text, bool inString) const {
        // 编辑器名称在运行期不存在，只有整串等于原名时才保守地算（可能是按名称登记的表键）；
        // "初始化完成" 这类只是包含原名的提示文字不算。
        if (inString && !oldName_.empty() && text == oldName_) return true;
        for (const auto& script : scripts_) {
            const std::array exact{"gg_trg_" + script, "InitTrig_" + script};
            const std::string generated = "Trig_" + script + "_";
            const auto generatedFunction = [&](std::string_view value, std::size_t at) {
                const auto rest = value.substr(at + generated.size());
                return rest.starts_with("Actions") || rest.starts_with("Conditions") || rest.starts_with("Func");
            };
            if (inString) {
                for (const auto& value : exact) if (text.find(value) != std::string_view::npos) return true;
                for (auto at = text.find(generated); at != std::string_view::npos; at = text.find(generated, at + 1))
                    if (generatedFunction(text, at)) return true;
            } else {
                for (const auto& value : exact) if (text == value) return true;
                if (text.starts_with(generated) && generatedFunction(text, 0)) return true;
            }
        }
        return false;
    }
    void Identifier() {
        const auto start = at_;
        while (at_ < code_.size() && IdentifierChar(static_cast<unsigned char>(code_[at_]))) ++at_;
        const auto name = code_.substr(start, at_ - start);
        lastIdentifier_ = name; lastIdentifierEnd_ = at_;
        if (NamesTrigger(name, false)) { Record(Evidence::NameUse, start); return; }
        const auto next = SkipSpaces(at_);
        const char following = next < code_.size() ? code_[next] : '\0';
        if (name == "ExecuteFunc") {
            if (following != '(') return; // native 声明或别处的名字，不是调用。
            auto cursor = SkipSpaces(next + 1);
            if (cursor < code_.size() && code_[cursor] == '"') {
                const auto literal = ReadString(cursor);
                cursor = SkipSpaces(cursor);
                if (cursor < code_.size() && code_[cursor] == ')') {
                    if (NamesTrigger(literal, false)) Record(Evidence::NameUse, start);
                    return;
                }
            }
            Record(Evidence::DynamicLookup, start);
        } else if (name == "_G") {
            if (following == '.') {
                // 静态成员访问；成员名本身在下一轮按标识符检查。
                return;
            }
            if (following == '[') {
                auto cursor = SkipSpaces(next + 1);
                if (cursor < code_.size() && (code_[cursor] == '"' || code_[cursor] == '\'')) {
                    const auto literal = ReadString(cursor);
                    cursor = SkipSpaces(cursor);
                    if (cursor < code_.size() && code_[cursor] == ']') {
                        if (NamesTrigger(literal, false)) Record(Evidence::NameUse, start);
                        return;
                    }
                }
            }
            Record(Evidence::DynamicLookup, start);
        } else if (name == "_ENV" || (name == "rawget" && following == '(')) {
            Record(Evidence::DynamicLookup, start);
        }
    }
    void Record(Evidence kind, std::size_t position) {
        auto& slot = kind == Evidence::NameUse ? named_ : dynamic_;
        if (slot.kind != Evidence::None) return;
        // position 可能在已越过的行里（跨行参数）；按实际位置重新数行，只在首次命中时发生。
        std::size_t line = 1, start = 0;
        for (std::size_t i = 0; i < position && i < code_.size(); ++i)
            if (code_[i] == '\n') { ++line; start = i + 1; }
        auto end = code_.find('\n', start);
        if (end == std::string_view::npos) end = code_.size();
        auto text = code_.substr(start, end - start);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
        constexpr std::size_t limit = 160;
        if (text.size() > limit) {
            std::size_t cut = limit;
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut; // 不切开 UTF-8 字符。
            text = text.substr(0, cut);
        }
        slot = {kind, line, std::string(text)};
    }

    std::string_view code_, oldName_;
    std::vector<std::string> scripts_;
    std::size_t at_ = 0;
    std::string_view lastIdentifier_;
    std::size_t lastIdentifierEnd_ = std::string_view::npos;
    TriggerRenameEvidence named_, dynamic_;
};
} // namespace

TriggerRenameEvidence FindTriggerRenameEvidence(std::string_view code, std::string_view oldName,
    std::span<const std::string> scriptNames) {
    return Scanner(code, oldName, scriptNames).Run();
}

} // namespace War3Map::Trigger::detail
