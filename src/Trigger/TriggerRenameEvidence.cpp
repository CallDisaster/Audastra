#include "TriggerRenameEvidence.h"

#include <algorithm>
#include <vector>

namespace War3Map::Trigger::detail {
namespace {
using Evidence = TriggerDocumentEditor::RenameEvidence;

bool IdentifierStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
bool IdentifierChar(unsigned char c) { return IdentifierStart(c) || (c >= '0' && c <= '9'); }
bool GeneratedSuffix(std::string_view rest) {
    return rest.starts_with("Actions") || rest.starts_with("Conditions") || rest.starts_with("Func");
}
// 字符串里按标识符边界找：gg_trg_A 不算出现在 "gg_trg_AB" 里。
bool ContainsSymbol(std::string_view text, std::string_view symbol) {
    for (auto at = text.find(symbol); at != std::string_view::npos; at = text.find(symbol, at + 1)) {
        const auto end = at + symbol.size();
        if ((at == 0 || !IdentifierChar(static_cast<unsigned char>(text[at - 1])))
            && (end == text.size() || !IdentifierChar(static_cast<unsigned char>(text[end])))) return true;
    }
    return false;
}
bool ContainsGenerated(std::string_view text, std::string_view prefix) {
    for (auto at = text.find(prefix); at != std::string_view::npos; at = text.find(prefix, at + 1))
        if ((at == 0 || !IdentifierChar(static_cast<unsigned char>(text[at - 1]))) && GeneratedSuffix(text.substr(at + prefix.size())))
            return true;
    return false;
}

class Scanner {
public:
    Scanner(std::string_view code, std::span<const CodeSymbolTarget> targets)
        : code_(code), targets_(targets), named_(targets.size()) {}

