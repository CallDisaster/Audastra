#pragma once

#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/JassDeclarationIndex.h"
#include "War3MapCore/Jass/SourceMap.h"
#include "War3MapCore/Lua/LuaSyntax.h"
#include "War3MapCore/Lua/LuaSemantic.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

namespace War3Map::Services {

enum class CodeLanguage : std::uint8_t { Jass, VJass, Lua53 };

// 请求拥有文本；工作线程不能借用实时工程或界面控件。来源名仅用于诊断，不读取磁盘。
struct CodeCheckSource {
    std::string name;
    std::string text;
};

struct CodeCheckContext {
    // 与保存检查一样按顺序登记实际 common/blizzard/地图声明。
    std::vector<CodeCheckSource> declarations;
    // 预处理只允许这些冻结文本；不存在的包含明确失败，不回退本机磁盘。
    std::vector<CodeCheckSource> includes;
    bool complete = false;
    std::string binding;
    std::string reason;
    Lua::SemanticContext luaSemantic;
};

struct CodeCheckRequest {
    CodeLanguage language = CodeLanguage::Jass;
    CodeCheckSource source;
    CodeCheckContext context;
    // 草稿在冻结的生产整图文本中的上下文；为空时直接检查正文。
    std::string prefix;
    std::string suffix;
    std::uint64_t generation = 0;
    bool debug = false;
    bool preprocessed = false; // 生产工程入口已完成同次owned预处理时才置true。
    // 输入经外部冻结预处理时可附真实来源映射；不按输出字符串猜原行。
    Jass::SourceMap sourceMap;
    Jass::JassCheckOptions jass;
    Lua::SyntaxOptions lua;
};

struct CodeCheckDiagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    std::string source;
    std::string code;
    std::string message;
    std::uint32_t line = 0; // 1基；0表示全局错误或尚不能定位。
    std::uint32_t byteColumn = 1; // 1基UTF-8字节列，Qt按请求正文转换UTF-16列。
    std::uint32_t byteLength = 0;
    bool originMapped = false;
    bool byteRangeExact = false; // 与可靠原行分开；行映射/生成锚点不声称字节范围精确。
};
struct CodeSymbolPosition {
    std::uint32_t file = 0, line = 0, byteColumn = 1, byteLength = 0;
    bool exact = false;
    // 前端真实字节provenance的行/列候选；PP仅有行映射时须与冻结原行逐字节相等才授予exact。
    std::uint32_t inputLine = 0, inputByteColumn = 1, inputByteLength = 0;
    bool inputExact = false;
};
struct CodeSemanticIndex {
    Jass::SymbolIndex parsed; // 实际checker身份/target；行映射不合并同名符号。
    std::vector<std::string> sources;
    std::vector<CodeSymbolPosition> definitions, references; // 与parsed同索引；未知来源line=0。
};

struct CodeCheckResult {
    std::uint64_t generation = 0;
    std::string contextBinding;
    bool completed = false;
    bool syntaxChecked = false; // 表示已执行；通过与否见Succeeded/diagnostics。
    bool semanticChecked = false; // 表示已执行；普通语义错误不清除此事实。
    bool contextComplete = false;
    bool cancelled = false;
    std::string reason;
    std::vector<CodeCheckDiagnostic> diagnostics;
    // 生产Parser的真实顶层声明，包含实际基础库和已经展开的整图来源；不宣称局部/引用全覆盖。
    Jass::DeclarationIndex declarations;
    CodeSemanticIndex semanticIndex;
    std::vector<CodeCheckSource> externalSources;
    bool externalSourcesComplete = true; // 只读导航清单；与语义检查是否完整分别报告。
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 同步核心服务只允许在后台/控制台调用；UI和MCP各自负责有界异步调度。
// 不执行作者代码、不启动外部程序、不读取未冻结的包含文件或运行库。
[[nodiscard]] CodeCheckResult CheckCode(const CodeCheckRequest& request, std::stop_token cancel = {});

} // namespace War3Map::Services
