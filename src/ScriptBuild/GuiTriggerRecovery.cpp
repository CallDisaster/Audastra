#include "War3MapCore/Script/MapScript.h"

#include "ScriptNames.h"
#include "War3MapCore/Trigger/TriggerCodeEditing.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// 代码转回 T：按生产生成器（TriggerEmitter）的写法把自定义代码解析回事件 / 条件 / 动作树，
// 再用同一生成器回生成并与正文逐词核对。解析只负责提出候选；是否接受只由回生成结果决定。
// 支持范围（与 YDWE 生成写法一致）：事件注册、条件函数、普通调用、设置变量、注释、返回、
// 多重 If、A/B/整数变量循环、单位组 / 玩家组 / 可破坏物 / 物品的“多个动作”遍历，以及
// 参数中的预设、变量（含数组下标）、字面量、函数调用与四则 / 字符串运算。
// 其余写法（YDWE 局部变量、计时器 / 触发器闭包、自定义代码行、条件函数参数等）指出行号后拒绝。
namespace War3Map::Script {
namespace {
using Trigger::UiDefinition;
using Trigger::UiDefinitionKind;

enum class Tok { Ident, Number, String, RawCode, Op, Comment };
struct Token {
    Tok kind = Tok::Op;
    std::string text;
    std::size_t line = 0;
    std::size_t begin = 0, end = 0; // 行内字节范围
};
struct Line {
    std::size_t number = 0;
    std::string_view text;
    std::vector<Token> tokens;
};
using Tokens = std::vector<Token>;

bool IdentStart(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
bool IdentPart(char c) { return IdentStart(c) || (c >= '0' && c <= '9'); }
std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
    return text;
}

// 逐行切词；字符串、四字码与行注释各自成一个词。字符串可跨行（生成器不转义换行），
// 跨行字符串所在的整段算作一行。不能切分时给出行号。
bool Lex(std::string_view text, std::vector<Line>& lines, std::size_t& badLine) {
    std::size_t number = 1, i = 0;
    for (;;) {
        const auto lineStart = i;
        Line line{number, {}, {}};
        while (i < text.size() && text[i] != '\n') {
            const char c = text[i];
            if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
            // 紧跟在标识符后的 /* … */ 是变量名里自带的说明（地图作者的写法），并入标识符，按原文比较。
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*' && !line.tokens.empty() && line.tokens.back().kind == Tok::Ident
                && line.tokens.back().end == i - lineStart) {
                const auto close = text.find("*/", i + 2);
                if (close == std::string_view::npos || text.substr(i, close - i).find('\n') != std::string_view::npos) { badLine = number; return false; }
                line.tokens.back().text += std::string(text.substr(i, close + 2 - i));
                i = close + 2; line.tokens.back().end = i - lineStart;
                continue;
            }
            Token token; token.line = number; token.begin = i - lineStart;
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                const auto stop = text.find('\n', i);
                const auto end = stop == std::string_view::npos ? text.size() : stop;
                token.kind = Tok::Comment; token.text = std::string(Trim(text.substr(i + 2, end - i - 2))); i = end;
            } else if (c == '"') {
                std::size_t j = i + 1;
                while (j < text.size() && text[j] != '"') {
                    if (text[j] == '\n') ++number;
                    j += text[j] == '\\' && j + 1 < text.size() && text[j + 1] != '\n' ? 2 : 1;
                }
                if (j >= text.size()) { badLine = token.line; return false; }
                token.kind = Tok::String; token.text = std::string(text.substr(i, j + 1 - i)); i = j + 1;
            } else if (c == '\'') {
                const auto j = text.find_first_of("'\n", i + 1);
                if (j == std::string_view::npos || text[j] != '\'') { badLine = number; return false; }
                token.kind = Tok::RawCode; token.text = std::string(text.substr(i, j + 1 - i)); i = j + 1;
            } else if ((c >= '0' && c <= '9') || c == '$' || (c == '.' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9')) {
                std::size_t j = i + 1;
                while (j < text.size() && (IdentPart(text[j]) || text[j] == '.')) ++j;
                token.kind = Tok::Number; token.text = std::string(text.substr(i, j - i)); i = j;
            } else if (IdentStart(c)) {
                std::size_t j = i + 1;
                while (j < text.size() && IdentPart(text[j])) ++j;
                token.kind = Tok::Ident; token.text = std::string(text.substr(i, j - i)); i = j;
            } else {
                const auto two = text.substr(i, 2);
                if (two == "==" || two == "!=" || two == "<=" || two == ">=") { token.text = std::string(two); i += 2; }
                else if (std::string_view("()[],=<>+-*/").find(c) != std::string_view::npos) { token.text = std::string(1, c); ++i; }
                else { badLine = number; return false; }
                token.kind = Tok::Op;
            }
            token.end = i - lineStart;
            line.tokens.push_back(std::move(token));
        }
        line.text = text.substr(lineStart, i - lineStart);
        if (!line.tokens.empty()) lines.push_back(std::move(line));
        if (i >= text.size()) return true;
        ++i; ++number;
    }
}

// 与 [b, e) 内左括号配对的右括号位置；没有时返回 e。
std::size_t Close(const Tokens& t, std::size_t open, std::size_t e) {
    int depth = 0;
    for (std::size_t i = open; i < e; ++i) {
        if (t[i].kind != Tok::Op) continue;
        if (t[i].text == "(" || t[i].text == "[") ++depth;
        else if (t[i].text == ")" || t[i].text == "]") { if (--depth == 0) return i; }
    }
    return e;
}
// 顶层（不在括号内）按某个词切开；sep 为 "," 或 "and" / "or"。
std::vector<std::pair<std::size_t, std::size_t>> SplitTop(const Tokens& t, std::size_t b, std::size_t e, std::string_view sep) {
    std::vector<std::pair<std::size_t, std::size_t>> parts;
    int depth = 0; std::size_t start = b;
    for (std::size_t i = b; i < e; ++i) {
        const auto& text = t[i].text;
        if (t[i].kind == Tok::Op && (text == "(" || text == "[")) ++depth;
        else if (t[i].kind == Tok::Op && (text == ")" || text == "]")) --depth;
        else if (depth == 0 && text == sep && (t[i].kind == Tok::Op || t[i].kind == Tok::Ident)) { parts.emplace_back(start, i); start = i + 1; }
    }
    parts.emplace_back(start, e);
    return parts;
}
bool Is(const Tokens& t, std::size_t i, std::string_view text) { return i < t.size() && t[i].text == text && t[i].kind != Tok::String && t[i].kind != Tok::Comment; }

std::string Unquote(std::string value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
    return value;
}
std::string UnescapeJassString(std::string_view literal) {
    std::string value;
    for (std::size_t i = 1; i + 1 < literal.size(); ++i) {
        if (literal[i] == '\\' && i + 2 < literal.size()) ++i;
        value.push_back(literal[i]);
    }
    return value;
}
std::string Field(const UiDefinition& definition, std::string_view key) {
    for (const auto& [name, value] : definition.Active().fields) if (name == key) return value;
    return {};
}
// 与 TriggerEmitter::ScriptName 相同：script_name / script 字段优先，否则用定义名。
std::string ScriptNameOf(const UiDefinition& definition) {
    for (const auto& [key, value] : definition.Active().fields)
        if ((key == "script_name" || key == "script") && !value.empty()) return Unquote(value);
    return definition.name;
}
// 参数槽类型（与生成器一样按原位置取类型）；含 nothing 槽的定义下标会错位，不作为候选。
std::optional<std::vector<std::string>> SlotTypes(const UiDefinition& definition) {
    std::vector<std::string> slots;
    for (const auto& argument : definition.Active().arguments) {
        if (argument.type == "nothing") return std::nullopt;
        slots.push_back(Unquote(argument.type));
    }
    return slots;
}

struct PArg;
struct PEca {
    std::int32_t type = 2;
    std::string name;
    std::optional<std::int32_t> childId;
    std::vector<PArg> args;
    std::vector<PEca> children;
};
struct PArg {
    std::int32_t type = 3;
    std::string value;
    std::vector<PEca> call;   // 0 或 1 个
    std::vector<PArg> index;  // 0 或 1 个
};

class Parser {
public:
    Parser(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog, const Trigger::TriggerRecord& record)
        : document_(document), catalog_(catalog), types_(catalog), record_(record) {
        const auto conv = ConvertObjectName(record.name.text);
        prefix_ = "Trig_" + conv; triggerVar_ = "gg_trg_" + conv; init_ = "InitTrig_" + conv;
        for (const auto* definition : catalog.Definitions(UiDefinitionKind::Event)) events_[ScriptNameOf(*definition)].push_back(definition);
        for (const auto* definition : catalog.Definitions(UiDefinitionKind::Condition)) conditions_[ScriptNameOf(*definition)].push_back(definition);
        for (const auto* definition : catalog.Definitions(UiDefinitionKind::Action)) actions_[ScriptNameOf(*definition)].push_back(definition);
        for (const auto* definition : catalog.Definitions(UiDefinitionKind::Define)) {
            if (definition->group != "TriggerParams") continue;
            const auto type = types_.PresetType(definition->name);
            const auto value = Field(*definition, "value");
            if (!type || value.empty()) continue;
            const auto parts = detail::SplitDefineValue(value);
            if (parts.size() < 3 || parts[2].empty()) continue;
            std::string script = parts[2];
            std::replace(script.begin(), script.end(), '`', '"');
            std::vector<Line> lines; std::size_t bad = 0;
            if (!Lex(script, lines, bad) || lines.size() != 1) continue;
            presets_[*type].push_back({std::move(lines.front().tokens), definition->name});
        }
        for (const auto& variable : document.variables) variables_.emplace(variable.name.text, &variable);
    }

