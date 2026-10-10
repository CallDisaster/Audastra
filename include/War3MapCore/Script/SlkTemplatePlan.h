#pragma once

#include "War3MapCore/Types.h"

#include <string_view>
#include <variant>

namespace War3Map::Script {

struct SlkTemplateSource {
    std::size_t offset = 0; // 输入中的 UTF-8 字节位置，包括可选的 <? 前缀
    std::size_t length = 0;
    std::size_t line = 1;
    std::size_t column = 1; // 1 基字节列
};

// 整数字面量及精确整数运算保留 int64_t；带小数点/指数的字面量、除法和幂保留 double。
// 整数限定在 [-9007199254740991, 9007199254740991]，避免模拟 Lua number 时悄悄丢精度。
using SlkTemplateScalar = std::variant<std::int64_t, double, std::string, bool>;

struct SlkTemplateFieldWrite {
    std::string field;
    SlkTemplateScalar value;
    SlkTemplateSource source;
};

struct SlkTemplateAbilityPlan {
    std::string parentId;
    std::string id;
    SlkTemplateSource source;
    // 按执行顺序保留每次写入，包括重复字段；解析器不合并、不静默覆盖。
    std::vector<SlkTemplateFieldWrite> writes;
};

struct SlkTemplatePlan {
    std::vector<SlkTemplateAbilityPlan> abilities;
    std::size_t executedSteps = 0; // 语句与表达式求值次数
    std::size_t loopIterations = 0; // 所有循环的总迭代数
};

struct SlkTemplatePlanLimits {
    // 这些默认值同时是硬上限；调用方只能收紧。0 表示该项完全没有预算。
    std::size_t sourceBytes = 1024 * 1024;
    std::size_t tokens = 65536;
    std::size_t syntaxNodes = 65536;
    std::size_t depth = 64; // 语法嵌套、AST 高度及执行嵌套
    std::size_t executedSteps = 100000;
    std::size_t loopIterations = 10000;
    std::size_t localBindings = 20000; // 总创建数，包括循环中的短期局部变量
    std::size_t arrayElements = 16384; // 所有求值后数组的元素总数
    std::size_t stringBytes = 65536; // 单个字符串
    std::size_t totalStringBytes = 4 * 1024 * 1024; // 求值及计划复制的累计字符串字节，含每条输出字段名
    std::size_t abilities = 4096;
    std::size_t fieldWrites = 65536;
};

// 纯计划解析：接受单个 <? ... ?> 语句块或裸块，不接受混合 JASS / 多个模板。
// 完整解析全部分支后才求值；不执行 Lua、require、I/O，不读取或修改物编，也不接入保存。
// 闭集：local/已有局部赋值、只读密集一维数组和 1 基索引、有限数/字符串/boolean、
// + - * / % ^、一元 -、..、== ~= < <= > >=、固定整数数值 for（入口求值一次）、
// .. 仅接受字符串/整数；目标 Lua 浮点转字符串格式未验证，浮点拼接明确拒绝。
// 数字与紧随的 .. 须分隔；按 Lua 词法拒绝 1..2 等非法数字。
// for 初值与步长须为整数类型；拒绝把浮点循环隐式改成整数循环。
// if/elseif/else、local alias = require 'slk'（也可带括号）、
// local object = alias.ability.<四字符父ID>:new(<字符串ID>)、object.<字段> = <标量>。
// 新能力 ID 限 4 个 ASCII 字母/数字：slk:new 将含 %W（包括下划线）的值视为动态标签，
// 本计划不猜测动态分配。父 ID 限四字节 ASCII 标识符。同 ID 再创建、循环控制变量赋值、
// 全局写入、_ENV 读取/绑定/替换、任意其他函数/成员读取/表写入、非有限数和未知语法均拒绝。
// 后缀索引仅允许名称或括号表达式；输出字段名单项最多 256 字节且计入累计字符串预算。
// 失败无部分计划；Diagnostic.origin 给出字节范围，message 包含行列。
[[nodiscard]] Result<SlkTemplatePlan> ParseSlkTemplatePlan(
    std::string_view source, const SlkTemplatePlanLimits& limits = {});

} // namespace War3Map::Script
