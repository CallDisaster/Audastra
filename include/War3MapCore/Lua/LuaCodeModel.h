#pragma once

#include "War3MapCore/Lua/LuaApiCatalog.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Lua {

// 地图里其它 Lua 文件：require 名（lessons.hello）→ 正文。没有该模块时返回 nullopt。
struct ModuleSources {
    std::function<std::vector<std::string>()> list;
    std::function<std::optional<std::string>(std::string_view name)> read;
};

enum class CompletionKind : std::uint8_t { Keyword, Module, Function, Method, Field, Variable, Constant, Local, Parameter };

struct CompletionItem {
    std::string label;
    CompletionKind kind = CompletionKind::Variable;
    std::string detail;        // 签名或类型
    std::string documentation;
};
// 位置都是 UTF-8 字节偏移；接受候选时替换 [replaceBegin, replaceEnd)。
struct CompletionList {
    std::size_t replaceBegin = 0, replaceEnd = 0;
    bool member = false;   // 点号或冒号之后
    bool moduleName = false; // require 字符串里的模块名
    std::vector<CompletionItem> items;
};
struct SignatureHelp {
    std::string label;
    std::vector<std::pair<std::size_t, std::size_t>> parameters; // label 内的字节范围
    int active = 0;
    std::string documentation;
};
struct HoverInfo {
    std::string kind, signature, documentation;
};
struct CodeWarning {
    std::string code, message;
    std::uint32_t line = 0, byteColumn = 1, byteLength = 0;
};

// 不执行代码的静态分析：作用域、局部变量类型推断、require 跨文件、jass.* 接口。
// Lua 是动态语言，推不出的类型按“未知”处理，不据此报错。
// 内部缓存解析结果；同一对象不能跨线程同时使用。
class CodeIntel final {
public:
    CodeIntel(std::shared_ptr<const ApiCatalog> catalog, ModuleSources modules, std::string currentModule = {});
    ~CodeIntel();
    CodeIntel(const CodeIntel&) = delete;
    CodeIntel& operator=(const CodeIntel&) = delete;

    // 同一份地图脚本里的其它代码块（如 WCT 头部与其它触发器）：它们定义的全局在这里也可见。
    void SetSharedSources(const std::vector<std::string>& sources);

    [[nodiscard]] CompletionList Complete(std::string_view source, std::size_t cursor);
    [[nodiscard]] std::optional<SignatureHelp> Signature(std::string_view source, std::size_t cursor);
    [[nodiscard]] std::optional<HoverInfo> Hover(std::string_view source, std::size_t cursor);
    // jass 接口用法检查：不存在的 native、参数个数、能确定时的参数类型，以及找不到的地图模块。
    [[nodiscard]] std::vector<CodeWarning> Check(std::string_view source);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace War3Map::Lua
