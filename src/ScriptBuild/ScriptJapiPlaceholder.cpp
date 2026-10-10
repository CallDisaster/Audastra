#include "ScriptJapiPlaceholder.h"

#include "War3MapCore/Hash.h"
#include "../Jass/JassLexer.h"
#include "../Jass/JassParser.h"
#include "../Jass/PpLexer.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>

namespace War3Map::Script::Detail {
namespace {
using namespace Jass::detail;
constexpr std::size_t kMaxText = 16u * 1024u * 1024u;
constexpr std::size_t kMaxFunctions = 65536;
constexpr std::size_t kMaxIncludes = 4096;
constexpr std::size_t kMaxCollisions = 256;
constexpr std::size_t kMaxOwnedScanBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxConditionalDepth = 256;
constexpr std::size_t kMaxConditionalBlocks = 65536;
using Outcome = Result<JapiPlaceholderResult>;

struct Range { std::size_t begin = 0, end = 0; };
struct Definition {
    std::string name;
    std::vector<std::string> types, names;
    std::string returns;
    Range range, body;
    std::uint32_t firstLine = 0, lastLine = 0;
    bool global = false, plain = false, marker = false;
};
struct Scan {
    std::vector<Definition> definitions;
    std::vector<std::string> natives;
    std::vector<Range> directives, templates;
    std::string error;
};

Outcome Fail(std::string code, std::string message) {
    return Outcome::Failure("Script.JapiPlaceholder", std::move(code), std::move(message));
}
void SetError(Scan& scan, std::string message) {
    if (scan.error.empty()) scan.error = std::move(message);
}
void Mask(std::string& text, Range range) {
    for (auto i = range.begin; i < range.end; ++i) if (text[i] != '\r' && text[i] != '\n') text[i] = ' ';
}
bool Overlaps(Range a, Range b) { return a.begin < b.end && b.begin < a.end; }
auto FirstOverlap(const std::vector<Range>& ranges, Range target) {
    return std::lower_bound(ranges.begin(), ranges.end(), target.begin,
        [](Range range, std::size_t begin) { return range.end <= begin; });
}
bool HasOverlap(const std::vector<Range>& ranges, Range target) {
    const auto at = FirstOverlap(ranges, target);
    return at != ranges.end() && Overlaps(*at, target);
}
std::string NormalizeEol(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            out += '\n';
        } else out += text[i];
    }
    return out;
}
std::size_t LineBegin(std::string_view text, std::size_t offset) {
    while (offset != 0 && text[offset - 1] != '\r' && text[offset - 1] != '\n') --offset;
    return offset;
}
std::size_t LineEnd(std::string_view text, std::size_t offset) {
    while (offset < text.size() && text[offset] != '\r' && text[offset] != '\n') ++offset;
    return offset;
}
std::uint32_t LineCount(std::string_view text) {
    std::uint32_t count = 1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
            ++count;
        } else if (text[i] == '\n') ++count;
    }
    return count;
}
std::string DefinitionText(std::string_view text, const Definition& definition) {
    return NormalizeEol(text.substr(definition.range.begin, definition.range.end - definition.range.begin));
}
bool SameAbi(const Definition& a, const Definition& b) {
    return a.types == b.types && a.returns == b.returns;
}
bool Modifier(std::string_view word) {
    return word == "private" || word == "public" || word == "static" || word == "constant"
        || word == "stub" || word == "readonly";
}
bool ScopeOpen(std::string_view word) {
    return word == "library" || word == "library_once" || word == "scope" || word == "struct"
        || word == "module" || word == "interface" || word == "globals";
}
std::string ScopeEnd(std::string_view word) {
    return word == "library_once" ? "endlibrary" : "end" + std::string(word);
}
bool ScopeClose(std::string_view word) {
    return word == "endlibrary" || word == "endscope" || word == "endstruct" || word == "endmodule"
        || word == "endinterface" || word == "endglobals";
}

