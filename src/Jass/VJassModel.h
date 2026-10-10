#pragma once

#include "JassParser.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// vJass 程序的结构模型（由 VJassScan 从记号建立，翻译与输出阶段只读）。
// 所有 string_view 指向源文本；偏移均为源文本字节偏移。
namespace War3Map::Jass::detail {

enum class Visibility : std::uint8_t { Default, Private, Public };

// 一个顶层或库内条目占据的整行范围 [begin, end)：从所在行行首到最后一行的换行之后。
struct LineRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

struct VGlobal {
    VarDecl decl;
    Visibility visibility = Visibility::Default;
    std::int32_t library = -1;
    std::int32_t scope = -1;
    std::string finalName;
};

struct VMember {
    VarDecl decl;
    bool isStatic = false;
};

enum class OperatorKind : std::uint8_t { None, Get, Set, Index, IndexSet };

struct VFunction {
    FunctionDecl decl;           // 名称、参数、返回类型与函数体记号范围
    Visibility visibility = Visibility::Default;
    std::int32_t library = -1;
    std::int32_t scope = -1;
    std::int32_t structIndex = -1; // 方法所属的结构
    bool isMethod = false;
    bool isStatic = false;
    OperatorKind op = OperatorKind::None;
    std::uint32_t headerBegin = 0; // 声明行行首
    std::uint32_t headerEnd = 0;   // 声明行换行之后（函数体从这里开始）
    std::uint32_t endToken = 0;    // endfunction / endmethod 记号偏移
    std::uint32_t endLineEnd = 0;  // 结束行换行之后
    std::string finalName;
    std::uint32_t order = 0; // 输出中的先后位置（方法调用须在定义之后）
};

struct VStruct {
    std::string_view name;
    Visibility visibility = Visibility::Default;
    std::int32_t library = -1;
    std::string_view parentName;
    std::int32_t parent = -1;
    std::vector<std::int32_t> children;
    std::vector<VMember> members;
    std::vector<std::int32_t> methods; // VFunction 下标（按声明顺序）
    std::string fullName;
    std::int32_t typeId = 0;
    std::uint32_t line = 0;
    std::int32_t onDestroy = -1; // VFunction 下标
    std::int32_t create = -1;    // 自定义 static create
};

struct VRequirement {
    std::string_view name;
    bool optional = false;
    std::uint32_t line = 0;
};

enum class ItemKind : std::uint8_t { Globals, Native, Function, Struct, Keyword };

struct VItem {
    ItemKind kind = ItemKind::Function;
    LineRange range;
    std::int32_t index = -1; // Function / Struct / Native：对应数组下标
};

struct VScope {
    std::string_view name;
    std::int32_t parent = -1;
    std::vector<std::int32_t> children;
    std::uint32_t line = 0;
    std::uint32_t order = 0;
    LineRange header;
    LineRange footer;
    std::vector<VItem> items;
    std::string path;
};

struct VLibrary {
    std::string_view name;
    bool once = false;
    bool duplicate = false; // 同名的 library_once：整段忽略
    std::vector<VRequirement> requirements;
    std::string_view initializer;
    std::uint32_t line = 0;
    LineRange header;
    LineRange footer; // endlibrary 行
    std::vector<VItem> items;
    std::int32_t level = 0;
};

// 顶层条目：库本身也作为条目出现，以保留原顺序中的注释位置。
struct VTopItem {
    enum class Kind : std::uint8_t { Item, Library } kind = Kind::Item;
    VItem item;
    std::int32_t library = -1;
};

struct VProgram {
    std::string_view text;
    std::vector<Token> tokens;
    SyntaxPool pool; // 全局变量与成员的初始值
    std::vector<VGlobal> globals;
    std::vector<LineRange> natives;       // 原生函数声明行（原样移到 globals 之后）
    std::vector<LineRange> debugLines;    // 以 debug 开头的行（已从记号中删除，输出时整行删除），按位置排序
    std::vector<NativeDecl> nativeDecls;  // 与 natives 一一对应的签名
    std::vector<VFunction> functions;
    std::vector<VStruct> structs;
    std::vector<VLibrary> libraries;
    std::vector<VScope> scopes;
    std::vector<VTopItem> top;
    // 库内 private / public 名称：{库, 源名称, 可见性}（函数、全局、结构、keyword）。
    struct ScopedName {
        std::int32_t library = -1;
        std::string_view name;
        Visibility visibility = Visibility::Default;
    };
    std::vector<ScopedName> scopedNames;
    std::vector<SyntaxError> errors;
    std::vector<SyntaxError> warnings; // 与 jasshelper 一样容忍、但值得提示的写法
    std::string unsupported; // 首个不支持的特性（原因与行号）
};

// 由源文本建立模型（只做结构解析；函数体在翻译阶段解析）。
[[nodiscard]] VProgram ScanVJass(std::string_view text, CheckBudget* budget = nullptr);

// 库内名称改写：private → 库__名，public → 库_名。
[[nodiscard]] std::string ScopedFinalName(std::string_view library, std::string_view name, Visibility visibility);

} // namespace War3Map::Jass::detail
