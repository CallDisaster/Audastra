#include "ActionBarLayer.h"
#include "CastSpellLayer.h"
#include "PartyLayer.h"
#include "TalentLayer.h"
#include "TargetLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/ActionBarTriggers.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 技能栏生成的自然 T：绑定 actionbar 模板的界面数值，用合成的原版签名层加 YDWE 技能属性与 AudastraUI 包，
// 生成文本须整批写进“技能栏”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

using AudastraUiTest::Contains;
using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture()
        : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer(), AudastraUiTest::TalentLayer(), AudastraUiTest::TargetLayer(),
              AudastraUiTest::ActionBarLayer(), AudastraUiTest::YdweAbilityStateLayer()}) {}
};

UiDescription BarDescription() {
    auto bar = NewUiDescriptionFromPreset("actionbar", "bar", "技能栏");
    Require(bar.Succeeded(), "技能栏模板：" + First(bar.diagnostics));
    return std::move(*bar.value);
}

ActionBarSystemOptions FullBinding(const UiDescription& d) {
    using namespace ActionBarPreset;
    ActionBarSystemOptions options;
    ActionBarFrameBinding& b = options.frame;
    b.description = &d;
    b.barShown = kBarShown;
    b.shown = kShown;
    b.icon = kIcon;
    b.key = kKey;
    b.cooldown = kCooldown;
    b.cooldownText = kCooldownText;
    b.noMana = kNoMana;
    b.use = kUse;
    options.entries = {{"Hpal", 1, "AHhb", "ReplaceableTextures\\CommandButtons\\BTNHolyBolt.blp", "T"},
        {"Hpal", 2, "AHds", "ReplaceableTextures\\CommandButtons\\BTNDivineIntervention.blp", "D"},
        {"Obla", 1, "AOwk", "ReplaceableTextures\\CommandButtons\\BTNWindWalkOn.blp", "W"}};
    return options;
}

bool Has(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}

void TestRefusals() {
    ActionBarSystemOptions none;
    const auto noFrame = GenerateActionBarSystem(none);
    Require(!noFrame.Succeeded() && Has(noFrame.diagnostics, "AUA_FRAME_INVALID"), "没有界面描述拒绝");

    UiDescription d = BarDescription();
    d.values.push_back(UiValue{"cells", "", UiValueType::String, 3, "", std::nullopt, std::nullopt});
    ActionBarSystemOptions bad = FullBinding(d);
    bad.name = "";
    bad.key = "技能";
    bad.players = 13;
    bad.frame.icon = "cells";
    bad.frame.key = std::string(ActionBarPreset::kCooldown);
    bad.entries = {{"Hpal", 0, "AHhb", "Icon", "T"}, {"Hpa", 13, "AHh", "", "\"T"}, {"Obla", 1, "AOwk", "Icon", ""}, {"Obla", 1, "AOmi", "Icon", ""}};
    const auto refused = GenerateActionBarSystem(bad);
    // 名字、键、玩家数、图标格数、热键类型；第 1 项格 0；第 2 项单位、技能、格、图标、热键；第 4 项重复。
    Require(!refused.Succeeded() && refused.diagnostics.size() == 12, "选项、绑定与登记一次全部列出：" + std::to_string(refused.diagnostics.size()));
    Require(Has(refused.diagnostics, "AUA_OPTION_INVALID") && Has(refused.diagnostics, "AUA_FRAME_INVALID") && Has(refused.diagnostics, "AUA_ENTRY_INVALID"),
        "选项、绑定与登记诊断");

    // 显示须是 1..24 格的布尔数组。
    ActionBarSystemOptions single = FullBinding(d);
    single.frame.shown = std::string(ActionBarPreset::kBarShown);
    Require(!GenerateActionBarSystem(single).Succeeded(), "显示不能是单值");
    // 关了技能状态就不能绑定冷却与法力。
    ActionBarSystemOptions plain = FullBinding(d);
    plain.abilityState = false;
    const auto noState = GenerateActionBarSystem(plain);
    Require(!noState.Succeeded() && Has(noState.diagnostics, "AUA_OPTION_INVALID"), "没开技能状态时冷却绑定拒绝");
    // 点击事件须在格数相同的重复框架里。
    UiDescription loose = BarDescription();
    loose.events.push_back(UiEvent{"whole", "bar", UiEventKind::Click});
    ActionBarSystemOptions outside = FullBinding(loose);
    outside.frame.use = "whole";
    Require(!GenerateActionBarSystem(outside).Succeeded(), "点击事件不在重复框架里拒绝");
}