// 只识别能够直接证明关闭的完整 #if 0 块；不求值宏或其它条件表达式。
// 使用真实PP同一条续接/词法链读取原字节，不能让模板mask或物理行注释制造伪指令。
void MaskRawDirectives(std::string_view source, std::string& view, Scan& scan) {
    auto spliced = SplicePpLines(source);
    if (!spliced.Succeeded()) { SetError(scan, "条件指令续接分析超过预算或失败"); return; }
    const auto& logical = *spliced.value;
    auto tokens = LexPp(logical.splices.empty() ? source : std::string_view(logical.text));
    logical.MapOrigins(tokens);
    const auto hasSplice = [&](Range range) {
        const auto at = std::lower_bound(logical.splices.begin(), logical.splices.end(), range.begin,
            [](const auto& splice, std::size_t begin) { return splice.physicalOffset + splice.length <= begin; });
        return at != logical.splices.end() && at->physicalOffset < range.end;
    };
    struct ConditionalFrame {
        std::size_t begin;
        bool literalFalse, alternative = false, sawElse = false;
    };
    std::vector<ConditionalFrame> conditionals;
    std::vector<Range> inactive;
    std::size_t blockCount = 0, overflowDepth = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (!tokens[i].Is("#") || !tokens[i].lineStart) continue;
        auto end = i + 1;
        while (end < tokens.size() && tokens[end].kind != PpKind::Newline && tokens[end].kind != PpKind::End) ++end;
        // 从#开始保留其前面的跨行块注释闭合符；换行仍保留在原位置。
        const Range range{tokens[i].offset, end < tokens.size() ? tokens[end].offset : source.size()};
        scan.directives.push_back(range);
        if (i + 1 < end) {
            const auto word = tokens[i + 1].text;
            if (word == "if" || word == "ifdef" || word == "ifndef") {
                if (++blockCount > kMaxConditionalBlocks) SetError(scan, "条件指令数量超过预算");
                if (overflowDepth || conditionals.size() >= kMaxConditionalDepth) {
                    ++overflowDepth;
                    SetError(scan, "条件指令嵌套超过预算");
                } else {
                    const bool literalFalse = word == "if" && end - i == 3
                        && tokens[i + 2].kind == PpKind::Number && tokens[i + 2].text == "0"
                        && !hasSplice(range) && !HasOverlap(scan.templates, range);
                    conditionals.push_back({range.begin, literalFalse});
                }
            } else if (word == "else" || word == "elif" || word == "elseif" || word == "endif") {
                if (word == "elseif") SetError(scan, "未支持的条件指令elseif不能授予关闭块证明");
                if (overflowDepth) {
                    if (word == "endif") --overflowDepth;
                } else if (conditionals.empty()) {
                    SetError(scan, "条件指令结束或分支标记不匹配");
                } else if (word == "endif") {
                    const auto frame = conditionals.back();
                    conditionals.pop_back();
                    const Range closed{frame.begin, range.end};
                    if (frame.literalFalse && !frame.alternative && end - i == 2
                        && !hasSplice(closed) && !HasOverlap(scan.templates, closed)
                        && inactive.size() < kMaxConditionalBlocks) inactive.push_back(closed);
                } else {
                    auto& frame = conditionals.back();
                    if (frame.sawElse) SetError(scan, "条件指令在else之后仍有同层分支");
                    frame.alternative = true;
                    frame.sawElse = word == "else";
                }
            }
        }
        i = end == 0 ? 0 : end - 1;
    }
    if (!conditionals.empty() || overflowDepth) SetError(scan, "文件结束时仍有未闭合条件指令");
    if (scan.error.empty()) scan.directives.insert(scan.directives.end(), inactive.begin(), inactive.end());
    // 关闭块也属于原始marker禁入范围；合并后保持FirstOverlap要求的有序非交叠不变量。
    std::sort(scan.directives.begin(), scan.directives.end(), [](Range a, Range b) { return a.begin < b.begin; });
    std::size_t merged = 0;
    for (const auto range : scan.directives) {
        if (merged && range.begin <= scan.directives[merged - 1].end)
            scan.directives[merged - 1].end = std::max(scan.directives[merged - 1].end, range.end);
        else scan.directives[merged++] = range;
    }
    scan.directives.resize(merged);
    for (const auto range : scan.directives) Mask(view, range);
}