    std::optional<Trigger::GuiTriggerFragment> Parse(std::string_view text) {
        std::vector<Line> lines; std::size_t bad = 0;
        if (!Lex(text, lines, bad)) return Fail(bad, "无法切分这一行（字符串或字符未闭合）。");
        // 函数划分：函数之外只允许注释。
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto& t = lines[i].tokens;
            if (t.front().kind == Tok::Comment && t.size() == 1) continue;
            if (!Is(t, 0, "function") || t.size() < 2 || t[1].kind != Tok::Ident)
                return Fail(lines[i].number, "函数之外的代码不能转回 T。");
            Function function; function.line = lines[i].number;
            function.returnsBoolean = t.back().text == "boolean";
            if (!Is(t, 2, "takes") || !Is(t, 3, "nothing") || !Is(t, 4, "returns") || t.size() != 6)
                return Fail(lines[i].number, "T 生成的函数都不带参数（takes nothing）。");
            std::size_t j = i + 1;
            while (j < lines.size() && !Is(lines[j].tokens, 0, "endfunction")) {
                if (Is(lines[j].tokens, 0, "function")) return Fail(lines[j].number, "函数缺少 endfunction。");
                function.body.push_back(lines[j]); ++j;
            }
            if (j >= lines.size()) return Fail(lines[i].number, "函数缺少 endfunction。");
            if (lines[j].tokens.size() != 1) return Fail(lines[j].number, "endfunction 之后不能再有代码。");
            if (!functions_.emplace(t[1].text, std::move(function)).second) return Fail(lines[i].number, "函数重复定义：" + t[1].text);
            i = j;
        }
        auto* init = Take(init_);
        if (!init) return Fail(lines.empty() ? 1 : lines.back().number, "找不到初始化函数 " + init_ + "（T 生成写法）。");
        std::vector<PEca> roots;
        std::string conditionFunction, actionFunction;
        const auto& body = init->body;
        if (body.empty() || !SameText(body.front().tokens, {"set", triggerVar_, "=", "CreateTrigger", "(", ")"}))
            return Fail(body.empty() ? init->line : body.front().number, "初始化函数须以 set " + triggerVar_ + " = CreateTrigger() 开始。");
        for (std::size_t i = 1; i < body.size(); ++i) {
            const auto& t = body[i].tokens;
            const auto n = body[i].number;
            if (SameText(t, {"call", "DisableTrigger", "(", triggerVar_, ")"})) continue; // 由“初始关闭”生成
            if (t.size() == 11 && SameText(t, {"call", "TriggerAddCondition", "(", triggerVar_, ",", "Condition", "(", "function",
                    t[8].text, ")", ")"})) {
                conditionFunction = t[8].text; continue;
            }
            if (SameText(t, {"call", "TriggerAddAction", "(", triggerVar_, ",", "function", t.size() > 6 ? t[6].text : "", ")"})) {
                actionFunction = t[6].text; continue;
            }
            auto event = Event(t, n);
            if (!event) return std::nullopt;
            roots.push_back(std::move(*event));
        }
        if (!conditionFunction.empty()) {
            auto* function = Take(conditionFunction);
            if (!function || !function->returnsBoolean) return Fail(init->line, "条件函数 " + conditionFunction + " 不存在或不返回 boolean。");
            if (function->body.size() != 1 || !Is(function->body.front().tokens, 0, "return"))
                return Fail(function->line, "条件函数须只有一行 return (条件 and 条件 …)。");
            const auto& t = function->body.front().tokens;
            if (!Is(t, 1, "(") || Close(t, 1, t.size()) != t.size() - 1) return Fail(function->body.front().number, "条件须写成 return (…)。");
            for (const auto& [b, e] : SplitTop(t, 2, t.size() - 1, "and")) {
                auto condition = Condition(t, b, e);
                if (!condition) return Fail(function->body.front().number, "无法识别为 T 条件：" + Text(t, b, e));
                roots.push_back(std::move(*condition));
            }
        }
        if (actionFunction.empty()) return Fail(init->line, "初始化函数缺少 TriggerAddAction。");
        auto* actions = Take(actionFunction);
        if (!actions || actions->returnsBoolean) return Fail(init->line, "动作函数 " + actionFunction + " 不存在。");
        if (!Actions(actions->body, 0, actions->body.size(), roots, std::nullopt)) return std::nullopt;
        for (const auto& [name, function] : functions_)
            if (!function.used) return Fail(function.line, "函数 " + name + " 不属于这个触发器的 T 写法。");
        Trigger::GuiTriggerFragment fragment; fragment.runOnInit = record_.runOnInit != 0;
        for (auto& root : roots) fragment.roots.push_back(Flatten(root, fragment));
        return fragment;
    }

