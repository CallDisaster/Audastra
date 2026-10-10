#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/VJass.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// 自研 vJass 翻译回归：原创小样例，断言输出的关键结构（按记号比较，不计空白），并用自研检查器验证输出是合法 JASS。
// 与 jasshelper 输出的整图对照（验收图记号级一致）记录在云端交接文档。
namespace {

using namespace War3Map;
using namespace War3Map::Jass;

constexpr std::string_view kCommon = R"(type agent extends handle
type widget extends agent
type unit extends widget
type trigger extends agent
type triggercondition extends agent
type boolexpr extends agent
type conditionfunc extends boolexpr
type filterfunc extends boolexpr
type group extends agent
type timer extends agent
native CreateTrigger takes nothing returns trigger
native TriggerAddCondition takes trigger whichTrigger, boolexpr condition returns triggercondition
native TriggerEvaluate takes trigger whichTrigger returns boolean
native Condition takes code func returns conditionfunc
native Filter takes code func returns filterfunc
native ExecuteFunc takes string funcName returns nothing
native CreateTimer takes nothing returns timer
native TimerStart takes timer whichTimer, real timeout, boolean periodic, code handlerFunc returns nothing
native CreateGroup takes nothing returns group
native GroupEnumUnitsInRange takes group whichGroup, real x, real y, real radius, boolexpr filter returns nothing
native I2S takes integer i returns string
)";

constexpr std::string_view kBlizzard = R"(function BJDebugMsg takes string msg returns nothing
endfunction
function InitBlizzard takes nothing returns nothing
endfunction
)";

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

VJassOptions Options() {
    VJassOptions options;
    options.baseScripts = {{"common.j", kCommon}, {"blizzard.j", kBlizzard}};
    options.initSuffix = 1234567;
    return options;
}

std::string Messages(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) text += diagnostic.message + " [" + diagnostic.code + "]\n";
    return text;
}

// 记号化并以单个空格连接：比较时不计空白与换行。
std::string Normalize(std::string_view text) {
    std::string out;
    bool separate = false;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || u >= 0x80 || c == '"' || c == '\'') {
            if (separate && !out.empty()) out += ' ';
            separate = false;
            out += c;
        } else if (std::isspace(u)) {
            separate = true;
        } else {
            if (!out.empty()) out += ' ';
            out += c;
            separate = true;
        }
    }
    return out;
}

bool Has(const std::string& text, std::string_view snippet) {
    return Normalize(text).find(Normalize(snippet)) != std::string::npos;
}

std::size_t Position(const std::string& text, std::string_view snippet) {
    return Normalize(text).find(Normalize(snippet));
}

// 翻译成功，且输出通过自研检查器。
std::string Translate(std::string_view script, const std::string& label) {
    const auto result = TranslateVJass(script, "war3map.j", Options());
    Require(result.unsupported.empty(), label + "：不应回退：" + result.unsupported);
    Require(result.Succeeded(), label + "：翻译失败\n" + Messages(result.diagnostics));
    const std::vector<JassSource> sources = {{"common.j", kCommon}, {"blizzard.j", kBlizzard}, {"war3map.j", result.text}};
    const auto check = CheckJass(sources);
    Require(check.Succeeded(), label + "：输出未通过检查\n" + Messages(check.diagnostics) + "\n" + result.text);
    return result.text;
}

void ExpectHas(const std::string& text, std::string_view snippet, const std::string& label) {
    Require(Has(text, snippet), label + "：输出缺少 “" + std::string(snippet) + "”\n" + text);
}

void ExpectMissing(const std::string& text, std::string_view snippet, const std::string& label) {
    Require(!Has(text, snippet), label + "：输出不应含有 “" + std::string(snippet) + "”\n" + text);
}

void ExpectUnsupported(std::string_view script, std::string_view reason, const std::string& label) {
    const auto result = TranslateVJass(script, "war3map.j", Options());
    Require(!result.unsupported.empty() && result.unsupported.find(reason) != std::string::npos,
        label + "：应回退到 jasshelper（" + std::string(reason) + "），实际：" + result.unsupported + "\n" + Messages(result.diagnostics));
    Require(!result.Succeeded() && result.text.empty(), label + "：回退时不应输出");
}