void TestGeneratedText() {
    const UiDescription d = BarDescription();
    const auto plan = GenerateActionBarSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->triggers
            == std::vector<std::string>{"技能栏 登记 Bar_Register", "技能栏 选择 Bar_Pick", "技能栏 取消选择 Bar_Unpick", "技能栏 刷新 Bar_Refresh",
                "技能栏 使用 Bar_Use"},
        "触发器名与顺序");
    Require(plan.value->slotCount == ActionBarPreset::kSlots && plan.value->registrationTrigger == "技能栏 登记 Bar_Register" && plan.value->setTrigger.empty(),
        "格数与登记触发器");
    const std::string& text = plan.value->featureText;
    const std::string unit = "udg_AUA_Bar_Unit[udg_AUA_Bar_Me]";
    const std::string ability = "udg_AUA_Bar_SlotAbility[udg_AUA_Bar_S]";
    const std::string cell = "[OperatorInt(udg_AUA_Bar_S, OperatorSubtract, 1)]";
    for (const std::string part : {std::string("SetVariable(udg_AUA_Bar_Count, 3)"), std::string("SetVariable(udg_AUA_Bar_Type[3], 'Obla')"),
             std::string("SetVariable(udg_AUA_Bar_Slot[2], 2)"), std::string("SetVariable(udg_AUA_Bar_Ability[1], 'AHhb')"),
             std::string("SetVariable(udg_AUA_Bar_Icon[1], \"ReplaceableTextures\\\\CommandButtons\\\\BTNHolyBolt.blp\")"),
             std::string("SetVariable(udg_AUA_Bar_Key[2], \"D\")"),
             std::string("TriggerRegisterPlayerSelectionEventBJ(Player11, SelDeselOptionSel)"),
             std::string("SetVariable(udg_AUA_Bar_Unit[GetConvertedPlayerId(GetTriggerPlayer())], GetTriggerUnit())"),
             std::string("TriggerRegisterTimerEventPeriodic(0.1)"),
             std::string("SetVariable(udg_AUA_Bar_Me, GetConvertedPlayerId(GetLocalPlayer()))"),
             "OperatorCompareUnitCode(GetUnitTypeId(" + unit + "), OperatorNotEqualENE, udg_AUA_Bar_LastType)",
             "SetVariable(udg_AUA_Bar_LastType, GetUnitTypeId(" + unit + "))",
             std::string("OperatorCompareUnitCode(udg_AUA_Bar_Type[udg_AUA_Bar_I], OperatorEqualENE, udg_AUA_Bar_LastType)"),
             std::string("SetVariable(udg_AUA_Bar_SlotKey[udg_AUA_Bar_Slot[udg_AUA_Bar_I]], udg_AUA_Bar_Key[udg_AUA_Bar_I])"),
             "SetVariable(udg_AUA_Bar_L, GetUnitAbilityLevelSwapped(" + ability + ", " + unit + "))",
             "SetVariable(udg_AUIV_bar_shown" + cell + ", true)", "SetVariable(udg_AUIV_bar_icon" + cell + ", udg_AUA_Bar_SlotIcon[udg_AUA_Bar_S])",
             "YDWEGetUnitAbilityState(" + unit + ", " + ability + ", ABILITY_STATE_COOLDOWN)",
             "YDWEGetUnitAbilityDataReal(" + unit + ", " + ability + ", udg_AUA_Bar_L, ABILITY_DATA_COOL)",
             "SetVariable(udg_AUIV_bar_cooldown" + cell + ", OperatorReal(udg_AUA_Bar_Cooldown, OperatorDivide, udg_AUA_Bar_Total))",
             "SetVariable(udg_AUIV_bar_cooldown_text" + cell + ", AudastraUIFormatReal(udg_AUA_Bar_Cooldown, 1))",
             "I2R(YDWEGetUnitAbilityDataInteger(" + unit + ", " + ability + ", udg_AUA_Bar_L, ABILITY_DATA_COST))",
             "OperatorCompareReal(GetUnitStateSwap(UnitStateMana, " + unit + "), OperatorLess, ",
             std::string("SetVariable(udg_AUIV_bar_bar_shown, false)"), std::string("ConditionalTriggerExecute(`技能栏 更新 bar_Update`)"),
             std::string("AudastraUITriggerRegisterEvent(\"bar.use\")"),
             std::string("SetVariable(udg_AUA_Bar_Click, OperatorInt(AudastraUIGetTriggerIndex(), OperatorAdd, 1))"),
             "OperatorCompareBoolean(IsUnitSelected(" + unit + ", GetLocalPlayer()), OperatorEqualENE, true)",
             std::string("ForceUIKeyBJ(GetLocalPlayer(), udg_AUA_Bar_SlotKey[udg_AUA_Bar_Click])"),
             "SelectUnitForPlayerSingle(" + unit + ", GetLocalPlayer())"}) {
        Require(Contains(text, part), "生成文本缺少：" + part + "\n" + text);
    }
    // 本机部分不把单位写进变量（各玩家的句柄引用要一致）：单位只在同步的选择事件与清理里赋值。
    Require(!Contains(text, "SetVariable(" + unit) && !Contains(text, "Self"), "本机部分不给单位变量赋值");

    // 焦点、不读技能状态、只绑定显示与点击：有“设定”，没有 YDWE 技能属性；没有登记时也写登记触发器（数量 0）。
    ActionBarSystemOptions focus;
    focus.name = "英雄";
    focus.key = "Hero";
    focus.source = TargetSource::Focus;
    focus.abilityState = false;
    focus.players = 4;
    focus.frame.description = &d;
    focus.frame.shown = std::string(ActionBarPreset::kShown);
    focus.frame.use = std::string(ActionBarPreset::kUse);
    const auto hero = GenerateActionBarSystem(focus);
    Require(hero.Succeeded(), "焦点生成：" + First(hero.diagnostics));
    Require(hero.value->triggers == std::vector<std::string>{"英雄 登记 Hero_Register", "英雄 设定 Hero_Set", "英雄 刷新 Hero_Refresh", "英雄 使用 Hero_Use"},
        "焦点的触发器");
    Require(hero.value->memberVariable == "AUA_Hero_Member" && hero.value->playerVariable == "AUA_Hero_Player" && hero.value->setTrigger == "英雄 设定 Hero_Set",
        "焦点的设定变量");
    Require(!Contains(hero.value->featureText, "YDWE") && Contains(hero.value->featureText, "SetVariable(udg_AUA_Hero_Count, 0)")
            && Contains(hero.value->featureText, "ForLoopVarMultiple(udg_AUA_Hero_P, 1, 4)"),
        "不读技能状态时不用 YDWE；按玩家数清理");
    for (const auto& name : hero.value->variables) {
        Require(std::find(plan.value->variables.begin(), plan.value->variables.end(), name) == plan.value->variables.end(), "两份技能栏的变量重名：" + name);
    }
    // 地图里已有登记时不再生成登记触发器。
    ActionBarSystemOptions keep = FullBinding(d);
    keep.includeRegistration = false;
    const auto kept = GenerateActionBarSystem(keep);
    Require(kept.Succeeded() && !Contains(kept.value->featureText, "登记 Bar_Register") && kept.value->registrationTrigger == "技能栏 登记 Bar_Register",
        "保留作者的登记");
}

