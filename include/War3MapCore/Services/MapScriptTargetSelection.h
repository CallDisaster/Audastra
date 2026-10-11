#pragma once

#include "War3MapCore/Services/LuaTriggerCandidate.h"
#include "War3MapCore/Script/LuaMapPipeline.h"

namespace War3Map::Services {

struct LuaMapTargetRequest {
    LuaCandidateProfile profile;
    Script::MapInitializationPlayerLayout playerLayout = Script::MapInitializationPlayerLayout::Classic12;
    Script::LuaSyntaxChecker checker;
    // 仅明确选择时允许v31内部从JASS切换Lua；不升级W3I版本或猜玩家档位。
    bool changeLanguageToLua = false;
    Script::LuaMapSaveDialect dialect = Script::LuaMapSaveDialect::NativeV31;
};

// 仅可信宿主（CLI选项、GUI选择、MCP进程配置）调用；地图内容与MCP请求不能指定执行器。
// UTF-8 TSV：schema\tforge-lua-map-save-v1；profile\t绝对路径\tSHA；checker同形；
// player_layout\tclassic12|extended24；language\tpreserve|select-lua；runtime\t绝对目录可重复至8项。
// 返回owned档案及已捕获检查器。读取配置不执行检查器，实际执行仍由公共保存流水线监督。
[[nodiscard]] Result<LuaMapTargetRequest> LoadLuaMapTargetConfiguration(
    const std::filesystem::path& configurationFile);

struct MapScriptTargetIdentity {
    std::uint64_t documentRevision = 0;
    std::string sourceSettingsSha256, inputSettingsSha256, selectedSettingsSha256;
    std::string targetRequestSha256;
    std::uint32_t sourceLanguage = 0;
};

class PreparedLuaMapTarget {
public:
    [[nodiscard]] const MapScriptTargetIdentity* Identity() const noexcept;
    [[nodiscard]] const Script::LuaMapBuildProfile* Profile() const noexcept;
    [[nodiscard]] std::span<const std::byte> SelectedSettings() const noexcept;
private:
    struct State;
    std::shared_ptr<const State> state_;
    friend Result<PreparedLuaMapTarget> PrepareMapScriptTargetSelection(std::span<const std::byte>,
        std::span<const std::byte>, const Trigger::TriggerDocument&, const Trigger::UiCatalog&,
        const LuaMapTargetRequest&, std::uint64_t);
};

// 纯内存目标选择；调用方须将选中设置与完整脚本放入同一冻结保存任务。
// 源/最终设置都须是真实v31；传统字段写权限与经典原版保存门禁不因此开放。
[[nodiscard]] Result<PreparedLuaMapTarget> PrepareMapScriptTargetSelection(
    std::span<const std::byte> sourceSettings, std::span<const std::byte> finalSettings,
    const Trigger::TriggerDocument& triggers, const Trigger::UiCatalog& catalog,
    const LuaMapTargetRequest& request, std::uint64_t documentRevision);

} // namespace War3Map::Services