void ExpectError(std::string_view script, std::string_view code, const std::string& label) {
    const auto result = TranslateVJass(script, "war3map.j", Options());
    bool found = false;
    for (const auto& diagnostic : result.diagnostics) found = found || diagnostic.code == code;
    Require(result.unsupported.empty() && !result.Succeeded() && found, label + "：应报 " + std::string(code) + "\n" + Messages(result.diagnostics));
}

constexpr std::string_view kMain = "function main takes nothing returns nothing\n    call InitBlizzard()\nendfunction\n";

void TestLibraries() {
    const std::string script = std::string(R"(globals
    integer Top = 0
endglobals
library Zed requires Alpha, optional Missing
    function ZedF takes nothing returns integer
        return Alpha_Pub()
    endfunction
endlibrary
library Alpha initializer Init
    globals
        private integer hidden = 1
        public integer shown = 2
    endglobals
    private function Init takes nothing returns nothing
        set hidden = shown
    endfunction
    public function Pub takes nothing returns integer
        return hidden
    endfunction
endlibrary
library beta
    function BetaF takes nothing returns nothing
    endfunction
endlibrary
library_once Once
    function OnceF takes nothing returns integer
        return 1
    endfunction
endlibrary
library_once Once
    function OnceF takes nothing returns integer
        return 2
    endfunction
endlibrary
)") + std::string(kMain);
    const auto out = Translate(script, "库");
    ExpectHas(out, "constant boolean LIBRARY_Alpha=true", "库常量");
    ExpectHas(out, "integer Alpha__hidden= 1", "private 全局改名");
    ExpectHas(out, "integer Alpha_shown= 2", "public 全局改名");
    ExpectHas(out, "function Alpha__Init takes nothing returns nothing\n        set Alpha__hidden = Alpha_shown", "库内使用短名称");
    ExpectHas(out, "return Alpha_Pub()", "库外使用 public 全名");
    // 同层按字节序（大写在前），依赖者在被依赖者之后
    const auto alpha = Position(out, "//library Alpha:");
    const auto once = Position(out, "//library Once:");
    const auto beta = Position(out, "//library beta:");
    const auto zed = Position(out, "//library Zed:");
    Require(alpha < once && once < beta && beta < zed, "库顺序：Alpha < Once < beta（同层）< Zed（依赖 Alpha）\n" + out);
    Require(Position(out, "LIBRARY_Once") < beta && out.find("return 2") == std::string::npos, "library_once 重复定义应忽略");
    ExpectHas(out, "call InitBlizzard()\n\ncall ExecuteFunc(\"Alpha__Init\")", "初始化函数插在 InitBlizzard 之后");
    ExpectHas(out, "trigger l__library_init", "有初始化函数时声明 l__library_init");
    Require(Position(out, "integer Top= 0") > Position(out, "//endglobals from Zed"), "脚本全局在各库之后");

    ExpectError(std::string("library A requires B\nendlibrary\n") + std::string(kMain), "VJASS_REQUIRES", "缺少依赖");
    ExpectError(std::string("library A requires B\nendlibrary\nlibrary B requires A\nendlibrary\n") + std::string(kMain), "VJASS_REQUIRES", "循环依赖");
    ExpectError(std::string("library A initializer Nope\nendlibrary\n") + std::string(kMain), "VJASS_INITIALIZER", "初始化函数不存在");
    ExpectError(std::string("library A\n    function F takes nothing returns nothing\n        set x = \n    endfunction\nendlibrary\n") +
        std::string(kMain), "VJASS_SYNTAX", "函数体语法错误");
    ExpectError(std::string("library A\n    function F takes nothing returns nothing\n    endfunction\nendlibrary\n"
                            "library B\n    function F takes nothing returns nothing\n    endfunction\nendlibrary\n") +
        std::string(kMain), "VJASS_REDECLARED", "不同库中的同名函数");
    ExpectError(std::string("library A\n    globals\n        integer g\n    endglobals\nendlibrary\n"
                            "globals\n    integer g\nendglobals\n") +
        std::string(kMain), "VJASS_REDECLARED", "同名全局变量");
}