// 只定位完整声明与作用域。函数头复用 Parser，绝不把 ParseProgram 的错误恢复当删除证据。
// 模板与 C 指令保留字节跨度；模板结果由第二阶段重新核对，不能把 mask 当实际求值。
Scan ScanDefinitions(std::string_view source, bool raw) {
    Scan scan;
    if (source.size() > kMaxText) { scan.error = "文本超过16MiB预算"; return scan; }
    std::string view(source);
    // 与现有 EvaluateTemplates 相同的边界；包括出现在注释/字符串中的标记。
    for (std::size_t at = 0; at < view.size();) {
        const auto begin = source.find("<?", at);
        if (begin == std::string_view::npos) break;
        const auto close = source.find("?>", begin + 2);
        if (close == std::string_view::npos) { scan.error = "模板标记未闭合"; return scan; }
        const Range range{begin, close + 2};
        scan.templates.push_back(range);
        Mask(view, range);
        at = range.end;
    }
    if (raw) MaskRawDirectives(source, view, scan);
    const auto tokens = LexJass(view, {true, true});
    std::vector<std::string> scopes;
    for (std::size_t i = 0; i < tokens.size();) {
        const auto& first = tokens[i];
        if (first.kind == TokenKind::End) break;
        if (first.kind == TokenKind::Newline) { ++i; continue; }
        if (first.kind == TokenKind::Pragma) {
            if (first.text.starts_with("//!")) SetError(scan, "后续vJass指令尚未展开，无法证明全局定义唯一性");
            ++i; continue;
        }
        if (first.kind == TokenKind::Invalid) SetError(scan, "作用域扫描遇到无法识别的记号");
        if (!AtLineStart(tokens, i)) { ++i; continue; }
        auto head = i;
        while (head < tokens.size() && Modifier(tokens[head].text)) ++head;
        const auto& token = tokens[head];
        const bool inInterface = !scopes.empty() && scopes.back() == "endinterface";
        if (inInterface && token.text != "method" && token.text != "endinterface")
            SetError(scan, "interface中只支持明确的一行method原型");
        if (first.text == "static" && token.Is(Keyword::If)) {
            scopes.push_back("endif");
        } else if (ScopeOpen(token.text)) {
            if (scopes.size() >= 256) { SetError(scan, "作用域嵌套超过预算"); break; }
            if (token.text == "interface" && (head + 2 >= tokens.size()
                || tokens[head + 1].kind != TokenKind::Identifier || !tokens[head + 2].EndsLine()))
                SetError(scan, "interface声明必须含明确名称且没有未知尾部");
            scopes.push_back(ScopeEnd(token.text));
        } else if (ScopeClose(token.text) || token.Is(Keyword::EndIf)) {
            if (token.text == "endinterface" && (head + 1 >= tokens.size() || !tokens[head + 1].EndsLine()))
                SetError(scan, "endinterface行含额外记号");
            if (scopes.empty() || scopes.back() != token.text) SetError(scan, "作用域结束标记不匹配");
            else scopes.pop_back();
        } else if (token.Is(Keyword::Native)) {
            if (head + 1 < tokens.size() && tokens[head + 1].kind == TokenKind::Identifier)
                scan.natives.emplace_back(tokens[head + 1].text);
            else SetError(scan, "native声明缺少名称");
        } else if (token.Is(Keyword::Function) || token.text == "method") {
            const bool method = token.text == "method";
            if (method && inInterface) {
                // interface方法是无body的一行原型，不寻找endmethod，也不登记为全局实现。
                SyntaxPool pool;
                std::vector<SyntaxError> errors;
                Parser parser(tokens, pool, errors);
                parser.Seek(head + 1);
                std::string_view name, returns;
                std::vector<Param> params;
                bool valid = head == i && parser.Name(name, "方法名") && parser.Accept(Keyword::Takes)
                    && parser.Params(params) && parser.Accept(Keyword::Returns) && parser.ReturnType(returns);
                if (valid && parser.AcceptWord("defaults")) valid = parser.Accept(Keyword::Nothing);
                valid = valid && parser.AtLineEnd();
                if (!valid || !errors.empty()) SetError(scan, "interface方法原型无法严格解析；仅支持可选defaults nothing");
                while (i < tokens.size() && !tokens[i].EndsLine()) ++i;
                continue;
            }
            if (method) {
                const auto owner = std::find_if(scopes.rbegin(), scopes.rend(), [](const auto& scope) { return scope != "endif"; });
                if (owner == scopes.rend() || (*owner != "endstruct" && *owner != "endmodule"))
                    SetError(scan, "带body的方法必须属于struct或module，不能位于全局或其它未知作用域");
            }
            Definition definition;
            definition.firstLine = first.line;
            definition.range.begin = LineBegin(source, first.offset);
            definition.global = scopes.empty();
            definition.plain = !method && head == i;
            // 删除函数不能使前面的pjass pragma改为附着到下一个函数。
            auto previous = i;
            while (previous != 0 && tokens[previous - 1].kind == TokenKind::Newline) --previous;
            if (previous != 0 && tokens[previous - 1].kind == TokenKind::Pragma) definition.plain = false;
            std::size_t body = head + 1;
            if (!method) {
                SyntaxPool pool;
                std::vector<SyntaxError> errors;
                Parser parser(tokens, pool, errors);
                parser.Seek(head + 1);
                std::string_view name, returns;
                std::vector<Param> params;
                const bool valid = parser.Name(name, "函数名") && parser.Accept(Keyword::Takes)
                    && parser.Params(params) && parser.Accept(Keyword::Returns) && parser.ReturnType(returns)
                    && parser.AtLineEnd();
                if (!valid || !errors.empty()) SetError(scan, "函数头无法严格解析");
                definition.name = name;
                definition.returns = returns;
                for (const auto& param : params) { definition.types.emplace_back(param.type); definition.names.emplace_back(param.name); }
                body = parser.Position();
            } else {
                while (body < tokens.size() && !tokens[body].EndsLine()) ++body;
            }
            auto end = body;
            for (; end < tokens.size() && tokens[end].kind != TokenKind::End; ++end) {
                if (!AtLineStart(tokens, end)) continue;
                const auto word = tokens[end].text;
                if (word == (method ? "endmethod" : "endfunction")) break;
                if (word == "function" || word == "method" || ScopeOpen(word) || ScopeClose(word)) {
                    SetError(scan, "函数结束之前出现其它声明，不能恢复后继续删除");
                    break;
                }
            }
            if (end >= tokens.size() || tokens[end].text != (method ? "endmethod" : "endfunction")) {
                SetError(scan, "函数缺少完整结束边界");
                i = std::max(end, i + 1); continue;
            }
            if (end + 1 < tokens.size() && !tokens[end + 1].EndsLine()) SetError(scan, "函数结束行含额外记号");
            definition.lastLine = tokens[end].line;
            definition.range.end = LineEnd(source, tokens[end].offset);
            definition.body = {tokens[body].offset, tokens[end].offset};
            std::size_t significant = 0;
            bool onlyMarker = true;
            for (auto at = body; at < end; ++at) {
                if (tokens[at].kind == TokenKind::Newline) continue;
                ++significant;
                onlyMarker = onlyMarker && tokens[at].kind == TokenKind::Identifier && tokens[at].text == "JapiPlaceHolder";
                if (tokens[at].kind == TokenKind::Invalid) SetError(scan, "函数体含无法识别的记号");
                if (tokens[at].kind == TokenKind::Pragma && tokens[at].text.starts_with("//!"))
                    SetError(scan, "函数中包含未展开vJass指令");
            }
            const bool hidden = HasOverlap(scan.directives, definition.range) || HasOverlap(scan.templates, definition.range);
            definition.marker = significant == 1 && onlyMarker && !hidden;
            if (!method) {
                if (scan.definitions.size() == kMaxFunctions) { SetError(scan, "函数数量超过预算"); break; }
                scan.definitions.push_back(std::move(definition));
            }
            i = end + 1; continue;
        }
        while (i < tokens.size() && !tokens[i].EndsLine()) ++i;
    }
    if (!scopes.empty()) SetError(scan, "文件结束时仍有未闭合作用域");
    return scan;
}

