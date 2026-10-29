#include "War3MapCore/Script/ScriptPreprocessSources.h"

#include "ScriptSourceBinding.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include "../Jass/PpLexer.h"
#include "../Jass/JassCheckBudget.h"

#include <Windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <exception>
#include <map>
#include <set>

namespace War3Map::Script {
namespace {
using Kind = ScriptToolchainBindingKind;
using ContentResult = Result<Jass::PreprocessIncludeContent>;
constexpr std::uint64_t kFileBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kTotalBytes = 32ull * 1024 * 1024;
constexpr std::size_t kCandidates = 4096, kFiles = 512;
constexpr std::size_t kMainBytes = 8 * 1024 * 1024, kOutputBytes = 16 * 1024 * 1024;
struct PreparedMain {
    std::string text;
    MainTransformReport report;
};
std::string_view Word(std::string_view& text) {
    text = Text::TrimHorizontal(text);
    const auto end = text.find_first_of(" \t=");
    const auto word = text.substr(0, end);
    text.remove_prefix(word.size());
    return word;
}
bool NameByte(unsigned char c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
// 同一个线性字节匹配器供碰撞检查和替换使用，长共前缀也受累计扫描量约束。
class LiteralMatcher {
public:
    explicit LiteralMatcher(std::string_view pattern) : pattern_(pattern), prefix_(pattern.size()) {
        for (std::size_t i = 1, matched = 0; i < pattern.size(); ++i) {
            while (matched && pattern[i] != pattern[matched]) matched = prefix_[matched - 1];
            if (pattern[i] == pattern[matched]) ++matched;
            prefix_[i] = matched;
        }
    }
    [[nodiscard]] std::size_t Find(std::string_view text, std::size_t from = 0) const {
        for (std::size_t i = from, matched = 0; i < text.size(); ++i) {
            while (matched && text[i] != pattern_[matched]) matched = prefix_[matched - 1];
            if (text[i] == pattern_[matched]) ++matched;
            if (matched == pattern_.size()) return i + 1 - matched;
        }
        return std::string_view::npos;
    }
private:
    std::string_view pattern_; // 仅接收已验证的非空原名/生成名，生命周期覆盖当前扫描。
    std::vector<std::size_t> prefix_;
};
bool SimpleValue(std::string_view text) {
    if (text == "null" || text == "true" || text == "false") return true;
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        bool escaped = false;
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            if (!escaped && text[i] == '"') return false;
            escaped = !escaped && text[i] == '\\';
        }
        return !escaped;
    }
    if (text.size() == 6 && text.front() == '\'' && text.back() == '\'') return text.substr(1, 4).find_first_of("'\\") == std::string_view::npos;
    if (text.starts_with("0x") || text.starts_with("$")) {
        text.remove_prefix(text.front() == '$' ? 1 : 2);
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    if (text.empty() || (text.front() != '-' && text.front() != '.' && (text.front() < '0' || text.front() > '9'))) return false;
    double value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value);
}
Result<PreparedMain> PrepareMain(const ScriptPreprocessSourceRequest& request) {
    using R = Result<PreparedMain>;
    const auto fail = [](std::string code, std::string message, std::uint32_t line = 0) {
        return R::Failure("Script.PreprocessSources", std::move(code), "主文行 " + std::to_string(line) + "：" + std::move(message));
    };
    if (request.mainPolicy != ScriptPreprocessMainPolicy::None && request.mainPolicy != ScriptPreprocessMainPolicy::LegacyGlobalBytesSubset) {
        return fail("SCRIPT_MAIN_POLICY_INVALID", "未知主文件前置策略。");
    }
    if (request.mainText.size() > kMainBytes) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "原主文超过8MiB。");
    auto hash = Sha256Text(request.mainText);
    if (!hash.Succeeded()) return {{}, std::move(hash.diagnostics)};
    PreparedMain prepared;
    auto& report = prepared.report;
    report.policy = request.mainPolicy;
    report.inputSha256 = report.outputSha256 = std::move(*hash.value);
    report.inputBytes = report.outputBytes = request.mainText.size();
    if (request.mainPolicy == ScriptPreprocessMainPolicy::None) return R::Success(std::move(prepared));
    const auto& main = request.mainText;
    if (main.empty() || main.back() != '\n' || main.starts_with("\xEF\xBB\xBF") || main.find('\0') != std::string::npos) {
        return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "子集要求无BOM/NUL且以LF或CRLF结束；不修正未知原文本。");
    }
    std::set<std::string_view> names;
    std::size_t nameBytes = 0;
    bool opened = false, seen = false, closed = false;
    std::uint32_t lineNumber = 0;
    constexpr std::string_view types = " integer real boolean string code handle agent event player widget unit destructable item ability buff force group trigger triggercondition triggeraction timer location region rect boolexpr sound conditionfunc filterfunc unitpool itempool race alliancetype racepreference gamestate igamestate fgamestate playerstate playerscore playergameresult unitstate aidifficulty eventid gameevent playerevent playerunitevent unitevent limitop widgetevent dialogevent unittype gamespeed gamedifficulty gametype mapflag mapvisibility mapsetting mapdensity mapcontrol playerslotstate volumegroup camerafield camerasetup playercolor placement startlocprio raritycontrol blendmode texmapflags effect effecttype weathereffect terraindeformation fogstate fogmodifier dialog button quest questitem defeatcondition timerdialog leaderboard multiboard multiboarditem trackable gamecache version itemtype texttag attacktype damagetype weapontype soundtype lightning pathingtype image ubersplat hashtable ";
    for (std::size_t offset = 0; offset < main.size();) {
        const auto end = main.find('\n', offset);
        auto raw = std::string_view(main).substr(offset, end - offset);
        offset = end + 1; ++lineNumber;
        if (raw.ends_with('\r')) raw.remove_suffix(1);
        if (raw.find('\r') != std::string_view::npos) return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "内部CR/CR-only不在声明子集。", lineNumber);
        auto rest = raw;
        const auto first = Word(rest);
        if (first == "globals" || first == "endglobals") {
            if (raw != first || (first == "globals" ? seen : !opened)) return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "全局边界必须列0且唯一，不能缩进、嵌套或附加文本。", lineNumber);
            opened = first == "globals";
            if (opened) seen = true; else closed = true;
            continue;
        }
        if (!opened || first.empty() || first.starts_with("//")) continue;
        if (first.starts_with("#") && raw.find("udg_") == std::string_view::npos) continue;
        auto type = first;
        const bool constant = type == "constant";
        if (constant) type = Word(rest);
        auto name = Word(rest);
        const bool array = name == "array";
        if (array) name = Word(rest);
        rest = Text::TrimHorizontal(rest);
        const bool nonAscii = std::any_of(name.begin(), name.end(), [](unsigned char c) { return c >= 0x80; });
        if (name.empty() || name.size() > 4096 || !std::all_of(name.begin(), name.end(), [&](unsigned char c) {
                return NameByte(c) || (nonAscii && c >= 0x40 && c != 0x7f); })) {
            return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "不是可确认的简单全局声明。", lineNumber);
        }
        if (!name.starts_with("udg_")) {
            if (raw.find("udg_") != std::string_view::npos || nonAscii) {
                return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "非udg声明含待解释udg文本或非ASCII名。", lineNumber);
            }
            continue; // 正常ASCII gg_*及其他非udg全局保持原文；不是完整类型/作用域检查。
        }
        if (name.size() == 4 || (constant && (array || rest.empty())) || types.find(" " + std::string(type) + " ") == std::string_view::npos ||
            raw.find("//") != std::string_view::npos || raw.find("/*") != std::string_view::npos || raw.find("*/") != std::string_view::npos ||
            (!rest.empty() && (array || !rest.starts_with("=") || !SimpleValue(Text::TrimHorizontal(rest.substr(1)))))) {
            return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "udg声明类型、初始化或注释界符不在已确认子集。", lineNumber);
        }
        if (names.size() >= 4096) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "udg声明超过4096。", lineNumber);
        if (!names.insert(name).second) return fail("SCRIPT_MAIN_TRANSFORM_COLLISION", "重复udg声明比旧算法更严格地拒绝。", lineNumber);
        if (nonAscii) {
            if (report.names.size() >= 256 || name.size() > 64 * 1024 - nameBytes) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "变换名数量或原名合计超过预算。", lineNumber);
            nameBytes += name.size();
            report.names.push_back({std::string(name), {}, lineNumber, 0});
        }
    }
    if (!seen || !closed) return fail("SCRIPT_MAIN_TRANSFORM_UNSUPPORTED", "要求唯一完整globals/endglobals块。");
    if (report.names.empty()) return R::Success(std::move(prepared));
    std::uint64_t searched = 0, hits = 0;
    const auto charge = [&](std::size_t bytes) { constexpr auto limit = 512ull * 1024 * 1024; if (bytes > limit - searched) return false; searched += bytes; return true; };
    const auto width = std::to_string(names.size()).size();
    for (std::size_t index = 0; index < report.names.size(); ++index) {
        auto& name = report.names[index];
        name.replacement = "xgdg_";
        auto value = index;
        for (std::size_t digit = 0; digit < width; ++digit, value /= 52) {
            const auto n = value % 52;
            name.replacement += static_cast<char>(n % 2 == 0 ? 'A' + n / 2 : 'z' - n / 2);
        }
        if (!charge(main.size() + name.replacement.size())) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "累计扫描超过512MiB。");
        if (LiteralMatcher(name.replacement).Find(main) != std::string::npos) return fail("SCRIPT_MAIN_TRANSFORM_COLLISION", "生成名与原全脚本文字碰撞；不自动避让。");
    }
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < report.names.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return report.names[a].original.size() > report.names[b].original.size(); });
    prepared.text = main;
    for (const auto index : order) {
        auto& name = report.names[index];
        if (!charge(prepared.text.size() + name.original.size())) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "累计扫描超过512MiB。");
        const LiteralMatcher matcher(name.original);
        std::string replaced;
        const auto append = [&](std::string_view part) { if (part.size() > kOutputBytes - replaced.size()) return false; replaced.append(part); return true; };
        std::size_t from = 0, found = 0;
        while ((found = matcher.Find(prepared.text, from)) != std::string::npos) {
            if (hits >= 65536) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "整段替换命中超过65536。");
            if (!append(std::string_view(prepared.text).substr(from, found - from)) || !append(name.replacement)) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "转换输出超过16MiB。");
            ++hits; ++name.occurrences; from = found + name.original.size();
        }
        if (!append(std::string_view(prepared.text).substr(from))) return fail("SCRIPT_MAIN_TRANSFORM_BUDGET", "转换输出超过16MiB。");
        prepared.text = std::move(replaced);
    }
    auto outputHash = Sha256Text(prepared.text);
    if (!outputHash.Succeeded()) return {{}, std::move(outputHash.diagnostics)};
    report.outputSha256 = std::move(*outputHash.value); report.outputBytes = prepared.text.size(); report.wholeTextReplacement = true;
    auto result = R::Success(std::move(prepared));
    result.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.PreprocessSources", "SCRIPT_MAIN_TRANSFORM_WHOLE_TEXT",
        "仅转换主文声明子集；整段字节替换包含字符串和注释，不证明include/宏生成名、作用域或完整旧执行链。", {}});
    return result;
}
struct AccessBudget {
    std::size_t visits = 0;
    std::uint64_t bytes = 0;
    bool Consume(std::uint64_t amount) {
        constexpr std::uint64_t limit = 64ull * 1024 * 1024;
        if (++visits > 16384 || amount > limit - bytes) return false;
        bytes += amount;
        return true;
    }
};

