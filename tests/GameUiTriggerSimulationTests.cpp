#include "War3MapCore/GameUi/TriggerSimulation.h"
#include "GameUiTestSupport.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::GameUi::Testing;

void Check(bool condition, const std::string& reason) { Require(condition, reason.c_str()); }

std::string Messages(const std::vector<Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& diagnostic : diagnostics) text += diagnostic.code + " " + diagnostic.message + "\n";
    return text;
}

// 原创的极简 common.j / Blizzard.j：只声明测试用到的类型与函数。
const char* const kCommon = R"(
type agent extends handle
type event extends agent
type player extends agent
type unit extends agent
type trigger extends agent
type triggeraction extends handle
type triggercondition extends agent
type boolexpr extends agent
type conditionfunc extends boolexpr
type hashtable extends agent
type playerstate extends handle

constant native ConvertPlayerState takes integer i returns playerstate
native CreateTrigger takes nothing returns trigger
native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction
native TriggerAddCondition takes trigger whichTrigger, boolexpr condition returns triggercondition
native Condition takes code func returns conditionfunc
native TriggerEvaluate takes trigger whichTrigger returns boolean
native TriggerExecute takes trigger whichTrigger returns nothing
native Player takes integer number returns player
native TriggerRegisterPlayerChatEvent takes trigger whichTrigger, player whichPlayer, string chatMessageToDetect, boolean exactMatchOnly returns event
native I2S takes integer i returns string
native InitHashtable takes nothing returns hashtable
native SaveInteger takes hashtable table, integer parentKey, integer childKey, integer value returns nothing
native LoadInteger takes hashtable table, integer parentKey, integer childKey returns integer
native StringHash takes string s returns integer
native KillUnit takes unit whichUnit returns nothing
native GetTriggerUnit takes nothing returns unit
native DisplayTextToPlayer takes player toPlayer, real x, real y, string message returns nothing

globals
    constant playerstate PLAYER_STATE_GOLD = ConvertPlayerState(1)
endglobals
)";

const char* const kBlizzard = R"(
globals
    integer bj_forLoopAIndex = 0
endglobals

function ConditionalTriggerExecute takes trigger trig returns nothing
    if TriggerEvaluate(trig) then
        call TriggerExecute(trig)
    endif
endfunction

function KillTriggerUnitBJ takes nothing returns nothing
    call KillUnit(GetTriggerUnit())
endfunction

function ModuloInteger takes integer dividend, integer divisor returns integer
    local integer modulus = dividend - (dividend / divisor) * divisor
    if (modulus < 0) then
        set modulus = modulus + divisor
    endif
    return modulus
endfunction
)";

// 作者自己写的模拟触发器：每跳扣血、记次数、写哈希表，再运行界面的“更新”触发器。
const char* const kMap = R"(
globals
    real udg_AUI_Hud_hp = 0
    integer udg_ticks = 0
    integer udg_saved = 0
    integer udg_big = 0
    integer udg_mul = 0
    boolean udg_same = false
    string array udg_names
    hashtable udg_table = InitHashtable()
    integer udg_check = 'A000'
    unit gg_unit_H000_0001 = null
    trigger gg_trg_Sim = null
    trigger gg_trg_Hud_Update = null
    trigger gg_trg_Bad = null
    trigger gg_trg_Spin = null
endglobals

function AudastraUIRegisterSimulationTimer takes trigger t, string description, string timer returns nothing
endfunction

function InitGlobals takes nothing returns nothing
    local integer i = 0
    loop
        exitwhen (i > 2)
        set udg_names[i] = ""
        set i = i + 1
    endloop
    call KillUnit(gg_unit_H000_0001)
    set udg_AUI_Hud_hp = 1.00
    set udg_big = 2147483647 + 1
    set udg_mul = 65536 * 65537
    set udg_same = (PLAYER_STATE_GOLD == ConvertPlayerState(1))
endfunction

function Trig_Sim_Conditions takes nothing returns boolean
    return udg_ticks < 100
endfunction

