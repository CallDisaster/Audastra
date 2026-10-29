#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/IdentifierRewrite.h"
#include "War3MapCore/Hash.h"
#include "../src/Jass/JassLexer.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// 自研 JASS 检查器回归：规则与 pjass 的可观察行为对照（原创小样例；验收图的 pjass 差分测试在云端交接文档记录）。
namespace {

using namespace War3Map;
using namespace War3Map::Jass;

constexpr std::string_view kCommon = R"(type agent extends handle
type widget extends agent
type unit extends widget
type player extends agent
type boolexpr extends agent
type conditionfunc extends boolexpr
type group extends agent
native DoNothing takes nothing returns nothing
constant native Player takes integer number returns player
native KillUnit takes unit whichUnit returns nothing
native Condition takes code func returns conditionfunc
native CreateGroup takes nothing returns group
native GroupEnumUnitsInRange takes group whichGroup, real x, real y, real radius, boolexpr filter returns nothing
constant native GetOwningPlayer takes unit whichUnit returns player
)";

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

JassCheckResult Check(std::string_view script, const JassCheckOptions& options = {}) {
    const std::vector<JassSource> sources = {{"common.j", kCommon}, {"war3map.j", script}};
    return CheckJass(sources, options);
}

std::string Codes(const JassCheckResult& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.message + " [" + diagnostic.code + "]\n";
    return text;
}

// 期望恰好出错（第一个错误在 line 行、代码为 code），或 line 为 0 时期望通过。
void Expect(std::string_view script, std::uint32_t line, std::string_view code, const std::string& label, const JassCheckOptions& options = {}) {
    const auto result = Check(script, options);
    if (line == 0) {
        Require(result.Succeeded(), label + "：应当通过\n" + Codes(result));
        return;
    }
    Require(!result.Succeeded(), label + "：应当报错");
    const auto& first = result.diagnostics.front();
    const std::string prefix = "war3map.j:" + std::to_string(line) + ":";
    Require(first.message.starts_with(prefix) && first.code == code, label + "：首个错误不符\n" + Codes(result));
}

void TestDeclarations() {
    const std::string source = "function A takes nothing returns nothing\r\n    call B()\rendfunction\nfunction B takes nothing returns nothing\nendfunction\n";
    Expect(source,
        2, "JASS_DECLARATION_ORDER", "调用之后才定义的函数");
    const auto result = Check(source);
    const auto& diagnostic = result.diagnostics.front();
    Require(diagnostic.origin && diagnostic.origin->source == "war3map.j" && diagnostic.origin->offset == source.find("B()")
        && diagnostic.origin->length == 3 && result.diagnosticOriginExact.size() == result.diagnostics.size()
        && result.diagnosticOriginExact.front() && diagnostic.message.ends_with("原文：    call B()"),
        "检查器按真实混合换行保留AST调用跨度、精确身份与完整原行摘录");
    Expect("function A takes integer n returns integer\n    if n > 0 then\n        return A(n - 1)\n    endif\n    return 0\nendfunction\n",
        0, "", "递归调用自身");
    Expect("globals\n    integer a = b\n    integer b = 1\nendglobals\n", 2, "JASS_UNDECLARED", "全局初始值引用之后的全局");
    Expect("globals\n    integer array a = 1\nendglobals\n", 2, "JASS_SYNTAX", "数组不能初始化");
    Expect("globals\n    code array c\nendglobals\n", 2, "JASS_CODE", "code 数组");
    Expect("globals\n    integer X = 0\nendglobals\nfunction X takes nothing returns nothing\nendfunction\n", 4, "JASS_REDECLARED", "函数与全局同名");
    Expect("function A takes nothing returns nothing\nendfunction\nnative N takes nothing returns nothing\n", 3, "JASS_SYNTAX",
        "原生函数须在函数之前");
    Expect("function A takes nothing returns nothing\n    local integer DoNothing = 0\nendfunction\n", 2, "JASS_REDECLARED",
        "局部变量与函数同名");
    Expect("type event extends handle\nfunction A takes nothing returns nothing\n    local string event\nendfunction\n", 3,
        "JASS_REDECLARED", "局部变量不能占用原版 event 类型名");
    Expect("function A takes nothing returns nothing\n    local integer unit = 0\nendfunction\n", 2,
        "JASS_REDECLARED", "局部变量不能占用基础脚本已声明的类型名");
    Expect("type event extends handle\nfunction A takes nothing returns nothing\n    local string eventText = \"\"\nendfunction\n", 0,
        "", "事件载荷使用不冲突的局部名称");
    Expect("function A takes integer p returns nothing\n    local integer p = 2\nendfunction\n", 0, "", "局部变量可以遮蔽参数");
    Expect("function A takes nothing returns nothing\n    local integer i = 0\n    local integer i = 1\nendfunction\n", 3,
        "JASS_REDECLARED", "局部变量重复");
    Expect("function A takes nothing returns nothing\n    call DoNothing()\n    local integer i = 0\nendfunction\n", 3, "JASS_SYNTAX",
        "local 位于语句之后");
}

