#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptPipeline.h"

#include <filesystem>

#include <vector>

namespace War3Map { class MapArchiveSession; }
namespace War3Map::Game { class GameDataSource; }

namespace War3Map::Script {

// 由原版物体目录 + 地图自定义物体回答整图脚本生成所需的物编问题。
struct ObjectQuerySources {
    const ObjectData::StandardObjectCatalog* standard = nullptr; // 调用方保证在生成期间有效
    std::vector<ObjectData::ObjectRecord> units;
    std::vector<ObjectData::ObjectRecord> items;
    std::vector<ObjectData::ObjectRecord> abilities;
};

[[nodiscard]] ObjectQueries BuildObjectQueries(ObjectQuerySources sources);

// 声音标签与时长（SoundQueries.cpp）：标签取游戏 UI/SoundInfo 表，时长读取音频文件头（地图导入文件优先）。
// game / archive 可为空；两者须在生成期间有效。
void AttachSoundQueries(ObjectQueries& queries, const Game::GameDataSource* game, const MapArchiveSession* archive);

// 模板求值上下文：abilityOrder 按技能的基础技能代码查命令 ID。
// 命令 ID 表取自工具目录 compiler/script/computed 下的 order_id / order_list 数据文件（只按文本解析，不执行）；
// 通道类技能（ANcl）取其自定义命令串。缺表或查不到时返回空，由求值器按 0 处理并警告。
[[nodiscard]] TemplateContext BuildTemplateContext(const std::filesystem::path& toolRoot,
    const ObjectData::StandardObjectCatalog* standard, std::vector<ObjectData::ObjectRecord> abilities);

} // namespace War3Map::Script