    // dynamic 为真时同时记录第一处动态查找（改名用）。
    void Run(bool dynamic) {
        dynamicWanted_ = dynamic;
        while (at_ < code_.size() && remaining_ > 0) {
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
                Match(text, true, start);
                continue;
            }
            if (Starts("##")) {
                // 记号拼接只有接在生成名前缀上才可能拼出某个触发器的符号。
                if (lastIdentifierEnd_ != std::string_view::npos && OnlySpaces(lastIdentifierEnd_, at_)
                    && (lastIdentifier_.starts_with("gg_trg_") || lastIdentifier_.starts_with("Trig_")
                        || lastIdentifier_.starts_with("InitTrig_")))
                    RecordDynamic(at_);
                at_ += 2;
                continue;
            }
            if (IdentifierStart(c)) { Identifier(); continue; }
            ++at_;
        }
    }
    const std::vector<TriggerRenameEvidence>& Named() const { return named_; }
    const TriggerRenameEvidence& Dynamic() const { return dynamic_; }

private:
    bool Starts(std::string_view text) const { return code_.substr(at_).starts_with(text); }
    void SkipToLineEnd() {
        const auto end = code_.find('\n', at_);
        at_ = end == std::string_view::npos ? code_.size() : end;
    }
    void SkipBlock(std::string_view close) {
        const auto end = code_.find(close, at_ + 2);
        at_ = end == std::string_view::npos ? code_.size() : end + close.size();
    }
    // 读一个带反斜杠转义的字符串或 JASS 四字符整数，返回去掉引号的原文；cursor 停在结束引号之后。
    std::string_view ReadString(std::size_t& cursor) const {
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
    bool Names(const CodeSymbolTarget& target, std::string_view text, bool inString) const {
        // 编辑器名称在运行期不存在，只有整串等于原名时才保守地算（可能是按名称登记的表键）；
        // "初始化完成" 这类只是包含原名的提示文字不算。
        if (inString && !target.displayName.empty() && text == target.displayName) return true;
        if (inString) {
            return std::any_of(target.exact.begin(), target.exact.end(), [&](const auto& s) { return ContainsSymbol(text, s); })
                || std::any_of(target.generated.begin(), target.generated.end(), [&](const auto& s) { return ContainsGenerated(text, s); });
        }
        return std::any_of(target.exact.begin(), target.exact.end(), [&](const auto& s) { return text == s; })
            || std::any_of(target.generated.begin(), target.generated.end(),
                [&](const auto& s) { return text.starts_with(s) && GeneratedSuffix(text.substr(s.size())); });
    }
    // 返回是否有目标命中。
    bool Match(std::string_view text, bool inString, std::size_t position) {
        bool any = false;
        for (std::size_t i = 0; i < targets_.size(); ++i) {
            if (named_[i].kind != Evidence::None || !Names(targets_[i], text, inString)) continue;
            Record(named_[i], Evidence::NameUse, position);
            --remaining_;
            any = true;
        }
        return any;
    }
    void Identifier() {
        const auto start = at_;
        while (at_ < code_.size() && IdentifierChar(static_cast<unsigned char>(code_[at_]))) ++at_;
        const auto name = code_.substr(start, at_ - start);
        lastIdentifier_ = name; lastIdentifierEnd_ = at_;
        if (Match(name, false, start)) return;
        const auto next = SkipSpaces(at_);
        const char following = next < code_.size() ? code_[next] : '\0';
        if (name == "ExecuteFunc") {
            if (following != '(') return; // native 声明或别处的名字，不是调用。
            auto cursor = SkipSpaces(next + 1);
            if (cursor < code_.size() && code_[cursor] == '"') {
                const auto literal = ReadString(cursor);
                cursor = SkipSpaces(cursor);
                if (cursor < code_.size() && code_[cursor] == ')') {
                    (void)Match(literal, false, start);
                    return;
                }
            }
            RecordDynamic(start);
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
                        (void)Match(literal, false, start);
                        return;
                    }
                }
            }
            RecordDynamic(start);
        } else if (name == "_ENV" || (name == "rawget" && following == '(')) {
            RecordDynamic(start);
        }
    }
    void RecordDynamic(std::size_t position) {
        if (dynamicWanted_ && dynamic_.kind == Evidence::None) Record(dynamic_, Evidence::DynamicLookup, position);
    }
    void Record(TriggerRenameEvidence& slot, Evidence kind, std::size_t position) const {
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

    std::string_view code_;
    std::span<const CodeSymbolTarget> targets_;
    std::vector<TriggerRenameEvidence> named_;
    std::size_t remaining_ = named_.size();
    bool dynamicWanted_ = false;
    std::size_t at_ = 0;
    std::string_view lastIdentifier_;
    std::size_t lastIdentifierEnd_ = std::string_view::npos;
    TriggerRenameEvidence dynamic_;
};
} // namespace

CodeSymbolTarget TriggerCodeSymbols(std::span<const std::string> scriptNames, std::string displayName) {
    CodeSymbolTarget target;
    target.displayName = std::move(displayName);
    for (const auto& script : scriptNames) {
        if (script.empty() || std::find(target.generated.begin(), target.generated.end(), "Trig_" + script + "_") != target.generated.end()) continue;
        target.exact.push_back("gg_trg_" + script);
        target.exact.push_back("InitTrig_" + script);
        target.generated.push_back("Trig_" + script + "_");
    }
    return target;
}

TriggerRenameEvidence FindTriggerRenameEvidence(std::string_view code, std::string_view oldName,
    std::span<const std::string> scriptNames) {
    const CodeSymbolTarget target = TriggerCodeSymbols(scriptNames, std::string(oldName));
    Scanner scanner(code, {&target, 1});
    // 只要第一处：找到名称使用就停；动态查找只在没有名称使用时返回。
    scanner.Run(true);
    return scanner.Named().front().kind != TriggerDocumentEditor::RenameEvidence::None ? scanner.Named().front() : scanner.Dynamic();
}

std::vector<TriggerRenameEvidence> FindSymbolUses(std::string_view code, std::span<const CodeSymbolTarget> targets) {
    Scanner scanner(code, targets);
    scanner.Run(false);
    return scanner.Named();
}

} // namespace War3Map::Trigger::detail