void TestAppliedAndCompiled() {
    Fixture f;
    const UiDescription d = BarDescription();
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    // 两行各 6 格：列号取余、行号整除。
    Require(Contains(ui.value->featureText, "ModuloInteger(GetForLoopIndexA(), 6)")
            && Contains(ui.value->featureText, "OperatorInt(GetForLoopIndexA(), OperatorDivide, 6)"),
        "重复框架按每行个数换行：\n" + ui.value->featureText);
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto plan = GenerateActionBarSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kActionBarTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进技能栏类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("技能栏 刷新 Bar_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("技能栏 刷新 Bar_Refresh") == before, "往返后触发器文本不变");

    const std::string script = f.Script();
    for (const char* part : {"call ForceUIKeyBJ(GetLocalPlayer(), udg_AUA_Bar_SlotKey[udg_AUA_Bar_Click])",
             "set udg_AUA_Bar_Ability[1] = 'AHhb'", "ModuloInteger(GetForLoopIndexA(), 6)",
             "YDWEGetUnitAbilityState(udg_AUA_Bar_Unit[udg_AUA_Bar_Me], udg_AUA_Bar_SlotAbility[udg_AUA_Bar_S], 1)"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part + "\n" + script);
    }
    // 本机有游戏脚本时用真实的 common.j 与 Blizzard.j 检查整图脚本（界面触发器与技能栏一起）。
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "action bar script checked against game scripts\n";
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
    std::cout << "game ui action bar trigger tests passed\n";
    return 0;
}
