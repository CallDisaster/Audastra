#include "CastSpellLayer.h"
#include "PartyLayer.h"
#include "TalentLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/TalentTriggers.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 统一天赋树生成的自然 T：绑定 talents 模板的界面数值与事件，用合成的原版签名层加 AudastraUI 包，
// 生成文本须整批写进“天赋系统”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

using AudastraUiTest::Contains;
using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture() : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer(), AudastraUiTest::TalentLayer()}) {}
};

UiDescription TalentDescription() {
    auto talents = NewUiDescriptionFromPreset("talents", "talents", "天赋树");
    Require(talents.Succeeded(), "天赋树模板：" + First(talents.diagnostics));
    return std::move(*talents.value);
}

TalentSystemOptions FullBinding(const UiDescription& d) {
    using namespace TalentPreset;
    TalentSystemOptions options;
    TalentFrameBinding& b = options.frame;
    b.description = &d;
    b.shown = kShown;
    b.entryShown = kEntryShown;
    b.entry = kEntry;
    b.title = kTitle;
    b.points = kPoints;
    b.level = kLevel;
    b.open = kOpen;
    b.close = kClose;
    b.tipShown = kTipShown;
    b.tipTitle = kTipTitle;
    b.tipText = kTipText;
    for (std::size_t c = 0; c < kOptions; ++c) {
        b.columns.push_back(TalentColumnBinding{std::string(kIcons[c]), std::string(kNames[c]), std::string(kPicked[c]), std::string(kPicks[c]),
            std::string(kEnters[c]), std::string(kLeaves[c])});
    }
    TalentHero hero{"O000", "沃金", {}};
    hero.entries.push_back(TalentEntry{1, 1, "毒性增强", "ReplaceableTextures\\CommandButtons\\BTNPoisonSting.blp", {}, "巫毒箭附带剧毒", {}});
    hero.entries.push_back(TalentEntry{1, 2, "迅捷", "ReplaceableTextures\\CommandButtons\\BTNBoots.blp", {}, "移动速度提高", "A001"});
    hero.entries.push_back(TalentEntry{2, 3, "灵魂链接", "UI\\Custom\\Link.blp", "UI\\Custom\\LinkOff.blp", "第二行\n说明", {}});
    options.heroes = {hero};
    options.afterPick = "沃金 天赋效果";
    return options;
}

bool Has(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}

void TestPreset() {
    const UiDescription d = TalentDescription();
    Require(d.values.size() == 6 + 3 * TalentPreset::kOptions + 3 && d.events.size() == 2 + 3 * TalentPreset::kOptions, "模板的数值与事件个数");
    Require(TalentDisabledIcon("ReplaceableTextures\\CommandButtons\\BTNBoots.blp") == "ReplaceableTextures\\CommandButtonsDisabled\\DISBTNBoots.blp",
        "按 BTN 推出灰色图标");
    Require(TalentDisabledIcon("replaceabletextures\\commandbuttons\\btnboots.blp") == "replaceabletextures\\CommandButtonsDisabled\\DISBTNboots.blp",
        "推灰色图标不分大小写");
    Require(TalentDisabledIcon("UI\\Custom\\Link.blp") == "UI\\Custom\\Link.blp", "推不出时原样");
}

