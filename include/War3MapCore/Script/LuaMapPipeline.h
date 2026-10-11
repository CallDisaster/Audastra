#pragma once

#include "War3MapCore/Script/LuaMapScript.h"
#include "War3MapCore/Script/ScriptPipeline.h"

namespace War3Map::Script {

struct LuaSyntaxCheckerIdentity {
    std::filesystem::path executable;
    std::string sha256;
    std::uint64_t bytes = 0;
    std::vector<std::filesystem::path> runtimeDirectories;
};

// 捕获及执行前后核验检查器文件；不声称锁住整套外部DLL或整个执行期。
class LuaSyntaxChecker {
public:
    [[nodiscard]] const LuaSyntaxCheckerIdentity* Identity() const noexcept;
private:
    struct State;
    std::shared_ptr<const State> state_;
    friend Result<LuaSyntaxChecker> CaptureLuaSyntaxChecker(const std::filesystem::path&,
        std::string_view, std::span<const std::filesystem::path>);
};

[[nodiscard]] Result<LuaSyntaxChecker> CaptureLuaSyntaxChecker(const std::filesystem::path& executable,
    std::string_view expectedSha256, std::span<const std::filesystem::path> runtimeDirectories = {});

enum class LuaMapSaveDialect : std::uint8_t { NativeV31, Classic127YDWE };

struct LuaMapBuildProfile {
    LuaTargetProfile target;
    std::vector<LuaCallbackBinding> callbacks;
    MapInitializationOptions initialization;
    LuaSyntaxChecker checker;
    std::string requestSha256;
    LuaMapSaveDialect dialect = LuaMapSaveDialect::NativeV31;
};

// 与JASS流水线结果分型；Lua语法通过不伪装成JASS类型检查或游戏运行通过。
struct LuaMapPipelineResult {
    std::string script, scriptSha256, targetSha256, sourceMapSha256;
    std::string checkerSha256, checkedBy;
    std::vector<PipelineStage> stages;
    std::vector<Diagnostic> diagnostics;
    bool syntaxChecked = false;
    [[nodiscard]] bool Succeeded() const noexcept;
};

// 只load文本并检查语法，不执行地图chunk、config/main或任何native。
// 输入必须来自完整typed地图writer；失败不返回可保存的脚本文本。
[[nodiscard]] LuaMapPipelineResult CheckLuaMapScript(const LuaMapScriptResult& generated,
    const LuaSyntaxChecker& checker, const std::filesystem::path& workDir,
    std::chrono::seconds timeout = std::chrono::seconds{60},
    const std::function<void(std::string_view)>& onStage = {});

} // namespace War3Map::Script