void TestTypes() {
    Expect("function A takes nothing returns nothing\n    local integer i = 1.5\nendfunction\n", 2, "JASS_TYPE", "real 不能转 integer");
    Expect("function A takes nothing returns nothing\n    local real r = 2\n    local string s = null\n    local unit u = null\n    local handle h = u\nendfunction\n",
        0, "", "合法的隐式转换");
    Expect("function A takes handle h returns nothing\n    local unit u = h\nendfunction\n", 2, "JASS_TYPE", "父类型不能转子类型");
    Expect("function A takes nothing returns nothing\n    local integer i = null\nendfunction\n", 2, "JASS_TYPE", "null 不能转 integer");
    Expect("function A takes unit u returns boolean\n    return u == Player(0) and u != null and 1 == 1.0\nendfunction\n", 0, "",
        "句柄之间、数字之间、与 null 比较");
    Expect("function A takes nothing returns boolean\n    return \"a\" == 1\nendfunction\n", 2, "JASS_TYPE", "字符串与整数比较");
    Expect("function A takes nothing returns boolean\n    return \"a\" < \"b\"\nendfunction\n", 2, "JASS_TYPE", "字符串大小比较");
    Expect("function A takes nothing returns string\n    return \"a\" + null\nendfunction\n", 2, "JASS_TYPE", "字符串加 null");
    Expect("function A takes nothing returns boolean\n    return not 1 == 1\nendfunction\n", 2, "JASS_TYPE", "not 比比较结合更紧");
    Expect("function A takes nothing returns nothing\n    if 1 then\n    endif\nendfunction\n", 2, "JASS_TYPE", "if 条件须为 boolean");
    Expect("function A takes nothing returns nothing\n    local integer i = 'abc'\nendfunction\n", 2, "JASS_RAWCODE", "rawcode 长度");
    Expect("function A takes nothing returns nothing\n    local string s = \"a\\qb\"\nendfunction\n", 2, "JASS_ESCAPE", "无效转义");
    Expect("function A takes nothing returns nothing\n    local integer x = 5 / 0\nendfunction\n", 0, "", "常量除以零不报错（与 pjass 一致）");
}