    std::size_t FailLine() const { return failLine_; }
    const std::string& FailReason() const { return failReason_; }

private:
    struct Function {
        std::size_t line = 0;
        std::vector<Line> body;
        bool returnsBoolean = false;
        bool used = false;
    };
    struct Preset { Tokens script; std::string name; };

    std::nullopt_t Fail(std::size_t line, std::string reason) {
        if (failLine_ == 0) { failLine_ = line; failReason_ = std::move(reason); }
        return std::nullopt;
    }
    Function* Take(const std::string& name) {
        const auto found = functions_.find(name);
        if (found == functions_.end() || found->second.used) return nullptr;
        found->second.used = true;
        return &found->second;
    }
    static bool SameText(const Tokens& t, std::initializer_list<std::string_view> expected) {
        if (t.size() != expected.size()) return false;
        std::size_t i = 0;
        for (const auto text : expected) if (t[i].kind == Tok::Comment || t[i++].text != text) return false;
        return true;
    }
    static std::string Text(const Tokens& t, std::size_t b, std::size_t e) {
        std::string text;
        for (std::size_t i = b; i < e; ++i) { if (i > b) text += ' '; text += t[i].text; }
        return text;
    }
    static bool SameTokens(const Tokens& t, std::size_t b, std::size_t e, const Tokens& script) {
        if (e - b != script.size()) return false;
        for (std::size_t i = 0; i < script.size(); ++i) if (t[b + i].text != script[i].text) return false;
        return true;
    }
    std::string BaseType(const std::string& type) const {
        const auto* info = types_.Find(type);
        return info && !info->baseType.empty() ? info->baseType : type;
    }