void TestRewrites() {
    const std::string script = R"(globals
    integer Count = 0
    boolexpr Keep = null
endglobals
native ExtraNative takes nothing returns nothing
library Lib
    globals
        integer shared = 0
    endglobals
    native LibNative takes integer i returns nothing
    function Filt takes nothing returns boolean
        return true
    endfunction
    function Use takes integer Count returns nothing
        local integer shared = Count
        local group g = CreateGroup()
        call GroupEnumUnitsInRange(g, 0, 0, 100, function Filt)
        set Keep = Filter(function Filt)
        debug call BJDebugMsg("debug only")
        call BJDebugMsg("a" + "b" + I2S(shared) + "c" + "d")
        call BJDebugMsg(("e" + "f") + "g")
        call BJDebugMsg("h" + "i" + I2S(shared))
        call BJDebugMsg("line1
line2")
        call BJDebugMsg("x" /*
            */+ "y")
    endfunction
endlibrary
function InitCustomTriggers takes nothing returns nothing
    call InitTrig_Disabled()
endfunction
)" + std::string(kMain);
    const auto out = Translate(script, "改写");
    Require(Position(out, "endglobals") < Position(out, "native ExtraNative") &&
            Position(out, "native ExtraNative") < Position(out, "native LibNative") &&
            Position(out, "native LibNative") < Position(out, "//library Lib:"),
        "原生函数移到 globals 之后（脚本在前、库在后）\n" + out);
    ExpectHas(out, "function Use takes integer l__Count returns nothing", "与地图全局同名的参数改名");
    ExpectHas(out, "local integer l__shared = l__Count", "与地图全局同名的局部变量改名");
    ExpectHas(out, "GroupEnumUnitsInRange(g, 0, 0, 100, Condition(function Filt))", "原生函数的 boolexpr 参数补 Condition");
    ExpectHas(out, "set Keep = Filter(function Filt)", "code 参数不补");
    ExpectMissing(out, "debug only", "非调试模式删除 debug 行");
    ExpectHas(out, "call BJDebugMsg(\"ab\" + I2S(l__shared) + \"cd\")", "链尾是字符串字面量时合并相邻字面量");
    ExpectHas(out, "call BJDebugMsg((\"ef\") + \"g\")", "括号内是独立的链");
    ExpectHas(out, "call BJDebugMsg(\"h\" + \"i\" + I2S(l__shared))", "链尾不是字面量时不合并");
    ExpectHas(out, "call BJDebugMsg(\"line1\\nline2\")", "字符串中的换行改写为 \\n");
    ExpectHas(out, "call BJDebugMsg(\"xy\")", "块注释连接的行");
    ExpectHas(out, "//Function not found: call InitTrig_Disabled()", "不存在的 InitTrig_ 调用注释掉");
    Require(out.find("/*") == std::string::npos, "块注释应删除");
}