void TestCalls() {
    Expect("function P takes integer p returns nothing\nendfunction\nfunction A takes nothing returns nothing\n    call P()\nendfunction\n", 4,
        "JASS_ARGUMENTS", "参数太少");
    Expect("function P takes integer p returns nothing\nendfunction\nfunction A takes nothing returns nothing\n    call P(\"s\")\nendfunction\n", 4,
        "JASS_TYPE", "参数类型");
    Expect("function A takes nothing returns nothing\n    local integer x = DoNothing()\nendfunction\n", 2, "JASS_TYPE", "nothing 不能作为值");
    Expect("function W takes integer x returns boolean\n    return true\nendfunction\nfunction A takes nothing returns nothing\n    local code c = function W\nendfunction\n",
        5, "JASS_CODE", "带参数的函数不能作为 code");
    Expect("function A takes nothing returns nothing\n    local code c = function DoNothing\nendfunction\n", 0, "", "原生函数可作为 code");
    Expect("constant function A takes nothing returns nothing\n    call DoNothing()\nendfunction\n", 2, "JASS_CONSTANT", "常量函数调用非常量函数");
    Expect("globals\n    integer G = 0\nendglobals\nconstant function A takes nothing returns player\n    set G = 1\n    return Player(0)\nendfunction\n", 5,
        "JASS_CONSTANT", "常量函数修改全局");
    const std::string filter = "function F takes nothing returns nothing\nendfunction\nfunction A takes nothing returns nothing\n"
                               "    call GroupEnumUnitsInRange(CreateGroup(), 0, 0, 1, Condition(function F))\nendfunction\n";
    Expect(filter, 0, "", "默认不检查 Condition 的函数返回值");
    JassCheckOptions options;
    options.filter = true;
    Expect(filter, 4, "JASS_FILTER", "+filter 时检查 Condition 的函数返回 boolean", options);
}

void TestFlow() {
    Expect("function A takes boolean c returns integer\n    if c then\n        return 1\n    endif\nendfunction\n", 5, "JASS_MISSING_RETURN",
        "缺少 else 分支的返回");
    Expect("function A takes boolean c returns integer\n    if c then\n        return 1\n    elseif not c then\n        return 2\n    else\n        return 3\n    endif\nendfunction\n",
        0, "", "所有分支都返回");
    Expect("function A takes boolean c returns integer\n    loop\n        exitwhen c\n        return 1\n    endloop\nendfunction\n", 0, "",
        "循环体返回视为返回（与 pjass 一致）");
    Expect("function A takes boolean c returns integer\n    local integer x\n    if c then\n        set x = 1\n    endif\n    return x\nendfunction\n",
        0, "", "按文本顺序判断初始化");
    Expect("function A takes nothing returns integer\n    local integer x\n    return x\nendfunction\n", 3, "JASS_UNINITIALIZED", "读取未赋值的局部变量");
    Expect("function A takes nothing returns integer\n    local integer x = x + 1\n    return x\nendfunction\n", 2, "JASS_UNINITIALIZED",
        "初始值中使用自身");
    Expect("function A takes nothing returns nothing\n    local integer array a\n    set a[1] = a[0]\nendfunction\n", 0, "", "数组不检查初始化");
    Expect("function A takes nothing returns nothing\n    exitwhen true\nendfunction\n", 2, "JASS_EXITWHEN", "loop 外的 exitwhen");
    Expect("function A takes nothing returns nothing\n    return 1\nendfunction\n", 2, "JASS_RETURN", "无返回值的函数返回值");
    Expect("function A takes nothing returns integer\n    return\nendfunction\n", 2, "JASS_RETURN", "有返回值的函数空 return");
    Expect("function A takes nothing returns nothing\n    if true then\n        call DoNothing()\nendfunction\n", 4, "JASS_SYNTAX", "缺少 endif");
}