void TestRefusals() {
    const auto noFrame = GenerateTalentSystem(TalentSystemOptions{});
    Require(!noFrame.Succeeded() && Has(noFrame.diagnostics, "AUT_FRAME_INVALID"), "没有界面描述拒绝");

    UiDescription d = TalentDescription();
    d.values.push_back(UiValue{"four", "", UiValueType::String, 4, "", std::nullopt, std::nullopt});
    TalentSystemOptions bad = FullBinding(d);
    bad.name = "";
    bad.syncUnitType = "foo";
    bad.players = 0;
    bad.unlockLevels = {5, 10};
    bad.frame.columns[1].icon = "four";
    bad.frame.columns[2].pick = "enter1";
    bad.heroes.push_back(TalentHero{"O000", "", {TalentEntry{9, 1, "x", "y", {}, {}, {}}, TalentEntry{1, 1, "", "", {}, {}, "A0"}}});
    const auto refused = GenerateTalentSystem(bad);
    // 名字、同步单位、玩家数；图标格数、事件重复绑定、点击事件类型；解锁等级个数；重复登记英雄、层超出、名字、图标、技能，一次列出。
    Require(!refused.Succeeded() && refused.diagnostics.size() == 12, "选项、绑定与登记一次全部列出：" + std::to_string(refused.diagnostics.size())
            + " " + First(refused.diagnostics));
    Require(Has(refused.diagnostics, "AUT_OPTION_INVALID") && Has(refused.diagnostics, "AUT_ENTRY_INVALID"), "选项与登记诊断");

    // 点击事件的框架不在重复“层数”次的框架里。
    UiDescription loose = TalentDescription();
    loose.events.push_back(UiEvent{"wrong", "close", UiEventKind::Click});
    TalentSystemOptions wrong = FullBinding(loose);
    wrong.frame.columns[0].pick = "wrong";
    Require(!GenerateTalentSystem(wrong).Succeeded(), "点击事件须在每层一行的重复框架里");

    TalentSystemOptions noTip = FullBinding(d);
    noTip.frame.tipShown.clear();
    Require(!GenerateTalentSystem(noTip).Succeeded(), "绑定说明框正文却没有说明框显示");
}

void TestGeneratedText() {
    const UiDescription d = TalentDescription();
    const auto plan = GenerateTalentSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->tiers == TalentPreset::kTiers && plan.value->options == TalentPreset::kOptions, "层数取自等级数组，列数取自绑定");
    Require(plan.value->triggers.size() == 11 + 3 * TalentPreset::kOptions && plan.value->triggers.front() == "天赋 登记 Talent_Register"
            && plan.value->bindTrigger == "天赋 绑定 Talent_Bind" && plan.value->pickedVariable == "AUT_Picked",
        "登记在前，绑定与选择结果");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"触发器 天赋 初始化 Talent_Init (init):", "SetVariable(udg_AUT_HeroType[1], 'O000')",
             "SetVariable(udg_AUT_IconOff[1], \"ReplaceableTextures\\\\CommandButtonsDisabled\\\\DISBTNPoisonSting.blp\")",
             "SetVariable(udg_AUT_Learn[2], 'A001')", "SetVariable(udg_AUT_IconOff[7], \"UI\\\\Custom\\\\LinkOff.blp\")",
             "SetVariable(udg_AUT_Text[7], \"第二行\\n说明\")", "SetVariable(udg_AUT_TierLevel[5], 25)",
             "TriggerRegisterPlayerSelectionEventBJ(Player11, SelDeselOptionSel)",
             "OperatorCompareUnitCode(GetUnitTypeId(GetTriggerUnit()), OperatorEqualENE, 'hfoo')",
             "CreateNUnitsAtLoc(1, 'hfoo', ConvertedPlayer(udg_AUT_P), udg_AUT_Point, 0)", "SetUnitUseFoodBJ(EnableDisableDisable, GetLastCreatedUnit())",
             "SelectUnitRemoveForPlayer(GetTriggerUnit(), GetTriggerPlayer())", "UnitAddAbilityBJ(udg_AUT_Learn[udg_AUT_K], udg_AUT_Hero[udg_AUT_P])",
             "ConditionalTriggerExecute(`沃金 天赋效果`)", "SelectUnitAddForPlayer(udg_AUT_Sync[", "AudastraUITriggerRegisterEvent(\"talents.pick2\")",
             "AudastraUITriggerRegisterEvent(\"talents.enter3\")", "TriggerRegisterAnyUnitEventBJ(PlayerUnitEventHero_Level)",
             "SetVariable(udg_AUIV_talents_picked3[udg_AUT_LRow], true)", "SetVariable(udg_AUIV_talents_icon1[udg_AUT_LRow], udg_AUT_IconOff[udg_AUT_LK])",
             "\"|cff808080需要 \"", "ConditionalTriggerExecute(`天赋树 更新 talents_Update`)"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part + "\n" + text);
    }
    const auto pathing = text.find("SetUnitPathing(GetLastCreatedUnit(), PathingOff)");
    Require(pathing != std::string::npos && text.find("SetUnitPositionLoc(GetLastCreatedUnit(), udg_AUT_Point)", pathing) != std::string::npos,
        "关掉碰撞后把同步单位放回可玩区域外");
    Require(!Contains(text, "GetForLoopIndexA") && !Contains(text, "ForLoopAMultiple"), "系统不用循环 A");
    Require(!Contains(text, "Player12"), "只为 12 个玩家登记选择事件");

    // 只绑必填项、保留作者的登记：没有入口、说明框与移入移出触发器，也不生成登记。
    TalentSystemOptions minimal;
    minimal.includeRegistration = false;
    minimal.unlockLevels = {2, 4, 6, 8, 10};
    minimal.frame.description = &d;
    minimal.frame.shown = TalentPreset::kShown;
    minimal.frame.level = TalentPreset::kLevel;
    minimal.frame.columns = {TalentColumnBinding{"icon1", "name1", "picked1", "pick1", {}, {}},
        TalentColumnBinding{"icon2", "name2", "picked2", "pick2", {}, {}}};
    const auto small = GenerateTalentSystem(minimal);
    Require(small.Succeeded() && small.value->options == 2 && small.value->triggers.size() == 10, "最小绑定：" + First(small.diagnostics));
    Require(!Contains(small.value->featureText, "Talent_Register:") && Contains(small.value->featureText, "ConditionalTriggerExecute(`天赋 登记 Talent_Register`)")
            && Contains(small.value->featureText, "SetVariable(udg_AUT_TierLevel[3], 6)") && !Contains(small.value->featureText, "tip_shown"),
        "保留登记时初始化仍先运行它；解锁等级按选项");
}

