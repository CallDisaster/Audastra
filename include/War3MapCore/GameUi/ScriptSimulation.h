#pragma once

#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/Types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

// 界面预览模拟：按作者填的输入值执行地图脚本里一个函数中与界面有关的语句，得到脚本框架在那一刻的样子。
// 只执行赋值、条件、循环（有次数上限）和界面设置调用；不执行脚本的其他效果，也不跟进对其他函数的调用。
// 函数内推不出的值（全局变量、其他函数的返回值、无法判断的条件）都列为输入，不猜。
namespace War3Map::GameUi {

enum class SimulationValueType : std::uint8_t { Number, Text, Boolean };

struct SimulationInput {
    std::string key;    // 表达式原文（去掉空白）；条件输入为 “if:行号”
    std::string label;  // 显示用的原文
    SimulationValueType type = SimulationValueType::Number;
    bool integer = false;
    std::string value;  // 当前值的文本形式：数值用 . 作小数点，布尔为 true/false
    std::uint32_t line = 0;
    bool condition = false;  // 选择条件分支
};

struct SimulationResult {
    std::string function;
    std::vector<ScriptFrame> frames;     // 模拟后的全部脚本框架，与 ScriptFramePlan::frames 同序
    std::vector<SimulationInput> inputs;  // 按首次用到的顺序
    std::vector<std::uint32_t> touched;   // 本次改变过的框架下标
    std::uint32_t appliedCalls = 0;
    std::vector<Diagnostic> diagnostics;
};

// 运行时作用在脚本框架上的函数名，按首次出现的顺序去重。
[[nodiscard]] std::vector<std::string> SimulationFunctions(const ScriptFramePlan& plan);

class ScriptSimulator {
public:
    ScriptSimulator(ScriptSimulator&&) noexcept;
    ScriptSimulator& operator=(ScriptSimulator&&) noexcept;
    ~ScriptSimulator();

    // 解析脚本并定位函数（耗时与脚本大小相当，可在后台执行）；之后 Run 只执行这个函数。
    [[nodiscard]] static Result<ScriptSimulator> Prepare(std::vector<MapScriptText> scripts, ScriptFramePlan plan,
        const std::string& function);

    [[nodiscard]] const std::string& Function() const noexcept;
    // values：输入键 → 文本值；缺少或无法解析的输入用默认值（数值 0、空文字、false）。
    [[nodiscard]] SimulationResult Run(const std::map<std::string, std::string>& values) const;

private:
    struct State;
    explicit ScriptSimulator(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace War3Map::GameUi