// 每次扫描只建一次索引。marker完整文本也只规范化一次；重复名称不构造候选笛卡尔积。
struct DefinitionIndex {
    std::map<std::string, std::vector<const Definition*>, std::less<>> names;
    std::set<std::string, std::less<>> natives;
    std::map<std::uint32_t, const Definition*> lines;
    std::map<std::string, const Definition*, std::less<>> markersByText;
    std::map<std::uint32_t, std::string> markerTextByLine;
};
DefinitionIndex Index(std::string_view text, const Scan& scan) {
    DefinitionIndex index;
    for (const auto& definition : scan.definitions) {
        index.names[definition.name].push_back(&definition);
        index.lines.emplace(definition.firstLine, &definition);
        if (definition.marker) {
            auto full = DefinitionText(text, definition);
            index.markersByText.emplace(full, &definition);
            index.markerTextByLine.emplace(definition.firstLine, std::move(full));
        }
    }
    for (const auto& native : scan.natives) index.natives.insert(native);
    return index;
}
std::span<const Definition* const> Named(const DefinitionIndex& index, std::string_view name) {
    const auto found = index.names.find(name);
    if (found == index.names.end()) return {};
    return {found->second.data(), found->second.size()};
}
bool HasNative(const DefinitionIndex& index, std::string_view name) { return index.natives.contains(name); }
bool Collides(const DefinitionIndex& index, std::string_view name) {
    const auto found = Named(index, name);
    return found.size() > 1 || (!found.empty() && HasNative(index, name));
}
struct OwnedScan {
    std::string_view text;
    Scan scan;
    DefinitionIndex index;
    std::uint32_t lines;
    explicit OwnedScan(std::string_view source)
        : text(source), scan(ScanDefinitions(source, true)), index(Index(source, scan)), lines(LineCount(source)) {}
};
bool MapsTo(const Jass::SourceMap& map, const Definition& output, std::string_view file,
    const Definition& source, std::uint32_t sourceLines) {
    const auto& files = map.Files();
    for (auto line = output.firstLine; line <= output.lastLine; ++line) {
        const auto origin = map.Find(line);
        if (!origin || origin->file >= files.size() || files[origin->file] != file || origin->line < source.firstLine
            || origin->line > source.lastLine || origin->line > sourceLines) return false;
        if (line == output.firstLine && origin->line != source.firstLine) return false;
        if (line == output.lastLine && origin->line != source.lastLine) return false;
    }
    return true;
}
bool SameHeader(const Definition& output, const Definition& source) {
    return output.name == source.name && SameAbi(output, source) && output.names == source.names;
}
bool PureNewlineTemplate(std::string_view block) {
    // 与EvaluateTemplates相同：只有紧跟<?的=才是表达式；字面量内部的字节绝不能归一化。
    if (!block.starts_with("<?=") || !block.ends_with("?>")) return false;
    auto expression = block.substr(3, block.size() - 5);
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    const auto first = expression.find_first_not_of(whitespace);
    if (first == std::string_view::npos) return false;
    const auto last = expression.find_last_not_of(whitespace);
    expression = expression.substr(first, last - first + 1);
    return expression == "'\\n'" || expression == "\"\\n\"";
}
bool SafeRemovedTemplates(std::string_view text, const Scan& scan, Range removed) {
    for (auto at = FirstOverlap(scan.templates, removed); at != scan.templates.end() && Overlaps(*at, removed); ++at) {
        const auto range = *at;
        if (range.begin < removed.begin || range.end > removed.end) return false;
        // 唯一允许从 stub 中一同删掉的模板是无副作用的换行字面量。
        if (!PureNewlineTemplate(text.substr(range.begin, range.end - range.begin))) return false;
    }
    return true;
}
bool DefaultStubExpansion(std::string_view text, const Scan& scan, const Definition& stub) {
    if (!SafeRemovedTemplates(text, scan, stub.range)) return false;
    std::string body(text.substr(stub.body.begin, stub.body.end - stub.body.begin));
    for (auto at = FirstOverlap(scan.templates, stub.range); at != scan.templates.end() && Overlaps(*at, stub.range); ++at) {
        if (at->begin < stub.body.begin || at->end > stub.body.end) return false;
        if (!PureNewlineTemplate(text.substr(at->begin, at->end - at->begin))) return false;
        Mask(body, {at->begin - stub.body.begin, at->end - stub.body.begin});
        // 已确认字面量确实生成换行；不能用空格掩盖换行终止//注释之后可能露出的语句。
        body[at->begin - stub.body.begin] = '\n';
    }
    const auto tokens = LexJass(body, {true, true});
    constexpr std::array<std::string_view, 6> expected = {"call", "ConvertRace", "(", "0", ")", "return"};
    constexpr std::array<TokenKind, 6> kinds = {TokenKind::Keyword, TokenKind::Identifier, TokenKind::Operator,
        TokenKind::Integer, TokenKind::Operator, TokenKind::Keyword};
    std::size_t found = 0;
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::End || token.kind == TokenKind::Newline) continue;
        if (found == expected.size() || token.text != expected[found] || token.kind != kinds[found]) return false;
        ++found;
    }
    return found == expected.size();
}
struct ActiveCandidate {
    const Definition* generated = nullptr;
    const Definition* input = nullptr;
    const Definition* output = nullptr;
};
std::optional<ActiveCandidate> MappedCandidate(const JapiPlaceholderRequest& request,
    const DefinitionIndex& generated, const DefinitionIndex& input, const Definition& output) {
    if (!request.sourceMap) return std::nullopt;
    const auto& files = request.sourceMap->Files();
    const auto origin = request.sourceMap->Find(output.firstLine);
    if (!origin || origin->file >= files.size() || files[origin->file] != request.mainSourceName) return std::nullopt;
    const auto raw = input.lines.find(origin->line);
    const auto identity = input.markerTextByLine.find(origin->line);
    if (raw == input.lines.end() || identity == input.markerTextByLine.end() || raw->second->name != output.name) return std::nullopt;
    const auto original = generated.markersByText.find(identity->second);
    if (original == generated.markersByText.end()) return std::nullopt;
    return ActiveCandidate{original->second, raw->second, &output};
}
std::string PathText(const std::filesystem::path& path) {
    const auto utf8 = path.lexically_normal().generic_u8string();
    return {utf8.begin(), utf8.end()};
}
} // namespace

