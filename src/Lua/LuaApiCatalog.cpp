#include "War3MapCore/Lua/LuaApiCatalog.h"

#include <algorithm>

namespace War3Map::Lua {
namespace {
// 参数写法 "name:type, name:type, ..."；"..." 表示可变参数。
ApiMember Function(std::string_view module, std::string name, std::string_view parameters, std::string returns, std::string documentation) {
    ApiMember member;
    member.name = std::move(name); member.kind = ApiKind::Function; member.module = std::string(module);
    member.returns = std::move(returns); member.documentation = std::move(documentation);
    while (!parameters.empty()) {
        auto comma = parameters.find(',');
        auto part = parameters.substr(0, comma);
        parameters = comma == std::string_view::npos ? std::string_view{} : parameters.substr(comma + 1);
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);
        if (part == "...") { member.variadic = true; continue; }
        const auto colon = part.find(':');
        member.parameters.push_back({std::string(part.substr(0, colon)),
            colon == std::string_view::npos ? std::string("any") : std::string(part.substr(colon + 1))});
    }
    return member;
}
ApiMember Value(std::string_view module, std::string name, ApiKind kind, std::string type, std::string documentation) {
    ApiMember member;
    member.name = std::move(name); member.kind = kind; member.module = std::string(module);
    member.type = std::move(type); member.documentation = std::move(documentation);
    return member;
}
void Sort(std::vector<ApiMember>& members) {
    std::sort(members.begin(), members.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
}
ApiModule StandardTable(std::string name, std::string documentation, std::vector<ApiMember> members) {
    ApiModule module{std::move(name), std::move(documentation), std::move(members), true, false};
    Sort(module.members);
    return module;
}

// 自有运行时开放的标准库：base、table、string、math、utf8，以及只含 traceback 的 debug。
// dofile/loadfile/load/collectgarbage/string.dump 被移除，io/os/coroutine/package 没有安装。
void AddStandardLibrary(ApiCatalog& catalog) {
    auto& globals = catalog.globals;
    for (auto member : {
        Function("", "assert", "v:any, message:string", "any", "v 为 false 或 nil 时抛出错误，否则返回全部参数。"),
        Function("", "error", "message:any, level:integer", "", "抛出错误。"),
        Function("", "getmetatable", "object:any", "table", "取得元表。"),
        Function("", "ipairs", "t:table", "function", "按 1、2、3… 顺序遍历数组部分。"),
        Function("", "next", "t:table, index:any", "any", "取得表中的下一个键值。"),
        Function("", "pairs", "t:table", "function", "遍历表的全部键值，顺序不确定。"),
        Function("", "pcall", "f:function, ...", "boolean", "保护模式调用函数；第一个返回值表示是否成功。"),
        Function("", "print", "...", "", "输出到测试日志。"),
        Function("", "rawequal", "a:any, b:any", "boolean", "不经元方法比较是否相等。"),
        Function("", "rawget", "t:table, key:any", "any", "不经元方法读取字段。"),
        Function("", "rawlen", "v:any", "integer", "不经元方法取得长度。"),
        Function("", "rawset", "t:table, key:any, value:any", "table", "不经元方法写入字段。"),
        Function("", "require", "name:string", "any", "加载模块：jass.* 运行库模块，或地图里的 .lua 文件（子目录用点号）。"),
        Function("", "select", "index:any, ...", "any", "取得第 index 个之后的参数；index 为 \"#\" 时返回参数个数。"),
        Function("", "setmetatable", "t:table, metatable:table", "table", "设置元表，返回 t。"),
        Function("", "tonumber", "value:any, base:integer", "number", "转换为数字，失败返回 nil。"),
        Function("", "tostring", "value:any", "string", "转换为文本。"),
        Function("", "type", "value:any", "string", "返回类型名：nil、number、string、boolean、table、function、userdata。"),
        Function("", "xpcall", "f:function, handler:function, ...", "boolean", "保护模式调用函数，出错时调用 handler。"),
    }) globals.push_back(std::move(member));
    globals.push_back(Value("", "_G", ApiKind::Variable, "table", "全局环境表。"));
    globals.push_back(Value("", "_VERSION", ApiKind::Constant, "string", "Lua 版本文本（Lua 5.3）。"));

    catalog.modules.emplace("string", StandardTable("string", "文本处理。也可以写成 s:upper() 这样的方法调用。", {
        Function("string", "byte", "s:string, i:integer, j:integer", "integer", "取得字符的字节值。"),
        Function("string", "char", "...", "string", "把字节值拼成文本。"),
        Function("string", "find", "s:string, pattern:string, init:integer, plain:boolean", "integer", "查找模式，返回起止位置。"),
        Function("string", "format", "format:string, ...", "string", "按格式拼接文本，例如 string.format(\"%d 金\", 100)。"),
        Function("string", "gmatch", "s:string, pattern:string", "function", "逐个匹配模式，用于 for … in。"),
        Function("string", "gsub", "s:string, pattern:string, replacement:any, n:integer", "string", "替换匹配的内容，第二个返回值是替换次数。"),
        Function("string", "len", "s:string", "integer", "字节长度。"),
        Function("string", "lower", "s:string", "string", "转为小写。"),
        Function("string", "match", "s:string, pattern:string, init:integer", "string", "匹配模式，返回捕获内容。"),
        Function("string", "pack", "format:string, ...", "string", "按格式打包为二进制文本。"),
        Function("string", "packsize", "format:string", "integer", "打包格式的字节数。"),
        Function("string", "rep", "s:string, n:integer, separator:string", "string", "重复 n 次。"),
        Function("string", "reverse", "s:string", "string", "反转。"),
        Function("string", "sub", "s:string, i:integer, j:integer", "string", "截取第 i 到第 j 个字节。"),
        Function("string", "unpack", "format:string, s:string, position:integer", "any", "按格式解包。"),
        Function("string", "upper", "s:string", "string", "转为大写。"),
    }));
    catalog.modules.emplace("table", StandardTable("table", "数组与表操作。", {
        Function("table", "concat", "list:table, separator:string, i:integer, j:integer", "string", "把数组元素连接成文本。"),
        Function("table", "insert", "list:table, ...", "", "插入元素：insert(t, v) 追加到末尾，insert(t, pos, v) 插入到 pos。"),
        Function("table", "move", "a1:table, f:integer, e:integer, t:integer, a2:table", "table", "移动一段元素。"),
        Function("table", "pack", "...", "table", "把参数打包成表，字段 n 为个数。"),
        Function("table", "remove", "list:table, position:integer", "any", "移除并返回元素，默认最后一个。"),
        Function("table", "sort", "list:table, comparator:function", "", "原地排序。"),
        Function("table", "unpack", "list:table, i:integer, j:integer", "any", "把数组展开为多个返回值。"),
    }));
    catalog.modules.emplace("math", StandardTable("math", "数学函数。random 使用游戏同步随机数。", {
        Function("math", "abs", "x:number", "number", "绝对值。"),
        Function("math", "acos", "x:number", "number", "反余弦（弧度）。"),
        Function("math", "asin", "x:number", "number", "反正弦（弧度）。"),
        Function("math", "atan", "y:number, x:number", "number", "反正切（弧度）。"),
        Function("math", "ceil", "x:number", "integer", "向上取整。"),
        Function("math", "cos", "x:number", "number", "余弦（弧度）。"),
        Function("math", "deg", "x:number", "number", "弧度转角度。"),
        Function("math", "exp", "x:number", "number", "e 的 x 次方。"),
        Function("math", "floor", "x:number", "integer", "向下取整。"),
        Function("math", "fmod", "x:number, y:number", "number", "x 除以 y 的余数（向零取整）。"),
        Value("math", "huge", ApiKind::Constant, "number", "正无穷大。"),
        Function("math", "log", "x:number, base:number", "number", "对数，默认自然对数。"),
        Function("math", "max", "x:number, ...", "number", "最大值。"),
        Value("math", "maxinteger", ApiKind::Constant, "integer", "最大整数。"),
        Function("math", "min", "x:number, ...", "number", "最小值。"),
        Value("math", "mininteger", ApiKind::Constant, "integer", "最小整数。"),
        Function("math", "modf", "x:number", "number", "整数部分与小数部分。"),
        Value("math", "pi", ApiKind::Constant, "number", "圆周率。"),
        Function("math", "rad", "x:number", "number", "角度转弧度。"),
        Function("math", "random", "m:integer, n:integer", "number", "随机数：无参数为 [0,1)，random(n) 为 1..n，random(m, n) 为 m..n。使用游戏同步随机数。"),
        Function("math", "randomseed", "x:integer", "", "设置随机种子。"),
        Function("math", "sin", "x:number", "number", "正弦（弧度）。"),
        Function("math", "sqrt", "x:number", "number", "平方根。"),
        Function("math", "tan", "x:number", "number", "正切（弧度）。"),
        Function("math", "tointeger", "x:number", "integer", "能精确表示为整数时返回整数，否则返回 nil。"),
        Function("math", "type", "x:any", "string", "\"integer\"、\"float\" 或 nil。"),
        Function("math", "ult", "m:integer, n:integer", "boolean", "按无符号整数比较 m < n。"),
    }));
    catalog.modules.emplace("utf8", StandardTable("utf8", "UTF-8 文本处理（中文按字符计数用它）。", {
        Function("utf8", "char", "...", "string", "把码点拼成 UTF-8 文本。"),
        Value("utf8", "charpattern", ApiKind::Constant, "string", "匹配一个 UTF-8 字符的模式。"),
        Function("utf8", "codepoint", "s:string, i:integer, j:integer", "integer", "取得码点。"),
        Function("utf8", "codes", "s:string", "function", "逐字符遍历，用于 for … in。"),
        Function("utf8", "len", "s:string, i:integer, j:integer", "integer", "字符个数。"),
        Function("utf8", "offset", "s:string, n:integer, i:integer", "integer", "第 n 个字符的字节位置。"),
    }));
    catalog.modules.emplace("debug", StandardTable("debug", "调试（只开放 traceback）。", {
        Function("debug", "traceback", "message:string, level:integer", "string", "取得当前调用栈文本。"),
    }));
    for (const auto* name : {"string", "table", "math", "utf8", "debug"}) {
        auto member = Value("", name, ApiKind::Module, "table", catalog.modules.at(name).documentation);
        member.table = name;
        globals.push_back(std::move(member));
    }
    Sort(globals);
}

void AddRuntimeModules(ApiCatalog& catalog, OwnedRuntimeInput& input) {
    ApiModule common{"jass.common", "common.j 里的全部 native；也能读取 JASS 全局常量。", {}, true, true};
    for (auto& native : input.natives) {
        ApiMember member;
        member.name = std::move(native.name); member.kind = ApiKind::Function; member.module = "jass.common";
        member.parameters = std::move(native.parameters); member.returns = native.returns == "nothing" ? std::string{} : std::move(native.returns);
        member.documentation = std::move(native.documentation);
        common.members.push_back(std::move(member));
    }
    for (auto& global : input.commonGlobals) {
        catalog.jassGlobals.insert(global.name);
        auto member = Value("jass.common", std::move(global.name), global.constant ? ApiKind::Constant : ApiKind::Variable,
            std::move(global.type), std::move(global.documentation));
        member.value = std::move(global.value);
        common.members.push_back(std::move(member));
    }
    // jass.common 找不到 native 时按全局读取，因此常量以外的全局也能读到，但不列入补全。
    // 没有加载 common.j 时成员未知，不能把访问判为错误。
    common.closed = catalog.nativesLoaded;
    Sort(common.members);
    // 同名只留一项（native 优先于同名全局，实际不会同时存在）。
    common.members.erase(std::unique(common.members.begin(), common.members.end(),
        [](const auto& left, const auto& right) { return left.name == right.name; }), common.members.end());

    ApiModule globals{"jass.globals", "JASS 全局变量（blizzard.j 与地图变量），只读。", {}, false, true};
    for (auto& global : input.otherGlobals) {
        catalog.jassGlobals.insert(global.name);
        auto member = Value("jass.globals", std::move(global.name), global.constant ? ApiKind::Constant : ApiKind::Variable,
            std::move(global.type), std::move(global.documentation));
        member.value = std::move(global.value);
        globals.members.push_back(std::move(member));
    }
    Sort(globals.members);
    globals.members.erase(std::unique(globals.members.begin(), globals.members.end(),
        [](const auto& left, const auto& right) { return left.name == right.name; }), globals.members.end());

    ApiModule runtime{"jass.runtime", "运行时选项。", {
        Value("jass.runtime", "catch_crash", ApiKind::Field, "boolean", "只能为 true：脚本让游戏状态失效时停止本次测试。"),
        Value("jass.runtime", "error_handle", ApiKind::Field, "function", "Lua 错误回调：function(message) … end，设为 nil 取消。"),
        Value("jass.runtime", "handle_level", ApiKind::Field, "integer", "只支持 2：handle 以持有引用的 userdata 传递。"),
        Value("jass.runtime", "sleep", ApiKind::Field, "boolean", "只能为 false：等待类 native 会被拒绝。"),
    }, true, true};
    ApiModule console{"jass.console", "控制台。", {
        Value("jass.console", "enable", ApiKind::Field, "boolean", "是否打开控制台输出。"),
    }, true, true};
    catalog.modules.emplace(common.name, std::move(common));
    catalog.modules.emplace(globals.name, std::move(globals));
    catalog.modules.emplace(runtime.name, std::move(runtime));
    catalog.modules.emplace(console.name, std::move(console));
}
} // namespace

const ApiMember* ApiModule::Find(std::string_view memberName) const {
    const auto found = std::lower_bound(members.begin(), members.end(), memberName,
        [](const ApiMember& member, std::string_view key) { return member.name < key; });
    return found != members.end() && found->name == memberName ? &*found : nullptr;
}
const ApiModule* ApiCatalog::Module(std::string_view name) const {
    const auto found = modules.find(name);
    return found == modules.end() ? nullptr : &found->second;
}
const ApiMember* ApiCatalog::Global(std::string_view name) const {
    const auto found = std::lower_bound(globals.begin(), globals.end(), name,
        [](const ApiMember& member, std::string_view key) { return member.name < key; });
    return found != globals.end() && found->name == name ? &*found : nullptr;
}
bool ApiCatalog::IsJassType(std::string_view type) const {
    static constexpr std::string_view primitives[] = {"integer", "real", "boolean", "string", "code", "handle"};
    return std::find(std::begin(primitives), std::end(primitives), type) != std::end(primitives) || typeParents.contains(type);
}
bool ApiCatalog::Extends(std::string_view type, std::string_view base) const {
    std::string current(type);
    for (int depth = 0; depth < 64; ++depth) {
        if (current == base) return true;
        const auto parent = typeParents.find(current);
        if (parent == typeParents.end()) return false;
        current = parent->second;
    }
    return false;
}

std::shared_ptr<const ApiCatalog> BuildOwnedRuntimeCatalog(OwnedRuntimeInput input) {
    auto catalog = std::make_shared<ApiCatalog>();
    catalog->binding = std::move(input.binding);
    catalog->source = std::move(input.source);
    catalog->nativesLoaded = !input.natives.empty();
    for (auto& [type, parent] : input.types) if (!type.empty()) catalog->typeParents.emplace(std::move(type), std::move(parent));
    AddStandardLibrary(*catalog);
    AddRuntimeModules(*catalog, input);
    return catalog;
}

const std::vector<std::string_view>& LuaKeywords() {
    static const std::vector<std::string_view> keywords{"and", "break", "do", "else", "elseif", "end", "false", "for",
        "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return keywords;
}

} // namespace War3Map::Lua
