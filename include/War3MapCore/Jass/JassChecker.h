#pragma once

#include "War3MapCore/Types.h"
#include "War3MapCore/Jass/JassSymbols.h"
#include "War3MapCore/Jass/SourceMap.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// 自研 JASS 语法与类型检查（替代 pjass）。规则按 pjass 的可观察行为对齐（独立实现）：
//   - 声明顺序：函数只能调用在它之前声明的函数 / 原生函数（及自身）；全局初始值只能引用之前的全局与函数；
//   - 类型：integer 可转 real；null 可转 string / code / 句柄；句柄子类型可转父类型；比较、运算与条件的类型规则；
//   - 常量不可赋值；常量函数只能调用常量函数、不能修改全局；数组必须带下标；局部变量须在读取前赋值（按文本顺序）；
//   - 有返回值的函数所有路径都要 return；function 引用的函数不能带参数；rawcode 为 1 或 4 个字符；
//   - 与 pjass 相同的开关：returnbug（1.20 返回值漏洞）、shadow、filter，以及函数前的 //# +flag 注释。
// 源文件按顺序作为一个整体检查（通常为 common.j、blizzard.j、war3map.j）；函数体按函数并行解析与检查。
namespace War3Map::Jass {

struct JassSource {
    std::string name;      // 诊断中的文件名
    std::string_view text; // 检查期间须保持有效
};

struct JassCheckOptions {
    bool returnBug = false; // +rb：只检查函数最后一条 return 的类型
    bool shadow = false;    // +shadow：局部变量与全局变量同名时报错
    bool filter = false;    // +filter：Filter / Condition 的函数必须返回 boolean
    unsigned threads = 0;   // 函数体期望并发数；0 按硬件，仍受 maxBodyWorkers 限制
    std::size_t maxErrors = 200;
    // 每次调用共用预算。达到硬预算、取消或超时均拒绝，不受逐函数忽略错误开关影响。
    std::size_t maxSources = 64;
    std::size_t maxSourceBytes = 32 * 1024 * 1024;
    std::size_t maxTotalBytes = 64 * 1024 * 1024;
    // 实际大图（约 6 MB、写法紧凑的主脚本）已超过 400 万个记号；上限只限制增长，不预先分配。
    std::size_t maxTokens = 12 * 1024 * 1024; // 所有来源合计，含每个来源的 End
    std::size_t maxAstNodes = 8 * 1024 * 1024; // 声明、表达式、语句及参数边合计
    std::size_t maxPoolNodes = 262144; // 单个全局/函数池，包含参数边
    std::size_t maxAstDepth = 128; // 真实子树深度；有效值最多 512，防止递归检查耗尽栈
    std::size_t maxNesting = 64;   // 解析递归结构；有效值最多 128
    std::size_t maxProducedDiagnostics = 4096; // 产生阶段预算，独立于显示条数
    std::size_t maxDiagnosticBytes = 2 * 1024 * 1024; // 产生及展示诊断的累计文本字节
    std::size_t maxDiagnosticMessageBytes = 2048;
    unsigned maxSourceWorkers = 2;
    unsigned maxBodyWorkers = 4;
    std::stop_token cancel;
    std::chrono::steady_clock::time_point deadline = (std::chrono::steady_clock::time_point::max)();
    std::chrono::milliseconds maxDuration{30000};
    bool syntaxOnly = false; // 缺完整声明时仅检查语法；保存/F9 默认仍完整语义
    bool collectSymbols = false; // 编辑器可选：与本次语义检查共用实际作用域，保存默认不收集。
    std::size_t maxSymbolDefinitions = 65536;
    std::size_t maxSymbolReferences = 524288;
    std::size_t maxSymbolBytes = 32 * 1024 * 1024;
    // 仅换算诊断文字中的位置；AST、符号和origin继续使用实际输入身份。
    // 用生成链已有的行映射，外部声明文件不会误套主正文的映射。
    std::string diagnosticSourceName;
    SourceMap diagnosticSourceMap;
};

struct JassCheckResult {
    std::vector<Diagnostic> diagnostics; // 普通错误以“文件:行: ”开头；硬预算错误独立收敛
    std::vector<bool> diagnosticOriginExact; // 与diagnostics同索引；只有实际AST原字节跨度为true。
    std::size_t lines = 0;
    std::size_t functions = 0;
    std::size_t errorsIgnored = 0; // 被 //# +nosemanticerror / +nosyntaxerror 忽略的错误数
    unsigned threadsUsed = 1;
    unsigned sourceThreadsUsed = 1;
    bool completed = true; // false 表示检查未完整执行，不能作为编译通过证据
    bool semanticChecked = false;
    std::size_t tokens = 0;
    std::size_t astNodes = 0;
    std::size_t maxAstDepthObserved = 0;
    std::size_t diagnosticBytes = 0;
    SymbolIndex symbols; // 不完整或独立索引预算耗尽不改变脚本校验结论。

    [[nodiscard]] bool Succeeded() const noexcept;
};

[[nodiscard]] JassCheckResult CheckJass(std::span<const JassSource> sources, const JassCheckOptions& options = {});

} // namespace War3Map::Jass
