#pragma once

#include "War3MapCore/MapArchiveSession.h"

#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/Script/ScriptBaseInputs.h"
#include "War3MapCore/Script/MapRuntimeBoot.h"
#include "War3MapCore/Types.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Jass { struct IdentifierRewriteResult; }

// 整图脚本编译流水线（与 YDWE 保存流程等价的行为，独立实现）：
//   生成脚本 → 代码注入 → 预处理（内置，或外部 Wave）→ 模板求值 → vJass 翻译（内置，或外部 jasshelper）
//   → 语法检查（内置，或外部 pjass）。
// 内置 vJass 翻译遇到尚未支持的特性或出错时自动改用 jasshelper（其内部调用 pjass 检查）。
// 外部程序只在工作目录中读写副本，不修改工具目录与地图。
namespace War3Map::Script {

struct ScriptFrameworkInputs;
struct ScriptPreprocessSources;

// 魔兽字符串哈希（StringHash 原生函数的算法：Jenkins lookup2，大小写与斜杠归一）。
[[nodiscard]] std::uint32_t StormStringHash(std::string_view bytes);

// ---- 代码注入 ----
// 函数库 jass 目录中 *.cfg 列出函数名，同名 .j 为注入文件；脚本中出现函数名即把整个文件追加到脚本末尾。
struct InjectTable {
    struct Entry {
        std::string functionName;
        std::filesystem::path file;
    };
    std::vector<Entry> entries; // 按函数名排序、去重（同名取修改时间较新的文件）
};

// 扫描各函数库的 jass 目录（按启用顺序）；version24 为 true 时取 [general] + [new] 段，否则取 [general] + [old] 段。
[[nodiscard]] InjectTable ScanInjectTable(const std::vector<std::filesystem::path>& jassDirs, bool version24,
    std::vector<Diagnostic>& diagnostics);

// 对 diagnostics[first..] 中“未声明的函数”错误：若在给定函数库 jass 目录（递归）里找到声明该函数的文件，
// 说明那个文件不会自动注入（例如没有 .cfg 的 MemHack 库），追加警告提示作者 #include（相对目录的路径）。
// 只提示，不替作者添加；同一文件的多个函数合并为一条。
void AppendMissingIncludeHints(std::vector<Diagnostic>& diagnostics, std::size_t first,
    const std::vector<std::filesystem::path>& jassDirs);

// 返回注入后的脚本；追加的文件按路径排序，每个文件后补 CRLF。
// requiredIdentifiers 是宿主明确选择的库依赖，不向作者脚本插入伪调用。
[[nodiscard]] std::string InjectCode(std::string script, const InjectTable& table, std::vector<Diagnostic>& diagnostics,
    std::vector<std::filesystem::path>* injected = nullptr, std::vector<std::string>* sourceSha256 = nullptr,
    std::span<const std::string_view> requiredIdentifiers = {});

struct ScriptInjectionSources;
// 代码编辑器只读检查使用同一完整标识符匹配与所选来源，全文由捕获服务拥有。
// 不扫描磁盘、不执行模板；取消/预算失败不返回部分注入文本。
[[nodiscard]] Result<std::string> InjectOwnedCode(std::string_view script,
    const ScriptInjectionSources& sources, const Jass::JassCheckOptions& check,
    std::vector<std::filesystem::path>* injected = nullptr);

// ---- 模板求值 ----
// Wave 输出中的 <?= 表达式 ?> 与 <? 语句 ?> 块。只支持受限子集：字面量、变量赋值、算术与连接、
// StringHash / abilityOrder / math.modf|floor|ceil|abs|max|min / tostring / tonumber，
// 以及 YDWE 的 import(名)(文本) / import(名)(相对路径, true)（把文本或地图目录下的文件写入地图）。
// 含其他语句的块（如 slk 物编生成）不执行并报错；未闭合模板或表达式无法求值也报错。
// 被拒绝的语句块及未闭合模板保留在失败结果中供诊断，调用方必须先检查 Succeeded() 再使用输出。
// 整份模板执行（YDWE Lua 模板）的结果：展开后的脚本与 SLK 刷新实际写出的七类物编。
struct TemplateTransactionOutput {
    std::string script;
    std::vector<MemberReplacement> objectMembers; // 空表示物编无变化
    std::vector<std::string> toolMessages;
    std::vector<MemberReplacement> imports; // 模板 import 写入的文件（规则同 EvaluateTemplates）
    std::vector<Diagnostic> diagnostics;    // 非致命提示（如 slk 报告中的读取错误）
};

struct TemplateContext {
    // abilityOrder(技能, 类型) 的命令 ID；无法确定时返回空（按 0 处理并警告）。
    std::function<std::optional<std::int64_t>(std::string_view ability, std::string_view type)> abilityOrder;
    // 可选：内置求值遇到不支持的语句块（如 slk 物编生成）时，改由此回调整份执行模板。
    // 未提供时这类块报错并中止；回调失败同样中止，不回退到跳过语句块。
    // libraries 为框架档案已开启的原创模板函数集（见 ScriptProfileFeatureExtension），可为空。
    std::function<Result<TemplateTransactionOutput>(std::string_view preprocessed,
        const std::vector<std::string>& libraries)> wholeChunk;
    // import 的路径形式相对此目录读取（YDWE 约定：地图文件所在目录），只读、不越出该目录。
    // 为空时只核对 import 写法与名称，不读文件、不产生成员（代码检查等只读场景）。
    std::optional<std::filesystem::path> importRoot;
};

// 一个模板块在输入与输出中的字节范围 [begin, end)。块外文本逐字节原样复制，调用方可据此把输出位置映射回输入。
struct TemplateSpan {
    std::size_t sourceBegin = 0, sourceEnd = 0;
    std::size_t outputBegin = 0, outputEnd = 0;
};

struct TemplateResult {
    std::string text;
    std::vector<Diagnostic> diagnostics;
    std::vector<TemplateSpan> spans; // 按出现顺序；未闭合的模板不登记（其后文本原样保留）
    std::size_t expressions = 0;
    std::size_t skippedBlocks = 0;
    std::vector<MemberReplacement> imports; // 模板 import 要写入地图的文件，按名称去重（后者覆盖前者）

