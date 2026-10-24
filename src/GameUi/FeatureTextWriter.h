#pragma once

#include "War3MapCore/Types.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 生成自然 T 的共用写法：紧凑 T 语句（Trigger/TriggerTextNotation.h）与功能写法（Trigger/TriggerFeatureText.h）。
// 界面触发器与读条系统等生成器共用；只拼文本，是否合法由写入触发器编辑器时的目录检查决定。
namespace War3Map::GameUi::detail {

using Lines = std::vector<std::string>;

[[nodiscard]] inline Diagnostic Note(DiagnosticSeverity severity, const char* code, std::string message) {
    return Diagnostic{severity, "GameUi", code, std::move(message), std::nullopt};
}

inline void Append(Lines& out, const Lines& in, int depth = 0) {
    const std::string pad(static_cast<std::size_t>(depth) * 4, ' ');
    for (const auto& line : in) out.push_back(pad + line);
}

// 紧凑 T 写法的文本字面量：\" \\ \n \r \t 转义。
[[nodiscard]] inline std::string Quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c);
        }
    }
    return out + "\"";
}

// JASS 实数字面量：定点、最多 6 位小数、去掉末尾的 0，不用指数写法。
[[nodiscard]] inline std::string Real(double value) {
    if (!std::isfinite(value)) value = 0.0;
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.6f", value);
    std::string text(buffer.data(), written > 0 ? static_cast<std::size_t>(written) : 0);
    while (text.ends_with('0')) text.pop_back();
    if (text.ends_with('.')) text.pop_back();
    if (text.empty() || text == "-0") text = "0";
    return text;
}

[[nodiscard]] inline std::string Call(std::string_view name, std::initializer_list<std::string> args = {}) {
    std::string out(name);
    out += '(';
    bool first = true;
    for (const auto& arg : args) {
        if (!first) out += ", ";
        out += arg;
        first = false;
    }
    return out + ')';
}

// 常用运算、比较与赋值（TriggerData 的函数名）。
[[nodiscard]] inline std::string Int(std::string_view a, std::string_view op, std::string_view b) {
    return Call("OperatorInt", {std::string(a), std::string(op), std::string(b)});
}
[[nodiscard]] inline std::string Arith(std::string_view a, std::string_view op, std::string_view b) {
    return Call("OperatorReal", {std::string(a), std::string(op), std::string(b)});
}
[[nodiscard]] inline std::string CompareInt(std::string_view a, std::string_view op, std::string_view b) {
    return Call("OperatorCompareInteger", {std::string(a), std::string(op), std::string(b)});
}
[[nodiscard]] inline std::string CompareReal(std::string_view a, std::string_view op, std::string_view b) {
    return Call("OperatorCompareReal", {std::string(a), std::string(op), std::string(b)});
}
[[nodiscard]] inline std::string IsTrue(std::string_view value) {
    return Call("OperatorCompareBoolean", {std::string(value), "OperatorEqualENE", "true"});
}
[[nodiscard]] inline std::string Set(std::string_view target, std::string_view value) {
    return Call("SetVariable", {std::string(target), std::string(value)});
}

[[nodiscard]] inline Lines If(const Lines& conditions, const Lines& then, const Lines& otherwise = {}) {
    Lines out{"IfThenElseMultiple() {", "    if:"};
    Append(out, conditions, 2);
    out.push_back("    then:");
    Append(out, then, 2);
    if (!otherwise.empty()) {
        out.push_back("    else:");
        Append(out, otherwise, 2);
    }
    out.push_back("}");
    return out;
}

// 带一个子块的多项动作（循环的 actions、运行触发器的 params 等）。
[[nodiscard]] inline Lines Block(const std::string& head, std::string_view branch, const Lines& body) {
    Lines out{head + " {", "    " + std::string(branch) + ":"};
    Append(out, body, 2);
    out.push_back("}");
    return out;
}

// 用自己的整数变量循环（不用 bj_forLoopAIndex），被运行的触发器里的“循环 A”不会打乱它。结束值每轮重新求。
[[nodiscard]] inline Lines ForLoopVar(const std::string& variable, std::string_view from, std::string_view to, const Lines& body) {
    return Block(Call("ForLoopVarMultiple", {variable, std::string(from), std::string(to)}), "actions", body);
}

// 触发器引用（触发器名作参数）。
[[nodiscard]] inline std::string TriggerRef(const std::string& name) { return "`" + name + "`"; }

// 功能写法里的名字：含 : ( ) 或首尾空格时加双引号。
[[nodiscard]] inline std::string FeatureName(const std::string& name) {
    const bool quote = name.find_first_of(":()") != std::string::npos || name.front() == ' ' || name.back() == ' ';
    return quote ? "\"" + name + "\"" : name;
}

// 可直接放进触发器名与注释的显示名：非空、无双引号、无控制字符。
[[nodiscard]] inline bool UsableLabel(std::string_view label) {
    if (label.empty() || label.find('"') != std::string_view::npos) return false;
    for (const unsigned char c : label) {
        if (c < 0x20) return false;
    }
    return true;
}

// 拼一段功能写法：先全部变量，再按加入顺序的触发器。
class FeatureTextBuilder {
public:
    // array 为空表示单值；0 写作 名字: 类型[]，N 写作 名字: 类型[N]。initial 为空时不写初值。
    void Variable(const std::string& name, std::string_view type, std::optional<std::uint32_t> array = std::nullopt,
        const std::string& initial = {}) {
        std::string line = "    " + name + ": " + std::string(type);
        if (array) line += *array ? "[" + std::to_string(*array) + "]" : std::string("[]");
        if (!initial.empty()) line += " = " + initial;
        variableLines_.push_back(std::move(line));
        variables.push_back(name);
    }

    // options 为功能写法的触发器选项（init、off、disabled）。
    void Trigger(const std::string& name, const Lines& events, const Lines& actions, const Lines& conditions = {},
        std::string_view options = {}) {
        std::string text = "触发器 " + FeatureName(name);
        if (!options.empty()) text += " (" + std::string(options) + ")";
        text += ":\n";
        const auto section = [&](std::string_view title, const Lines& lines, bool always) {
            if (lines.empty() && !always) return;
            text += "    " + std::string(title) + ":\n";
            for (const auto& line : lines) text += "        " + line + "\n";
        };
        section("events", events, false);
        section("conditions", conditions, false);
        section("actions", actions, true);
        triggerText_ += text;
        triggers.push_back(name);
    }

    [[nodiscard]] std::string Text() const {
        std::string text;
        if (!variableLines_.empty()) {
            text = "变量:\n";
            for (const auto& line : variableLines_) text += line + "\n";
        }
        return text + triggerText_;
    }

    std::vector<std::string> variables;  // 不带 udg_，按加入顺序
    std::vector<std::string> triggers;   // 按加入顺序

private:
    std::vector<std::string> variableLines_;
    std::string triggerText_;
};

} // namespace War3Map::GameUi::detail