struct PathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.native().size()),
            b.c_str(), static_cast<int>(b.native().size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool Within(const std::filesystem::path& candidate, const std::filesystem::path& root) {
    auto part = candidate.begin();
    for (const auto& expected : root) {
        if (part == candidate.end() || PathLess{}(*part, expected) || PathLess{}(expected, *part)) return false;
        ++part;
    }
    return true;
}
std::string Text(const std::vector<std::byte>& bytes) {
    return bytes.empty() ? std::string{} : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
bool FeaturePolicyMatches(const ScriptUserSourcePlan& plan, ScriptFrameworkDirectivePolicy policy) {
    if (policy == ScriptFrameworkDirectivePolicy::None) return plan.profileDirectives.empty();
    const std::vector<ScriptProfileDirective> expected{ScriptProfileDirective::Def, ScriptProfileDirective::Ifdef,
        ScriptProfileDirective::Else, ScriptProfileDirective::Endif, ScriptProfileDirective::Include};
    return policy == ScriptFrameworkDirectivePolicy::FeatureFlagsV1 && !plan.profileIdentitySha256.empty() &&
        plan.profileDirectiveDialectVersion == "ydwe-dollar-features-v1" && plan.profileDirectives == expected;
}
struct FrameworkPhaseResult {
    std::string text;
    Jass::SourceLexicalView lexicalView;
    Jass::SourceMap sourceMap;
    std::vector<std::string> flags;
};
// 这是框架公开规则的原创有界特性子集；不是 HC 的全文字符串替换实现。
// $include 独立于 C 条件先展开，所有无值 $def 再登记，最后选择 $ifdef 分支。
class FrameworkFeatures {
public:
    explicit FrameworkFeatures(const Jass::PreprocessOptions& options) : options_(options), budget_(options.check) {}
    Result<FrameworkPhaseResult> Run(std::string_view text, const std::filesystem::path& source) {
        try {
            return RunChecked(text, source);
        } catch (const Jass::detail::CheckAborted& interrupted) {
            const auto code = interrupted.Cause() == Jass::detail::CheckFailure::Cancelled
                ? "SCRIPT_FRAMEWORK_CHECK_CANCELLED" : interrupted.Cause() == Jass::detail::CheckFailure::Deadline
                ? "SCRIPT_FRAMEWORK_CHECK_DEADLINE" : "SCRIPT_FRAMEWORK_CHECK_BUDGET";
            return Result<FrameworkPhaseResult>::Failure("Script.PreprocessSources", code,
                "框架预处理被取消、超时或达到共享预算；未发布部分结果。");
        }
    }
private:
    Result<FrameworkPhaseResult> RunChecked(std::string_view text, const std::filesystem::path& source) {
        budget_.Poll();
        if (!Expand(text, source, 0, options_.mainLexicalView ? &*options_.mainLexicalView : nullptr)) return {{}, std::move(diagnostics_)};
        std::set<std::string> flags;
        for (const auto& line : lines_) {
            budget_.Poll();
            if (line.directive != "$def") continue;
            if (!Flag(line.argument) || line.extra) return Failed("SCRIPT_FRAMEWORK_DOLLAR_DEF_UNSUPPORTED",
                "只支持ASCII无值特性声明；有值替换、GBK宏名与宿主预定义未取得资格。", line);
            flags.insert(line.argument);
            if (flags.size() > 1024) return Failed("SCRIPT_FRAMEWORK_DOLLAR_BUDGET", "$def特性超过1024项。", line);
        }
        struct Branch { bool parent, condition, alternate = false; };
        std::vector<Branch> stack;
        bool active = true;
        FrameworkPhaseResult output;
        output.flags.assign(flags.begin(), flags.end());
        std::vector<std::uint32_t> sourceFiles;
        for (const auto& path : files_) {
            const auto utf8 = path.u8string();
            sourceFiles.push_back(output.sourceMap.AddFile(std::string(utf8.begin(), utf8.end())));
        }
        for (const auto& line : lines_) {
            budget_.Poll();
            if (line.directive == "$ifdef") {
                if (!Flag(line.argument) || line.extra || stack.size() >= 64)
                    return Failed("SCRIPT_FRAMEWORK_DOLLAR_CONDITION_INVALID", "$ifdef需要单个ASCII特性名且深度不超过64。", line);
                const bool selected = flags.contains(line.argument);
                stack.push_back({active, selected}); active = active && selected;
            } else if (line.directive == "$else") {
                if (line.extra || !line.argument.empty() || stack.empty() || stack.back().alternate)
                    return Failed("SCRIPT_FRAMEWORK_DOLLAR_CONDITION_INVALID", "$else无匹配分支、重复或带参数。", line);
                stack.back().alternate = true; active = stack.back().parent && !stack.back().condition;
            } else if (line.directive == "$endif") {
                if (line.extra || !line.argument.empty() || stack.empty())
                    return Failed("SCRIPT_FRAMEWORK_DOLLAR_CONDITION_INVALID", "$endif无匹配分支或带参数。", line);
                active = stack.back().parent; stack.pop_back();
            } else if (!line.directive.empty() && line.directive != "$def") {
                return Failed("SCRIPT_FRAMEWORK_DOLLAR_UNSUPPORTED", "未知框架指令未静默交给Wave。", line);
            }
            output.sourceMap.Map(outputLine_++, {sourceFiles[line.file], line.line});
            if (line.directive.empty() && active) {
                const auto base = static_cast<std::uint32_t>(output.text.size());
                output.text += line.text;
                output.lexicalView.text += line.lexicalView.text;
                for (auto span : line.lexicalView.opaque) { span.offset += base; output.lexicalView.opaque.push_back(std::move(span)); }
            } else {
                const auto ending = line.text.ends_with("\r\n") ? "\r\n" : line.text.ends_with('\r') ? "\r" : "\n";
                output.text.append(ending); output.lexicalView.text.append(ending);
            }
            if (output.text.size() > kOutputBytes) return Failed("SCRIPT_FRAMEWORK_DOLLAR_BUDGET", "框架输出超过16MiB。", line);
        }
        if (!stack.empty()) return Failed("SCRIPT_FRAMEWORK_DOLLAR_CONDITION_INVALID", "存在未闭合$ifdef。", lines_.back());
        // 整段分词保留跨行字符串/注释；不能逐行重新分词而把注释正文当成代码。
        auto lex = Jass::detail::LexPpBounded(output.lexicalView.text, budget_, War3Map::Text::SourceEncoding::Utf8);
        if (!lex.Succeeded()) return {{}, std::move(lex.diagnostics)};
        bool cDirective = false;
        for (const auto& token : *lex.value) {
            budget_.PollOccasionally(token.offset);
            if (token.kind == Jass::detail::PpKind::Newline) cDirective = false;
            if (token.lineStart && token.Is("#")) cDirective = true;
            if (!cDirective && token.kind == Jass::detail::PpKind::Identifier && flags.contains(std::string(token.text))) {
                if (token.line == 0 || token.line > lines_.size()) return Result<FrameworkPhaseResult>::Failure(
                    "Script.PreprocessSources", "SCRIPT_FRAMEWORK_DOLLAR_SOURCE_INVALID", "框架输出来源行无效。");
                return Failed("SCRIPT_FRAMEWORK_DOLLAR_REPLACEMENT_UNSUPPORTED", "无值特性在普通代码中引用，未猜测替换成1。", lines_[token.line - 1]);
            }
        }
        return Result<FrameworkPhaseResult>::Success(std::move(output));
    }
private:
    struct Line {
        std::string text, directive, argument;
        std::uint32_t file = 0, line = 0;
        std::uint64_t offset = 0;
        bool extra = false;
        std::uint64_t sourceBytes = 0;
        Jass::SourceLexicalView lexicalView;
    };
    static bool Flag(std::string_view name) {
        return !name.empty() && name.size() <= 512 && (name.front() == '_' ||
            (name.front() >= 'A' && name.front() <= 'Z') || (name.front() >= 'a' && name.front() <= 'z')) &&
            std::all_of(name.begin(), name.end(), [](unsigned char c) { return NameByte(c); });
    }
    Result<FrameworkPhaseResult> Failed(std::string code, std::string message, const Line& line) {
        auto result = Result<FrameworkPhaseResult>::Failure("Script.PreprocessSources", std::move(code), std::move(message));
        result.diagnostics.back().origin = OriginSpan{files_[line.file], line.offset, line.sourceBytes, {}};
        return result;
    }
    bool Stop(std::string code, std::string message, const Line& line) {
        auto result = Failed(std::move(code), std::move(message), line);
        diagnostics_.insert(diagnostics_.end(), result.diagnostics.begin(), result.diagnostics.end()); return false;
    }
    bool Expand(std::string_view text, const std::filesystem::path& source, std::uint32_t depth,
        const Jass::SourceLexicalView* explicitView = nullptr) {
        budget_.RegisterSource(text.size());
        const auto file = static_cast<std::uint32_t>(files_.size()); files_.push_back(source);
        if (depth >= 64 || files_.size() > kFiles || text.size() > kOutputBytes - expandedBytes_)
            return Stop("SCRIPT_FRAMEWORK_DOLLAR_BUDGET", "框架包含深度、访问文件或累计文本超过预算。", {"", "", "", file, 1});
        expandedBytes_ += text.size();
        Jass::SourceLexicalView view;
        if (explicitView) view = *explicitView;
        else if (options_.lexicalViewProvider) {
            auto made = options_.lexicalViewProvider(text, source);
            if (!made.Succeeded()) { diagnostics_.insert(diagnostics_.end(), made.diagnostics.begin(), made.diagnostics.end()); return false; }
            view = std::move(*made.value);
        } else view.text = text;
        auto validView = Jass::ValidateSourceLexicalView(text, view);
        if (!validView.Succeeded()) { diagnostics_.insert(diagnostics_.end(), validView.diagnostics.begin(), validView.diagnostics.end()); return false; }
        auto lex = Jass::detail::LexPpBounded(view.text, budget_, War3Map::Text::SourceEncoding::Utf8);
        if (!lex.Succeeded()) {
            for (auto& diagnostic : lex.diagnostics) {
                if (!diagnostic.origin) diagnostic.origin = OriginSpan{source}; else diagnostic.origin->source = source;
            }
            diagnostics_.insert(diagnostics_.end(), lex.diagnostics.begin(), lex.diagnostics.end()); return false;
        }
        std::map<std::uint32_t, std::vector<const Jass::detail::PpToken*>> directives;
        for (std::size_t i = 0; i < lex.value->size(); ++i) {
            budget_.PollOccasionally(i);
            const auto& token = (*lex.value)[i];
            if (!token.lineStart || token.kind != Jass::detail::PpKind::Identifier || !token.text.starts_with('$')) continue;
            auto& items = directives[token.line];
            for (; i < lex.value->size() && (*lex.value)[i].kind != Jass::detail::PpKind::Newline &&
                (*lex.value)[i].kind != Jass::detail::PpKind::End; ++i) items.push_back(&(*lex.value)[i]);
        }
        std::uint32_t lineNumber = 1;
        std::size_t opaqueIndex = 0;
        for (std::size_t begin = 0; begin < text.size(); ++lineNumber) {
            budget_.Poll();
            auto end = text.find_first_of("\r\n", begin);
            if (end == std::string_view::npos) end = text.size();
            else if (text[end++] == '\r' && end < text.size() && text[end] == '\n') ++end;
            Line line{std::string(text.substr(begin, end - begin)), "", "", file, lineNumber, begin};
            line.lexicalView.text = view.text.substr(begin, end - begin);
            while (opaqueIndex < view.opaque.size() && view.opaque[opaqueIndex].offset < begin) ++opaqueIndex;
            for (auto position = opaqueIndex; position < view.opaque.size() && view.opaque[position].offset + view.opaque[position].length <= end; ++position) {
                auto span = view.opaque[position]; span.offset -= static_cast<std::uint32_t>(begin);
                line.lexicalView.opaque.push_back(std::move(span));
            }
            if (const auto found = directives.find(lineNumber); found != directives.end()) {
                const auto& args = found->second;
                line.directive = args[0]->text; line.offset = args[0]->offset;
                if (args.size() > 1) line.argument = args[1]->text;
                line.extra = args.size() > 2;
            }
            line.sourceBytes = end - line.offset;
            if (line.directive == "$include") {
                if (line.extra || line.argument.size() < 3 || line.argument.front() != '"' || line.argument.back() != '"' ||
                    line.argument.find_first_of("\r\n") != std::string::npos)
                    return Stop("SCRIPT_FRAMEWORK_DOLLAR_INCLUDE_INVALID", "$include只支持单个明确引号路径。", line);
                std::string name;
                for (std::size_t i = 1; i + 1 < line.argument.size(); ++i) {
                    if (line.argument[i] == '\\' && i + 2 < line.argument.size() && line.argument[i + 1] == '\\') ++i;
                    name += line.argument[i];
                }
                const auto relative = std::filesystem::path(std::u8string_view(
                    reinterpret_cast<const char8_t*>(name.data()), name.size()));
                if (relative.is_absolute() || relative.has_root_name() || relative.empty())
                    return Stop("SCRIPT_FRAMEWORK_DOLLAR_INCLUDE_INVALID", "$include需要来源根内的相对路径。", line);
                std::vector<std::filesystem::path> roots{source.parent_path()};
                roots.insert(roots.end(), options_.userIncludes.begin(), options_.userIncludes.end());
                roots.insert(roots.end(), options_.systemIncludes.begin(), options_.systemIncludes.end());
                bool included = false;
                std::optional<std::filesystem::path> skipped;
                for (std::size_t ordinal = 0; ordinal < roots.size(); ++ordinal) {
                    budget_.Poll();
                    const auto& root = roots[ordinal];
                    const auto path = (root / relative).lexically_normal();
                    Jass::IncludeCandidateRequest candidate;
                    candidate.phase = Jass::IncludePhase::FrameworkDollar;
                    candidate.requester = source; candidate.requesterStageText = text;
                    candidate.searchDirectory = root; candidate.candidate = path; candidate.spelling = name;
                    candidate.requesterLine = line.line; candidate.requesterOffset = static_cast<std::uint32_t>(line.offset);
                    candidate.requesterLength = static_cast<std::uint32_t>(line.sourceBytes);
                    candidate.searchOrdinal = static_cast<std::uint32_t>(ordinal);
                    candidate.searchRole = ordinal == 0 ? Jass::IncludeSearchRole::CurrentFile :
                        ordinal <= options_.userIncludes.size() ? Jass::IncludeSearchRole::User : Jass::IncludeSearchRole::System;
                    auto eligibility = Jass::CheckIncludeCandidate(options_, candidate);
                    if (!eligibility.Succeeded()) {
                        for (auto& diagnostic : eligibility.diagnostics) {
                            diagnostic.message += " [$include请求 " + std::to_string(line.line) + " → " + name + "]";
                            diagnostics_.push_back(std::move(diagnostic));
                        }
                        return false;
                    }
                    if (*eligibility.value == Jass::IncludeCandidateEligibility::SkippedOutOfScope) {
                        if (!skipped) skipped = path;
                        continue;
                    }
                    auto content = options_.includeProvider(path);
                    if (!content.Succeeded()) { diagnostics_.insert(diagnostics_.end(), content.diagnostics.begin(), content.diagnostics.end()); return false; }
                    if (!content.value->exists) continue;
                    if (!Expand(content.value->text, path, depth + 1)) return false;
                    included = true; break;
                }
                if (!included && skipped) {
                    const auto path = skipped->generic_u8string();
                    return Stop("SCRIPT_INCLUDE_PATH_SCOPE", "$include未选中来源；范围外候选未读取：" + std::string(path.begin(), path.end()), line);
                }
                if (!included) return Stop("SCRIPT_FRAMEWORK_DOLLAR_INCLUDE_MISSING", "$include所有明确候选均不存在。", line);
            } else {
                if (lines_.size() >= 262144) return Stop("SCRIPT_FRAMEWORK_DOLLAR_BUDGET", "框架文本超过262144行预算。", line);
                if (!line.text.ends_with('\n') && !line.text.ends_with('\r')) { line.text += '\n'; line.lexicalView.text += '\n'; }
                lines_.push_back(std::move(line));
            }
            begin = end;
        }
        return true;
    }
    const Jass::PreprocessOptions& options_;
    Jass::detail::CheckBudget budget_;
    std::vector<std::filesystem::path> files_;
    std::vector<Line> lines_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t expandedBytes_ = 0;
    std::uint32_t outputLine_ = 1;
};
bool ProfileRequestMatches(const ScriptUserSourcePlan& plan, const ScriptPreprocessSourceRequest& request) {
    if (request.includeCandidatePolicy == ScriptIncludeCandidatePolicy::BoundRootsSearch && plan.profileIdentitySha256.empty()) return false;
    if (plan.profileIdentitySha256.empty()) return request.frameworkDirectivePolicy == ScriptFrameworkDirectivePolicy::None;
    const auto expected = std::to_string(plan.profileTargetVersionMacro);
    return request.mainPolicy == ScriptPreprocessMainPolicy::None &&
        FeaturePolicyMatches(plan, request.frameworkDirectivePolicy) &&
        plan.profileTargetVersionMacro >= 100 && plan.profileTargetVersionMacro <= 199 &&
        CompareStringOrdinal(request.mainDirectory.c_str(), -1, plan.mapDirectory.c_str(), -1, TRUE) == CSTR_EQUAL &&
        std::all_of(request.defines.begin(), request.defines.end(), [&](const auto& define) {
            return define.first != "WARCRAFT_VERSION" || define.second == expected;
        });
}
Jass::PreprocessOptions Options(const ScriptPreprocessSources& sources) {
    Jass::PreprocessOptions options;
    options.captureMacroAudit = sources.request.captureMacroAudit;
    options.check = sources.request.check;
    options.userIncludes = sources.entryPlan.orderedUserIncludes;
    options.systemIncludes = sources.systemIncludes;
    options.forceIncludes = sources.forceIncludes;
    options.defines = sources.request.defines;
    if (!sources.entryPlan.profileIdentitySha256.empty() &&
        std::none_of(options.defines.begin(), options.defines.end(), [](const auto& define) { return define.first == "WARCRAFT_VERSION"; })) {
        options.defines.emplace_back("WARCRAFT_VERSION", std::to_string(sources.entryPlan.profileTargetVersionMacro));
    }
    options.maxOutputBytes = kOutputBytes;
    if (!sources.entryPlan.profileIdentitySha256.empty()) {
        const auto plan = sources.entryPlan;
        options.lexicalViewProvider = [plan](std::string_view original, const std::filesystem::path& path) {
            return MakeFrameworkLexicalView(original, path, FrameworkEncodingFor(plan, path));
        };
        options.mainLexicalView = sources.request.mainLexicalView;
    }
    return options;
}
// 框架编译插件：已开启的 $def 特性按档案顺序在框架强制包含之后追加头文件；
// 未实现的插件在框架阶段正文（代码标识符，不含注释/字符串）出现触发名时拒绝，不静默交给 jasshelper。
Status ApplyFrameworkPlugins(const ScriptUserSourcePlan& plan, const FrameworkPhaseResult& framework,
    Jass::PreprocessOptions& options) {
    for (const auto& extension : plan.profileFeatureExtensions) {
        if (std::binary_search(framework.flags.begin(), framework.flags.end(), extension.feature))
            options.forceIncludes.push_back(extension.forceInclude);
    }
    if (plan.profileUnsupportedPlugins.empty()) return {};
    auto lex = Jass::detail::LexPp(framework.lexicalView.text, War3Map::Text::SourceEncoding::Utf8);
    if (!lex.Succeeded()) return {std::move(lex.diagnostics)};
    for (const auto& token : *lex.value) {
        if (token.kind != Jass::detail::PpKind::Identifier) continue;
        for (const auto& plugin : plan.profileUnsupportedPlugins) {
            if (std::find(plugin.identifiers.begin(), plugin.identifiers.end(), token.text) == plugin.identifiers.end()) continue;
            auto failure = Status::Failure("Script.PreprocessSources", "SCRIPT_FRAMEWORK_PLUGIN_UNSUPPORTED",
                "框架编译插件 " + plugin.name + " 尚未支持（触发名 " + std::string(token.text) + "）；未发布部分结果。");
            const auto position = framework.sourceMap.Find(token.line);
            if (position && position->file < framework.sourceMap.Files().size())
                failure.diagnostics.back().message += " 位置：" + framework.sourceMap.Files()[position->file] + ":" + std::to_string(position->line);
            return failure;
        }
    }
    return {};
}
Jass::PreprocessResult PreprocessOwned(const ScriptPreprocessSources& sources, std::string_view text,
    Jass::PreprocessOptions options, FrameworkPhaseResult& framework, Jass::PreprocessCache& cache) {
    if (sources.request.frameworkDirectivePolicy == ScriptFrameworkDirectivePolicy::None)
        return Jass::Preprocess(text, sources.request.mainName, sources.request.mainDirectory, options, &cache);
    auto phase = FrameworkFeatures(options).Run(text, sources.request.mainDirectory / sources.request.mainName);
    if (!phase.Succeeded()) { Jass::PreprocessResult result; result.diagnostics = std::move(phase.diagnostics); return result; }
    framework = std::move(*phase.value);
    if (auto plugins = ApplyFrameworkPlugins(sources.entryPlan, framework, options); !plugins.Succeeded()) {
        Jass::PreprocessResult result; result.diagnostics = std::move(plugins.diagnostics); return result;
    }
    options.mainLexicalView = framework.lexicalView;
    const auto provider = options.includeProvider;
    options.includeProvider = [provider, viewProvider = options.lexicalViewProvider](const std::filesystem::path& path) -> ContentResult {
        auto content = provider(path);
        if (!content.Succeeded() || !content.value->exists) return content;
        auto lexical = viewProvider ? viewProvider(content.value->text, path) :
            Result<Jass::SourceLexicalView>::Success({content.value->text, {}});
        if (!lexical.Succeeded()) return {{}, std::move(lexical.diagnostics)};
        auto lex = Jass::detail::LexPp(lexical.value->text, War3Map::Text::SourceEncoding::Utf8);
        if (!lex.Succeeded()) {
            for (auto& diagnostic : lex.diagnostics) {
                if (!diagnostic.origin) diagnostic.origin = OriginSpan{path}; else diagnostic.origin->source = path;
            }
            return {{}, std::move(lex.diagnostics)};
        }
        for (const auto& token : *lex.value) if (token.lineStart && token.kind == Jass::detail::PpKind::Identifier && token.text.starts_with('$')) {
            auto failure = ContentResult::Failure("Script.PreprocessSources", "SCRIPT_FRAMEWORK_DOLLAR_LATE_INCLUDE",
                "C/Wave阶段才读取的来源含框架$指令；未把它加入不同阶段的宏空间。请使用明确$include来源。");
            failure.diagnostics.back().origin = OriginSpan{path, token.offset, token.text.size(), {}};
            return failure;
        }
        return content;
    };
    auto result = Jass::Preprocess(framework.text, sources.request.mainName, sources.request.mainDirectory, options, &cache);
    if (!result.Succeeded()) return result;
    // 只组合中间主文；C阶段自己的包含文件行不能错误投射到主文相同行。
    Jass::SourceMap composed;
    std::map<std::string, std::uint32_t> mappedFiles;
    std::uint32_t outputLines = 1;
    for (std::size_t i = 0; i < result.text.size(); ++i) if (result.text[i] == '\r' || result.text[i] == '\n') {
        if (result.text[i] == '\r' && i + 1 < result.text.size() && result.text[i + 1] == '\n') ++i;
        ++outputLines;
    }
    for (std::uint32_t line = 1; line <= outputLines; ++line) {
        auto position = result.sourceMap.Find(line);
        if (!position || position->file >= result.sourceMap.Files().size()) continue;
        auto name = result.sourceMap.Files()[position->file];
        if (name == sources.request.mainName) {
            const auto original = framework.sourceMap.Find(position->line);
            if (!original || original->file >= framework.sourceMap.Files().size()) continue;
            name = framework.sourceMap.Files()[original->file]; position = original;
        }
        auto found = mappedFiles.find(name);
        if (found == mappedFiles.end()) found = mappedFiles.emplace(name, composed.AddFile(name)).first;
        composed.Map(line, {found->second, position->line});
    }
    result.sourceMap = std::move(composed);
    for (auto& macro : result.macroAudit) if (macro.source == sources.request.mainName) {
        const auto original = framework.sourceMap.Find(macro.line);
        if (original && original->file < framework.sourceMap.Files().size()) {
            macro.source = framework.sourceMap.Files()[original->file]; macro.line = original->line;
        } else { macro.source.clear(); macro.line = 0; }
    }
    return result;
}
bool SameRequest(const ScriptPreprocessSourceRequest& a, const ScriptPreprocessSourceRequest& b) {
    if (a.captureMacroAudit != b.captureMacroAudit || a.forbidAiIncludes != b.forbidAiIncludes) return false;
    return a.mainText == b.mainText && a.mainName == b.mainName &&
        a.mainDirectory == b.mainDirectory && a.defines == b.defines && a.mainPolicy == b.mainPolicy &&
        a.frameworkDirectivePolicy == b.frameworkDirectivePolicy &&
        a.includeCandidatePolicy == b.includeCandidatePolicy &&
        a.mainLexicalView.has_value() == b.mainLexicalView.has_value() &&
        (!a.mainLexicalView || (a.mainLexicalView->text == b.mainLexicalView->text && a.mainLexicalView->opaque == b.mainLexicalView->opaque));
}
bool SameBinding(const ScriptIncludeSource& a, const ScriptIncludeSource& b) {
    return a.binding.path == b.binding.path && a.binding.kind == b.binding.kind &&
        a.binding.sha256 == b.binding.sha256 && a.binding.bytes == b.binding.bytes &&
        a.binding.maxBytes == b.binding.maxBytes && a.bytes == b.bytes && a.lastWriteTicks == b.lastWriteTicks;
}
ContentResult Failure(std::string code, std::string message, const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    if (!text.empty()) message += " [请求来源 " + std::string(text.begin(), text.end()) + "]";
    auto result = ContentResult::Failure("Script.PreprocessSources", std::move(code), std::move(message));
    result.diagnostics.back().origin = OriginSpan{path};
    return result;
}
using EligibilityResult = Result<Jass::IncludeCandidateEligibility>;
EligibilityResult PolicyFailure(std::string code, std::string message, const std::filesystem::path& path) {
    auto result = Failure(std::move(code), std::move(message), path);
    return {{}, std::move(result.diagnostics)};
}
struct CandidatePolicy {
    std::vector<std::filesystem::path> roots, exact;
    std::vector<ScriptProfileIncludeAccess> ordinary;
    ScriptIncludeCandidatePolicy mode = ScriptIncludeCandidatePolicy::Strict;
    std::string sha256;
    EligibilityResult Evaluate(const Jass::IncludeCandidateRequest& request) const {
        // 资格只消费冻结纯值。先检查拼写，避免 lexical .. 消去非法组件后取得资格。
        if (request.spelling.empty() || request.spelling.size() > 4096)
            return PolicyFailure("SCRIPT_INCLUDE_PATH_INVALID", "包含拼写为空或超过路径预算。", request.candidate);
        const std::filesystem::path spelling(std::u8string(request.spelling.begin(), request.spelling.end()));
        for (const auto& part : spelling.relative_path()) {
            if (part.native().find_first_of(L":*?\"<>|") != std::wstring::npos ||
                std::any_of(part.native().begin(), part.native().end(), [](wchar_t c) { return c < 32; }))
                return PolicyFailure("SCRIPT_INCLUDE_PATH_INVALID", "包含拼写含设备/ADS/控制字符或非法组件。", request.candidate);
        }
        std::filesystem::path path;
        if (std::any_of(request.candidate.native().begin(), request.candidate.native().end(), [](wchar_t c) { return c < 32; }) ||
            !Detail::NormalizeSourcePath(request.candidate, path))
            return PolicyFailure("SCRIPT_INCLUDE_PATH_INVALID", "包含候选不是有界绝对普通路径。", request.candidate);
        const bool allowed = std::any_of(roots.begin(), roots.end(), [&](const auto& root) { return Within(path, root); }) ||
            std::any_of(exact.begin(), exact.end(), [&](const auto& file) { return !PathLess{}(path, file) && !PathLess{}(file, path); }) ||
            MatchScriptIncludeAccess(ordinary, path) != nullptr;
        if (allowed) return EligibilityResult::Success(Jass::IncludeCandidateEligibility::Allowed);
        if (mode == ScriptIncludeCandidatePolicy::BoundRootsSearch)
            return EligibilityResult::Success(Jass::IncludeCandidateEligibility::SkippedOutOfScope);
        return PolicyFailure("SCRIPT_INCLUDE_PATH_SCOPE", "包含路径不在明确绑定的普通来源根内。", path);
    }
};
Result<CandidatePolicy> MakeCandidatePolicy(const ScriptPreprocessSources& sources) {
    using R = Result<CandidatePolicy>;
    CandidatePolicy policy; policy.mode = sources.request.includeCandidatePolicy;
    if (sources.entryPlan.orderedUserIncludes.size() + sources.systemIncludes.size() + sources.entryPlan.orderedForceIncludes.size() +
        sources.entryPlan.profileOrdinaryIncludeAccess.size() > 512 || sources.entryPlan.profileOrdinaryIncludeAccess.size() > 128)
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_BUDGET", "包含根与精确来源超过路径预算。");
    if (policy.mode != ScriptIncludeCandidatePolicy::Strict && policy.mode != ScriptIncludeCandidatePolicy::BoundRootsSearch)
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_POLICY_INVALID", "未知包含候选策略。");
    std::string identity = "include-bound-roots-v1:" + std::to_string(static_cast<int>(policy.mode)) + "\n" +
        sources.entryPlan.profileIdentitySha256 + "\n";
    const auto append = [&](const std::filesystem::path& input, std::vector<std::filesystem::path>& output, char role) {
        std::filesystem::path normalized;
        if (!Detail::NormalizeSourcePath(input, normalized)) return false;
        output.push_back(normalized);
        const auto utf8 = normalized.generic_u8string();
        identity += role; identity += std::to_string(utf8.size()) + ":";
        identity.append(utf8.begin(), utf8.end()); identity += '\n';
        return true;
    };
    if (!append(sources.request.mainDirectory, policy.roots, 'M'))
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_MAIN_PATH", "主文件目录不是有界绝对普通路径。");
    for (const auto& root : sources.entryPlan.orderedUserIncludes) if (!append(root, policy.roots, 'U'))
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_PATH", "用户包含根不是有界绝对普通路径。");
    for (const auto& root : sources.systemIncludes) if (!append(root, policy.roots, 'S'))
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_PATH", "系统包含根不是有界绝对普通路径。");
    for (const auto& force : sources.entryPlan.orderedForceIncludes) if (!append(force, policy.exact, 'F'))
        return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_PATH", "强制包含不是有界绝对普通路径。");
    if (sources.forceIncludes.size() > 512) return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_BUDGET", "强制包含数超过路径预算。");
    for (const auto& force : sources.forceIncludes) {
        if (force.size() > 4096) return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ROOT_BUDGET", "强制包含拼写超过路径预算。");
        identity += "f" + std::to_string(force.size()) + ":" + force + "\n";
    }
    if (!sources.entryPlan.profileOrdinaryIncludeAccess.empty()) {
        if (sources.entryPlan.profileIdentitySha256.size() != 64 || sources.entryPlan.bindings.size() > 128 ||
            !std::all_of(sources.entryPlan.profileIdentitySha256.begin(), sources.entryPlan.profileIdentitySha256.end(), [](unsigned char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }))
            return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ACCESS_INVALID", "普通include资格必须来自已捕获的显式档案。");
        identity += "ordinary-include-access-v1:" + std::to_string(sources.entryPlan.profileOrdinaryIncludeAccess.size()) + "\n";
        for (const auto& access : sources.entryPlan.profileOrdinaryIncludeAccess) {
            std::filesystem::path path;
            if ((access.kind != ScriptIncludeAccessKind::Directory && access.kind != ScriptIncludeAccessKind::ExactFile) ||
                (access.encoding != ScriptProfileEncoding::Utf8 && access.encoding != ScriptProfileEncoding::Gbk &&
                    access.encoding != ScriptProfileEncoding::UniqueUtf8OrGbk) ||
                std::any_of(access.path.native().begin(), access.path.native().end(), [](wchar_t c) { return c < 32; }) ||
                !Detail::NormalizeSourcePath(access.path, path) || path != access.path ||
                std::any_of(policy.ordinary.begin(), policy.ordinary.end(), [&](const auto& prior) {
                    return !PathLess{}(prior.path, path) && !PathLess{}(path, prior.path);
                })) return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ACCESS_INVALID", "普通include资格的路径、类型、编码或唯一身份无效。");
            const auto binding = std::find_if(sources.entryPlan.bindings.begin(), sources.entryPlan.bindings.end(), [&](const auto& value) {
                return !PathLess{}(value.path, path) && !PathLess{}(path, value.path);
            });
            const auto expected = access.kind == ScriptIncludeAccessKind::Directory ? Kind::Directory : Kind::RegularFile;
            if (binding == sources.entryPlan.bindings.end() || (binding->kind != expected &&
                    (access.required || binding->kind != Kind::Missing)))
                return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ACCESS_UNBOUND", "普通include资格缺少相同路径、类型和required策略的正/负入口绑定。");
            if (binding->maxBytes != kFileBytes || binding->bytes > kFileBytes ||
                (binding->kind == Kind::RegularFile ? (binding->sha256.size() != 64 ||
                    !std::all_of(binding->sha256.begin(), binding->sha256.end(), [](unsigned char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                    })) : (!binding->sha256.empty() || binding->bytes != 0)))
                return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_ACCESS_UNBOUND", "普通include入口正/负内容身份或绑定预算无效。");
            policy.ordinary.push_back(access);
            const auto utf8 = path.generic_u8string();
            identity += std::to_string(utf8.size()) + ":"; identity.append(utf8.begin(), utf8.end());
            identity += ":" + std::to_string(static_cast<int>(access.kind)) + ":" +
                std::to_string(static_cast<int>(access.encoding)) + ":" + (access.required ? "1\n" : "0\n");
            identity += std::to_string(static_cast<int>(binding->kind)) + ":" + binding->sha256 + ":" +
                std::to_string(binding->bytes) + ":" + std::to_string(binding->maxBytes) + "\n";
        }
    }
    auto hash = Sha256Text(identity);
    if (!hash.Succeeded()) return {{}, std::move(hash.diagnostics)};
    policy.sha256 = std::move(*hash.value);
    return R::Success(std::move(policy));
}
// 元数据预算独立于读取字节：skipped 同样占访问、唯一路径与 owned trace 空间。
struct CandidateTraceBuilder {
    std::size_t visits = 0, bytes = 0;
    std::set<std::filesystem::path, PathLess> paths;
    struct StageHash { const char* data = nullptr; std::size_t size = 0; std::string sha256; };
    // 相同 owned 字节的稳定词法缓存不重复哈希；同名虚拟主文/物理 include 仍按实际阶段区分。
    std::map<std::pair<Jass::IncludePhase, std::filesystem::path>, StageHash> stageHashes;
    Result<ScriptIncludeCandidateTrace> Make(const Jass::IncludeCandidateRequest& request,
        Jass::IncludeCandidateEligibility eligibility) {
        using R = Result<ScriptIncludeCandidateTrace>;
        const auto cost = sizeof(ScriptIncludeCandidateTrace) + request.spelling.size() + 64 +
            (request.requester.native().size() + request.searchDirectory.native().size() + request.candidate.native().size()) * sizeof(wchar_t);
        if (++visits > 16384 || cost > kMainBytes - bytes || request.requesterStageText.size() > kOutputBytes ||
            request.requesterOffset > request.requesterStageText.size() ||
            request.requesterLength > request.requesterStageText.size() - request.requesterOffset)
            return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_TRACE_BUDGET", "包含候选访问、阶段位置或 owned trace 超过预算。");
        bytes += cost; paths.insert(request.candidate);
        if (paths.size() > kCandidates) return R::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_SOURCE_BUDGET", "包含唯一路径超过来源查询预算。");
        const auto key = std::make_pair(request.phase, request.requester);
        auto hash = stageHashes.find(key);
        if (hash == stageHashes.end() || hash->second.data != request.requesterStageText.data() || hash->second.size != request.requesterStageText.size()) {
            auto made = Sha256Text(request.requesterStageText);
            if (!made.Succeeded()) return {{}, std::move(made.diagnostics)};
            StageHash value{request.requesterStageText.data(), request.requesterStageText.size(), std::move(*made.value)};
            hash = stageHashes.insert_or_assign(key, std::move(value)).first;
        }
        ScriptIncludeCandidateTrace trace;
        trace.phase = request.phase; trace.requester = request.requester; trace.searchDirectory = request.searchDirectory;
        trace.candidate = request.candidate; trace.requesterStageSha256 = hash->second.sha256; trace.spelling = request.spelling;
        trace.requesterLine = request.requesterLine; trace.requesterOffset = request.requesterOffset;
        trace.requesterLength = request.requesterLength; trace.searchOrdinal = request.searchOrdinal;
        trace.searchRole = request.searchRole; trace.eligibility = eligibility; trace.angled = request.angled; trace.forced = request.forced;
        return R::Success(std::move(trace));
    }
};
} // namespace

Result<ScriptPreprocessSources> CaptureScriptPreprocessSources(const ScriptUserSourcePlan& entryPlan,
    const ScriptUserSourceRequest& currentSources, const ScriptPreprocessSourceRequest& request) {
    using CaptureResult = Result<ScriptPreprocessSources>;
    auto valid = VerifyScriptUserSourcePlan(entryPlan, currentSources);
    if (!valid.Succeeded()) return {{}, std::move(valid.diagnostics)};
    if (request.mainText.size() > kMainBytes || request.mainName.empty() || request.mainName.size() > 4096 ||
        request.mainName.find('\0') != std::string::npos || request.defines.size() > 128) return CaptureResult::Failure(
            "Script.PreprocessSources", "SCRIPT_INCLUDE_REQUEST_BUDGET", "预处理请求超过输入或宏预算。");
    std::size_t defineBytes = 0;
    for (const auto& [name, value] : request.defines) {
        if (name.empty() || name.size() > 4096 || value.size() > 4096 || name.find('\0') != std::string::npos ||
            value.find('\0') != std::string::npos || (defineBytes += name.size() + value.size()) > 64 * 1024) {
            return CaptureResult::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_REQUEST_BUDGET", "预定义宏无效或超过预算。");
        }
    }
    try {
        ScriptPreprocessSources sources;
        sources.entryPlan = entryPlan;
        sources.request = request;
        if (!Detail::NormalizeSourcePath(request.mainDirectory, sources.request.mainDirectory)) return CaptureResult::Failure(
            "Script.PreprocessSources", "SCRIPT_INCLUDE_MAIN_PATH", "主文件目录必须是有界绝对普通路径。");
        if (!ProfileRequestMatches(entryPlan, sources.request)) {
            return CaptureResult::Failure("Script.PreprocessSources", "SCRIPT_PROFILE_PREPROCESS_REJECTED",
                "通用档案禁止历史全文替换；主文来源目录和目标版本宏必须与已绑定档案一致。");
        }
        sources.systemIncludes = entryPlan.profileIdentitySha256.empty() ?
            currentSources.layout->systemIncludes : entryPlan.orderedSystemIncludes;
        const auto& forces = entryPlan.profileIdentitySha256.empty() ?
            currentSources.layout->forceIncludes : entryPlan.orderedForceIncludes;
        for (const auto& force : forces) {
            const auto name = force.u8string();
            sources.forceIncludes.emplace_back(name.begin(), name.end());
        }
        auto prepared = PrepareMain(sources.request);
        if (!prepared.Succeeded()) return {{}, std::move(prepared.diagnostics)};
        sources.preparedMainText = std::move(prepared.value->text);
        sources.mainTransform = std::move(prepared.value->report);
        sources.mainSha256 = sources.mainTransform.inputSha256;
        auto policy = MakeCandidatePolicy(sources);
        if (!policy.Succeeded()) return {{}, std::move(policy.diagnostics)};
        sources.includeCandidatePolicySha256 = policy.value->sha256;
        CandidateTraceBuilder traceBuilder;
        std::map<std::filesystem::path, std::size_t, PathLess> index;
        std::size_t regularFiles = 0;
        AccessBudget access;
        auto options = Options(sources);
        options.includeCandidatePolicy = [&](const Jass::IncludeCandidateRequest& candidate) -> EligibilityResult {
            auto eligibility = policy.value->Evaluate(candidate);
            if (!eligibility.Succeeded()) return eligibility;
            auto trace = traceBuilder.Make(candidate, *eligibility.value);
            if (!trace.Succeeded()) return {{}, std::move(trace.diagnostics)};
            sources.includeCandidates.push_back(std::move(*trace.value));
            return eligibility;
        };
        options.includeProvider = [&](const std::filesystem::path& input) -> ContentResult {
            if (request.forbidAiIncludes) {
                auto extension = input.extension().wstring();
                std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return std::towlower(c); });
                if (extension == L".ai") return Failure("SCRIPT_AUDIT_AI_INCLUDE_FORBIDDEN", "删除审计不读取AI引导文件。", input);
                static const std::set<std::wstring> textExtensions{L"", L".j", L".h", L".hpp", L".hh", L".hxx", L".i", L".inc", L".txt", L".cfg"};
                if (!textExtensions.contains(extension)) return Failure("SCRIPT_AUDIT_TEXT_INCLUDE_REQUIRED", "删除审计只读取文本库包含文件。", input);
            }
            std::filesystem::path path;
            if (!Detail::NormalizeSourcePath(input, path) || sources.includeCandidates.empty() ||
                sources.includeCandidates.back().eligibility != Jass::IncludeCandidateEligibility::Allowed ||
                sources.includeCandidates.back().candidate != path) return Failure(
                    "SCRIPT_INCLUDE_POLICY_MISMATCH", "包含读取缺少对应资格预检。", input);
            if (const auto prior = index.find(path); prior != index.end()) {
                sources.includeCandidates.back().sourceIndex = static_cast<std::uint32_t>(prior->second);
                const auto& file = sources.includes[prior->second];
                if (!access.Consume(file.binding.bytes)) return Failure(
                    "SCRIPT_INCLUDE_ACCESS_BUDGET", "包含访问或累计读取字节超过本次工作预算。", path);
                return ContentResult::Success({file.binding.kind == Kind::RegularFile, Text(file.bytes)});
            }
            if (sources.includes.size() >= kCandidates) return Failure(
                "SCRIPT_INCLUDE_SOURCE_BUDGET", "包含候选数超过来源查询预算。", path);
            auto captured = Detail::CaptureSourceBinding(path, Kind::RegularFile, false,
                std::min(kFileBytes, kTotalBytes - sources.totalIncludeBytes));
            if (!captured.Succeeded()) return {{}, std::move(captured.diagnostics)};
            if (!access.Consume(captured.value->binding.bytes)) return Failure(
                "SCRIPT_INCLUDE_ACCESS_BUDGET", "包含访问或累计读取字节超过本次工作预算。", path);
            if (captured.value->binding.kind == Kind::RegularFile && ++regularFiles > kFiles) return Failure(
                "SCRIPT_INCLUDE_SOURCE_BUDGET", "实际包含文件数超过来源查询预算。", path);
            captured.value->binding.maxBytes = kFileBytes;
            sources.totalIncludeBytes += captured.value->binding.bytes;
            const auto position = sources.includes.size();
            sources.includeCandidates.back().sourceIndex = static_cast<std::uint32_t>(position);
            sources.includes.push_back({std::move(captured.value->binding), std::move(captured.value->bytes),
                captured.value->lastWriteTicks});
            index.emplace(path, position);
            const auto& file = sources.includes.back();
            return ContentResult::Success({file.binding.kind == Kind::RegularFile, Text(file.bytes)});
        };
        Jass::PreprocessCache cache;
        FrameworkPhaseResult framework;
        sources.preprocessed = PreprocessOwned(sources, sources.preparedMainText.empty() ? sources.request.mainText : sources.preparedMainText,
            std::move(options), framework, cache);
        sources.frameworkText = std::move(framework.text);
        sources.frameworkSourceMap = std::move(framework.sourceMap);
        sources.frameworkFeatureFlags = std::move(framework.flags);
        sources.preprocessed.diagnostics.insert(sources.preprocessed.diagnostics.begin(),
            prepared.diagnostics.begin(), prepared.diagnostics.end());
        if (!sources.preprocessed.Succeeded() && !request.captureMacroAudit) return {{}, std::move(sources.preprocessed.diagnostics)};
        CaptureResult result = CaptureResult::Success(std::move(sources));
        result.diagnostics = result.value->preprocessed.diagnostics;
        return result;
    } catch (const std::exception&) {
        return CaptureResult::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_CAPTURE_FAILED", "无法完整捕获预处理来源。");
    }
}

