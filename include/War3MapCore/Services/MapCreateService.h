#pragma once

#include "War3MapCore/Services/MapSaveService.h"

namespace War3Map::Services {

// 内建模板只编码编辑器自己生成的数据，不携带 Blizzard/YDWE 资源或地图脚本。
// 当前唯一配置有原版新建样本；新尺寸/环境需各自验证，不能外推地形保存规则。
inline constexpr std::string_view kClassicFlatMapProfile = "classic-lordaeron-64-flat-v1";

struct MapCreateOptions {
    std::string profile = std::string(kClassicFlatMapProfile);
    std::string name = "New map";
    std::string author;
    // 可选：随新地图写入的原创文件（例如 Lua 教程脚本）；名字为地图内相对路径，不能覆盖地图自身成员。
    std::vector<MapArchiveMember> extraMembers;
    // 可选：新建一个“地图初始化”触发器，每行一个“自定义代码”动作（例如手写 Lua 的 exec-lua 入口）。
    std::string initTriggerName;
    std::vector<std::string> initCustomScript;
};

struct MapCreateRequest {
    MapCreateOptions options;
    std::filesystem::path target;
    // 必须尚不存在。保留新建种子和失败诊断，不覆盖已有文件或调用者工作目录。
    std::filesystem::path workspace;
    std::filesystem::path toolRoot;
    std::filesystem::path gameRoot;
    // 编译工具与函数库数据可独立提供；为空时沿用工具根。
    std::filesystem::path uiRoot;
};

struct MapCreateReceipt {
    std::filesystem::path path;
    std::string sha256;
    std::string profile;
    std::size_t memberCount = 0;
    bool scriptRebuilt = false;
};

// UI/CLI/Agent 的共用输入检查。元数据为 UTF-8，无 NUL/控制字符且有界。
[[nodiscard]] Status ValidateMapCreateOptions(const MapCreateOptions& options);

// 一个人类玩家、中心开始点、平地无水，没有单位/地物/用户触发器。
// 通过现有 PrepareScriptSaveInputs/MapSaveService 编译并回读后才发布目标；
// 结果不表示原版真保存或游戏验收。现有地图的 W3E/WPM 门禁不受影响。
[[nodiscard]] Result<MapCreateReceipt> CreateMapCandidate(
    const MapCreateRequest& request, const MapSaveService::Progress& progress = {});

} // namespace War3Map::Services
