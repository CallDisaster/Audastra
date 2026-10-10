#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Script {

enum class ScriptToolchainLayoutKind { Modern, LegacyShareMpq, LegacyXueyue = LegacyShareMpq };
enum class ScriptToolchainBindingKind { Missing, RegularFile, Directory, Other };
// 框架入口可用不等于全部旧扩展可执行；必须携带逐项捕获的输入进入实际阶段。
enum class ScriptToolchainExecutionPolicy { Unsupported, Modern, FrameworkInputsRequired };

struct ToolchainLayoutRequest {
    std::filesystem::path toolRoot;
    int warcraftVersion = 24; // 20 = return bug；24 = 1.24+，不是实际版本宏。
    std::optional<int> actualVersionMacro;
};

struct ScriptToolchainFileBinding {
    std::filesystem::path path;
    ScriptToolchainBindingKind kind = ScriptToolchainBindingKind::Missing;
    std::string sha256; // 普通文件才有摘要，缺失文件同样绑定以拒绝后来新增。
    std::uint64_t bytes = 0;
    std::uint64_t maxBytes = 0;
};

struct ScriptToolchainLayout {
    ScriptToolchainLayoutKind kind = ScriptToolchainLayoutKind::Modern;
    std::filesystem::path root;
    std::vector<std::filesystem::path> systemIncludes;
    std::vector<std::filesystem::path> forceIncludes;
    std::filesystem::path commonScript;
    std::filesystem::path blizzardScript;
    // 缺失、空或非普通文件的外部工具保持空路径；不影响现代内置阶段可用。
    std::filesystem::path waveExecutable;
    std::filesystem::path jassHelperExecutable;
    std::filesystem::path pjassExecutable;
    int versionMacro = 124;
    bool allowReturnBug = false;
    bool semanticsSupported = false; // 旧布局可识别，但未授权执行旧 rewrite/模板链。
    ScriptToolchainExecutionPolicy executionPolicy = ScriptToolchainExecutionPolicy::Unsupported;
    std::vector<ScriptToolchainFileBinding> bindingFiles;
};

// 只读、有界解析关键路径与文件；旧布局只有FrameworkInputsRequired资格，不授予整链语义。
[[nodiscard]] Result<ScriptToolchainLayout> ResolveScriptToolchainLayout(const ToolchainLayoutRequest& request);
// 只验证解析时绑定的关键文件/路径，不代表整棵 include 树或外部执行仍持有租约。
[[nodiscard]] Status VerifyScriptToolchainLayout(const ScriptToolchainLayout& layout);
// 独立 UI 可以没有编译器布局；已识别但损坏/冲突的布局必须保留解析错误，旧语义仍拒绝。
[[nodiscard]] Status CheckScriptUiToolchainSemantics(const ToolchainLayoutRequest& request);

} // namespace War3Map::Script
