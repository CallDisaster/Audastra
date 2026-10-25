#pragma once

#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Agent 进游戏测试（JASS 地图）：一段紧凑 T 动作作为“测试场景”临时触发器加入测试副本的文档副本，
// 游戏开始 startSeconds 秒后运行一次。场景里用 Preload(文本) 记录一行，以“通过”“失败”开头的行计为断言；
// 场景结束时记下读取的全局变量和结束标记，用游戏的预载文件写到游戏目录下的结果文件，宿主读回。
// 只接受 T 写法；外层的记录开始、读取与写出由本服务生成。副本只用于本次测试，不进入作者文档、撤销栈或地图。
namespace War3Map::Services {

struct MapTestScenarioRequest {
    std::string actions;             // 紧凑 T 动作，规则同 replace_trigger_text；不能分段
    std::vector<std::string> reads;  // 结束时记录的全局变量：名字或 名字[下标]，可带 udg_；最多 32 个
    double startSeconds = 1.0;       // 游戏开始后多少秒运行，0..600
    std::string resultFile;          // 结果文件，相对游戏目录，例如 Maps\Audastra\scenario-xxxx.txt
};

struct MapTestScenarioPlan {
    StableId triggerId = 0;
    std::vector<std::pair<std::string, std::string>> reads;  // 请求原文、JASS 写法
    std::string resultFile;
};

struct MapTestScenarioReport {
    bool started = false;                                    // 读到开始标记：游戏已载入，场景开始执行
    bool finished = false;                                   // 读到结束标记：场景动作全部执行完
    std::vector<std::string> lines;                          // 场景记录的行，按记录顺序
    std::vector<std::string> files;                          // 记录期间游戏写入的已载入资源路径（按路径形状识别，同形的记录行也归这里）
    std::vector<std::pair<std::string, std::string>> reads;  // 名字、值
    std::size_t passed = 0, failed = 0;                      // 以“通过”“失败”开头的行数
};

inline constexpr std::string_view kMapTestScenarioCategory = "Audastra Agent Scenario";
inline constexpr std::string_view kMapTestScenarioTrigger = "AudastraAgentScenario";

// 错误行列是 actions 文本自身的行列；变量不存在、数组下标不符、结果文件路径不安全时拒绝。
[[nodiscard]] Result<MapTestScenarioPlan> AddMapTestScenarioTrigger(Trigger::TriggerDocument& copy,
    const Trigger::UiCatalog& catalog, const MapTestScenarioRequest& request,
    const Trigger::MapObjectReferenceSnapshot* mapObjects = nullptr);

// text 是游戏写出的结果文件全文。不是预载行的内容不计入；资源路径形状的行放进 files，不计入 lines 和断言。
[[nodiscard]] Result<MapTestScenarioReport> ParseMapTestScenarioResult(std::string_view text, const MapTestScenarioPlan& plan);

} // namespace War3Map::Services