    // —— 参数值 ——
    // score 累加类型证据：类型完全一致 +2，可接受 +1，字面量 0；候选间取证据最多者。
    std::optional<PArg> Value(const Tokens& t, std::size_t b, std::size_t e, const std::string& slot, int& score, std::string_view raw) {
        if (b >= e) return std::nullopt;
        if (slot == "boolexpr" || slot == "boolcall" || slot == "code" || slot == "eventcall") return std::nullopt;
        if (slot == "scriptcode") { PArg arg; arg.value = std::string(raw); return arg; }
        if (const auto found = presets_.find(slot); found != presets_.end())
            for (const auto& preset : found->second)
                if (SameTokens(t, b, e, preset.script)) { score += 2; PArg arg; arg.type = 0; arg.value = preset.name; return arg; }
        const auto kind = types_.LiteralKind(slot);
        using Literal = Trigger::TriggerTypeSystem::Literal;
        const auto literal = [&](std::string value) -> std::optional<PArg> {
            if (!types_.ValidateLiteral(slot, value).Succeeded()) return std::nullopt;
            PArg arg; arg.value = std::move(value); return arg;
        };
        if (e - b == 1) {
            const auto& token = t[b];
            if (token.kind == Tok::String && (kind == Literal::String || BaseType(slot) == "string" || slot == "StringExt"))
                return literal(UnescapeJassString(token.text));
            if (token.kind == Tok::RawCode && kind == Literal::ObjectCode) return literal(token.text.substr(1, token.text.size() - 2));
            if (token.kind == Tok::Number && (kind == Literal::Integer || kind == Literal::Real)) return literal(token.text);
            if (token.kind == Tok::Ident && (token.text == "true" || token.text == "false") && kind == Literal::Boolean) return literal(token.text);
        }
        if (e - b == 2 && Is(t, b, "-") && t[b + 1].kind == Tok::Number && (kind == Literal::Integer || kind == Literal::Real))
            return literal("-" + t[b + 1].text);
        if (t[b].kind == Tok::Ident) {
            if (auto variable = Variable(t, b, e, slot, score)) return variable;
            if (Is(t, b + 1, "(") && Close(t, b + 1, e) == e - 1) return Call(t, b, e, slot, score);
        }
        if (Is(t, b, "(") && Close(t, b, e) == e - 1) return Binary(t, b, e, slot, score);
        return std::nullopt;
    }
    // udg_ 变量（可带数组下标）或预生成的 gg_ 全局量。
    std::optional<PArg> Variable(const Tokens& t, std::size_t b, std::size_t e, const std::string& slot, int& score,
        std::string* declaredType = nullptr) {
        const auto& name = t[b].text;
        const bool indexed = e - b > 1;
        if (indexed && (!Is(t, b + 1, "[") || Close(t, b + 1, e) != e - 1)) return std::nullopt;
        PArg arg; arg.type = 1;
        std::string type;
        bool array = false;
        if (name.starts_with("udg_")) {
            // 参数值原样带上名字后的 /* 说明 */；变量按完整名或去掉说明后的名字查找。
            auto found = variables_.find(name.substr(4));
            if (found == variables_.end()) {
                const auto note = name.find("/*");
                if (note != std::string::npos) found = variables_.find(name.substr(4, note - 4));
            }
            if (found == variables_.end()) return std::nullopt;
            arg.value = name.substr(4); type = found->second->typeName.text; array = found->second->isArray != 0;
        } else if (name.starts_with("gg_trg_")) {
            for (const auto& record : document_.triggers)
                if ("gg_trg_" + ConvertObjectName(record.name.text) == name) { arg.value = "gg_trg_" + record.name.text; break; }
            if (arg.value.empty()) return std::nullopt;
            type = "trigger";
        } else {
            static constexpr std::pair<std::string_view, std::string_view> kGenerated[] = {
                {"gg_rct_", "rect"}, {"gg_snd_", "sound"}, {"gg_cam_", "camerasetup"},
                {"gg_unit_", "unit"}, {"gg_item_", "item"}, {"gg_dest_", "destructable"}};
            for (const auto& [prefix, generatedType] : kGenerated)
                if (name.starts_with(prefix)) { arg.value = name; type = generatedType; }
            if (arg.value.empty()) return std::nullopt;
        }
        if (indexed != array) return std::nullopt;
        if (!slot.empty() && slot != "AnyGlobal" && !types_.Accepts(slot, type)) return std::nullopt;
        score += type == slot ? 2 : 1;
        if (indexed) {
            int indexScore = 0;
            auto index = Value(t, b + 2, e - 1, "integer", indexScore, {});
            if (!index) return std::nullopt;
            arg.index.push_back(std::move(*index));
        }
        if (declaredType) *declaredType = type;
        return arg;
    }
    // 值位置的函数调用按定义名写出（与生成器相同）。
    std::optional<PArg> Call(const Tokens& t, std::size_t b, std::size_t e, const std::string& slot, int& score) {
        const auto* definition = catalog_.Find(UiDefinitionKind::Call, t[b].text);
        if (!definition) return std::nullopt;
        const auto returns = Unquote(Field(*definition, "returns"));
        if (!types_.Accepts(slot, returns)) return std::nullopt;
        auto eca = Invocation(*definition, 3, t, b + 2, e - 1, score);
        if (!eca) return std::nullopt;
        score += returns == slot ? 2 : 1;
        PArg arg; arg.type = 2; arg.value = definition->name; arg.call.push_back(std::move(*eca));
        return arg;
    }
    // (A 运算 B)：四则运算函数（第二个参数为 ArithmeticOperator）或字符串连接。
    std::optional<PArg> Binary(const Tokens& t, std::size_t b, std::size_t e, const std::string& slot, int& score) {
        int depth = 0;
        for (std::size_t i = b + 1; i + 1 < e; ++i) {
            const auto& token = t[i];
            if (token.kind == Tok::Op && (token.text == "(" || token.text == "[")) { ++depth; continue; }
            if (token.kind == Tok::Op && (token.text == ")" || token.text == "]")) { --depth; continue; }
            if (depth != 0 || token.kind != Tok::Op || i == b + 1 || std::string_view("+-*/").find(token.text) == std::string_view::npos
                || token.text.size() != 1 || t[i - 1].kind == Tok::Op && t[i - 1].text != ")" && t[i - 1].text != "]") continue;
            std::optional<PArg> best; int bestScore = -1;
            for (const auto* definition : catalog_.Definitions(UiDefinitionKind::Call)) {
                const auto slots = SlotTypes(*definition);
                const auto returns = Unquote(Field(*definition, "returns"));
                if (!slots || !types_.Accepts(slot, returns)) continue;
                std::vector<std::pair<std::size_t, std::size_t>> spans;
                if (slots->size() == 3 && (*slots)[1] == "ArithmeticOperator") spans = {{b + 1, i}, {i, i + 1}, {i + 1, e - 1}};
                else if (definition->name == "OperatorString" && slots->size() == 2 && token.text == "+") spans = {{b + 1, i}, {i + 1, e - 1}};
                else continue;
                int candidateScore = returns == slot ? 2 : 1;
                PEca eca; eca.type = 3; eca.name = definition->name;
                bool ok = true;
                for (std::size_t k = 0; k < spans.size() && ok; ++k) {
                    auto arg = Value(t, spans[k].first, spans[k].second, (*slots)[k], candidateScore, {});
                    ok = arg.has_value();
                    if (ok) eca.args.push_back(std::move(*arg));
                }
                if (ok && candidateScore > bestScore) {
                    PArg arg; arg.type = 2; arg.value = definition->name; arg.call.push_back(std::move(eca));
                    best = std::move(arg); bestScore = candidateScore;
                }
            }
            if (best) { score += bestScore; return best; }
            return std::nullopt;
        }
        return std::nullopt;
    }
    // 定义的全部参数：逗号分隔，与参数槽一一对应。
    std::optional<PEca> Invocation(const UiDefinition& definition, std::int32_t type, const Tokens& t, std::size_t b, std::size_t e,
        int& score, const Line* line = nullptr) {
        const auto slots = SlotTypes(definition);
        if (!slots) return std::nullopt;
        const auto spans = b == e ? std::vector<std::pair<std::size_t, std::size_t>>{} : SplitTop(t, b, e, ",");
        if (spans.size() != slots->size()) return std::nullopt;
        PEca eca; eca.type = type; eca.name = definition.name;
        for (std::size_t k = 0; k < spans.size(); ++k) {
            const auto [sb, se] = spans[k];
            std::string_view raw;
            if (line && sb < se) raw = line->text.substr(t[sb].begin, t[se - 1].end - t[sb].begin);
            auto arg = Value(t, sb, se, (*slots)[k], score, raw);
            if (!arg) return std::nullopt;
            eca.args.push_back(std::move(*arg));
        }
        return eca;
    }
    // 同名脚本的多个定义里取类型证据最多的一个。
    std::optional<PEca> Best(const std::vector<const UiDefinition*>* candidates, std::int32_t type, const Tokens& t,
        std::size_t b, std::size_t e, const Line* line = nullptr) {
        if (!candidates) return std::nullopt;
        std::optional<PEca> best; int bestScore = -1;
        for (const auto* definition : *candidates) {
            int score = 0;
            auto eca = Invocation(*definition, type, t, b, e, score, line);
            if (eca && score > bestScore) { best = std::move(eca); bestScore = score; }
        }
        return best;
    }
    template<class Map> static const std::vector<const UiDefinition*>* Lookup(const Map& map, const std::string& name) {
        const auto found = map.find(name);
        return found == map.end() ? nullptr : &found->second;
    }

