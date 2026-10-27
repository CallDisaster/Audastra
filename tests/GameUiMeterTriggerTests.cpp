#include "CastSpellLayer.h"
#include "MeterLayer.h"
#include "PartyLayer.h"
#include "TalentLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/MeterTriggers.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 伤害统计生成的自然 T：绑定 meter 模板的界面数值，用合成的原版签名层加 YDWE 伤害事件与 AudastraUI 包，
// 生成文本须整批写进“伤害统计”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

using AudastraUiTest::Contains;
using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture()
        : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer(), AudastraUiTest::TalentLayer(), AudastraUiTest::MeterLayer(),
              AudastraUiTest::YdweDamageLayer()}) {}
};

UiDescription MeterDescription() {
    auto meter = NewUiDescriptionFromPreset("meter", "meter", "伤害统计");
    Require(meter.Succeeded(), "伤害统计模板：" + First(meter.diagnostics));
    return std::move(*meter.value);
}

MeterSystemOptions FullBinding(const UiDescription& d) {
    using namespace MeterPreset;
    MeterSystemOptions options;
    MeterFrameBinding& b = options.frame;
    b.description = &d;
    b.shown = kShown;
    b.name = kName;
    b.amount = kAmount;
    b.amountText = kAmountText;
    b.share = kShare;
    b.percent = kPercent;
    b.rate = kRate;
    b.color = kColor;
    b.total = kTotal;
    b.time = kTime;
    return options;
}

bool Has(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}

void TestRefusals() {
    MeterSystemOptions none;
    const auto noFrame = GenerateMeterSystem(none);
    Require(!noFrame.Succeeded() && Has(noFrame.diagnostics, "AUM_FRAME_INVALID"), "没有界面描述拒绝");

    UiDescription d = MeterDescription();
    d.values.push_back(UiValue{"cells", "", UiValueType::Real, 3, "0", std::nullopt, std::nullopt});
    MeterSystemOptions bad = FullBinding(d);
    bad.name = "";
    bad.key = "伤害";
    bad.players = 13;
    bad.frame.amount = "cells";
    bad.frame.name = std::string(MeterPreset::kAmount);
    bad.frame.total = std::string(MeterPreset::kRate);
    const auto refused = GenerateMeterSystem(bad);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 6, "名字、键、玩家数、格数、类型、单值一次全部列出：" + First(refused.diagnostics));
    Require(Has(refused.diagnostics, "AUM_OPTION_INVALID") && Has(refused.diagnostics, "AUM_FRAME_INVALID"), "选项与绑定诊断");

    // 显示须是 1..12 格的布尔数组。
    MeterSystemOptions single = FullBinding(d);
    single.frame.shown = std::string(MeterPreset::kTotal);
    Require(!GenerateMeterSystem(single).Succeeded(), "显示不能是单值");
    UiDescription many = MeterDescription();
    many.values.front().size = 13;
    Require(!GenerateMeterSystem(FullBinding(many)).Succeeded(), "行数最多 12");
}