bool HasActiveJapiPlaceholders(const JapiPlaceholderRequest& request) {
    if (request.generated.find("JapiPlaceHolder") == std::string_view::npos || !request.sourceMap || request.sourceMap->Empty()
        || request.generated.size() > kMaxText || request.preprocessorInput.size() > kMaxText || request.preprocessed.size() > kMaxText)
        return false;
    const auto generated = ScanDefinitions(request.generated, true);
    if (!generated.error.empty()) return false;
    const auto input = ScanDefinitions(request.preprocessorInput, true);
    const auto pp = ScanDefinitions(request.preprocessed, false);
    if (!input.error.empty() || !pp.error.empty()) return false;
    const auto generatedIndex = Index(request.generated, generated);
    const auto inputIndex = Index(request.preprocessorInput, input);
    const auto ppIndex = Index(request.preprocessed, pp);
    const auto inputLines = LineCount(request.preprocessorInput);
    for (const auto& [name, definitions] : ppIndex.names) {
        if (!Collides(ppIndex, name)) continue;
        for (const auto* definition : definitions) {
            const auto mapped = MappedCandidate(request, generatedIndex, inputIndex, *definition);
            if (!mapped) continue;
            const auto& candidate = *mapped->generated;
            const auto& raw = *mapped->input;
            if (candidate.global && candidate.plain && raw.global && raw.plain && definition->global && definition->plain
                && SameHeader(*definition, raw) && MapsTo(*request.sourceMap, *definition, request.mainSourceName, raw, inputLines)) return true;
        }
    }
    return false;
}

