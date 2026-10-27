#include "CastSpellLayer.h"
#include "PartyLayer.h"
#include "TalentLayer.h"
#include "TargetLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/TargetTriggers.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 目标框架生成的自然 T：绑定 target 模板的界面数值与点击事件，用合成的原版签名层加 AudastraUI 包，
// 生成文本须整批写进“目标框架”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

using AudastraUiTest::Contains;
using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture()
        : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer(), AudastraUiTest::TalentLayer(), AudastraUiTest::TargetLayer()}) {}
};

UiDescription TargetDescription() {
    auto target = NewUiDescriptionFromPreset("target", "target", "目标");
    Require(target.Succeeded(), "目标框架模板：" + First(target.diagnostics));
    return std::move(*target.value);
}

TargetSystemOptions FullBinding(const UiDescription& d) {
    using namespace TargetPreset;
    TargetSystemOptions options;
    TargetFrameBinding& b = options.frame;
    b.description = &d;
    b.shown = kShown;
    b.health = kHealth;
    b.mana = kMana;
    b.healthNow = kHealthNow;
    b.healthMax = kHealthMax;
    b.name = kName;
    b.level = kLevel;
    b.dead = kDead;
    b.portrait = kPortrait;
    b.select = kSelect;
    for (const auto icon : kBuffs) b.buffIcons.emplace_back(icon);
    options.buffs = {UnitBuff{"B000", "ReplaceableTextures\\CommandButtons\\BTNHeal"}};
    options.portraits = {UnitPortrait{"Hpal", "ReplaceableTextures\\CommandButtons\\BTNHeroPaladin"}};
    return options;
}

bool Has(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}

void TestRefusals() {
    TargetSystemOptions none;
    const auto noFrame = GenerateTargetSystem(none);
    Require(!noFrame.Succeeded() && Has(noFrame.diagnostics, "AUF_FRAME_INVALID"), "没有界面描述拒绝");

    UiDescription d = TargetDescription();
    d.values.push_back(UiValue{"cells", "", UiValueType::Real, 3, "0", std::nullopt, std::nullopt});
    d.events.push_back(UiEvent{"hover", "panel", UiEventKind::Enter});
    TargetSystemOptions bad = FullBinding(d);
    bad.name = "";
    bad.key = "目标";
    bad.players = 0;
    bad.frame.health = "cells";
    bad.frame.level = std::string(TargetPreset::kName);
    bad.frame.select = "hover";
    const auto refused = GenerateTargetSystem(bad);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 6, "名字、键、玩家数、格数、等级类型、事件类型一次全部列出：" + First(refused.diagnostics));
    Require(Has(refused.diagnostics, "AUF_OPTION_INVALID"), "选项诊断");

    TargetSystemOptions arrayShown = FullBinding(d);
    arrayShown.frame.shown = "cells";
    Require(!GenerateTargetSystem(arrayShown).Succeeded(), "显示须是布尔单值");

    // 点击事件的框架在重复框架里。
    UiDescription repeated = TargetDescription();
    repeated.frames.front().repeat = UiRepeat{2, 0.0, -0.06};
    Require(!GenerateTargetSystem(FullBinding(repeated)).Succeeded(), "点击事件不能在重复框架里");
}

