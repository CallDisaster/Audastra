#include "LuaTriggerEmitter.h"
#include "ScriptNames.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace War3Map::Script::lua {

std::string Quote(std::string_view text) {
    std::string result = "\"";
    for (const unsigned char value : text) {
        if (value == '\\' || value == '"') { result += '\\'; result += static_cast<char>(value); }
        else if (value < 32 || value == 127) {
            result += '\\';
            result += static_cast<char>('0' + value / 100);
            result += static_cast<char>('0' + value / 10 % 10);
            result += static_cast<char>('0' + value % 10);
        } else result += static_cast<char>(value);
    }
    return result + '"';
}

bool Identifier(std::string_view text) {
    const auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (text.empty() || text.size() > 128 || !alpha(text.front())) return false;
    if (!std::all_of(text.begin(), text.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); })) return false;
    constexpr std::string_view keywords[] = {"and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return std::find(std::begin(keywords), std::end(keywords), text) == std::end(keywords);
}

[[noreturn]] void Emitter::Fail(std::string code, std::string message) const {
    throw LuaSourceDiagnostic{{DiagnosticSeverity::Error, "Script.Lua", std::move(code),
        std::move(message), currentOrigin_}, currentTrigger_, currentNode_};
}

Emitter::Visit::Visit(Emitter& emitter, bool argument, std::uint32_t index)
    : emitter_(emitter), key_((static_cast<std::uint64_t>(argument) << 32) | index),
      previousId_(emitter.currentNode_), previousOrigin_(emitter.currentOrigin_) {
    if (++emitter_.visited_ > 200000 || emitter_.visiting_.size() >= 96
        || !emitter_.visiting_.insert(key_).second) {
        emitter_.Fail("LUA_GRAPH_BUDGET_OR_CYCLE", "活动源节点成环或超过展开预算/96层深度。");
    }
    if (argument) {
        if (index >= emitter_.document_.argumentNodes.size()) emitter_.Fail("LUA_ARGUMENT_INDEX", "参数索引越界。");
        const auto& node = emitter_.document_.argumentNodes[index];
        emitter_.currentNode_ = node.id; emitter_.currentOrigin_ = node.origin;
    } else {
        if (index >= emitter_.document_.ecaNodes.size()) emitter_.Fail("LUA_ECA_INDEX", "ECA 索引越界。");
        const auto& node = emitter_.document_.ecaNodes[index];
        emitter_.currentNode_ = node.id; emitter_.currentOrigin_ = node.origin;
    }
}
Emitter::Visit::~Visit() {
    emitter_.visiting_.erase(key_);
    emitter_.currentNode_ = previousId_;
    emitter_.currentOrigin_ = std::move(previousOrigin_);
}

std::string Emitter::Field(const Trigger::UiDefinitionRevision& definition, std::string_view key) const {
    for (const auto& [name, value] : definition.fields) if (name == key) {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
        return value;
    }
    return {};
}
std::string Emitter::Define(std::string_view group, std::string_view name, std::size_t column) const {
    const auto* definition = catalog_.Find(Trigger::UiDefinitionKind::Define, name, group);
    if (!definition || definition->revisions.empty()) return {};
    const auto values = detail::SplitDefineValue(Field(definition->Active(), "value"));
    return column < values.size() ? values[column] : std::string{};
}
std::string Emitter::Base(std::string type) const {
    if (type.size() >= 2 && type.front() == '"' && type.back() == '"') type = type.substr(1, type.size() - 2);
    std::set<std::string> path;
    for (;;) {
        if (path.size() >= 32 || !path.insert(type).second) Fail("LUA_TYPE_ALIAS_CYCLE", "UI 类型别名成环或超过预算。");
        // 复用 UI 的类型目录；编译可赋值性仍以目标 API 为准，不能套用 UI 对未知类型的宽松策略。
        const auto* info = types_.Find(type);
        const auto base = info ? info->baseType : std::string{};
        if (base.empty() || base == type) return type;
        type = base;
    }
}
bool Emitter::KnownType(const std::string& type) const {
    return type == "integer" || type == "real" || type == "boolean" || type == "string"
        || type == "code" || type == "nothing" || type == "handle" || parents_.contains(type);
}
bool Emitter::Assignable(std::string actual, const std::string& expected) const {
    if (actual == expected || (actual == "integer" && expected == "real")) return true;
    if (actual == "null") return expected == "string" || expected == "code" || expected == "handle" || parents_.contains(expected);
    for (std::size_t depth = 0; depth < 32; ++depth) {
        const auto parent = parents_.find(actual);
        if (parent == parents_.end()) return false;
        actual = parent->second;
        if (actual == expected) return true;
    }
    return false;
}
Expression Emitter::Checked(Expression expression, const std::string& expected) const {
    if (!Assignable(expression.type, expected)) Fail("LUA_TYPE_MISMATCH", expression.type + " 不能用于 " + expected + " 槽。");
    if (expected == "integer") expression.code = "__forge_i32(" + expression.code + ")";
    else if (expected == "real") {
        expression.code = "__forge_real(" + expression.code + ")";
        // 在 real 运算的操作数边界提升；先前完成的 integer 子表达式（含除法）不改写。
        expression.type = "real";
    }
    return expression;
}
std::string Emitter::Default(const std::string& type) const {
    if (type == "integer" || type == "real") return "0";
    if (type == "boolean") return "false";
    // 经典 VM 的 string token 0 是 null，和已分配的空串保持区别。
    return "nil";
}
std::string Emitter::CheckExpressionSize(std::string text) const {
    if (text.size() > 256 * 1024) Fail("LUA_EXPRESSION_BUDGET", "单个表达式超过 256 KiB 预算。");
    return text;
}