void TestStructs() {
    const std::string script = R"(library Shapes
    private keyword Node
    globals
        private Node last = 0
    endglobals
    struct Vec
        real x = 0.
        real y
        static integer made = 0
        static constant integer LIMIT = 10
        static method create takes real x, real y returns thistype
            local thistype this = thistype.allocate()
            set .x = x
            set this.y = y
            set thistype.made = .made + 1
            return this
        endmethod
        method len2 takes nothing returns real
            return .x * .x + this.y * this.y
        endmethod
        method scaled takes real f returns thistype
            return thistype.create(.x * f, .y * f)
        endmethod
        method operator lensq takes nothing returns real
            return this.len2()
        endmethod
        method operator px= takes real v returns nothing
            set .x = v
        endmethod
        static method tick takes nothing returns nothing
            set .made = .made + 1
        endmethod
    endstruct
    private struct Node
        Vec value
        static method operator [] takes integer i returns thistype
            return i
        endmethod
        static method operator []= takes integer i, thistype n returns nothing
            set last = n
        endmethod
        method onDestroy takes nothing returns nothing
            call .value.destroy()
        endmethod
    endstruct
    struct Shape
        real area = 1.
        method onDestroy takes nothing returns nothing
            set .area = 0.
        endmethod
    endstruct
    struct Circle extends Shape
        real radius = 2.
        static method create takes nothing returns thistype
            local thistype this = thistype.allocate()
            set .area = .radius * .radius
            return this
        endmethod
    endstruct
    struct Square extends Shape
    endstruct
    function UseAll takes nothing returns real
        local Vec v = Vec.create(1., 2.)
        local Vec w = v.scaled(2.)
        local integer raw = integer(w)
        local Vec back = Vec(raw)
        local Node n = Node.create()
        local Circle c = Circle.create()
        local Square s = Square.create()
        set n.value = v
        set Node[3] = n
        set w.px = 5.
        call TimerStart(CreateTimer(), 1., true, function Vec.tick)
        call n.destroy()
        call c.destroy()
        call s.destroy()
        return w.lensq + back.len2() + Vec.LIMIT + Vec.made + Node[3].value.x
    endfunction
endlibrary
)" + std::string(kMain);
    const auto out = Translate(script, "结构");
    ExpectHas(out, "constant integer si__Vec=1", "类型编号");
    ExpectHas(out, "constant integer si__Shapes__Node=2", "private 结构全名");
    ExpectHas(out, "real array s__Vec_x", "实例成员为数组");
    ExpectHas(out, "integer s__Vec_made= 0", "静态成员与初始值");
    ExpectHas(out, "constant integer s__Vec_LIMIT= 10", "静态常量成员");
    ExpectHas(out, "integer Shapes__last= 0", "keyword 声明的结构类型作为 integer");
    ExpectHas(out, "set s__Vec_x[this]= 0.", "分配器中的成员初始值");
    ExpectHas(out, "function s__Vec_create takes real x,real y returns integer", "静态方法");
    ExpectHas(out, "local integer this = s__Vec__allocate()", "thistype.allocate");
    ExpectHas(out, "set s__Vec_x[this] = x", ".成员");
    ExpectHas(out, "set s__Vec_made = s__Vec_made + 1", "静态方法中的 .静态成员");
    ExpectHas(out, "function s__Vec_len2 takes integer this returns real", "实例方法带 this");
    ExpectHas(out, "return s__Vec_create(s__Vec_x[this]*f,s__Vec_y[this]*f)", "方法中调用静态方法");
    ExpectHas(out, "function s__Vec__get_lensq takes integer this returns real", "取值运算符");
    ExpectHas(out, "local integer back = (raw)", "结构类型转换");
    ExpectHas(out, "local integer raw = (w)", "integer 类型转换");
    ExpectHas(out, "set s__Shapes__Node_value[n] = v", "实例成员赋值");
    ExpectHas(out, "call s__Shapes__Node__staticsetindex(3,n)", "静态 []= 运算符");
    ExpectHas(out, "call s__Vec__set_px(w,5.)", "赋值运算符");
    ExpectHas(out, "function s__Vec_tick", "function 结构.方法");
    ExpectHas(out, "call s__Shapes__Node_deallocate(n)", "onDestroy 之后直接析构");
    ExpectHas(out, "call s__Shapes__Node_onDestroy(this)", "直接析构调用 onDestroy");
    ExpectHas(out, "call sc__Shape_deallocate(c)", "有子结构时经触发器析构");
    ExpectHas(out, "call sc__Shape_deallocate(s)", "子结构的析构用根结构");
    ExpectHas(out, "s__Vec__get_lensq(w) + s__Vec_len2(back) + s__Vec_LIMIT + s__Vec_made + s__Vec_x[s__Shapes__Node_value[s__Shapes__Node__staticgetindex(3)]]",
        "表达式中的成员、运算符与静态访问");
    ExpectHas(out, "local integer this = s__Shape__allocate()\n local integer kthis", "子结构分配器调用根结构");
    ExpectHas(out, "set si__Shape_type[this]=4", "子结构类型编号写入根结构");
    ExpectHas(out, "set s__Circle_radius[this]= 2.", "子结构成员初始值");
    ExpectHas(out, "set s__Shape_area[this] = s__Circle_radius[this] * s__Circle_radius[this]", "子结构访问父结构成员");
    ExpectHas(out, "call TriggerEvaluate(st__Shape_onDestroy[si__Shape_type[this]])", "按实际类型调用 onDestroy");
    ExpectHas(out, "set st__Shape_onDestroy[3]=CreateTrigger()\n    set st__Shape_onDestroy[4]=st__Shape_onDestroy[3]", "initstructs 共用触发器");
    ExpectHas(out, "function sa__Shapes__Node_onDestroy takes nothing returns boolean\nlocal integer this=f__arg_this", "onDestroy 触发器函数");
    ExpectHas(out, "call ExecuteFunc(\"jasshelper__initstructs1234567\")", "main 中调用结构初始化");
    // onDestroy 只用游戏脚本时调用器直接复制函数体；用到地图函数时经触发器
    ExpectHas(out, "function sc__Shape_onDestroy takes integer this returns nothing\n set s__Shape_area[this] = 0.", "内联的 onDestroy 调用器");
    ExpectHas(out, "function sc__Shapes__Node_onDestroy takes integer this returns nothing\n set f__arg_this=this\n call TriggerEvaluate(st__Shapes__Node_onDestroy)",
        "经触发器的 onDestroy 调用器");

    // 同一输入得到同一输出（初始化函数名按内容计算）
    auto options = Options();
    options.initSuffix = 0;
    const auto first = TranslateVJass(script, "war3map.j", options);
    const auto second = TranslateVJass(script, "war3map.j", options);
    Require(first.Succeeded() && first.text == second.text && first.text.find("jasshelper__initstructs") != std::string::npos, "输出应确定");

    // 析构触发器的初始化顺序与 jasshelper 相同：结构表第一项在前，其余倒序（Plain 是第一项，但不需要触发器）
    std::string order = "struct Plain\nendstruct\n";
    for (const auto* name : {"A", "B", "C"}) {
        order += std::string("struct ") + name + "\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n";
    }
    const auto ordered = Translate(order + std::string(kMain), "析构触发器顺序");
    const auto a = Position(ordered, "set st__A_onDestroy=CreateTrigger()");
    const auto b = Position(ordered, "set st__B_onDestroy=CreateTrigger()");
    const auto c = Position(ordered, "set st__C_onDestroy=CreateTrigger()");
    Require(a != std::string::npos && b != std::string::npos && c != std::string::npos && c < b && b < a,
        "析构触发器顺序应为 C、B、A\n" + ordered);
    // 子结构：最后一个父结构不是第一项的子结构与第二项交换（A、B 有子结构，K 属于 B）
    const auto family = Translate("struct P\nendstruct\nstruct A\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n"
                                  "struct B\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n"
                                  "struct KA extends A\nendstruct\nstruct C\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n"
                                  "struct KB extends B\nendstruct\nstruct D\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n" +
                                  std::string(kMain), "子结构与析构触发器顺序");
    std::vector<std::size_t> positions;
    for (const auto* name : {"D", "A", "C", "B"}) positions.push_back(Position(family, std::string("function sa__") + name + "_onDestroy"));
    Require(std::ranges::is_sorted(positions) && std::ranges::find(positions, std::string::npos) == positions.end(),
        "子结构时的析构触发器顺序应为 D、A、C、B（与 jasshelper 一致）\n" + family);

    ExpectError(std::string("struct S\nendstruct\nfunction F takes nothing returns nothing\n    local S s = 0\n    set s.missing = 1\nendfunction\n") +
        std::string(kMain), "VJASS_MEMBER", "不存在的成员");
    ExpectError(std::string("function F takes integer i returns nothing\n    set i.x = 1\nendfunction\n") + std::string(kMain), "VJASS_MEMBER",
        "非结构值的成员访问");
}