Status VerifyScriptPreprocessSources(const ScriptPreprocessSources& sources,
    const ScriptUserSourceRequest& currentSources, const ScriptPreprocessSourceRequest& currentRequest) {
    auto captured = CaptureScriptPreprocessSources(sources.entryPlan, currentSources, currentRequest);
    if (!captured.Succeeded()) return {std::move(captured.diagnostics)};
    const auto& now = *captured.value;
    if (sources.executionSupported || !SameRequest(sources.request, now.request) ||
        sources.mainSha256 != now.mainSha256 || sources.preparedMainText != now.preparedMainText ||
        sources.mainTransform != now.mainTransform || sources.systemIncludes != now.systemIncludes ||
        sources.frameworkText != now.frameworkText || sources.frameworkFeatureFlags != now.frameworkFeatureFlags ||
        sources.includeCandidatePolicySha256 != now.includeCandidatePolicySha256 || sources.includeCandidates != now.includeCandidates ||
        sources.forceIncludes != now.forceIncludes || sources.totalIncludeBytes != now.totalIncludeBytes ||
        sources.preprocessed.text != now.preprocessed.text || sources.includes.size() != now.includes.size() ||
        sources.preprocessed.lexicalView.text != now.preprocessed.lexicalView.text ||
        sources.preprocessed.lexicalView.opaque != now.preprocessed.lexicalView.opaque ||
        !std::equal(sources.includes.begin(), sources.includes.end(), now.includes.begin(), SameBinding)) {
        return Status::Failure("Script.PreprocessSources", "SCRIPT_INCLUDE_SOURCE_STALE", "预处理请求、候选路径或包含内容已经变化。");
    }
    return {std::move(captured.diagnostics)};
}

