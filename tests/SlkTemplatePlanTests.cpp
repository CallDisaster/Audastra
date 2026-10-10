#include "War3MapCore/Script/SlkTemplatePlan.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

// 原创闭集样例；不读取私有地图、YDWE、Lua 或游戏数据。
namespace {

using namespace War3Map::Script;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SlkTemplatePlan Plan(std::string_view code) {
    auto result = ParseSlkTemplatePlan(code);
    if (!result.Succeeded()) {
        throw std::runtime_error("计划意外失败：" + (result.diagnostics.empty() ? std::string("无诊断") : result.diagnostics.front().message));
    }
    return std::move(*result.value);
}

void Rejected(std::string_view code, std::string_view diagnostic, const SlkTemplatePlanLimits& limits = {}) {
    const auto result = ParseSlkTemplatePlan(code, limits);
    Require(!result.Succeeded() && !result.value, "失败不能返回部分能力计划");
    Require(result.diagnostics.size() == 1 && result.diagnostics.front().code == diagnostic,
        "错误分类应为 " + std::string(diagnostic) + (result.diagnostics.empty() ? "" : "，实际为 " + result.diagnostics.front().code));
    const auto& error = result.diagnostics.front();
    Require(error.origin.has_value() && error.origin->offset <= code.size(), "错误必须定位到输入字节范围");
    Require(error.message.find("行") != std::string::npos && error.message.find("列") != std::string::npos, "错误必须报告行列");
}

void TestOriginalObjects() {
    const auto plan = Plan(R"(<?
local suffixes = {'a', 'b', 'c',}
local records = require 'slk'
for i = 1, 3 do
    local object = records.ability.Ab01:new('Q0' .. suffixes[i] .. i)
    local amount = 2^(i-1)
    if i == 3 then
        amount = -amount
    end
    object.DataA1 = amount
    object.Name = '原创样例-' .. i
    object.Cost = 0.5
    object.Enabled = i ~= 2
end
?>)");
    Require(plan.abilities.size() == 3 && plan.loopIterations == 3, "循环应生成三个不同能力");
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& ability = plan.abilities[i];
        Require(ability.parentId == "Ab01" && ability.id == std::string("Q0") + static_cast<char>('a' + i) + std::to_string(i + 1), "ID 连接或父 ID 错误");
        Require(ability.writes.size() == 4, "字段顺序或数量错误");
        Require(ability.writes[0].field == "DataA1" && std::get<double>(ability.writes[0].value) == (i == 0 ? 1.0 : i == 1 ? 2.0 : -4.0), "幂的字段值与浮点类型错误");
        Require(std::get<std::string>(ability.writes[1].value) == "原创样例-" + std::to_string(i + 1), "字符串字段错误");
        Require(std::get<double>(ability.writes[2].value) == 0.5 && std::get<bool>(ability.writes[3].value) == (i != 1), "实数/boolean 类型未保留");
        Require(ability.source.line == 5 && ability.writes[0].source.line == 10, "创建与写入的源码行错误");
    }
}

