#pragma once

#include "War3MapCore/Types.h"

#include <span>
#include <string_view>

namespace War3Map::Script {

enum class ScriptProfileLayoutKind { LegacyShareMpq, ModernUi };
enum class ScriptProfileBaseLibraryCategory { Pre124 = 20, Post124 = 24 };
enum class ScriptProfileEncoding { Utf8, Gbk, UniqueUtf8OrGbk };
enum class ScriptProfileSourceRole { Injection, UserInclude, SystemInclude, ForceInclude, Native };
enum class ScriptProfileTextPolicy { NormalTextOnly };
enum class ScriptProfileNamingPolicy { IdentifierOnly };
enum class ScriptProfileReturnbugPolicy { InventoriedOnly = 0, Strict = 1 };
inline constexpr std::string_view kPre124ReturnbugPatternSet = "ydwebase-returnbug-pre124-v1";
enum class ScriptProfileDirective { Def, Ifdef, Else, Endif, Include };

struct ScriptProfileSourceRoot {
    ScriptProfileSourceRole role = ScriptProfileSourceRole::UserInclude;
    // ForceInclude 为文件；其他角色为目录。类型、可读性及原字节由既有来源捕获服务绑定。
    std::filesystem::path path;
    bool required = true;
    ScriptProfileEncoding encoding = ScriptProfileEncoding::Utf8;
    [[nodiscard]] bool operator==(const ScriptProfileSourceRoot&) const = default;
};
struct ScriptProfileSourceEncoding {
    std::filesystem::path path;
    ScriptProfileEncoding encoding = ScriptProfileEncoding::Utf8; // 显式单文件选择，不接受Unique作为override。
    [[nodiscard]] bool operator==(const ScriptProfileSourceEncoding&) const = default;
};
enum class ScriptIncludeAccessKind { Directory, ExactFile };
struct ScriptProfileIncludeAccess {
    std::filesystem::path path;
    ScriptIncludeAccessKind kind = ScriptIncludeAccessKind::Directory;
    ScriptProfileEncoding encoding = ScriptProfileEncoding::UniqueUtf8OrGbk;
    bool required = true;
    [[nodiscard]] bool operator==(const ScriptProfileIncludeAccess&) const = default;
};

// 纯词法资格/编码匹配，不访问FS。ExactFile优先，其余用最具体Directory；调用方先验证档案。
// 同一规范路径的重复/类型/编码冲突由Validate拒绝；更具体条目是显式覆盖，不依声明顺序猜编码。
[[nodiscard]] const ScriptProfileIncludeAccess* MatchScriptIncludeAccess(
    std::span<const ScriptProfileIncludeAccess> declarations, const std::filesystem::path& source);

// 纯逻辑档案声明，不包含可执行命令、DLL入口或任意模板正文。
// 有序来源角色独立；主文目录只供相对include，不会自动成为注入根。
struct ScriptToolchainProfile {
    std::uint32_t schemaVersion = 1;
    std::uint64_t profileRevision = 1;
    std::string profileId;
    std::string rulesetVersion = "ydwe-legacy-framework-v1";
    ScriptProfileLayoutKind layoutKind = ScriptProfileLayoutKind::ModernUi;
    std::filesystem::path toolRoot;
    std::filesystem::path uiRoot;
    std::filesystem::path mainSourceDirectory;
    ScriptProfileEncoding mainEncoding = ScriptProfileEncoding::Utf8;
    std::vector<ScriptProfileSourceRoot> sourceRoots;
    std::vector<ScriptProfileSourceEncoding> sourceEncodings;
    // 普通include的读取资格，独立于搜索顺序、注入/native/工具权限，不隐式加入来源根。
    std::vector<ScriptProfileIncludeAccess> ordinaryIncludeAccess;
    // 实际目标宏与基础库/类型类别独立；Pre124也不自动取得returnbug资格。
    int targetVersionMacro = 124;
    ScriptProfileBaseLibraryCategory baseLibraryCategory = ScriptProfileBaseLibraryCategory::Post124;
    ScriptProfileTextPolicy textPolicy = ScriptProfileTextPolicy::NormalTextOnly;
    ScriptProfileNamingPolicy namingPolicy = ScriptProfileNamingPolicy::IdentifierOnly;
    std::string templateApiVersion;
    std::vector<std::string> templateApis;
    ScriptProfileReturnbugPolicy returnbugPolicy = ScriptProfileReturnbugPolicy::Strict;
    // Strict必须为空；InventoriedOnly仅可声明Pre124的YDWEBase_returnbug模式集。
    // 1.27a MemHack的AI及配对载荷是普通不透明导入成员，不属于主脚本类型例外。
    std::string returnbugPatternSetVersion;
    std::string directiveDialectVersion = "ydwe-dollar-v1";
    std::vector<ScriptProfileDirective> directives;
    [[nodiscard]] bool operator==(const ScriptToolchainProfile&) const = default;
};

// 只验证有界声明并返回完整逻辑身份SHA-256；不读取、枚举或执行外部来源。
// 不证明旧方言/模板/returnbug已实现，也不替代布局及来源Capture/Verify。
[[nodiscard]] Result<std::string> ValidateScriptToolchainProfile(
    const ScriptToolchainProfile& profile);

} // namespace War3Map::Script
