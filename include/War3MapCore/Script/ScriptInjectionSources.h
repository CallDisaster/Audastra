#pragma once

#include "War3MapCore/Script/ScriptUserSourcePlan.h"

namespace War3Map::Script {

enum class ScriptInjectionCoverage { InjectionConfigAndLibraries };
enum class ScriptInjectionSection { General, New, Old };
enum class ScriptInjectionLinePolicy { ExistingLfControlTrim, Lua535WindowsTextC, FrameworkText };

struct ScriptInjectionSourceFile {
    ScriptToolchainFileBinding binding;
    std::vector<std::byte> bytes;
    std::uint64_t lastWriteTicks = 0; // 精确 Windows FILETIME；不转浮点数。
};
struct ScriptInjectionTreeNode {
    std::filesystem::path path;
    ScriptToolchainBindingKind kind = ScriptToolchainBindingKind::Missing;
    [[nodiscard]] bool operator==(const ScriptInjectionTreeNode&) const = default;
};
struct ScriptInjectionRegistration {
    std::string key; // FrameworkText为ASCII标识符；历史离线策略保留原键及大小写。
    std::size_t configFile = 0, libraryFile = 0; // files 中的索引，避免逐行复制路径。
    std::uint32_t line = 0;
    ScriptInjectionSection section = ScriptInjectionSection::General;
    bool patternUnsupported = false;
    [[nodiscard]] bool operator==(const ScriptInjectionRegistration&) const = default;
};
struct ScriptInjectionChoice {
    std::string key;
    std::size_t selectedFile = 0; // files 索引；不同 J 的相同 mtime 必须拒绝。
    std::vector<std::size_t> shadowedFiles, registrationIndexes;
    [[nodiscard]] bool operator==(const ScriptInjectionChoice&) const = default;
};
struct ScriptInjectionTextBoundary {
    std::size_t configFile = 0;
    std::uint64_t originalOffset = 0; // 原 cfg 中首个 CTRL-Z；此处起不参与文本解释，原字节仍完整拥有。
    [[nodiscard]] bool operator==(const ScriptInjectionTextBoundary&) const = default;
};
struct ScriptInjectionSources {
    ScriptInjectionCoverage coverage = ScriptInjectionCoverage::InjectionConfigAndLibraries;
    bool executionSupported = false;
    bool version24 = true; // [general]+[new] 的1.24+选择策略，不是 layout 的实际版本宏。
    bool literalLookupSupported = true; // 只描述原键中未使用 Lua 模式元字符。
    bool legacyLineSemanticsProven = false; // 独立Lua文本实测不是嵌入式旧宿主运行证据。
    std::string entryPlanIdentitySha256;
    std::vector<ScriptToolchainFileBinding> orderedRoots;
    std::vector<ScriptInjectionTreeNode> tree;
    std::vector<ScriptInjectionSourceFile> files; // 全部 cfg 与同路径 J 的 owned 原字节。
    std::vector<ScriptInjectionRegistration> registrations; // 包括未选版本段。
    std::vector<ScriptInjectionChoice> choices; // 当前版本有效键，含所选与遮蔽来源。
    std::uint64_t totalFileBytes = 0;
    ScriptInjectionLinePolicy linePolicy = ScriptInjectionLinePolicy::ExistingLfControlTrim;
    std::vector<ScriptInjectionTextBoundary> textBoundaries; // 至多每 cfg 一条；受同一索引预算约束。
};

// 先 VerifyUserPlan；8192节点/4096cfg/4096不同J，cfg<=1MiB、J<=8MiB、总内容<=32MiB。
// 注册<=16384、key<=4096字节、索引估算<=64MiB；不含 include/native 内容冻结，不授予执行。
// 显式Lua535WindowsTextC只模拟独立Lua5.3.5 Windows C文本读取，不转码，不改原文件/J字节。
// FrameworkText解释首UTF8 BOM及CRLF/LF/CR、空格/HT、#与行尾//；原字节完整保留。
// key仅支持ASCII标识符；非法控制字节或未知编码明确拒绝。profile绑定强制FrameworkText。
// 有profile时默认Existing自动选择FrameworkText，显式Lua535WindowsTextC明确拒绝。
[[nodiscard]] Result<ScriptInjectionSources> CaptureScriptInjectionSources(const ScriptUserSourcePlan& entryPlan,
    const ScriptUserSourceRequest& currentRequest, bool version24,
    ScriptInjectionLinePolicy linePolicy = ScriptInjectionLinePolicy::ExistingLfControlTrim);
// 当前完整计划也参与身份比较，旧 snapshot 不能借新计划绕过关键来源变化。
// 重枚举/重捕获，不是延续到编译期间的目录或内容租约。
[[nodiscard]] Status VerifyScriptInjectionSources(const ScriptInjectionSources& sources,
    const ScriptUserSourcePlan& entryPlan, const ScriptUserSourceRequest& currentRequest, bool version24,
    ScriptInjectionLinePolicy linePolicy = ScriptInjectionLinePolicy::ExistingLfControlTrim);

} // namespace War3Map::Script