void TestGeneratedText() {
    const UiDescription d = MeterDescription();
    const auto plan = GenerateMeterSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->triggers == std::vector<std::string>{"伤害 记录 Damage_Record", "伤害 刷新 Damage_Refresh", "伤害 清零 Damage_Reset"},
        "触发器名与顺序");
    Require(plan.value->rows == MeterPreset::kRows && plan.value->resetTrigger == "伤害 清零 Damage_Reset"
            && plan.value->amountVariable == "AUM_Damage_Amount",
        "行数、清零触发器与伤害变量");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"YDWESyStemAnyUnitDamagedRegistTrigger()", "SetVariable(udg_AUM_Damage_Source, GetEventDamageSource())",
             "OperatorCompareReal(GetEventDamage(), OperatorGreater, 0)",
             "OperatorCompareBoolean(IsUnitEnemy(GetTriggerUnit(), GetOwningPlayer(udg_AUM_Damage_Source)), OperatorEqualENE, true)",
             "OperatorCompareInteger(GetConvertedPlayerId(GetOwningPlayer(udg_AUM_Damage_Source)), OperatorLessEq, 12)",
             "SetVariable(udg_AUM_Damage_Amount[udg_AUM_Damage_P], OperatorReal(udg_AUM_Damage_Amount[udg_AUM_Damage_P], OperatorAdd, GetEventDamage()))",
             "TriggerRegisterTimerEventPeriodic(0.5)", "SetVariable(udg_AUM_Damage_Time, OperatorReal(udg_AUM_Damage_Time, OperatorAdd, 0.5))",
             "ForLoopVarMultiple(udg_AUM_Damage_R, 1, 8)",
             "OperatorCompareReal(udg_AUM_Damage_Amount[udg_AUM_Damage_P], OperatorGreater, udg_AUM_Damage_Amount[udg_AUM_Damage_Best])",
             "SetVariable(udg_AUIV_meter_shown[OperatorInt(udg_AUM_Damage_R, OperatorSubtract, 1)], true)",
             "SetVariable(udg_AUIV_meter_name[OperatorInt(udg_AUM_Damage_R, OperatorSubtract, 1)], GetPlayerName(ConvertedPlayer(udg_AUM_Damage_Best)))",
             "AudastraUIFormatReal(OperatorReal(udg_AUM_Damage_Amount[udg_AUM_Damage_Best], OperatorDivide, 10000), 1), \"万\")",
             "OperatorReal(udg_AUM_Damage_Amount[udg_AUM_Damage_Best], OperatorDivide, udg_AUM_Damage_Top)",
             "OperatorReal(udg_AUM_Damage_Amount[udg_AUM_Damage_Best], OperatorDivide, RMaxBJ(udg_AUM_Damage_Time, 1))",
             "\"ReplaceableTextures\\\\TeamColor\\\\TeamColor\"", "I2S(OperatorInt(udg_AUM_Damage_Best, OperatorSubtract, 1))",
             "SetVariable(udg_AUIV_meter_total, udg_AUM_Damage_Total)", "SetVariable(udg_AUIV_meter_time, udg_AUM_Damage_Time)",
             "ConditionalTriggerExecute(`伤害统计 更新 meter_Update`)", "SetVariable(udg_AUM_Damage_Running, false)"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part + "\n" + text);
    }
    Require(!Contains(text, "GetLocalPlayer"), "统计与刷新对所有玩家相同，不用本地玩家");

    // 不只记打敌人：没有敌对判断。两份系统的键不同，变量与触发器互不相同。
    MeterSystemOptions all = FullBinding(d);
    all.name = "承伤";
    all.key = "All";
    all.enemiesOnly = false;
    all.players = 4;
    const auto every = GenerateMeterSystem(all);
    Require(every.Succeeded(), "不限敌人生成：" + First(every.diagnostics));
    Require(!Contains(every.value->featureText, "IsUnitEnemy") && Contains(every.value->featureText, "ForLoopVarMultiple(udg_AUM_All_P, 1, 4)"),
        "不限敌人时没有敌对判断，按玩家数循环");
    for (const auto& name : every.value->variables) {
        Require(std::find(plan.value->variables.begin(), plan.value->variables.end(), name) == plan.value->variables.end(), "两份系统的变量重名：" + name);
    }

    // 只绑定显示时只写显示。
    MeterSystemOptions minimal;
    minimal.frame.description = &d;
    minimal.frame.shown = std::string(MeterPreset::kShown);
    const auto bare = GenerateMeterSystem(minimal);
    Require(bare.Succeeded() && !Contains(bare.value->featureText, "AudastraUIFormatReal") && !Contains(bare.value->featureText, "TeamColor"),
        "没绑定的数值不写");
}

void TestAppliedAndCompiled() {
    Fixture f;
    const UiDescription d = MeterDescription();
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto plan = GenerateMeterSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kMeterTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进伤害统计类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("伤害 刷新 Damage_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("伤害 刷新 Damage_Refresh") == before, "往返后触发器文本不变");

    const std::string script = f.Script();
    for (const char* part : {"call YDWESyStemAnyUnitDamagedRegistTrigger(", "set udg_AUM_Damage_Source = GetEventDamageSource()",
             "set udg_AUM_Damage_Amount[udg_AUM_Damage_P] = (udg_AUM_Damage_Amount[udg_AUM_Damage_P] + GetEventDamage())"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part + "\n" + script);
    }
    // 本机有游戏脚本时用真实的 common.j 与 Blizzard.j 检查整图脚本（界面触发器与伤害统计一起）。
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "meter script checked against game scripts\n";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        TestRefusals();
        TestGeneratedText();
        TestAppliedAndCompiled();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui meter trigger tests passed\n";
    return 0;
}