function Trig_Sim_Actions takes nothing returns nothing
    set udg_ticks = udg_ticks + 1
    set udg_AUI_Hud_hp = udg_AUI_Hud_hp - 0.25
    if (udg_AUI_Hud_hp < 0) then
        set udg_AUI_Hud_hp = 1
    endif
    set udg_names[ModuloInteger(udg_ticks, 3)] = "tick " + I2S(udg_ticks)
    call SaveInteger(udg_table, 0, StringHash("n"), LoadInteger(udg_table, 0, StringHash("n")) + 2)
    set udg_saved = LoadInteger(udg_table, 0, StringHash("N"))
    call ConditionalTriggerExecute(gg_trg_Hud_Update)
endfunction

function InitTrig_Sim takes nothing returns nothing
    set gg_trg_Sim = CreateTrigger()
    call AudastraUIRegisterSimulationTimer(gg_trg_Sim, "Hud", "tick")
    call TriggerRegisterPlayerChatEvent(gg_trg_Sim, Player(0), "-hp", true)
    call TriggerAddCondition(gg_trg_Sim, Condition(function Trig_Sim_Conditions))
    call TriggerAddAction(gg_trg_Sim, function Trig_Sim_Actions)
endfunction

function Trig_Hud_Update_Actions takes nothing returns nothing
    call DisplayTextToPlayer(Player(0), 0, 0, "update")
endfunction

function InitTrig_Hud_Update takes nothing returns nothing
    set gg_trg_Hud_Update = CreateTrigger()
    call TriggerAddAction(gg_trg_Hud_Update, function Trig_Hud_Update_Actions)
endfunction

function Trig_Bad_Actions takes nothing returns nothing
    call KillTriggerUnitBJ()
endfunction

function InitTrig_Bad takes nothing returns nothing
    set gg_trg_Bad = CreateTrigger()
    call AudastraUIRegisterSimulationTimer(gg_trg_Bad, "Hud", "slow")
    call TriggerAddAction(gg_trg_Bad, function Trig_Bad_Actions)
endfunction

function Trig_Spin_Actions takes nothing returns nothing
    loop
        set udg_mul = udg_mul + 0
    endloop
endfunction

function InitTrig_Spin takes nothing returns nothing
    set gg_trg_Spin = CreateTrigger()
    call AudastraUIRegisterSimulationTimer(gg_trg_Spin, "Hud", "spin")
    call TriggerAddAction(gg_trg_Spin, function Trig_Spin_Actions)
endfunction

function InitCustomTriggers takes nothing returns nothing
    call InitTrig_Sim()
    call InitTrig_Hud_Update()
    call InitTrig_Bad()
    call InitTrig_Spin()
endfunction
)";

// 每跳减 0.1：游戏里实数为单精度，第 7 跳就低于 0.3，第 10 跳到 0 以下（双精度要晚一跳）。
const char* const kDrain = R"(
globals
    real udg_hp = 1
    real udg_third = 0
    boolean udg_low = false
    boolean udg_empty = false
    trigger gg_trg_Drain = null
endglobals

function AudastraUIRegisterSimulationTimer takes trigger t, string description, string timer returns nothing
endfunction

function Trig_Drain_Actions takes nothing returns nothing
    set udg_hp = udg_hp - 0.1
    set udg_low = udg_hp < 0.3
    set udg_empty = udg_hp <= 0
    set udg_third = 1.0 / 3.0
endfunction

function InitTrig_Drain takes nothing returns nothing
    set gg_trg_Drain = CreateTrigger()
    call AudastraUIRegisterSimulationTimer(gg_trg_Drain, "Hp", "tick")
    call TriggerAddAction(gg_trg_Drain, function Trig_Drain_Actions)
endfunction

function InitCustomTriggers takes nothing returns nothing
    call InitTrig_Drain()
endfunction
)";

std::vector<MapScriptText> Scripts(std::string map) {
    return {{"common.j", ScriptLanguage::Jass, kCommon}, {"Blizzard.j", ScriptLanguage::Jass, kBlizzard},
        {"war3map.j", ScriptLanguage::Jass, std::move(map)}};
}