void TestGeneratedText() {
    const UiDescription d = TargetDescription();
    const auto plan = GenerateTargetSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->triggers.size() == 6 && plan.value->triggers.front() == "目标 登记 Target_Register"
            && plan.value->triggers.back() == "目标 选中 Target_Select",
        "选中来源生成 6 个触发器，登记在前、选中在后");
    Require(plan.value->setTrigger.empty() && plan.value->memberVariable.empty(), "选中来源没有设定触发器");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"TriggerRegisterPlayerSelectionEventBJ(Player11, SelDeselOptionSel)",
             "TriggerRegisterPlayerSelectionEventBJ(Player00, SelDeselOptionDesel)",
             "SetVariable(udg_AUF_Target_Unit[GetConvertedPlayerId(GetTriggerPlayer())], GetTriggerUnit())",
             "OperatorCompareUnit(udg_AUF_Target_Unit[GetConvertedPlayerId(GetTriggerPlayer())], OperatorEqualENE, GetTriggerUnit())",
             "SetVariable(udg_AUF_Target_Self, udg_AUF_Target_Unit[GetConvertedPlayerId(GetLocalPlayer())])",
             "SetVariable(udg_AUIV_target_shown, true)", "SetVariable(udg_AUIV_target_shown, false)",
             "SetVariable(udg_AUIV_target_hp_now, GetUnitStateSwap(UnitStateLife, udg_AUF_Target_Self))",
             "SetVariable(udg_AUIV_target_level, GetHeroLevel(udg_AUF_Target_Self))", "SetVariable(udg_AUIV_target_level, GetUnitLevel(udg_AUF_Target_Self))",
             "GetHeroProperName(udg_AUF_Target_Self)", "UnitHasBuffBJ(udg_AUF_Target_Self, udg_AUF_Target_BuffCode[udg_AUF_Target_BuffI])",
             "SetVariable(udg_AUIV_target_buff6, udg_AUF_Target_BuffHit[6])",
             "SetVariable(udg_AUIV_target_portrait, udg_AUF_Target_TypeIcon[udg_AUF_Target_TypeI])",
             "SetVariable(udg_AUF_Target_ClickResult, AudastraUIClickUnit(udg_AUF_Target_Self))",
             "SelectUnitForPlayerSingle(udg_AUF_Target_Self, GetLocalPlayer())", "AudastraUITriggerRegisterEvent(\"target.select\")",
             "ConditionalTriggerExecute(`目标 更新 target_Update`)"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part + "\n" + text);
    }
    Require(!Contains(text, "Player12"), "默认只记 12 个玩家");

    // 焦点：设定触发器读单位与玩家变量；两份系统的键不同，变量与触发器互不相同。
    TargetSystemOptions focus = FullBinding(d);
    focus.name = "焦点";
    focus.key = "Focus";
    focus.source = TargetSource::Focus;
    focus.players = 4;
    const auto focused = GenerateTargetSystem(focus);
    Require(focused.Succeeded(), "焦点生成：" + First(focused.diagnostics));
    Require(focused.value->setTrigger == "焦点 设定 Focus_Set" && focused.value->memberVariable == "AUF_Focus_Member"
            && focused.value->playerVariable == "AUF_Focus_Player",
        "焦点的设定触发器与变量");
    Require(Contains(focused.value->featureText, "SetVariable(udg_AUF_Focus_Unit[GetConvertedPlayerId(udg_AUF_Focus_Player)], udg_AUF_Focus_Member)")
            && !Contains(focused.value->featureText, "TriggerRegisterPlayerSelectionEventBJ")
            && Contains(focused.value->featureText, "ForLoopVarMultiple(udg_AUF_Focus_P, 1, 4)"),
        "焦点不注册选择事件，按玩家数清理");
    for (const auto& name : focused.value->variables) {
        Require(std::find(plan.value->variables.begin(), plan.value->variables.end(), name) == plan.value->variables.end(), "两份系统的变量重名：" + name);
    }
}

void TestAppliedAndCompiled() {
    Fixture f;
    const UiDescription d = TargetDescription();
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto plan = GenerateTargetSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kTargetTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进目标框架类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);
    Require(f.Trigger("目标 登记 Target_Register")->runOnInit != 0 && f.Trigger("目标 头像 Target_Portrait")->runOnInit != 0,
        "登记与头像在地图初始化时运行");

    TargetSystemOptions focus = FullBinding(d);
    focus.name = "焦点";
    focus.key = "Focus";
    focus.source = TargetSource::Focus;
    const auto focused = GenerateTargetSystem(focus);
    Require(focused.Succeeded(), "焦点生成");
    Require(f.Apply(kTargetTriggerCategory, focused.value->featureText).Succeeded(), "焦点写进同一类别");
    Require(f.Trigger("目标 刷新 Target_Refresh") != nullptr && f.Trigger("焦点 刷新 Focus_Refresh") != nullptr, "目标与焦点并存");

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("目标 刷新 Target_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("目标 刷新 Target_Refresh") == before, "往返后触发器文本不变");

    const std::string script = f.Script();
    for (const char* part : {"_Target_Pick, Player(0), true)", "_Target_Unpick, Player(11), false)",
             "set udg_AUF_Target_Self = udg_AUF_Target_Unit[GetConvertedPlayerId(GetLocalPlayer())]",
             "set udg_AUF_Focus_Unit[GetConvertedPlayerId(udg_AUF_Focus_Player)] = udg_AUF_Focus_Member",
             "set udg_AUF_Target_ClickResult = AudastraUIClickUnit(udg_AUF_Target_Self)"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part + "\n" + script);
    }
    // 本机有游戏脚本时用真实的 common.j 与 Blizzard.j 检查整图脚本（界面触发器与目标、焦点一起）。
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "target script checked against game scripts\n";
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
    std::cout << "game ui target trigger tests passed\n";
    return 0;
}