    // —— 事件、条件、动作 ——
    std::optional<PEca> Event(const Tokens& t, std::size_t line) {
        if (t.size() < 6 || !Is(t, 0, "call") || t[1].kind != Tok::Ident || !Is(t, 2, "(") || Close(t, 2, t.size()) != t.size() - 1
            || !Is(t, 3, triggerVar_))
            return Fail(line, "无法识别为 T 事件注册。");
        const bool more = Is(t, 4, ",");
        if (!more && !Is(t, 4, ")")) return Fail(line, "无法识别为 T 事件注册。");
        auto event = Best(Lookup(events_, t[1].text), 0, t, more ? 5 : 4, t.size() - 1);
        if (!event) return Fail(line, "函数库中没有参数相符、脚本名为 " + t[1].text + " 的事件。");
        return event;
    }
    std::optional<PEca> Condition(const Tokens& t, std::size_t b, std::size_t e) {
        if (b >= e) return std::nullopt;
        if (Is(t, b, "(") && Close(t, b, e) == e - 1) {
            // (条件 and 条件) / (条件 or 条件)：多个条件；(true) / (false) 为空的“且 / 或”。
            for (const auto* junction : {"and", "or"}) {
                const auto parts = SplitTop(t, b + 1, e - 1, junction);
                const bool empty = e - b == 3 && Is(t, b + 1, junction == std::string_view("and") ? "true" : "false");
                // ((条件))：只有一个子条件的“且”，内层本身是完整的带括号条件。
                const bool single = junction == std::string_view("and") && parts.size() == 1 && Is(t, b + 1, "(")
                    && Close(t, b + 1, e) == e - 2;
                if (parts.size() < 2 && !empty && !single) continue;
                PEca eca; eca.type = 1; eca.name = junction == std::string_view("and") ? "AndMultiple" : "OrMultiple";
                if (!catalog_.Find(UiDefinitionKind::Condition, eca.name)) return std::nullopt;
                if (!empty) for (const auto& [pb, pe] : parts) {
                    auto child = Condition(t, pb, pe);
                    if (!child) return std::nullopt;
                    child->childId = 0; eca.children.push_back(std::move(*child));
                }
                return eca;
            }
            // (A 比较 B)：第二个参数为比较运算符的条件。
            int depth = 0;
            for (std::size_t i = b + 1; i + 1 < e; ++i) {
                const auto& token = t[i];
                if (token.kind == Tok::Op && (token.text == "(" || token.text == "[")) { ++depth; continue; }
                if (token.kind == Tok::Op && (token.text == ")" || token.text == "]")) { --depth; continue; }
                if (depth != 0 || token.kind != Tok::Op || i == b + 1) continue;
                static constexpr std::string_view kComparisons[] = {"==", "!=", "<", ">", "<=", ">="};
                if (std::find(std::begin(kComparisons), std::end(kComparisons), token.text) == std::end(kComparisons)) continue;
                std::optional<PEca> best; int bestScore = -1;
                for (const auto* definition : catalog_.Definitions(UiDefinitionKind::Condition)) {
                    const auto slots = SlotTypes(*definition);
                    if (!slots || slots->size() != 3 || ((*slots)[1] != "ComparisonOperator" && (*slots)[1] != "EqualNotEqualOperator")) continue;
                    // 比较式没有逗号，按运算符位置逐个取值。
                    PEca compare; compare.type = 1; compare.name = definition->name;
                    int score = 0;
                    auto left = Value(t, b + 1, i, (*slots)[0], score, {});
                    auto op = Value(t, i, i + 1, (*slots)[1], score, {});
                    auto right = Value(t, i + 1, e - 1, (*slots)[2], score, {});
                    if (!left || !op || !right || op->type != 0) continue;
                    compare.args = {std::move(*left), std::move(*op), std::move(*right)};
                    if (score > bestScore) { best = std::move(compare); bestScore = score; }
                }
                return best;
            }
            return std::nullopt;
        }
        if (t[b].kind == Tok::Ident && Is(t, b + 1, "(") && Close(t, b + 1, e) == e - 1)
            return Best(Lookup(conditions_, t[b].text), 1, t, b + 2, e - 1);
        // 布尔变量：生成器不写函数名，只写参数。
        if (const auto* definition = catalog_.Find(UiDefinitionKind::Condition, "GetBooleanVariableValue")) {
            int score = 0;
            if (auto eca = Invocation(*definition, 1, t, b, e, score)) return eca;
        }
        return std::nullopt;
    }
    // 动作行 [b, e)；parent 非空时为块内子动作的分支号。
    bool Actions(const std::vector<Line>& body, std::size_t b, std::size_t e, std::vector<PEca>& out, std::optional<std::int32_t> block) {
        for (std::size_t i = b; i < e; ++i) {
            const auto& t = body[i].tokens;
            const auto line = body[i].number;
            std::optional<PEca> action;
            if (t.size() == 1 && t.front().kind == Tok::Comment) {
                if (!catalog_.Find(UiDefinitionKind::Action, "CommentString")) return Fail(line, "函数库中没有“注释”动作。"), false;
                PEca comment; comment.name = "CommentString";
                PArg text; text.value = t.front().text; comment.args.push_back(std::move(text));
                action = std::move(comment);
            } else if (SameText(t, {"return"})) {
                PEca ret; ret.name = "ReturnAction"; action = std::move(ret);
            } else if (Is(t, 0, "if")) {
                std::size_t elseLine = e, endLine = e;
                if (!Block(body, i, e, "if", "endif", &elseLine, endLine)) return Fail(line, "if 缺少配对的 else / endif（T 的 If 总是写出 else）。"), false;
                if (t.size() < 4 || !Is(t, t.size() - 1, "then") || !Is(t, 1, "(") || Close(t, 1, t.size()) != t.size() - 2)
                    return Fail(line, "无法识别为 T 的 If（多个条件）。"), false;
                PEca branch; branch.name = "IfThenElseMultiple";
                const bool always = t.size() == 5 && Is(t, 2, "true");
                if (!always) for (const auto& [pb, pe] : SplitTop(t, 2, t.size() - 2, "and")) {
                    auto condition = Condition(t, pb, pe);
                    if (!condition) return Fail(line, "无法识别为 T 条件：" + Text(t, pb, pe)), false;
                    condition->childId = 0; branch.children.push_back(std::move(*condition));
                }
                if (elseLine == e) return Fail(line, "T 的 If 总是写出 else。"), false;
                if (!Actions(body, i + 1, elseLine, branch.children, 1) || !Actions(body, elseLine + 1, endLine, branch.children, 2)) return false;
                action = std::move(branch);
                i = endLine;
            } else if (Is(t, 0, "set")) {
                std::size_t consumed = 0;
                if (!Assignment(body, i, e, action, consumed)) return false;
                i += consumed - 1;
            } else if (Is(t, 0, "call") && t.size() >= 4 && t[1].kind == Tok::Ident && Is(t, 2, "(") && Close(t, 2, t.size()) == t.size() - 1) {
                action = CallAction(body[i]);
                if (!action) return false;
            } else {
                return Fail(line, "无法识别为 T 动作。"), false;
            }
            if (block) action->childId = *block;
            out.push_back(std::move(*action));
        }
        return true;
    }
    // 从 open 行开始找配对的结束行（以及同层 else）；嵌套 if / loop 分别计数。
    static bool Block(const std::vector<Line>& body, std::size_t open, std::size_t e, std::string_view opening,
        std::string_view closing, std::size_t* elseLine, std::size_t& endLine) {
        int depth = 0;
        for (std::size_t j = open; j < e; ++j) {
            const auto& t = body[j].tokens;
            if (Is(t, 0, opening)) ++depth;
            else if (Is(t, 0, closing)) { if (--depth == 0) { endLine = j; return true; } }
            else if (elseLine && depth == 1 && SameText(t, {"else"})) *elseLine = j;
        }
        return false;
    }
    std::optional<PEca> CallAction(const Line& line) {
        const auto& t = line.tokens;
        const auto& name = t[1].text;
        // 遍历的“多个动作”：最后一个参数为 function <名>A，子动作在该函数里。
        static const std::unordered_map<std::string_view, std::string_view> kEnumerations = {
            {"ForGroupBJ", "ForGroupMultiple"}, {"ForForce", "ForForceMultiple"},
            {"EnumDestructablesInRectAll", "EnumDestructablesInRectAllMultiple"},
            {"EnumDestructablesInCircleBJ", "EnumDestructablesInCircleBJMultiple"}, {"EnumItemsInRectBJ", "EnumItemsInRectBJMultiple"}};
        if (const auto found = kEnumerations.find(name); found != kEnumerations.end() && t.size() >= 7
            && Is(t, t.size() - 3, "function") && Is(t, t.size() - 4, ",")) {
            const auto* definition = catalog_.Find(UiDefinitionKind::Action, found->second);
            auto* helper = Take(t[t.size() - 2].text);
            if (!definition || !helper) return Fail(line.number, "遍历的子动作函数 " + t[t.size() - 2].text + " 不存在或已被使用。");
            int score = 0;
            auto eca = Invocation(*definition, 2, t, 3, t.size() - 4, score, &line);
            if (!eca) return Fail(line.number, std::string(found->second) + " 的参数与函数库定义不符。");
            if (!Actions(helper->body, 0, helper->body.size(), eca->children, 0)) return std::nullopt;
            return eca;
        }
        auto action = Best(Lookup(actions_, name), 2, t, 3, t.size() - 1, &line);
        if (!action) return Fail(line.number, actions_.contains(name) ? name + " 的参数与函数库定义不符（或含函数 / 条件参数）。"
            : "函数库中没有脚本名为 " + name + " 的动作。");
        return action;
    }
    // set 行：普通赋值，或 A/B/整数变量循环的开头。consumed 为用掉的行数。
    bool Assignment(const std::vector<Line>& body, std::size_t i, std::size_t e, std::optional<PEca>& action, std::size_t& consumed) {
        const auto& t = body[i].tokens;
        const auto line = body[i].number;
        std::size_t equals = 2;
        if (Is(t, 2, "[")) equals = Close(t, 2, t.size()) + 1;
        if (t.size() < 4 || t[1].kind != Tok::Ident || !Is(t, equals, "=") || equals + 1 >= t.size())
            return Fail(line, "无法识别为 T 的设置变量。"), false;
        const auto& target = t[1].text;
        for (const auto* letter : {"A", "B"}) {
            const std::string index = std::string("bj_forLoop") + letter + "Index";
            if (target != index) continue;
            // set 索引 = 起点 / set 索引End = 终点 / loop / exitwhen 索引 > 索引End / … / set 索引 = 索引 + 1 / endloop
            if (i + 3 >= e || !Is(body[i + 1].tokens, 0, "set") || !Is(body[i + 1].tokens, 1, index + "End") || !Is(body[i + 1].tokens, 2, "=")
                || !SameText(body[i + 2].tokens, {"loop"}) || !SameText(body[i + 3].tokens, {"exitwhen", index, ">", index + "End"}))
                return Fail(line, std::string("无法识别为 T 的循环（整数 ") + letter + "）。"), false;
            std::size_t endLine = e;
            if (!Block(body, i + 2, e, "loop", "endloop", nullptr, endLine) || endLine < i + 5
                || !SameText(body[endLine - 1].tokens, {"set", index, "=", index, "+", "1"}))
                return Fail(line, "循环缺少 T 写法的递增与 endloop。"), false;
            const auto* definition = catalog_.Find(UiDefinitionKind::Action, std::string("ForLoop") + letter + "Multiple");
            const auto slots = definition ? SlotTypes(*definition) : std::nullopt;
            if (!slots || slots->size() != 2) return Fail(line, "函数库中没有对应的循环动作。"), false;
            int score = 0;
            const auto& endTokens = body[i + 1].tokens;
            auto from = Value(t, 3, t.size(), (*slots)[0], score, {});
            auto to = Value(endTokens, 3, endTokens.size(), (*slots)[1], score, {});
            if (!from || !to) return Fail(line, "循环起点或终点无法识别。"), false;
            PEca loop; loop.name = definition->name; loop.args = {std::move(*from), std::move(*to)};
            if (!Actions(body, i + 4, endLine - 1, loop.children, 0)) return false;
            action = std::move(loop); consumed = endLine - i + 1;
            return true;
        }
        // 整数变量循环：set 变量 = 起点 / loop / exitwhen 变量 > 终点 / … / set 变量 = 变量 + 1 / endloop
        if (i + 2 < e && SameText(body[i + 1].tokens, {"loop"}) && Is(body[i + 2].tokens, 0, "exitwhen")) {
            const Tokens lhs(t.begin() + 1, t.begin() + static_cast<std::ptrdiff_t>(equals));
            const auto& exit = body[i + 2].tokens;
            std::size_t endLine = e;
            if (!Block(body, i + 1, e, "loop", "endloop", nullptr, endLine) || endLine < i + 4
                || exit.size() < lhs.size() + 3 || !SameTokens(exit, 1, 1 + lhs.size(), lhs) || !Is(exit, 1 + lhs.size(), ">"))
                return Fail(line, "无法识别为 T 的整数变量循环。"), false;
            Tokens step(lhs); step.insert(step.begin(), t[0]);
            const auto& last = body[endLine - 1].tokens;
            if (last.size() != 2 * lhs.size() + 4 || !SameTokens(last, 1, 1 + lhs.size(), lhs) || !Is(last, 1 + lhs.size(), "=")
                || !SameTokens(last, 2 + lhs.size(), 2 + 2 * lhs.size(), lhs) || !Is(last, 2 + 2 * lhs.size(), "+") || !Is(last, 3 + 2 * lhs.size(), "1"))
                return Fail(line, "整数变量循环缺少 T 写法的递增。"), false;
            const auto* definition = catalog_.Find(UiDefinitionKind::Action, "ForLoopVarMultiple");
            const auto slots = definition ? SlotTypes(*definition) : std::nullopt;
            if (!slots || slots->size() != 3) return Fail(line, "函数库中没有整数变量循环动作。"), false;
            int score = 0;
            auto variable = Variable(t, 1, equals, "integer", score);
            auto from = Value(t, equals + 1, t.size(), (*slots)[1], score, {});
            auto to = Value(exit, 2 + lhs.size(), exit.size(), (*slots)[2], score, {});
            if (!variable || !from || !to) return Fail(line, "整数变量循环的变量、起点或终点无法识别。"), false;
            PEca loop; loop.name = definition->name; loop.args = {std::move(*variable), std::move(*from), std::move(*to)};
            if (!Actions(body, i + 3, endLine - 1, loop.children, 0)) return false;
            action = std::move(loop); consumed = endLine - i + 1;
            return true;
        }
        int score = 0;
        std::string type;
        auto variable = Variable(t, 1, equals, "AnyGlobal", score, &type);
        if (!variable) return Fail(line, "赋值目标不是触发器变量：" + Text(t, 1, equals)), false;
        auto value = Value(t, equals + 1, t.size(), type, score, {});
        if (!value) return Fail(line, "无法识别为 " + type + " 类型的值：" + Text(t, equals + 1, t.size())), false;
        PEca set; set.name = "SetVariable"; set.args = {std::move(*variable), std::move(*value)};
        action = std::move(set); consumed = 1;
        return true;
    }

