#pragma once

#include "War3MapCore/GameUi/TriggerSimulation.h"
#include "War3MapCore/Services/CodeCheckProject.h"

#include <QElapsedTimer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace War3Map::Modules {

// 界面模拟的运行状态（GameUiTriggerSimulation.cpp）。
struct GameUiTriggerSimulation {
    QString description;  // 正在模拟的界面编号
    std::optional<GameUi::TriggerSimulator> simulator;
    std::uint64_t generation = 0;  // 准备任务的代号：重置后旧任务的结果丢弃
    bool preparing = false;
    bool running = false;
    bool stepPending = false;  // 准备好后推进一次
    double time = 0.0;
    std::uint32_t fired = 0;
    std::map<std::string, double, std::less<>> due;  // 计时器编号 → 下次到期时刻
    std::set<std::string, std::less<>> finished;     // 已到期的一次性计时器
    // 拦截的界面触发器（脚本变量名）→ 运行到时切换的状态；空为“创建”“更新”，只读回数值。
    std::map<std::string, std::optional<std::string>, std::less<>> intercepted;
    std::map<std::string, std::string, std::less<>> names;  // 触发器脚本变量名 → 触发器编辑器里的名字
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<Services::CodeCheckProjectCache> cache = std::make_shared<Services::CodeCheckProjectCache>();
    QTimer clock;
    QElapsedTimer wall;
};

// 后台准备的结果：解析、拦截与初始化之后的模拟器。
struct GameUiTriggerSimulationJob {
    std::optional<GameUi::TriggerSimulator> simulator;
    std::vector<Diagnostic> diagnostics;
    GameUi::TriggerRunResult init;
};

// 解析脚本、设好拦截并初始化（可在工作线程执行）。
void RunTriggerSimulationJob(GameUiTriggerSimulationJob& job, std::vector<GameUi::MapScriptText> scripts,
    std::vector<std::string> intercepted);

} // namespace War3Map::Modules