void TestPrecedenceAndTypes() {
    const auto plan = Plan(R"(
local s = require('slk')
local o = s.ability.Ab01:new('Q101')
o.NegativePower = -2^2
o.RightPower = 2^3^2
o.FractionPower = 2^-2
o.Grouped = (-2)^2
o.ModPositive = -5 % 3
o.ModNegative = 5 % -3
o.Division = 6 / 2
o.Real = 3.0
o.Exponent = 3e0
o.Join = 'n' .. 1 .. 2
o.Escape = 'a\n\t\'b'
o.LargePower = 2^53
o.MixedPower = 2^3 * 2
)");
    const auto& writes = plan.abilities.front().writes;
    Require(std::get<double>(writes[0].value) == -4.0, "幂优先于一元负号且保留浮点类型");
    Require(std::get<double>(writes[1].value) == 512.0, "幂须右结合且保留浮点类型");
    Require(std::get<double>(writes[2].value) == 0.25, "负指数须保留实数");
    Require(std::get<double>(writes[3].value) == 4.0, "括号幂须保留浮点类型");
    Require(std::get<std::int64_t>(writes[4].value) == 1 && std::get<std::int64_t>(writes[5].value) == -1, "取模须按 floor 除法符号");
    for (std::size_t i = 6; i < 9; ++i) Require(std::get<double>(writes[i].value) == 3.0, "除法/小数/指数不能改成整数类型");
    Require(std::get<std::string>(writes[9].value) == "n12", "整数连接或右结合错误");
    Require(std::get<std::string>(writes[10].value) == "a\n\t'b", "合法字符串转义错误");
    Require(std::get<double>(writes[11].value) == 9007199254740992.0, "有限浮点幂不得应用整数字面量上限");
    Require(std::get<double>(writes[12].value) == 16.0, "幂结果参与混合运算时不得变回整数");
}

void TestConservativeNumericConcat() {
    for (const auto code : {
        "local x = 'n' .. 1..2", "local x = 1...2", "local x = 1.5..2",
        "local x = 1e2..'x'", "local x = .5..'x'", "if false then local x = 1..2 end"}) {
        Rejected(code, "slk-plan.number");
    }
    for (const auto code : {
        "local x = 'n' .. 1.0", "local x = 1e0 .. 'n'", "local x = 'n' .. (2^0)",
        "local x = (6/2) .. 'n'", "local n = 2^3 local x = 'n' .. n",
        "local values = {2^3} local x = 'n' .. values[1]"}) {
        Rejected(code, "slk-plan.float-concat");
    }
    Rejected("local s=require 'slk' local o=s.ability.Ab01:new('Q102') o.Name='kept' o.Name='n' .. 1.0",
        "slk-plan.float-concat");
    const auto plan = Plan("local s=require 'slk' local o=s.ability.Ab01:new('Q102') o.Name='n' .. 0 .. (-2) .. 9007199254740991");
    Require(std::get<std::string>(plan.abilities.front().writes.front().value) == "n0-29007199254740991",
        "整数拼接必须保留零、负号与精确边界值");
    Rejected("for i=2^0,2 do local x='n' .. i end", "slk-plan.integer");
    Rejected("for i=1,2,1.0 do local x='n' .. i end", "slk-plan.integer");
    const auto integerLoop = Plan("local s=require 'slk' local o=s.ability.Ab01:new('Q103') for i=1,2.0 do o.Name='n' .. i end");
    Require(integerLoop.abilities.front().writes.size() == 2 &&
        std::get<std::string>(integerLoop.abilities.front().writes[1].value) == "n2",
        "整数初值/步长及整数值上界必须保留整数循环变量");
}

void TestScopesAndOrderedWrites() {
    const auto plan = Plan(R"(
local s = require 'slk'
local o = s.ability.Ab01:new('Q201')
local i = 7
local count = 3
local total = 0
for i = count, 1, -1 do
    count = 0
    if i > 1 then
        local i = 10
        total = total + i
    elseif i == 1 then
        total = total + i
    else
        total = -100
    end
end
o.Total = total
o.IndexAfter = i
o.Repeated = 1
o.Repeated = 2
local alias = o
alias.Name = 'alias'
local a = {4, 8}
local b = a
o.Array = b[2]
for outer = 1, 2 do
    for inner = 1, 2 do
        o.Nested = outer * 10 + inner
    end
end
)");
    const auto& writes = plan.abilities.front().writes;
    Require(std::get<std::int64_t>(writes[0].value) == 21, "循环边界只能在入口求值；if 局部影子不能改循环变量");
    Require(std::get<std::int64_t>(writes[1].value) == 7, "循环退出后必须恢复外层同名变量");
    Require(writes[2].field == writes[3].field && std::get<std::int64_t>(writes[2].value) == 1
        && std::get<std::int64_t>(writes[3].value) == 2, "重复字段必须保留有序写入");
    Require(std::get<std::string>(writes[4].value) == "alias" && std::get<std::int64_t>(writes[5].value) == 8, "局部能力/只读数组别名错误");
    Require(plan.loopIterations == 9 && std::get<std::int64_t>(writes.back().value) == 22, "嵌套循环计数错误");
    Rejected("local s=require 'slk' for i=1,1 do i=2 end", "slk-plan.loop-control");
    Rejected("if true then local hidden=1 end local n=hidden", "slk-plan.local");
    Rejected("local x={1} x[1]=2", "slk-plan.syntax");
    Rejected("local x={{1}}", "slk-plan.type");
}

