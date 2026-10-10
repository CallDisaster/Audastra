#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/LuaMapPipeline.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Script/SlkMapSourceSnapshot.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace War3Map { class MapArchiveSession; }
namespace War3Map::Game { class GameDataSource; }

// 整图脚本构建服务：从地图归档读取放置 / 区域 / 镜头 / 声音 / 设置等成员，结合（可能已编辑的）
// 触发器与物编生成脚本，再交给外部工具链做预处理与语法检查。地图归档只读。
namespace War3Map::Script {

struct MapScriptFinalMembers {
    // 宿主冻结的本次保存输入；BuildMapScript 返回前不得修改或销毁。
    std::span<const MemberReplacement> replacements;
    std::span<const std::string> removals;
};

struct MapScriptBuildRequest {
    const MapArchiveSession* archive = nullptr;
    // 尚未发布到归档的公共地图成员。生成脚本时优先读取这些字节，
    // 避免地图内容已改变但 config/main 仍来自旧源归档。
    std::vector<MemberReplacement> editedMapMembers;
    std::vector<std::string> removedMapMembers;
    // 原生保存 job 使用完整最终成员视图，包含七类物编与资源；避免复制大型导入文件。
    // 与上面的自持有 overlay 互斥。只读视图不是磁盘候选或 SLK 语义证明。
    std::optional<MapScriptFinalMembers> finalSaveMembers;
    std::uint64_t documentRevision = 0; // 与冻结成员同次捕获；CLI 新快照可为 0
    const Trigger::TriggerDocument* triggers = nullptr;
    const Trigger::UiCatalog* catalog = nullptr;
    const ObjectData::StandardObjectCatalog* standard = nullptr; // 可空：退化为默认规则并警告
    const Game::GameDataSource* game = nullptr;                  // 可空：不写声音标签，游戏音频无时长
    // 地图自定义物体（使用编辑后的版本）。
    std::vector<ObjectData::ObjectRecord> units;
    std::vector<ObjectData::ObjectRecord> items;
    std::vector<ObjectData::ObjectRecord> abilities;
    // 启用的函数库层（ui 下的相对目录，按启用顺序），用于注入与 Wave 包含路径。
    std::vector<std::string> layers;
    ToolchainOptions toolchain; // toolRoot / workDir 必填；jassDirs 为空时由 layers 推出
    bool compile = true;        // false 时只生成，不运行外部工具链
    // 同次owned保存快照中的明确Lua档案；空值保持经典JASS路径。
    const LuaMapBuildProfile* luaTarget = nullptr;
    // 保存服务已验证的明确语言选择，只在luaTarget存在时借用；其他成员仍取最终视图。
    std::span<const std::byte> selectedLuaSettings;
};

struct MapScriptBuildResult {
    MapScriptResult generated;
    std::optional<PipelineResult> pipeline;
    std::optional<LuaMapScriptResult> luaGenerated;
    std::optional<LuaMapPipelineResult> luaPipeline;
    std::vector<Diagnostic> diagnostics; // 读取地图成员等准备阶段的诊断
    // 实际 wholechunk 使用的原始/最终来源；也供 native 三方祖先核验使用。
    // 拒绝路径可保留诊断快照；有此值不授予跳过模板或保存权限。
    std::optional<SlkMapSourceSnapshot> slkSources;

    // 生成成功，且（要求编译时）语法检查通过。
    [[nodiscard]] bool Succeeded() const noexcept;
    // 可写回地图的最终脚本；未编译或失败时为空。
    [[nodiscard]] const std::string* FinalScript() const noexcept;
};

[[nodiscard]] MapScriptBuildResult BuildMapScript(const MapScriptBuildRequest& request);

// 根据已解析W3I语言选择成员；Lua固定war3map.lua，经典JASS保持原位置。
[[nodiscard]] std::string MapScriptMemberName(const MapArchiveSession& archive);

} // namespace War3Map::Script