Result<JapiPlaceholderResult> ReconcileJapiPlaceholders(const JapiPlaceholderRequest& request) {
    JapiPlaceholderResult output{std::string(request.preprocessed), {}};
    if (request.generated.find("JapiPlaceHolder") == std::string_view::npos) return Outcome::Success(std::move(output));
    if (request.generated.size() > kMaxText || request.preprocessorInput.size() > kMaxText || request.preprocessed.size() > kMaxText)
        return Fail("JAPI_PLACEHOLDER_BUDGET", "占位协调文本超过16MiB预算。");
    const auto generated = ScanDefinitions(request.generated, true);
    const auto generatedIndex = Index(request.generated, generated);
    if (generatedIndex.markersByText.empty()) return Outcome::Success(std::move(output));
    const auto input = ScanDefinitions(request.preprocessorInput, true);
    const auto pp = ScanDefinitions(request.preprocessed, false);
    const auto inputIndex = Index(request.preprocessorInput, input);
    const auto ppIndex = Index(request.preprocessed, pp);
    std::map<std::string, ActiveCandidate, std::less<>> active;
    for (const auto& [name, definitions] : ppIndex.names) {
        // 单定义runtime hook和未实际输出的候选不是本服务的替换对象。
        if (!Collides(ppIndex, name)) continue;
        if (!request.sourceMap || request.sourceMap->Empty()) {
            const auto original = Named(generatedIndex, name);
            if (std::any_of(original.begin(), original.end(), [](const auto* definition) { return definition->marker; }))
                return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "占位碰撞候选没有内置PP来源映射。");
            continue;
        }
        for (const auto* definition : definitions) {
            const auto mapped = MappedCandidate(request, generatedIndex, inputIndex, *definition);
            if (!mapped) continue;
            if (Named(inputIndex, name).size() != 1 || Named(generatedIndex, name).size() != 1 || definitions.size() != 2
                || HasNative(inputIndex, name) || HasNative(generatedIndex, name) || HasNative(ppIndex, name))
                return Fail("JAPI_PLACEHOLDER_AMBIGUOUS", "占位碰撞不是唯一的一对，或存在native冲突：" + name);
            if (active.size() == kMaxCollisions) return Fail("JAPI_PLACEHOLDER_BUDGET", "占位碰撞名称超过256项预算。");
            if (!active.emplace(name, *mapped).second)
                return Fail("JAPI_PLACEHOLDER_AMBIGUOUS", "同名活跃占位输出不唯一：" + name);
        }
    }
    // 未碰撞的旧stub、未知作用域中的单stub及#if 0候选均保持原字节，交原编译器处理。
    if (active.empty()) return Outcome::Success(std::move(output));
    const auto& map = *request.sourceMap;
    const auto& files = map.Files();
    if (!generated.error.empty() || !input.error.empty() || !pp.error.empty()) return Fail("JAPI_PLACEHOLDER_STRUCTURE_UNSUPPORTED",
        "不能严格核定全局作用域：" + (!generated.error.empty() ? generated.error : !input.error.empty() ? input.error : pp.error));
    if (request.includes.size() > kMaxIncludes) return Fail("JAPI_PLACEHOLDER_BUDGET", "实际包含来源数量超过预算。");
    std::map<std::string, const JapiPlaceholderInclude*> includes;
    for (const auto& include : request.includes) {
        if (!include.path.is_absolute() || include.text.size() > kMaxText || include.sha256.empty()
            || !includes.emplace(PathText(include.path), &include).second)
            return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "实际包含来源缺失、重复或超过预算。");
    }
    std::vector<Range> removals;
    const auto inputLines = LineCount(request.preprocessorInput);
    std::map<std::string, std::unique_ptr<OwnedScan>, std::less<>> ownedScans;
    std::size_t ownedScanBytes = 0;
    for (const auto& [name, mapped] : active) {
        const auto* candidate = mapped.generated;
        const auto* stub = mapped.output;
        const auto& rawStub = *mapped.input;
        const auto ppDefs = Named(ppIndex, name);
        if (!candidate->global || !candidate->plain || !rawStub.global || !rawStub.plain || !stub->global || !stub->plain)
            return Fail("JAPI_PLACEHOLDER_SCOPE", "占位定义不属于可核定的普通全局函数：" + candidate->name);
        if (!SameHeader(*stub, rawStub) || !MapsTo(map, *stub, request.mainSourceName, rawStub, inputLines))
            return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "占位完整跨度或签名无法核定：" + candidate->name);
        if (!DefaultStubExpansion(request.preprocessed, pp, *stub)) return Fail("JAPI_PLACEHOLDER_EXPANSION_UNSUPPORTED",
            "裸marker的实际展开不是已确认的默认占位正文，未删除任何函数：" + candidate->name);
        const auto* real = ppDefs[0] == stub ? ppDefs[1] : ppDefs[0];
        if (!real->global || !real->plain || real->marker)
            return Fail("JAPI_PLACEHOLDER_SCOPE", "替代定义不是普通全局非占位实现：" + candidate->name);
        if (!SameAbi(*candidate, *real)) return Fail("JAPI_PLACEHOLDER_ABI_MISMATCH", "替代实现的参数类型顺序或返回类型不同：" + candidate->name);
        const auto origin = map.Find(real->firstLine);
        if (!origin || origin->file >= files.size()) return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "替代实现缺少来源：" + candidate->name);
        const auto includeIt = includes.find(files[origin->file]);
        if (includeIt == includes.end()) return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "替代实现不来自本次实际包含快照：" + candidate->name);
        const auto& include = *includeIt->second;
        auto cached = ownedScans.find(files[origin->file]);
        if (cached == ownedScans.end()) {
            if (include.text.size() > kMaxOwnedScanBytes - ownedScanBytes)
                return Fail("JAPI_PLACEHOLDER_BUDGET", "实际实现来源累计扫描超过64MiB预算。");
            ownedScanBytes += include.text.size();
            const auto hash = Sha256Text(include.text);
            if (!hash.Succeeded() || *hash.value != include.sha256) return Fail("JAPI_PLACEHOLDER_SOURCE_CHANGED", "包含快照SHA不匹配：" + candidate->name);
            // PP 对UTF-8 BOM只剥离字节，不增加行号；SHA仍覆盖完整原文件。每个来源只验证/扫描一次。
            std::string_view includeText = include.text;
            if (includeText.starts_with("\xef\xbb\xbf")) includeText.remove_prefix(3);
            cached = ownedScans.emplace(files[origin->file], std::make_unique<OwnedScan>(includeText)).first;
        }
        const auto& owned = *cached->second;
        const auto sourceDefs = Named(owned.index, candidate->name);
        if (!owned.scan.error.empty() || sourceDefs.size() != 1 || HasNative(owned.index, candidate->name))
            return Fail("JAPI_PLACEHOLDER_SOURCE_UNKNOWN", "实际包含中无法核定唯一原始定义：" + candidate->name);
        const auto& source = *sourceDefs.front();
        if (!source.global || !source.plain || source.marker || !SameHeader(*real, source)
            || !MapsTo(map, *real, files[origin->file], source, owned.lines)
            || DefinitionText(request.preprocessed, *real) != DefinitionText(owned.text, source))
            return Fail("JAPI_PLACEHOLDER_IMPLEMENTATION_UNSUPPORTED", "替代实现不是同源完整原始函数，或含本轮未支持的宏/模板改写：" + candidate->name);
        output.retained.push_back({candidate->name, candidate->types, candidate->returns,
            DefinitionText(request.preprocessed, *real), include.path, include.sha256, source.firstLine,
            candidate->firstLine, stub->firstLine, candidate->names != real->names});
        removals.push_back(stub->range);
    }
    for (const auto range : removals) Mask(output.text, range);
    auto result = Outcome::Success(std::move(output));
    for (const auto& retained : result.value->retained) if (retained.parameterNamesDiffer) {
        result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.JapiPlaceholder", "JAPI_PLACEHOLDER_PARAMETER_NAMES_DIFFER",
            "已核对类型与顺序；" + retained.name + "的形式参数名不同，调用点实参保持原顺序。", {}});
    }
    return result;
}