void TestFlags() {
    const std::string returnBug = "function A takes unit u returns integer\n    return u\n    return 0\nendfunction\n";
    Expect(returnBug, 2, "JASS_TYPE", "默认检查每条 return");
    JassCheckOptions options;
    options.returnBug = true;
    Expect(returnBug, 0, "", "+rb 只检查最后一条 return", options);
    Expect("//# +rb\nfunction A takes unit u returns integer\n    return u\n    return 0\nendfunction\n", 0, "", "函数前的 //# +rb");
    const auto ignored = Check("//# +nosemanticerror\nfunction A takes nothing returns nothing\n    local integer x = 1.5\nendfunction\n"
                               "function B takes nothing returns nothing\n    local integer y = 1.5\nendfunction\n");
    Require(!ignored.Succeeded() && ignored.errorsIgnored == 1 && ignored.diagnostics.size() == 1 &&
            ignored.diagnostics.front().message.starts_with("war3map.j:6:"),
        "//# 开关只作用于紧随的函数\n" + Codes(ignored));
    Expect("globals\n    integer g = 0\nendglobals\nfunction A takes nothing returns nothing\n    local integer g = 1\nendfunction\n", 0, "",
        "默认允许局部变量与全局同名");
    options = {};
    options.shadow = true;
    Expect("globals\n    integer g = 0\nendglobals\nfunction A takes nothing returns nothing\n    local integer g = 1\nendfunction\n", 5,
        "JASS_SHADOW", "+shadow", options);
}

void TestParallel() {
    // 多个函数分多线程检查，诊断按行号排序且与单线程一致。
    std::string script;
    for (int i = 0; i < 400; ++i) {
        script += "function F" + std::to_string(i) + " takes nothing returns integer\n";
        script += i % 50 == 7 ? "    return 1.5\n" : "    return " + std::to_string(i) + "\n";
        script += "endfunction\n";
    }
    JassCheckOptions single;
    single.threads = 1;
    JassCheckOptions many;
    many.threads = 8;
    const auto a = Check(script, single);
    const auto b = Check(script, many);
    Require(a.functions == 400 && b.threadsUsed > 1, "多线程检查");
    Require(Codes(a) == Codes(b) && a.diagnostics.size() == 8, "多线程结果与单线程一致\n" + Codes(b));
    Require(a.diagnostics.front().message.starts_with("war3map.j:23:"), "诊断按行号排序");
}

void TestEncodedLexer() {
    using namespace War3Map::Jass::detail;
    using Text::SourceEncoding;
    for (const auto& [encoding, name] : std::vector<std::pair<SourceEncoding, std::string>>{
        {SourceEncoding::Utf8, "udg_变量"}, {SourceEncoding::Gbk, std::string("udg_\xB1\xE4\xC1\xBF")}}) {
        const auto source = "integer " + name + " = 0\r\n";
        const auto result = LexJass(source, encoding);
        Require(result.Succeeded() && result.value->at(1).kind == TokenKind::Identifier
            && result.value->at(1).text == name && result.value->at(1).offset == 8
            && result.value->back().offset == source.size(), "编码标识符保留原字节/offset");
        Require(LexJass(source).at(1).text == "udg_", "默认词法仍只识别ASCII名称");
    }
    const std::string gbk = std::string("\"\x9C\x5C\x5C\" '\x8A\x5C\x5C' // \x81\x5C\rset x=1");
    const auto quoted = LexJass(gbk, SourceEncoding::Gbk);
    Require(quoted.Succeeded() && quoted.value->at(0).text == gbk.substr(0, 5)
        && quoted.value->at(1).text == gbk.substr(6, 5) && quoted.value->at(3).line == 2
        && ValidStringEscapes(quoted.value->at(0).text, SourceEncoding::Gbk), "字节词法中尾5C仍是转义，补第二个5C才闭合");
    Require(!LexJass(std::string("\"\x9C\x5C\""), SourceEncoding::Gbk).Succeeded(), "GBK尾5C按字节吞掉结束引号，不因编码声明绕过");
    const auto broken = LexJass(std::string("// \xC0\xAF\nset x=1"), SourceEncoding::Utf8);
    Require(broken.Succeeded(), "注释内原始字节不按来源编码拒绝");
    Require(!LexJass("// \x1A\nset x=1", SourceEncoding::Utf8).Succeeded(), "显式ASCII控制字节门禁不因不透明字节策略放宽");
    Expect("function F takes nothing returns string\nreturn \"\x9C\x5C\x5C\"\nendfunction\n", 0, "", "未知编码字符串按字节通过JASS检查");
    for (const auto text : {"udg_。", "udg_😀", "/* unfinished", "\"unfinished", "x\x1A y"}) {
        Require(!LexJass(text, SourceEncoding::Utf8, {.blockComments = true}).Succeeded(), "不支持标量/坏边界拒绝");
    }
    Require(!LexJass("", static_cast<SourceEncoding>(99)).Succeeded(), "空文也不能带未知编码");
}