std::string Value(const TriggerSimulator& simulator, std::string_view name, std::optional<std::uint32_t> index = std::nullopt) {
    return simulator.Global(name, index).value_or("<none>");
}

// 初始化：全局量与触发器建立，游戏函数所在的语句跳过并记提示，其余照常；事件注册不计算参数。
void TestInitialize() {
    auto prepared = TriggerSimulator::Prepare(Scripts(kMap));
    Check(prepared.Succeeded(), "prepare: " + Messages(prepared.diagnostics));
    TriggerSimulator& simulator = *prepared.value;
    simulator.Intercept({"gg_trg_Hud_Update"});
    const auto init = simulator.Initialize();
    Check(init.completed, "initialize completes");
    Check(HasCode(init.diagnostics, "AUI_SIM_INIT_SKIPPED") && init.diagnostics.size() == 1 &&
              init.diagnostics[0].severity == DiagnosticSeverity::Info && init.diagnostics[0].message.find("KillUnit") != std::string::npos &&
              init.diagnostics[0].message.find("war3map.j:") != std::string::npos,
        "only the KillUnit statement is skipped: " + Messages(init.diagnostics));
    Check(Value(simulator, "udg_AUI_Hud_hp") == "1", "statements after the skipped one run");
    Check(Value(simulator, "udg_names", 2).empty() && Value(simulator, "udg_names", 7) == "<none>",
        "array elements set by the loop; unset string elements read as null");
    Check(Value(simulator, "udg_check") == std::to_string(0x41303030), "rawcode literal");
    Check(Value(simulator, "udg_big") == "-2147483648" && Value(simulator, "udg_mul") == "65536", "32-bit integer wrap");
    Check(Value(simulator, "udg_same") == "true", "Convert* constants compare equal");
    const auto subscriptions = simulator.Subscriptions();
    Check(subscriptions.size() == 3 && subscriptions[0] == std::pair<std::string, std::string>{"Hud", "tick"} &&
              subscriptions[1].second == "slow" && subscriptions[2].second == "spin",
        "simulation timer subscriptions in registration order");
}

// 到期：执行订阅的触发器（条件成立才执行），全局量与哈希表在多次之间保留，“更新”触发器被拦截交给调用方。
void TestFire() {
    auto prepared = TriggerSimulator::Prepare(Scripts(kMap));
    TriggerSimulator& simulator = *prepared.value;
    simulator.Intercept({"gg_trg_Hud_Update"});
    (void)simulator.Initialize();

    const auto first = simulator.Fire("Hud", "tick");
    Check(first.completed && first.diagnostics.empty(), "tick runs: " + Messages(first.diagnostics));
    Check(first.triggers == 1 && first.intercepted == std::vector<std::string>{"gg_trg_Hud_Update"}, "update trigger intercepted");
    Check(Value(simulator, "udg_AUI_Hud_hp") == "0.75" && Value(simulator, "udg_ticks") == "1" &&
              Value(simulator, "udg_names", 1) == "tick 1",
        "first tick values");
    for (int i = 0; i < 4; ++i) Check(simulator.Fire("Hud", "tick").completed, "later ticks run");
    Check(Value(simulator, "udg_AUI_Hud_hp") == "1" && Value(simulator, "udg_ticks") == "5" && Value(simulator, "udg_names", 2) == "tick 5",
        "values persist across ticks: hp " + Value(simulator, "udg_AUI_Hud_hp"));
    Check(Value(simulator, "udg_saved") == "10", "hashtable persists and StringHash ignores case");
    Check(simulator.Fire("Hud", "other").triggers == 0, "unsubscribed timer runs nothing");

    // 起始值写入；条件不成立时不执行动作。
    Check(simulator.SetGlobal("udg_AUI_Hud_hp", std::nullopt, "0.5") && Value(simulator, "udg_AUI_Hud_hp") == "0.5", "set real");
    Check(simulator.SetGlobal("udg_names", 1, "a") && Value(simulator, "udg_names", 1) == "a", "set array element");
    Check(!simulator.SetGlobal("udg_ticks", std::nullopt, "x") && !simulator.SetGlobal("gg_trg_Sim", std::nullopt, "1") &&
              !simulator.SetGlobal("udg_names", std::nullopt, "a") && !simulator.SetGlobal("udg_missing", std::nullopt, "1"),
        "invalid start values rejected");
    Check(simulator.SetGlobal("udg_ticks", std::nullopt, "100"), "set integer");
    const auto blocked = simulator.Fire("Hud", "tick");
    Check(blocked.completed && blocked.triggers == 0 && Value(simulator, "udg_AUI_Hud_hp") == "0.5", "condition false skips actions");
}

