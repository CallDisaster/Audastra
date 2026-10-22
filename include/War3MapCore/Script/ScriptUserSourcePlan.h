#pragma once

#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"

namespace War3Map::Trigger { class UiCatalog; }

namespace War3Map::Script {

enum class ScriptUserBindingCoverage { EntrypointsOnly };
enum class ScriptNativeSourceState { NotApplicable, Missing, PresentUnsupported };

struct ScriptUserSourceRequest {
    const ScriptToolchainLayout* layout = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    std::filesystem::path uiRoot; // 已载目录的真实数据根；不回溯或猜测。
    std::filesystem::path legacyUserLibraryRoot; // 宿主明确选择的 Component/R 根，非某个 UI 层。
    std::filesystem::path mapDirectory; // 旧布局只用于 include，不扫描 cfg。
    std::vector<std::filesystem::path> explicitInjectionRoots;
    std::vector<std::filesystem::path> explicitUserIncludes;
    const ScriptToolchainProfile* profile = nullptr; // 显式通用档案；与旧入口参数互斥，不授予执行。
};

struct ScriptUserSourcePlan {
    ScriptUserBindingCoverage bindingCoverage = ScriptUserBindingCoverage::EntrypointsOnly;
    bool executionSupported = false;
    ScriptToolchainLayoutKind layoutKind = ScriptToolchainLayoutKind::Modern;
    std::filesystem::path toolRoot, uiRoot, legacyUserLibraryRoot, mapDirectory;
    std::string layoutIdentitySha256, catalogSha256;
    std::vector<std::string> effectiveLayers;
    std::vector<std::filesystem::path> orderedInjectionRoots, orderedUserIncludes;
    std::filesystem::path nativeRoot;
    ScriptNativeSourceState nativeState = ScriptNativeSourceState::NotApplicable;
    std::vector<ScriptToolchainFileBinding> bindings;
    std::uint64_t totalFileBytes = 0;
    std::string profileIdentitySha256;
    int profileTargetVersionMacro = 0;
    std::string profileDirectiveDialectVersion;
    std::vector<ScriptProfileDirective> profileDirectives;
    std::vector<ScriptProfileFeatureExtension> profileFeatureExtensions;
    std::vector<ScriptProfileUnsupportedPlugin> profileUnsupportedPlugins;
    ScriptProfileEncoding profileMainEncoding = ScriptProfileEncoding::Utf8;
    std::vector<ScriptProfileSourceRoot> profileEncodingRoots;
    std::vector<ScriptProfileSourceEncoding> profileSourceEncodings;
    // 规范路径/type/required正负绑定；不混入orderedUser/System的搜索序列。
    std::vector<ScriptProfileIncludeAccess> profileOrdinaryIncludeAccess;
    std::vector<std::filesystem::path> orderedSystemIncludes, orderedForceIncludes;
};

// 最多128个入口/关键来源，普通文件<=4MiB、合计<=32MiB；拒绝 reparse 与不完整读取。
// 不解析旧配置重算 UI 层；catalog SHA/layers 是已载逻辑身份，不是 UI 磁盘全树租约。
// 不冻结全部 cfg/include 内容，不执行旧 Lua/native，不授予脚本保存能力。
[[nodiscard]] Result<ScriptUserSourcePlan> CaptureScriptUserSourcePlan(const ScriptUserSourceRequest& request);
// currentRequest 必须来自宿主当前已载 catalog/布局，不可用 plan 的旧目录指针冒充当前状态。
// 工具链自身的物理文件仍需独立 VerifyScriptToolchainLayout；这里复核其逻辑布局身份。
[[nodiscard]] Status VerifyScriptUserSourcePlan(const ScriptUserSourcePlan& plan,
    const ScriptUserSourceRequest& currentRequest);

} // namespace War3Map::Script