Status VerifyJapiPlaceholderResult(std::string_view text, std::span<const JapiPlaceholderRetained> retained) {
    if (retained.empty()) return Status::Ok();
    const auto fail = [](std::string message) { return Status::Failure("Script.JapiPlaceholder", "JAPI_PLACEHOLDER_TEMPLATE_CHANGED", std::move(message)); };
    if (text.size() > kMaxText || retained.size() > kMaxCollisions) return fail("模板后验证超过预算。");
    const auto scan = ScanDefinitions(text, false);
    if (!scan.error.empty() || !scan.templates.empty()) return fail("模板后无法核定完整声明与作用域：" + scan.error);
    const auto index = Index(text, scan);
    std::set<std::string> names;
    for (const auto& expected : retained) {
        const auto found = Named(index, expected.name);
        if (!names.insert(expected.name).second || found.size() != 1 || HasNative(index, expected.name))
            return fail("模板后实现不再唯一或存在native冲突：" + expected.name);
        const auto& actual = *found.front();
        if (!actual.global || !actual.plain || actual.marker || actual.types != expected.parameterTypes || actual.returns != expected.returnType
            || DefinitionText(text, actual) != expected.definition)
            return fail("模板改变了已核定实现的作用域、签名或完整内容：" + expected.name);
    }
    return Status::Ok();
}

} // namespace War3Map::Script::Detail