void TestFallback() {
    const std::string main(kMain);
    ExpectUnsupported("//! textmacro T\n//! endtextmacro\n" + main, "//! textmacro", "//! 指令");
    const auto scoped = Translate("scope Outer\n    globals\n        integer value = 3\n    endglobals\n    function onInit takes nothing returns nothing\n        call BJDebugMsg(I2S(value))\n    endfunction\n    scope Inner\n        function Hidden takes nothing returns nothing\n        endfunction\n    endscope\nendscope\n" + main, "scope 私有名与初始化");
    ExpectHas(scoped, "integer Outer__value= 3", "scope 全局变量使用路径前缀");
    ExpectHas(scoped, "function Outer__onInit takes nothing returns nothing", "scope 函数使用路径前缀");
    ExpectHas(scoped, "function Outer__Inner__Hidden takes nothing returns nothing", "嵌套 scope 使用完整路径");
    ExpectHas(scoped, "call ExecuteFunc(\"Outer__onInit\")", "scope 的 onInit 按源码顺序初始化");
    ExpectHas(scoped, "call BJDebugMsg(I2S(Outer__value))", "scope 内引用改写到私有名");
    ExpectUnsupported("scope S initializer Missing\nendscope\n" + main, "initializer", "scope initializer 仍拒绝");
    ExpectUnsupported("scope S\n    struct T\n    endstruct\nendscope\n" + main, "struct", "scope 内 struct 仍拒绝");
    ExpectUnsupported("library L\n    module M\n    endmodule\nendlibrary\n" + main, "module", "module");
    ExpectUnsupported("interface I\nendinterface\n" + main, "interface", "interface");
    ExpectUnsupported("function F takes nothing returns nothing\n    static if true then\n    endif\nendfunction\n" + main, "static if", "static if");
    ExpectUnsupported("globals\n    key K\nendglobals\n" + main, "key", "key");
    ExpectUnsupported("struct S\n    method m takes nothing returns nothing\n    endmethod\nendstruct\nfunction F takes nothing returns nothing\n    local S s = 0\n"
                      "    call s.m.execute()\nendfunction\n" + main,
        ".execute", ".execute");
    ExpectUnsupported("struct S\n    method a takes nothing returns nothing\n        call this.b()\n    endmethod\n    method b takes nothing returns nothing\n"
                      "    endmethod\nendstruct\n" + main,
        "定义之前", "方法在定义之前调用");
    ExpectUnsupported("struct P\nendstruct\nstruct C extends P\n    method onDestroy takes nothing returns nothing\n    endmethod\nendstruct\n" + main,
        "子结构定义 onDestroy", "子结构 onDestroy");
    ExpectUnsupported("struct A\nendstruct\nstruct B extends A\nendstruct\nstruct C extends B\nendstruct\n" + main, "多层 extends", "多层继承");
    ExpectUnsupported("struct S extends array\nendstruct\n" + main, "extends array", "extends array");
    ExpectUnsupported("struct S\n    static method onInit takes nothing returns nothing\n    endmethod\nendstruct\n" + main, "onInit", "onInit");
    ExpectUnsupported("library L initializer I\n    function I takes nothing returns nothing\n    endfunction\nendlibrary\n"
                      "function main takes nothing returns nothing\nendfunction\n",
        "InitBlizzard", "main 中没有 InitBlizzard");
    auto options = Options();
    options.debug = true;
    const auto result = TranslateVJass(main, "war3map.j", options);
    Require(!result.unsupported.empty(), "调试模式应回退");
}

} // namespace

int main() {
    try {
        TestLibraries();
        TestRewrites();
        TestStructs();
        TestFallback();
    } catch (const std::exception& error) {
        std::cerr << "VJass: " << error.what() << '\n';
        return 1;
    }
    std::cout << "VJass: 4 scenario groups passed\n";
    return 0;
}