// 停下：游戏函数报出名字与位置，死循环受步数限制；一个触发器停下不影响别的。
void TestStops() {
    auto prepared = TriggerSimulator::Prepare(Scripts(kMap));
    TriggerSimulator& simulator = *prepared.value;
    (void)simulator.Initialize();
    const auto bad = simulator.Fire("Hud", "slow");
    // 停在 Blizzard.j 里：位置报作者脚本里调用它的那一行和触发器，库里的位置写在后面。
    Check(!bad.completed && HasCode(bad.diagnostics, "AUI_SIM_UNSUPPORTED") &&
              bad.diagnostics[0].message.find("GetTriggerUnit") != std::string::npos &&
              bad.diagnostics[0].message.starts_with("war3map.j:") &&
              bad.diagnostics[0].message.find("触发器 gg_trg_Bad：") != std::string::npos &&
              bad.diagnostics[0].message.find("（在 Blizzard.j:") != std::string::npos,
        "unsupported game function reported at the author's call: " + Messages(bad.diagnostics));
    const auto spin = simulator.Fire("Hud", "spin");
    Check(!spin.completed && HasCode(spin.diagnostics, "AUI_SIM_STEPS"), "endless loop stopped");
    // 没拦截“更新”时，它里面的游戏函数照样停下，但前面的赋值保留。
    const auto tick = simulator.Fire("Hud", "tick");
    Check(!tick.completed && HasCode(tick.diagnostics, "AUI_SIM_UNSUPPORTED") && Value(simulator, "udg_ticks") == "1",
        "assignments before the stop are kept");

    const auto broken = TriggerSimulator::Prepare(Scripts("function Broken takes nothing\nendfunction\n"));
    Check(!broken.Succeeded() && HasCode(broken.diagnostics, "AUI_SIM_SYNTAX"), "syntax errors fail preparation");
}

// 实数按单精度运算与读回。
void TestSinglePrecision() {
    auto prepared = TriggerSimulator::Prepare(Scripts(kDrain));
    Check(prepared.Succeeded(), "prepare drain: " + Messages(prepared.diagnostics));
    TriggerSimulator& simulator = *prepared.value;
    Check(simulator.Initialize().completed, "drain initializes");
    const auto fire = [&simulator](int times) {
        for (int i = 0; i < times; ++i) Check(simulator.Fire("Hp", "tick").completed, "drain tick runs");
    };
    fire(3);
    Check(Value(simulator, "udg_hp") == "0.6999999", "single precision readback: " + Value(simulator, "udg_hp"));
    Check(Value(simulator, "udg_third") == "0.33333334", "single precision division: " + Value(simulator, "udg_third"));
    fire(3);
    Check(Value(simulator, "udg_low") == "false", "six ticks stay above 0.3");
    fire(1);
    Check(Value(simulator, "udg_low") == "true", "seventh tick drops below 0.3 as in the game");
    fire(2);
    Check(Value(simulator, "udg_empty") == "false", "nine ticks stay above 0");
    fire(1);
    Check(Value(simulator, "udg_empty") == "true" && Value(simulator, "udg_hp") == "-7.4505806e-08",
        "tenth tick reaches 0 or below as in the game: " + Value(simulator, "udg_hp"));
    Check(simulator.SetGlobal("udg_hp", std::nullopt, "0.1") && Value(simulator, "udg_hp") == "0.1", "set real keeps short text");
}

} // namespace

int main() {
    try {
        TestInitialize();
        TestFire();
        TestStops();
        TestSinglePrecision();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "game ui trigger simulation tests passed\n";
    return 0;
}