void TestLuaGrammarAndEnvironment() {
    for (const auto code : {"local x=+1", "local x=1 + +2", "if false then local x=+1 end",
        "local x={7}[1]", "if false then local x={7}[1] end", "local x='text'[1]"}) {
        Rejected(code, "slk-plan.syntax");
    }
    for (const auto code : {"local _ENV={} local s=require 'slk'", "_ENV={}",
        "for _ENV=1,2 do end", "if false then local _ENV={} end", "local x=_ENV"}) {
        Rejected(code, "slk-plan.environment");
    }
    Rejected("local s=require 'slk' local o=s.ability.Ab01:new('Q501') o.Name='kept' o.X=+1", "slk-plan.syntax");
    const auto plan = Plan(R"(
local _env = {3, 8}
local s = require 'slk'
local o = s.ability.Ab01:new('Q501')
o.Parenthesized = ({7})[1]
o.Named = _env[2]
o.Negated = -_env[1]
o.Addition = 2 + 3
)");
    const auto& writes = plan.abilities.front().writes;
    Require(writes.size() == 4 && std::get<std::int64_t>(writes[0].value) == 7 &&
        std::get<std::int64_t>(writes[1].value) == 8 && std::get<std::int64_t>(writes[2].value) == -3 &&
        std::get<std::int64_t>(writes[3].value) == 5,
        "合法名称/括号索引、一元负号及二元加法必须保留；_env 不是 _ENV");
}

void TestClosedGrammarAndFailure() {
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('Q301') local b=s.ability.Ab01:new('Q301')", "slk-plan.duplicate-id");
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('ABC')", "slk-plan.rawcode");
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('A BC')", "slk-plan.rawcode");
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('Q_01')", "slk-plan.rawcode");
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('Q-01')", "slk-plan.rawcode");
    Rejected("local s=require 'slk' local a=s.ability.Ab01:new('Ab01')", "slk-plan.rawcode");
    Rejected("local s=1 local a=s.ability.Ab01:new('Q302')", "slk-plan.capability");
    Rejected("local s=require 'io'", "slk-plan.capability");
    Rejected("global=1", "slk-plan.local");
    Rejected("if false then os.execute('blocked') end", "slk-plan.syntax");
    Rejected("for i=1,0 do unknown() end", "slk-plan.syntax");
    Rejected("if false then local x=io.open('blocked') end", "slk-plan.syntax");
    Rejected("<? local x=1 ?> trailing", "slk-plan.syntax");
    Rejected("<? local x=1", "slk-plan.syntax");
    Rejected("<? local x=1 ?> <? local y=2 ?>", "slk-plan.syntax");
    Rejected("local x='unterminated", "slk-plan.string");
    Rejected("--[[ unterminated", "slk-plan.syntax");
    Rejected("local x = 1 / 0", "slk-plan.number");
    Rejected("local x = 0 % 0", "slk-plan.number");
    Rejected("local x = (-1)^.5", "slk-plan.nonfinite");
    Rejected("local x = 1e308 * 1e308", "slk-plan.nonfinite");
    Rejected("if false then local x=1e999 end", "slk-plan.nonfinite");
    Rejected("local x = 9007199254740992", "slk-plan.integer");
    Rejected("local x = 9007199254740991 + 1", "slk-plan.integer");
    Rejected("local x = {'a'} local y=x[0]", "slk-plan.index");
    Rejected("local x = {'a'} local y=x[1.5]", "slk-plan.integer");
    Rejected("if 1 then local x=2 end", "slk-plan.type");
    Rejected("for i=1,2,0 do end", "slk-plan.loop-control");
    Rejected("for i=1,2.5 do end", "slk-plan.integer");
    const auto located = ParseSlkTemplatePlan("<?\nlocal value = 1\nunknown = 2\n?>");
    Require(located.diagnostics.front().origin->offset == 19 && located.diagnostics.front().message.find("第 3 行，第 1 列") != std::string::npos, "定位不能使用去除模板前缀后的偏移");
    Require(Plan("-- line\n--[[ block ]]\nlocal n=1;").abilities.empty(), "合法纯局部块应返回空计划");
}