void TestBoundedByteLexer() {
    using namespace War3Map::Jass::detail;
    const auto expectBudget = [](std::string_view source, const LexOptions& options, std::size_t maximum,
        std::size_t offset, const std::string& label) {
        const auto result = LexJassBounded(source, options, maximum);
        Require(!result.Succeeded() && !result.value && result.diagnostics.size() == 1
            && result.diagnostics.front().code == "SOURCE_LEX_TOKEN_BUDGET"
            && result.diagnostics.front().origin && result.diagnostics.front().origin->offset == offset,
            label + "：明确预算诊断且不返回部分记号");
    };
    const auto equivalent = [](std::string_view source, const LexOptions& options, const std::string& label) {
        const auto original = LexJass(source, options);
        const auto bounded = LexJassBounded(source, options, original.size());
        Require(bounded.Succeeded() && bounded.value->size() == original.size()
            && bounded.value->capacity() <= original.size(), label + "：预算包含End且扩容不越界");
        for (std::size_t i = 0; i < original.size(); ++i) {
            const auto& a = original[i];
            const auto& b = bounded.value->at(i);
            Require(a.kind == b.kind && a.keyword == b.keyword && a.line == b.line && a.offset == b.offset
                && a.text == b.text && a.text.data() == b.text.data(), label + "：词法及原字节视图与旧入口一致");
        }
    };
    expectBudget("", {}, 0, 0, "空脚本零预算也不能存End");
    expectBudget("x", {}, 0, 0, "非空脚本零预算");
    equivalent("", {}, "空脚本恰好一个End");
    equivalent("a+b", {}, "普通记号恰好预算");
    expectBudget("a+b", {}, 3, 3, "End也受预算约束");
    expectBudget("a+b", {}, 2, 2, "普通记号写入前拒绝");
    expectBudget("x\r\n", {}, 1, 1, "CRLF写入前拒绝");
    expectBudget("x\n//# +rb", {}, 2, 2, "Pragma写入前拒绝");
    equivalent("//# +rb\r\n//! textmacro X\nset x=$FF+0xAA+1.25\r", {false, true}, "指令与混合换行");
    equivalent("/* 中文\r\n注释 */ set x='ABCD' // ordinary\n", {true, true}, "可选块注释");
    const std::string mixed = "// UTF-8 中文\r// GBK " + std::string("\xB1\xE4\xC1\xBF")
        + "\ncall F(\"中文\", 'A', '\x81\x42\x43\x44')\n";
    equivalent(mixed, {}, "混合注释及原字节字面量");
    equivalent(std::string("\xC0\xAF") + " @ \"unfinished", {}, "非法记号仍由旧字节词法识别");

    // 约160KiB记号即可覆盖从小初始容量增长至非二次幂上限，无需巨量分配。
    const std::string dense(4999, '+');
    const auto exact = LexJassBounded(dense, {}, 5000);
    Require(exact.Succeeded() && exact.value->size() == 5000 && exact.value->capacity() <= 5000
        && exact.value->back().kind == TokenKind::End, "后续扩容钳制到实际5000预算");
    expectBudget(dense, {}, 7, 7, "密集记号的小预算提前停止");

    // 16MiB注释只生成End；不能按源字节预留约128MiB记号空间。
    std::string comment(16u * 1024u * 1024u, 'x');
    comment[0] = comment[1] = '/';
    const auto sparse = LexJassBounded(comment, {}, 2u * 1024u * 1024u);
    Require(sparse.Succeeded() && sparse.value->size() == 1 && sparse.value->capacity() <= 4096
        && sparse.value->front().kind == TokenKind::End && sparse.value->front().offset == comment.size(),
        "长注释初始容量很小且保留真实End位置");
}