    std::uint32_t Flatten(PEca& eca, Trigger::GuiTriggerFragment& fragment) {
        const auto index = static_cast<std::uint32_t>(fragment.ecas.size());
        fragment.ecas.emplace_back();
        Trigger::EcaNode node;
        node.type = eca.type; node.childId = eca.childId; node.name = {eca.name, Trigger::TextEncoding::Utf8, {}}; node.enabled = 1;
        for (auto& arg : eca.args) node.arguments.push_back(FlattenArg(arg, fragment));
        for (auto& child : eca.children) node.children.push_back(Flatten(child, fragment));
        fragment.ecas[index] = std::move(node);
        return index;
    }
    std::uint32_t FlattenArg(PArg& arg, Trigger::GuiTriggerFragment& fragment) {
        const auto index = static_cast<std::uint32_t>(fragment.arguments.size());
        fragment.arguments.emplace_back();
        Trigger::ArgumentNode node;
        node.type = arg.type; node.value = {arg.value, Trigger::TextEncoding::Utf8, {}};
        if (!arg.call.empty()) { node.insertCall = 1; node.callEca = Flatten(arg.call.front(), fragment); }
        if (!arg.index.empty()) { node.insertIndex = 1; node.indexArgument = FlattenArg(arg.index.front(), fragment); }
        fragment.arguments[index] = std::move(node);
        return index;
    }