    [[nodiscard]] bool Succeeded() const noexcept;
};

[[nodiscard]] TemplateResult EvaluateTemplates(std::string_view text, const TemplateContext& context);

// 整份执行（wholeChunk）的输出没有逐块位置。按 builtin（同一输入的内置求值）登记的块位置，把块外文本
// 逐段对回输出：块外文本须逐字节出现；内置求值已得到结果的块，其输出也须与整份执行一致；只有未执行的块
// 输出长度由下一段块外文本的位置确定。任何一处对不上返回空，不按近似位置猜测。
[[nodiscard]] std::optional<TemplateResult> AlignTemplateOutput(std::string_view input, const TemplateResult& builtin,
    std::string output);

// ---- 工具链 ----
// 各阶段使用的实现：内置（自研）或外部程序（YDWE 自带）。
enum class ScriptEngine { Builtin, External };

struct ToolchainOptions {
    std::filesystem::path toolRoot;              // YDWE 根目录（compiler/、bin/）
    std::vector<std::filesystem::path> jassDirs; // 启用函数库的 jass 目录，按启用顺序
    std::filesystem::path workDir;               // 中间文件目录（会被创建）；每个阶段的输入输出都留在这里
    int warcraftVersion = 24;                    // 24 = 1.24+，20 = 1.20
    std::optional<int> actualVersionMacro;       // 实际 WARCRAFT_VERSION；不改变 return bug 策略
    std::filesystem::path uiRoot;                // UI 层来源可与工具根分离，空值保持历史默认
    std::shared_ptr<const ScriptToolchainLayout> layout; // 保存准备冻结一次，各阶段共用并复核
    ScriptBasePolicy basePolicy = ScriptBasePolicy::BoundLayout;
    std::optional<int> gameLibraryVersion;        // 明确验证的游戏基础库策略，与实际版本宏分离
    std::shared_ptr<const ScriptBaseInputs> baseInputs; // 两基础库独立来源与完整字节；外部程序也只读副本
    // 旧框架仅消费明确档案与owned来源；空值不会退回旧目录扫描或Lua宿主。
    std::shared_ptr<const ScriptFrameworkInputs> frameworkInputs;
    std::string versionString = "1.32.13";       // YDWE_VERSION_STRING
    bool scriptInjection = true;
    bool debug = false;
    bool optimize = false;
    std::optional<MapRuntimeBootPlan> mapRuntimeBoot;
    ScriptEngine preprocessor = ScriptEngine::Builtin; // 外部为 Wave
    ScriptEngine vjass = ScriptEngine::Builtin;        // 外部为 jasshelper；内置不支持时自动改用外部
    ScriptEngine checker = ScriptEngine::Builtin;      // 外部为 pjass（仅在内置 vJass 翻译成功时使用；jasshelper 自带 pjass）
    bool crossCheck = false;                           // 内置检查之后再运行 pjass，结论不一致时警告（对照验证用）
    Jass::PreprocessCache* preprocessCache = nullptr;  // 可空；跨多次构建复用函数库文件的词法结果
    std::chrono::seconds timeout{600};
    // 每个阶段开始时调用（阶段名：inject / preprocess / wave / template / vjass / jasshelper / check / pjass）；可在工作线程上调用。
    std::function<void(std::string_view stage)> onStage;
};

struct PipelineStage {
    std::string name;
    std::filesystem::path output;
    std::chrono::milliseconds elapsed{0};
    int exitCode = 0;
    std::string log; // 内置阶段的统计，或外部程序的输出（截断）
};

struct PipelineResult {
    std::string script; // 最终 war3map.j（vJass 翻译输出）；失败时为空
    // 模板阶段未来生成的七类完整 native 成员。只有完整流水线成功后才能交给
    // PrepareScriptSaveBundle 校验；空集合表示没有物编输出，不代表删除七类。
    std::vector<MemberReplacement> objectMembers;
    // 模板 import 写入的普通地图成员；由保存合并为待写成员，不能覆盖脚本、物编或归档内部文件。
    std::vector<MemberReplacement> templateImports;
    // 模板阶段实际求值：内置求值的模板、表达式与跳过块数，以及是否整份交给 Lua 沙盒执行。
    std::size_t templateExpressions = 0;
    bool templateWholeChunk = false;
    std::vector<PipelineStage> stages;
    std::vector<Diagnostic> diagnostics;
    bool syntaxChecked = false; // 语法检查已完成且通过
    std::string checkedBy;      // 完成翻译与检查的工具，如“内置 vJass 翻译 + 内置检查”
    std::shared_ptr<const ScriptBaseInputs> baseInputs; // 本次检查所用基础库，供回执记录来源与摘要
    std::shared_ptr<const ScriptPreprocessSources> frameworkPreprocessSources;
    std::shared_ptr<const Jass::IdentifierRewriteResult> frameworkIdentifierReport;
    std::string frameworkProfileSha256;
    std::vector<std::string> injectedSourceSha256; // 实际注入原字节，不是编译后重读的磁盘身份。

    [[nodiscard]] bool Succeeded() const noexcept;
};

// 由 UI 数据根得到工具链根：UI 根可以是 YDWE 根，也可以是其下的 ui 目录（compiler/ 在上一级）。
// 两处都没有 compiler/ 时原样返回，由调用方报告缺少工具链。
[[nodiscard]] std::filesystem::path ResolveScriptToolRoot(const std::filesystem::path& uiRoot);

// 从生成的脚本（GenerateMapScript 的输出）编译到最终脚本。任何阶段失败即停止并返回诊断。
[[nodiscard]] PipelineResult CompileMapScript(std::string_view generated, const ToolchainOptions& options,
    const TemplateContext& templates);

} // namespace War3Map::Script