void TestAppliedAndCompiled() {
    Fixture f;
    const UiDescription d = TalentDescription();
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    // 选定后运行的作者触发器须已在地图里。
    Require(f.Apply("沃金", "触发器 沃金 天赋效果:\n    actions:\n        CommentString(\"按 AUT_PickTier、AUT_PickOption 生效\")\n").Succeeded(),
        "写进作者触发器");
    const auto plan = GenerateTalentSystem(FullBinding(d));
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kTalentTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进天赋系统类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);
    Require(f.Trigger("天赋 初始化 Talent_Init")->runOnInit != 0 && f.Trigger("天赋 登记 Talent_Register")->runOnInit == 0,
        "初始化在地图初始化时运行，登记由初始化先运行");

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("天赋 刷新 Talent_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("天赋 刷新 Talent_Refresh") == before, "往返后触发器文本不变");

    // 重新生成整体替换系统触发器。
    Require(f.Apply(kTalentTriggerCategory, plan.value->featureText).Succeeded(), "重新生成");

    const std::string script = f.Script();
    for (const char* part : {"call TriggerRegisterPlayerSelectionEventBJ(gg_trg_", "call CreateNUnitsAtLoc(1, 'hfoo', ConvertedPlayer(udg_AUT_P), udg_AUT_Point",
             "call SelectUnitAddForPlayer(udg_AUT_Sync[", "set udg_AUT_Picked[((udg_AUT_P * 8) + udg_AUT_T)] = udg_AUT_O",
             "call UnitAddAbilityBJ(udg_AUT_Learn[udg_AUT_K], udg_AUT_Hero[udg_AUT_P])", "set udg_AUT_HeroType[1] = 'O000'",
             "GetHeroProperName(udg_AUT_Hero[udg_AUT_LP])"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part);
    }
    // 本机有游戏脚本时用真实的 common.j 与 Blizzard.j 检查整图脚本（界面触发器与天赋系统一起）。
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "talent script checked against game scripts\n";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        TestPreset();
        TestRefusals();
        TestGeneratedText();
        TestAppliedAndCompiled();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui talent trigger tests passed\n";
    return 0;
}