Expression Emitter::Literal(std::string value, const std::string& expectedSlot, bool preset) {
    if (value.size() > 65536) Fail("LUA_LITERAL_BUDGET", "字面量超过 64 KiB 预算。");
    const auto expected = Base(expectedSlot);
    const auto role = types_.LiteralKind(expectedSlot);
    if (preset) {
        value = Define("TriggerParams", value, 2);
        if (value.empty()) Fail("LUA_PRESET_UNRESOLVED", "预设值不存在或没有可解析的 Lua 字面量。");
        if (auto global = BoundGlobal(value)) return Checked(std::move(*global), expected);
        if (auto constant = BoundConstant(value)) return Checked(std::move(*constant), expected);
        if (expected == "string") {
            if (value.size() < 2 || value.front() != '`' || value.back() != '`')
                Fail("LUA_PRESET_UNTRANSLATED", "字符串预设不是明确的反引号字面量。");
            value = value.substr(1, value.size() - 2);
            // 预设中 JASS 转义的语义另需解析；不能当普通字符串再次转义。
            if (value.find('\\') != std::string::npos) Fail("LUA_PRESET_UNTRANSLATED", "带转义的字符串预设尚未转换。");
        }
    }
    if (expected == "string") return {"string", Quote(value), value};
    if (auto global = BoundGlobal(value)) return Checked(std::move(*global), expected);
    if (auto constant = BoundConstant(value)) return Checked(std::move(*constant), expected);
    if (role == Trigger::TriggerTypeSystem::Literal::Other && !types_.ValidateLiteral(expectedSlot, value).Succeeded())
        Fail("LUA_LITERAL_UNTRANSLATED", "对象编号类型没有有效的整数父链：" + expectedSlot + "。");
    if (value == "null") return Checked({"null", "nil", {}}, expected);
    if (expected == "boolean" && (value == "true" || value == "false")) return {expected, value, {}};
    if (expected == "code" && callableSymbols_.contains(value)) return {expected, value, {}};
    if (role == Trigger::TriggerTypeSystem::Literal::ObjectCode) {
        // 目录预设是 JASS 表达式，固定值是裸四字节；不要把预设的十进制 1234 当成 '1234'。
        if (preset && value.size() == 6 && value.front() == '\'' && value.back() == '\'') {
            value = value.substr(1, 4);
        } else if (preset && Trigger::TriggerTypeSystem::IsIntegerLiteral(value)) {
            std::int32_t integer = 0;
            (void)std::from_chars(value.data(), value.data() + value.size(), integer);
            return {"integer", std::to_string(integer), {}};
        }
        if (!Trigger::TriggerTypeSystem::IsObjectCodeLiteral(value))
            Fail("LUA_LITERAL_UNTRANSLATED", "对象编号固定值必须是四个安全字节，不加引号或转义。");
        return Native("FourCC", {{"string", Quote(value), value}}, "integer");
    }
    if (expected == "integer") {
        std::int32_t integer = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) return {expected, std::to_string(integer), {}};
    }
    if (expected == "real") {
        double real = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), real);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(real)) {
            if (value.find_first_of(".eE") == std::string::npos) value += ".0";
            return {expected, value, {}};
        }
    }
    Fail("LUA_LITERAL_UNTRANSLATED", "不能把源字面量安全解释为 " + expected + "；不透传为代码。");
}

} // namespace War3Map::Script::lua