    const Trigger::TriggerDocument& document_;
    const Trigger::UiCatalog& catalog_;
    Trigger::TriggerTypeSystem types_;
    const Trigger::TriggerRecord& record_;
    std::string prefix_, triggerVar_, init_;
    std::unordered_map<std::string, std::vector<const UiDefinition*>> events_, conditions_, actions_;
    std::unordered_map<std::string, std::vector<Preset>> presets_;
    std::unordered_map<std::string, const Trigger::VariableRecord*> variables_;
    std::unordered_map<std::string, Function> functions_;
    std::size_t failLine_ = 0;
    std::string failReason_;
};

// 逐词核对作者正文与回生成结果：空白、空行、注释前后空格不计；两边各自定义的函数名可整体一一改名
// （作者插入动作后辅助函数编号变化）。返回第一处不一致的作者行号，0 表示一致。
std::size_t FirstMismatch(std::string_view authored, std::string_view generated, std::string& expected) {
    std::vector<Line> left, right; std::size_t bad = 0;
    if (!Lex(authored, left, bad)) return bad;
    if (!Lex(generated, right, bad)) { expected = "生成结果无法切分"; return 1; }
    Tokens a, b;
    for (auto& line : left) for (auto& token : line.tokens) a.push_back(std::move(token));
    for (auto& line : right) for (auto& token : line.tokens) b.push_back(std::move(token));
    const auto defined = [](const Tokens& tokens) {
        std::unordered_set<std::string> names;
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i)
            if (tokens[i].kind == Tok::Ident && tokens[i].text == "function" && tokens[i + 1].kind == Tok::Ident
                && i + 2 < tokens.size() && tokens[i + 2].text == "takes") names.insert(tokens[i + 1].text);
        return names;
    };
    const auto leftNames = defined(a), rightNames = defined(b);
    std::unordered_map<std::string, std::string> forward, backward;
    const auto count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        bool same = x.kind == y.kind && x.text == y.text;
        if (!same && x.kind == Tok::Ident && y.kind == Tok::Ident && leftNames.contains(x.text) && rightNames.contains(y.text)) {
            const auto [f, fNew] = forward.emplace(x.text, y.text);
            const auto [g, gNew] = backward.emplace(y.text, x.text);
            same = f->second == y.text && g->second == x.text;
        } else if (same && x.kind == Tok::Ident && leftNames.contains(x.text)) {
            const auto [f, fNew] = forward.emplace(x.text, y.text);
            const auto [g, gNew] = backward.emplace(y.text, x.text);
            same = f->second == y.text && g->second == x.text;
        }
        if (!same) { expected = y.kind == Tok::Comment ? "// " + y.text : y.text; return x.line; }
    }
    if (a.size() != b.size()) {
        expected = a.size() < b.size() ? b[count].text : "（正文结束）";
        return a.size() < b.size() ? (a.empty() ? 1 : a.back().line) : a[count].line;
    }
    return 0;
}

