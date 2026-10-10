#pragma once

#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"
#include "War3MapCore/Script/ScriptToolchainProfile.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

// 调用方只负责从工程或源归档取得已经解析的语义输入。层序必须是本次解析
// UiCatalog 使用的有效层序，不得以数据根中的全部目录代替。
struct ScriptSaveInputSources {
    std::unique_ptr<Trigger::TriggerDocument> triggers;
    std::unique_ptr<Trigger::UiCatalog> catalog;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kMapSaveObjectTypeCount> editableInputs;
    std::vector<ObjectData::ObjectRecord> units;
    std::vector<ObjectData::ObjectRecord> items;
    std::vector<ObjectData::ObjectRecord> abilities;
    std::vector<std::string> layers;
    std::optional<std::vector<std::string>> expectedEffectiveLayers;
    std::filesystem::path toolRoot;
    std::filesystem::path uiRoot; // 已解析 catalog 的真实目录；不是编译工具的替代根
    std::filesystem::path gameRoot; // 空路径明确表示本次不提供游戏数据。
    // 仅声明既有框架来源的单文件编码；不改变工具/UI根、版本、层序或读取资格。
    std::vector<Script::ScriptProfileSourceEncoding> sourceEncodings;
    // 普通include的显式读取资格，不增加搜索目录或注入/native来源。
    std::vector<Script::ScriptProfileIncludeAccess> includeAccess;
    // 空值沿用经典JASS。Lua必须显式提交完整档案和检查器，不依靠扩展名猜测。
    std::optional<LuaMapTargetRequest> luaTarget;
    std::string scriptMember;
    std::uint64_t documentRevision = 0;
    bool allowMissingToolchain = false; // 仅供可安全沿用源脚本的事务。
};

// UI、CLI、MCP共用相同的逐文件声明语法及有界路径规则；不读取声明指向的来源。
// 只接受utf8/gbk，最多1024文件、单路径4096字符、总路径256Ki字符。
[[nodiscard]] Result<Script::ScriptProfileSourceEncoding> ParseScriptSourceEncodingDeclaration(
    const std::filesystem::path& source, std::string_view encoding);
[[nodiscard]] Result<std::vector<Script::ScriptProfileSourceEncoding>> NormalizeScriptSourceEncodingDeclarations(
    std::vector<Script::ScriptProfileSourceEncoding> declarations);

// 只声明，不读取FS。directory/file与utf8/gbk/auto使用同一纯档案验证规则。
// 最多128项、单路径4096字符；总路径及冲突沿用档案的更紧预算。
[[nodiscard]] Result<Script::ScriptProfileIncludeAccess> ParseScriptIncludeAccessDeclaration(
    const std::filesystem::path& source, std::string_view kind, std::string_view encoding, bool required);
[[nodiscard]] Result<std::vector<Script::ScriptProfileIncludeAccess>> NormalizeScriptIncludeAccessDeclarations(
    std::vector<Script::ScriptProfileIncludeAccess> declarations);

enum class ScriptSavePreparationState {
    Ready,
    Skipped,
};

enum class ScriptRebuildCapabilityState { Unknown, Missing, Unsupported, Ready };

struct ScriptRebuildCapability {
    ScriptRebuildCapabilityState state = ScriptRebuildCapabilityState::Unknown;
    std::optional<Script::ScriptToolchainLayout> layout;
    std::vector<Diagnostic> diagnostics;
};

// 只读查询布局与独立 UI 的重建资格；不创建工作目录、不执行脚本、不取得保存许可。
// Ready 仅表示工具链入口可用，不证明某张地图的脚本能够编译。UI 可缓存提示，保存必须重新查询。
[[nodiscard]] ScriptRebuildCapability QueryScriptRebuildCapability(
    const std::filesystem::path& toolRoot, const std::filesystem::path& uiRoot = {});

// 唯一的无 Qt 脚本保存输入准备路径。成功时冻结输入并分配独占工作目录；
// 跳过时只设置 task.scriptSkipped。失败不向 task.script 发布部分输入。
[[nodiscard]] Result<ScriptSavePreparationState> PrepareScriptSaveInputs(
    MapSaveTask& task, ScriptSaveInputSources&& sources);

// 仅在宿主已接受回读候选并切换/确认保存基线后调用。失败与未提交目录永久保留诊断；
// 完整成功目录只留最近若干个。清理失败只追加警告，不反转已完成的地图保存。
void FinalizeScriptWorkspace(MapSaveTask& task) noexcept;

} // namespace War3Map::Services