void TestBudgets() {
    const std::string object = "local s=require 'slk' local o=s.ability.Ab01:new('Q401') o.X=1 o.X=2";
    SlkTemplatePlanLimits limits;
    limits.sourceBytes = 4; Rejected("local x=1", "slk-plan.budget", limits);
    limits = {}; limits.tokens = 2; Rejected("local x=1", "slk-plan.budget", limits);
    limits = {}; limits.syntaxNodes = 1; Rejected("local x=1", "slk-plan.budget", limits);
    limits = {}; limits.depth = 3; Rejected("local x=(((1)))", "slk-plan.budget", limits);
    limits = {}; limits.executedSteps = 1; Rejected("local x=1", "slk-plan.budget", limits);
    limits = {}; limits.loopIterations = 2; Rejected("for i=1,3 do end", "slk-plan.budget", limits);
    Rejected("for i=1,10001 do end", "slk-plan.budget");
    limits = {}; limits.localBindings = 1; Rejected("local x=1 local y=2", "slk-plan.budget", limits);
    limits = {}; limits.arrayElements = 1; Rejected("local x={1,2}", "slk-plan.budget", limits);
    limits = {}; limits.stringBytes = 2; Rejected("if false then local x='abc' end", "slk-plan.budget", limits);
    limits = {}; limits.totalStringBytes = 16; Rejected("local x='abcdefgh' x=x..x", "slk-plan.budget", limits);
    Rejected("local s=require 'slk' local o=s.ability.Ab01:new('Q402') o." + std::string(257, 'x') + "=1", "slk-plan.budget");
    const std::string repeatedNames = "local s=require 'slk' local o=s.ability.Ab01:new('Q402') for i=1,3 do o.Repeated=1 end";
    limits = {}; limits.totalStringBytes = 24; Rejected(repeatedNames, "slk-plan.budget", limits);
    limits.totalStringBytes = 28;
    Require(ParseSlkTemplatePlan(repeatedNames, limits).Succeeded(), "累计字段名预算须允许精确边界内的计划");
    limits = {}; limits.abilities = 0; Rejected(object, "slk-plan.budget", limits);
    limits = {}; limits.fieldWrites = 1; Rejected(object, "slk-plan.budget", limits);
    limits = {}; ++limits.depth; Rejected("", "slk-plan.limits", limits);
    limits = {}; limits.loopIterations = 0;
    Require(ParseSlkTemplatePlan("for i=2,1 do end", limits).Succeeded(), "零迭代不得消耗迭代预算");
    Rejected("for i=2,1 do return 1 end", "slk-plan.syntax", limits);
}

} // namespace

int main() {
    try {
        TestOriginalObjects();
        TestPrecedenceAndTypes();
        TestConservativeNumericConcat();
        TestScopesAndOrderedWrites();
        TestLuaGrammarAndEnvironment();
        TestClosedGrammarAndFailure();
        TestBudgets();
        std::cout << "SlkTemplatePlan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