Jass::PreprocessResult ReplayScriptPreprocessSources(const ScriptPreprocessSources& sources) {
    const auto failed = [](std::string code, std::string message) {
        Jass::PreprocessResult result;
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", std::move(code), std::move(message), {}});
        return result;
    };
    if (!ProfileRequestMatches(sources.entryPlan, sources.request)) {
        Jass::PreprocessResult result;
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_PROFILE_PREPROCESS_REJECTED",
            "通用档案回放必须保留已绑定的主文策略、来源目录和目标版本宏。", {}});
        return result;
    }
    if (sources.includes.size() > kCandidates || sources.request.mainText.size() > kMainBytes) {
        Jass::PreprocessResult result;
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_INCLUDE_SOURCE_BUDGET",
            "预处理快照超过回放预算。", {}});
        return result;
    }
    auto prepared = PrepareMain(sources.request);
    if (!prepared.Succeeded()) { Jass::PreprocessResult result; result.diagnostics = std::move(prepared.diagnostics); return result; }
    if (sources.executionSupported || sources.mainSha256 != prepared.value->report.inputSha256 ||
        sources.preparedMainText != prepared.value->text || sources.mainTransform != prepared.value->report) {
        Jass::PreprocessResult result;
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_MAIN_TRANSFORM_SNAPSHOT_MISMATCH",
            "拥有的主文、策略、转换结果或报告身份不匹配；未执行预处理。", {}});
        return result;
    }
    auto options = Options(sources);
    auto policy = MakeCandidatePolicy(sources);
    if (!policy.Succeeded()) { Jass::PreprocessResult result; result.diagnostics = std::move(policy.diagnostics); return result; }
    if (policy.value->sha256 != sources.includeCandidatePolicySha256 || sources.includeCandidates.size() > 16384)
        return failed("SCRIPT_INCLUDE_POLICY_MISMATCH", "包含策略、冻结根或候选 trace 身份不匹配。");
    std::map<std::filesystem::path, std::size_t, PathLess> index;
    std::uint64_t ownedBytes = 0;
    std::size_t regularFiles = 0;
    for (std::size_t position = 0; position < sources.includes.size(); ++position) {
        const auto& file = sources.includes[position];
        std::filesystem::path path;
        if (!Detail::NormalizeSourcePath(file.binding.path, path) || path != file.binding.path ||
            !index.emplace(path, position).second || file.bytes.size() > kFileBytes ||
            file.bytes.size() > kTotalBytes - ownedBytes || file.binding.bytes != file.bytes.size() ||
            (file.binding.kind != Kind::RegularFile && file.binding.kind != Kind::Missing))
            return failed("SCRIPT_INCLUDE_SOURCE_INVALID", "owned 包含正/负来源结构或预算无效。");
        ownedBytes += file.bytes.size();
        if (file.binding.kind == Kind::Missing) {
            if (!file.bytes.empty() || !file.binding.sha256.empty()) return failed("SCRIPT_INCLUDE_SOURCE_INVALID", "Missing 候选不能持有内容或 SHA。");
        } else {
            if (++regularFiles > kFiles) return failed("SCRIPT_INCLUDE_SOURCE_BUDGET", "owned 实际文件数超过包含预算。");
            auto hash = Sha256(file.bytes);
            if (!hash.Succeeded() || *hash.value != file.binding.sha256)
                return failed("SCRIPT_INCLUDE_SOURCE_INVALID", "owned 包含内容与绑定 SHA 不匹配。");
        }
    }
    if (ownedBytes != sources.totalIncludeBytes) return failed("SCRIPT_INCLUDE_SOURCE_INVALID", "owned 包含累计字节不匹配。");
    CandidateTraceBuilder traceBuilder;
    std::size_t tracePosition = 0;
    options.includeCandidatePolicy = [&](const Jass::IncludeCandidateRequest& candidate) -> EligibilityResult {
        auto eligibility = policy.value->Evaluate(candidate);
        if (!eligibility.Succeeded()) return eligibility;
        auto made = traceBuilder.Make(candidate, *eligibility.value);
        if (!made.Succeeded()) return {{}, std::move(made.diagnostics)};
        if (*eligibility.value == Jass::IncludeCandidateEligibility::Allowed) {
            const auto owned = index.find(candidate.candidate);
            if (owned == index.end()) return PolicyFailure("SCRIPT_INCLUDE_UNBOUND", "回放候选缺少 owned 正/负来源。", candidate.candidate);
            made.value->sourceIndex = static_cast<std::uint32_t>(owned->second);
        }
        if (tracePosition >= sources.includeCandidates.size() || *made.value != sources.includeCandidates[tracePosition])
            return PolicyFailure("SCRIPT_INCLUDE_TRACE_MISMATCH", "回放包含候选顺序、阶段身份、资格或来源索引与快照不匹配。", candidate.candidate);
        ++tracePosition;
        return eligibility;
    };
    AccessBudget access;
    options.includeProvider = [&](const std::filesystem::path& input) -> ContentResult {
        std::filesystem::path path;
        if (!Detail::NormalizeSourcePath(input, path)) return Failure("SCRIPT_INCLUDE_PATH_SCOPE", "无效包含路径。", input);
        const auto found = index.find(path);
        if (found == index.end()) return Failure("SCRIPT_INCLUDE_UNBOUND", "回放遇到没有绑定的包含候选，禁止回退读取磁盘。", path);
        const auto& file = sources.includes[found->second];
        if (!access.Consume(file.binding.bytes)) return Failure(
            "SCRIPT_INCLUDE_ACCESS_BUDGET", "包含访问或累计读取字节超过本次工作预算。", path);
        return ContentResult::Success({file.binding.kind == Kind::RegularFile, Text(file.bytes)});
    };
    Jass::PreprocessCache cache;
    FrameworkPhaseResult framework;
    auto result = PreprocessOwned(sources, prepared.value->text.empty() ? sources.request.mainText : prepared.value->text,
        std::move(options), framework, cache);
    if (result.Succeeded() && tracePosition != sources.includeCandidates.size()) {
        result.text.clear();
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_INCLUDE_TRACE_MISMATCH",
            "回放未完整消费拥有的包含候选 trace，未发布结果。", {}});
    }
    if (result.Succeeded() && (result.lexicalView.text != sources.preprocessed.lexicalView.text ||
        result.lexicalView.opaque != sources.preprocessed.lexicalView.opaque)) {
        result.text.clear();
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_FRAMEWORK_ENCODING_SNAPSHOT_MISMATCH",
            "逐来源词法资格或原字节位置与拥有快照不一致。", {}});
    }
    if (result.Succeeded() && (framework.text != sources.frameworkText || framework.flags != sources.frameworkFeatureFlags)) {
        result.text.clear();
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Script.PreprocessSources", "SCRIPT_FRAMEWORK_DOLLAR_SNAPSHOT_MISMATCH",
            "框架阶段拥有文本与特性身份不匹配；未发布回放结果。", {}});
    }
    result.diagnostics.insert(result.diagnostics.begin(), prepared.diagnostics.begin(), prepared.diagnostics.end());
    return result;
}

} // namespace War3Map::Script