void TestIdentifierRewrite() {
    using Text::SourceEncoding;
    const auto requestFor = [](std::string_view text, SourceEncoding encoding = SourceEncoding::Utf8) {
        IdentifierRewriteRequest request;
        request.text = text;
        request.encoding = encoding;
        request.sourceName = "合并阶段.j";
        request.expectedInputSha256 = *Sha256Text(text).value;
        request.globalScopeIdentity = "map-script/root";
        return request;
    };
    const std::string source = "\xEF\xBB\xBF" "globals\r\n  integer udg_变量=1 // udg_变量\r"
        "integer array udg_数组\nstring keep=\"udg_变量\"\ninteger raw='中A'\nendglobals\n"
        "/* udg_变量 */\nfunction Read takes nothing returns integer\nlocal integer xgdg_A=0\n"
        "set udg_数组[udg_变量]=udg_变量+xgdg_A\nreturn udg_数组[udg_变量]\nendfunction\n";
    const auto rewritten = RewriteUdgIdentifiers(requestFor(source));
    Require(rewritten.Succeeded() && rewritten.value->coverage == IdentifierRewriteCoverage::RootGlobalJass
        && rewritten.value->mappings.size() == 2, "根中文标量/数组绑定成功");
    const auto& renamed = *rewritten.value;
    const auto& scalar = renamed.mappings[0];
    Require(scalar.references.size() == 3 && renamed.mappings[1].references.size() == 2
        && scalar.declaration.origin.offset == source.find("udg_变量") && scalar.declaration.line == 2
        && scalar.codepoints == U"udg_变量" && scalar.generatedName.size() == 26
        && scalar.declaration.origin.fingerprint == renamed.inputSha256, "声明、真实引用计数和字节来源绑定");
    std::string expected = source;
    std::vector<std::pair<std::size_t, std::size_t>> replacements;
    for (std::size_t i = 0; i < renamed.mappings.size(); ++i) {
        const auto& mapping = renamed.mappings[i];
        replacements.emplace_back(static_cast<std::size_t>(mapping.declaration.origin.offset), i);
        for (const auto& reference : mapping.references) replacements.emplace_back(static_cast<std::size_t>(reference.origin.offset), i);
    }
    std::sort(replacements.rbegin(), replacements.rend());
    for (const auto& [offset, i] : replacements) expected.replace(offset, renamed.mappings[i].originalBytes.size(), renamed.mappings[i].generatedName);
    Require(renamed.text == expected && renamed.outputSha256 == *Sha256Text(expected).value, "除报告标识符跨度外逐字节相同");
    // 严格JASS checker不解释BOM/块注释；移去已保留的两段再证明生成名是合法JASS变量。
    auto checkable = renamed.text.substr(3);
    const auto comment = checkable.find("/* udg_变量 */");
    checkable.erase(comment, std::string("/* udg_变量 */").size());
    Require(Check(checkable).Succeeded(), "真实声明/下标/右值的生成名通过既有严格类型检查");
    for (const auto& [encoding, name] : std::vector<std::pair<SourceEncoding, std::string>>{
        {SourceEncoding::Utf8, "udg_变量"}, {SourceEncoding::Gbk, "udg_\xB1\xE4\xC1\xBF"}}) {
        const auto text = "globals\ninteger unrelated\ninteger " + name + "\nendglobals\n";
        auto request = requestFor(text, encoding);
        request.sourceName = "elsewhere.j";
        const auto result = RewriteUdgIdentifiers(request);
        Require(result.Succeeded() && result.value->mappings[0].generatedName == scalar.generatedName,
            "编码、来源文件、声明位置及无关变量不参与稳定名");
        request.globalScopeIdentity = "other-script/root";
        Require(RewriteUdgIdentifiers(request).value->mappings[0].generatedName != scalar.generatedName, "语义域参与稳定名");
    }
    const std::string gbk = "globals\ninteger udg_\xB1\xE4\xC1\xBF\nstring s=\"\x9C\x5C\x5C\"\nendglobals\n";
    const auto gbkResult = RewriteUdgIdentifiers(requestFor(gbk, SourceEncoding::Gbk));
    Require(gbkResult.Succeeded() && gbkResult.value->text.find("\"\x9C\x5C\x5C\"") != std::string::npos, "按字节闭合的GBK字面量未被改名或转码");
    const std::string base = "globals\ninteger udg_变量\nendglobals\n";
    const auto reject = [&](std::string_view text, std::string_view code) {
        const auto result = RewriteUdgIdentifiers(requestFor(text));
        Require(!result.value && !result.diagnostics.empty() && result.diagnostics.front().code == code, "拒绝且无部分结果：" + std::string(code));
    };
    reject(base + "function F takes integer udg_变量 returns nothing\nendfunction\n", "IDENTIFIER_REWRITE_SHADOW_UNSUPPORTED");
    reject(base + "function F takes nothing returns nothing\nlocal integer udg_变量\nendfunction\n", "IDENTIFIER_REWRITE_SHADOW_UNSUPPORTED");
    reject(base + "function F takes nothing returns nothing\ncall udg_变量()\nendfunction\n", "IDENTIFIER_REWRITE_UNBOUND");
    reject(base + "function F takes nothing returns integer\nreturn xgdg_A\nendfunction\n", "IDENTIFIER_REWRITE_LEGACY_ALIAS_UNPROVEN");
    reject("library L\n" + base + "endlibrary\n", "IDENTIFIER_REWRITE_SCOPE_UNSUPPORTED");
    reject("//! textmacro X\n" + base, "IDENTIFIER_REWRITE_UNEXPANDED");
    reject(base + "globals\ninteger " + scalar.generatedName + "\nendglobals\n", "IDENTIFIER_REWRITE_NAME_CONFLICT");
    reject(base + base, "IDENTIFIER_REWRITE_DECLARATION_CONFLICT");
    auto limited = requestFor(source);
    limited.limits.maxReferences = 1;
    Require(!RewriteUdgIdentifiers(limited).value, "引用总预算不返回部分脚本");
    limited = requestFor(base);
    limited.limits.maxOutputBytes = base.size();
    Require(!RewriteUdgIdentifiers(limited).value, "展开输出预算");
    limited = requestFor(base);
    limited.expectedInputSha256[0] = 'z';
    Require(!RewriteUdgIdentifiers(limited).value, "冻结输入变化拒绝");
    limited.encoding.reset();
    Require(RewriteUdgIdentifiers(limited).diagnostics.front().code == "IDENTIFIER_REWRITE_ENCODING", "缺编码拒绝");
    const std::string noCandidate = "function 中文 takes nothing returns integer\nreturn xgdg_Custom\nendfunction\n";
    const auto skipped = RewriteUdgIdentifiers(requestFor(noCandidate));
    Require(skipped.Succeeded() && skipped.value->text == noCandidate && skipped.value->mappings.empty()
        && skipped.value->coverage == IdentifierRewriteCoverage::NoCandidate, "无候选不擅自判定或改动无关中文/旧别名语义");
}

} // namespace

int main() {
    try {
        TestDeclarations();
        TestTypes();
        TestCalls();
        TestFlow();
        TestFlags();
        TestParallel();
        TestEncodedLexer();
        TestBoundedByteLexer();
        TestIdentifierRewrite();
    } catch (const std::exception& error) {
        std::cerr << "JassChecker: " << error.what() << '\n';
        return 1;
    }
    std::cout << "JassChecker: 9 scenario groups passed\n";
    return 0;
}
