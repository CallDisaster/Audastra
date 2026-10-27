#pragma once

#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 编辑器里执行地图触发器（界面模拟用）。读入 common.j、Blizzard.j 与地图脚本，执行 InitGlobals 与 InitCustomTriggers
// 建立全局量和触发器；之后模拟计时器每次到期，执行订阅它的触发器。全局量在多次执行之间保留。
// 能执行赋值、条件、循环、函数互调、纯计算的原生函数（数学、文字、类型转换、哈希表）和触发器的建立、开关、条件与执行；
// 事件注册（TriggerRegister*）不做任何事，也不计算参数。碰到其他游戏函数（单位、玩家、计时器、界面、等待……）就停下这次执行
// 并报出调用位置，不猜结果。初始化时某条语句停下只跳过这一条（记一条提示），其余照常执行。
namespace War3Map::GameUi {

namespace detail {
struct TriggerSimulatorState;
}

// AudastraUI 函数库里“界面模拟计时器到期”事件的注册函数：(触发器, 界面编号, 计时器编号)。游戏里是空函数。
inline constexpr std::string_view kSimulationTimerEvent = "AudastraUIRegisterSimulationTimer";

struct TriggerRunResult {
    std::uint32_t triggers = 0;            // 执行了动作的触发器个数
    std::vector<std::string> intercepted;  // 按顺序运行到的拦截触发器（脚本变量名）
    std::vector<Diagnostic> diagnostics;
    bool completed = true;  // 中途停下时为 false，已经做过的赋值保留
};

class TriggerSimulator {
public:
    TriggerSimulator(TriggerSimulator&&) noexcept;
    TriggerSimulator& operator=(TriggerSimulator&&) noexcept;
    ~TriggerSimulator();

    // scripts 依次为 common.j、Blizzard.j 与地图脚本（只取 JASS）；只解析顶层声明，不执行。顶层有语法错误时失败；
    // 函数体在第一次执行到时解析，有错时那次执行停下并报出。
    [[nodiscard]] static Result<TriggerSimulator> Prepare(std::vector<MapScriptText> scripts);

    // 运行到这些触发器（脚本变量名，如 gg_trg_Hud_Update）时不执行，按顺序记进结果，交给调用方。
    void Intercept(std::vector<std::string> triggers);
    // 执行 InitGlobals 与 InitCustomTriggers；跳过的语句记为提示，completed 仍为 true。
    TriggerRunResult Initialize();
    // 模拟计时器（界面编号, 计时器编号）到期：依次执行订阅它且开着的触发器（条件都成立才执行动作）。
    TriggerRunResult Fire(std::string_view description, std::string_view timer);
    // 订阅了模拟计时器的（界面编号, 计时器编号），按注册顺序。
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> Subscriptions() const;

    // 全局量的当前值：整数、实数最短写法、true/false、字符串原文；不存在、不是基本类型或还没赋值时为空。
    [[nodiscard]] std::optional<std::string> Global(std::string_view name, std::optional<std::uint32_t> index = std::nullopt) const;
    // 按声明类型写入全局量（起始值用）；类型不是整数、实数、布尔或字符串，或文字不合法时返回 false。
    bool SetGlobal(std::string_view name, std::optional<std::uint32_t> index, std::string_view text);

private:
    explicit TriggerSimulator(std::unique_ptr<detail::TriggerSimulatorState> state);
    std::unique_ptr<detail::TriggerSimulatorState> state_;
};

} // namespace War3Map::GameUi