Status Mismatch(std::size_t line, std::string reason) {
    return Status::Failure("Script.Trigger", "CODE_RECOVERY_REJECTED", "第 " + std::to_string(line) + " 行：" + std::move(reason));
}

// 候选回生成并与正文核对；一致才算可转回。
Status Verify(const MapScriptInputs& inputs, const Trigger::TriggerRecord& record, const Trigger::GuiTriggerFragment& fragment,
    std::string_view text) {
    auto probe = Trigger::TriggerCodeEditor::RecoveryProbe(*inputs.triggers, *inputs.catalog, record.id, fragment);
    if (!probe.Succeeded() || !probe.value) return {std::move(probe.diagnostics)};
    auto probeInputs = inputs; probeInputs.triggers = &*probe.value;
    auto generated = GenerateGuiTriggerSource(probeInputs, record.id);
    if (!generated.Succeeded() || !generated.value) return {std::move(generated.diagnostics)};
    for (const auto& diagnostic : generated.diagnostics)
        if (diagnostic.severity == DiagnosticSeverity::Warning)
            return Status::Failure("Script.Trigger", "CODE_RECOVERY_WARNING", "转回后生成有警告：" + diagnostic.message);
    if (generated.value->effectiveRunOnInit != (record.runOnInit != 0))
        return Status::Failure("Script.Trigger", "CODE_RECOVERY_INIT", "“地图初始化时运行”与转回后的事件不一致。");
    std::string expected;
    if (const auto line = FirstMismatch(text, generated.value->text, expected); line != 0)
        return Mismatch(line, "与按 T 重新生成的代码不一致（此处应为“" + expected + "”）。");
    return Status::Ok();
}
} // namespace

Result<GuiTriggerRecovery> RecoverGuiTrigger(const MapScriptInputs& inputs, StableId triggerId,
    const Trigger::GuiTriggerFragment* snapshot) {
    try {
        if (!inputs.triggers || !inputs.catalog)
            return Result<GuiTriggerRecovery>::Failure("Script.Trigger", "CODE_RECOVERY_CONTEXT", "转回缺少当前触发器文档或函数库。");
        const Trigger::TriggerRecord* record = nullptr;
        for (const auto& trigger : inputs.triggers->triggers) {
            if (trigger.id != triggerId) continue;
            if (record) return Result<GuiTriggerRecovery>::Failure("Script.Trigger", "CODE_RECOVERY_AMBIGUOUS", "转回目标身份重复。");
            record = &trigger;
        }
        if (!record || record->type != 0 || record->customTextFlag != 1 || !record->ecas.empty())
            return Result<GuiTriggerRecovery>::Failure("Script.Trigger", "CODE_RECOVERY_CUSTOM_REQUIRED", "只能把自定义代码触发器转回 T。");
        const auto text = Trigger::TriggerCodeEditor::NormalizedText(record->customText);
        // 转换前快照：正文没改、引用的变量和函数库也没变时，回生成与正文一致，原样恢复（含禁用行与注释）。
        if (snapshot) {
            auto exact = *snapshot; exact.runOnInit = snapshot->runOnInit;
            if (Verify(inputs, *record, exact, text).Succeeded()) {
                GuiTriggerRecovery recovery; recovery.fragment = std::move(exact); recovery.fromSnapshot = true;
                return Result<GuiTriggerRecovery>::Success(std::move(recovery));
            }
        }
        Parser parser(*inputs.triggers, *inputs.catalog, *record);
        auto fragment = parser.Parse(text);
        if (!fragment) return {std::nullopt, Mismatch(parser.FailLine(), parser.FailReason()).diagnostics};
        if (auto verified = Verify(inputs, *record, *fragment, text); !verified.Succeeded()) return {std::nullopt, std::move(verified.diagnostics)};
        GuiTriggerRecovery recovery; recovery.fragment = std::move(*fragment);
        return Result<GuiTriggerRecovery>::Success(std::move(recovery));
    } catch (const std::exception& error) {
        return Result<GuiTriggerRecovery>::Failure("Script.Trigger", "CODE_RECOVERY_FAILED", error.what());
    }
}
} // namespace War3Map::Script
