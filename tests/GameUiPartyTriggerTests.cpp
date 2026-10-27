#include "CastSpellLayer.h"
#include "PartyLayer.h"
#include "TriggerSystemFixture.h"
#include "War3MapCore/GameUi/PartyTriggers.h"
#include "War3MapCore/GameUi/UiPresets.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

// 团队框架生成的自然 T：绑定 party 模板的界面数值与点击事件，用合成的原版签名层加 AudastraUI 包，
// 生成文本须整批写进“团队框架”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

using AudastraUiTest::Contains;
using AudastraUiTest::First;
using AudastraUiTest::Require;

struct Fixture : AudastraUiTest::SystemFixture {
    Fixture() : SystemFixture({AudastraUiTest::SpellLayer(), AudastraUiTest::PartyLayer()}) {}
};

UiDescription PartyDescription() {
    auto party = NewUiDescriptionFromPreset("party", "party", "队伍");
    Require(party.Succeeded(), "团队框架模板：" + First(party.diagnostics));
    return std::move(*party.value);
}

PartySystemOptions FullBinding(const UiDescription& d) {
    using namespace PartyPreset;
    PartySystemOptions options;
    options.frame = PartyFrameBinding{&d, std::string(kShown), std::string(kHealth), std::string(kMana), std::string(kName),
        std::string(kDead), std::string(kFar), std::string(kSelect)};
    return options;
}

bool Has(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& d) { return d.code == code; });
}

void TestRefusals() {
    PartySystemOptions none;
    const auto noFrame = GeneratePartySystem(none);
    Require(!noFrame.Succeeded() && Has(noFrame.diagnostics, "AUP_FRAME_INVALID"), "没有界面描述拒绝");

    UiDescription d = PartyDescription();
    d.values.push_back(UiValue{"single", "", UiValueType::Real, 0, "0", std::nullopt, std::nullopt});
    d.values.push_back(UiValue{"three", "", UiValueType::Boolean, 3, "false", std::nullopt, std::nullopt});
    d.events.push_back(UiEvent{"hover", "slot", UiEventKind::Enter});
    PartySystemOptions bad = FullBinding(d);
    bad.name = "";
    bad.frame.health = "single";
    bad.frame.dead = "three";
    bad.frame.name = "missing";
    bad.frame.select = "hover";
    bad.farRange = 10.0;
    const auto refused = GeneratePartySystem(bad);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 6, "名字、格数、类型、缺数值、事件类型、距离一次全部列出：" + First(refused.diagnostics));
    Require(Has(refused.diagnostics, "AUP_OPTION_INVALID"), "选项诊断");

    PartySystemOptions single = FullBinding(d);
    single.frame.shown = "single";
    Require(!GeneratePartySystem(single).Succeeded(), "显示须是布尔数组");

    // 点击事件的框架不在重复框架里。
    UiDescription loose = PartyDescription();
    UiFrame title;
    title.id = "title";
    title.type = UiFrameType::Button;
    title.text = "队伍";
    title.anchors = {UiAnchor{}};
    loose.frames.push_back(title);
    loose.events = {UiEvent{"select", "title", UiEventKind::Click}};
    Require(!GeneratePartySystem(FullBinding(loose)).Succeeded(), "点击事件须在重复框架里");
}

void TestGeneratedText() {
    const UiDescription d = PartyDescription();
    const auto plan = GeneratePartySystem(FullBinding(d));
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->slotCount == PartyPreset::kSlots, "格数取自显示数组");
    Require(plan.value->triggers.size() == 7 && plan.value->triggers.front() == "团队 加入 Party_Join"
            && plan.value->triggers.back() == "团队 选中 Party_Select",
        "生成 7 个触发器，加入在前、选中在后");
    Require(plan.value->joinTrigger == "团队 加入 Party_Join" && plan.value->leaveTrigger == "团队 移出 Party_Leave"
            && plan.value->memberVariable == "AUP_Member",
        "加入、移出与队员变量");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"触发器 团队 初始化 Party_Init (init):", "ForGroupMultiple(GetUnitsInRectAll(GetPlayableMapRect()))",
             "TriggerRegisterEnterRectSimple(GetPlayableMapRect())", "OperatorCompareBoolean(IsUnitIllusionBJ(udg_AUP_Member), OperatorEqualENE, false)",
             "OperatorCompareInteger(udg_AUP_Count, OperatorLess, 5)", "SetVariable(udg_AUP_Unit[udg_AUP_Count], UnitNull)",
             "SetVariable(udg_AUIV_party_shown[OperatorInt(udg_AUP_I, OperatorSubtract, 1)], true)",
             "GetUnitStateSwap(UnitStateMaxMana, udg_AUP_Unit[udg_AUP_I])", "GetPlayerName(GetOwningPlayer(udg_AUP_Unit[udg_AUP_I]))",
             "OperatorComparePlayer(GetOwningPlayer(udg_AUP_Unit[udg_AUP_I]), OperatorEqualENE, GetLocalPlayer())", "1440000",
             "AudastraUITriggerRegisterEvent(\"party.select\")", "SelectUnitForPlayerSingle(udg_AUP_Unit[udg_AUP_Click], GetLocalPlayer())",
             "SetVariable(udg_AUP_ClickResult, AudastraUIClickUnit(udg_AUP_Unit[udg_AUP_Click]))",
             "OperatorCompareInteger(udg_AUP_ClickResult, OperatorLessEq, 0)",
             "ConditionalTriggerExecute(`队伍 更新 party_Update`)"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part + "\n" + text);
    }
    Require(!Contains(text, "GetForLoopIndexA") && !Contains(text, "ForLoopAMultiple"), "系统不用循环 A");

    // 只绑显示与名字、不自动加入：没有坐标、选中与英雄触发器；名字取单位名。
    PartySystemOptions minimal;
    minimal.autoHeroes = false;
    minimal.nameSource = PartyNameSource::Unit;
    minimal.frame = PartyFrameBinding{&d, "shown", {}, {}, "name", {}, {}, {}};
    const auto small = GeneratePartySystem(minimal);
    Require(small.Succeeded() && small.value->triggers.size() == 3, "最小绑定只有加入、移出、刷新：" + First(small.diagnostics));
    Require(!Contains(small.value->featureText, "AUP_Point") && !Contains(small.value->featureText, "GetLocalPlayer")
            && Contains(small.value->featureText, "GetHeroProperName(udg_AUP_Unit[udg_AUP_I])"),
        "不算距离时不记坐标，名字按英雄名或单位名");
}

void TestAppliedAndCompiled() {
    Fixture f;
    const UiDescription d = PartyDescription();
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto plan = GeneratePartySystem(FullBinding(d));
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kPartyTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进团队框架类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);
    Require(f.Trigger("团队 初始化 Party_Init")->runOnInit != 0, "初始化在地图初始化时运行");

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("团队 刷新 Party_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("团队 刷新 Party_Refresh") == before, "往返后触发器文本不变");

    // 重新生成整体替换系统触发器。
    Require(f.Apply(kPartyTriggerCategory, plan.value->featureText).Succeeded(), "重新生成");

    const std::string script = f.Script();
    for (const char* part : {"call ForGroupBJ(GetUnitsInRectAll(GetPlayableMapRect())", "exitwhen udg_AUP_I > 5",
             "call SelectUnitForPlayerSingle(udg_AUP_Unit[udg_AUP_Click], GetLocalPlayer())", "call RemoveLocation(udg_AUP_Point)",
             "set udg_AUP_ClickResult = AudastraUIClickUnit(udg_AUP_Unit[udg_AUP_Click])", "if ((udg_AUP_ClickResult <= 0)) then",
             "set udg_AUIV_party_hp[(udg_AUP_I - 1)]", "call TriggerRegisterEnterRectSimple(", "IsUnitIllusionBJ(udg_AUP_Member)"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part);
    }
}

// 增减益：登记触发器列出增减益与图标，每格按登记顺序填进图标数值；模板的图标框架按数值换贴图、为空时隐藏。
void TestBuffs() {
    const UiDescription d = PartyDescription();
    PartySystemOptions options = FullBinding(d);
    for (const auto id : PartyPreset::kBuffs) options.frame.buffIcons.emplace_back(id);
    options.buffs = {PartyBuff{"B000", "ReplaceableTextures\\CommandButtons\\BTNHeal"}, PartyBuff{"B001", "ReplaceableTextures\\CommandButtons\\BTNSlow"}};

    PartySystemOptions bad = options;
    bad.buffs.push_back(PartyBuff{"B0", ""});
    bad.buffs.push_back(PartyBuff{"B000", "x"});
    bad.frame.buffIcons.push_back("buff1");
    const auto refused = GeneratePartySystem(bad);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 4, "增减益码、图标、重复登记、重复绑定一起报：" + First(refused.diagnostics));
    PartySystemOptions unbound = FullBinding(d);
    unbound.buffs = options.buffs;
    Require(!GeneratePartySystem(unbound).Succeeded(), "登记了增减益却没有图标数值");

    const auto plan = GeneratePartySystem(options);
    Require(plan.Succeeded(), "带增减益生成：" + First(plan.diagnostics));
    Require(plan.value->triggers.size() == 8 && plan.value->triggers.front() == "团队 登记 Party_Register"
            && plan.value->registrationTrigger == "团队 登记 Party_Register",
        "登记触发器在最前");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"触发器 团队 登记 Party_Register (init):", "SetVariable(udg_AUP_BuffCode[2], 'B001')",
             "UnitHasBuffBJ(udg_AUP_Unit[udg_AUP_I], udg_AUP_BuffCode[udg_AUP_BuffI])",
             "OperatorCompareInteger(udg_AUP_BuffN, OperatorLess, 4)",
             "SetVariable(udg_AUIV_party_buff2[OperatorInt(udg_AUP_I, OperatorSubtract, 1)], udg_AUP_BuffHit[2])"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part);
    }
    PartySystemOptions keep = options;
    keep.includeRegistration = false;
    const auto kept = GeneratePartySystem(keep);
    Require(kept.Succeeded() && kept.value->triggers.size() == 7 && !Contains(kept.value->featureText, "Party_Register (init)"),
        "已有登记时不再生成登记");

    Fixture f;
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(Contains(ui.value->featureText, "SetVariable(udg_AUIL_party_icon1_texture[GetForLoopIndexA()], udg_AUIV_party_buff1[GetForLoopIndexA()])")
            && Contains(ui.value->featureText, "AudastraUISetTexture(udg_AUI_party_icon1[GetForLoopIndexA()], udg_AUIL_party_icon1_texture[GetForLoopIndexA()]"),
        "图标贴图在更新里按数值设置，没变就不重发");
    Require(Contains(ui.value->featureText, "OperatorCompareString(udg_AUIV_party_buff1[GetForLoopIndexA()], OperatorNotEqualENE, \"\")"),
        "图标数值为空时隐藏");
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto applied = f.Apply(kPartyTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "带增减益写入：" + First(applied.diagnostics));
    Require(f.Trigger("团队 登记 Party_Register")->runOnInit != 0, "登记在地图初始化时运行");
    const std::string script = f.Script();
    for (const char* part : {"set udg_AUP_BuffCode[1] = 'B000'", "UnitHasBuffBJ(udg_AUP_Unit[udg_AUP_I], udg_AUP_BuffCode[udg_AUP_BuffI])"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part);
    }
    // 本机有游戏脚本时用真实的 common.j 与 Blizzard.j 检查整图脚本（界面触发器与团队框架一起）。
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "party script checked against game scripts\n";
}

// 生命数字与头像：数字取单位当前/最大生命；头像触发器列出单位类型与图标，每格按单位类型填头像，没登记的为空。
void TestNumbersAndPortraits() {
    using namespace PartyPreset;
    const UiDescription d = PartyDescription();
    PartySystemOptions options = FullBinding(d);
    options.frame.healthNow = std::string(kHealthNow);
    options.frame.healthMax = std::string(kHealthMax);
    options.frame.portrait = std::string(kPortrait);
    options.portraits = {PartyPortrait{"Hpal", "ReplaceableTextures\\CommandButtons\\BTNHeroPaladin"},
        PartyPortrait{"H000", "ReplaceableTextures\\CommandButtons\\BTNJaina"}};

    PartySystemOptions bad = options;
    bad.frame.healthNow = std::string(kName);
    bad.portraits.push_back(PartyPortrait{"Hp", ""});
    bad.portraits.push_back(PartyPortrait{"Hpal", "x"});
    const auto refused = GeneratePartySystem(bad);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 4, "数字类型、单位类型、图标、重复登记一起报：" + First(refused.diagnostics));
    Require(Has(refused.diagnostics, "AUP_PORTRAIT_INVALID"), "头像诊断");
    PartySystemOptions unbound = FullBinding(d);
    unbound.portraits = options.portraits;
    Require(!GeneratePartySystem(unbound).Succeeded(), "登记了头像却没有头像数值");

    const auto plan = GeneratePartySystem(options);
    Require(plan.Succeeded(), "带数字与头像生成：" + First(plan.diagnostics));
    Require(plan.value->triggers.size() == 8 && plan.value->triggers.front() == "团队 头像 Party_Portrait"
            && plan.value->portraitTrigger == "团队 头像 Party_Portrait",
        "头像触发器在最前");
    const std::string& text = plan.value->featureText;
    const std::string cell = "[OperatorInt(udg_AUP_I, OperatorSubtract, 1)]";
    for (const std::string part : {std::string("触发器 团队 头像 Party_Portrait (init):"), std::string("SetVariable(udg_AUP_TypeCode[2], 'H000')"),
             "SetVariable(udg_AUIV_party_hp_now" + cell + ", GetUnitStateSwap(UnitStateLife, udg_AUP_Unit[udg_AUP_I]))",
             "SetVariable(udg_AUIV_party_hp_max" + cell + ", GetUnitStateSwap(UnitStateMaxLife, udg_AUP_Unit[udg_AUP_I]))",
             "SetVariable(udg_AUIV_party_portrait" + cell + ", \"\")",
             std::string("OperatorCompareUnitCode(GetUnitTypeId(udg_AUP_Unit[udg_AUP_I]), OperatorEqualENE, udg_AUP_TypeCode[udg_AUP_TypeI])"),
             "SetVariable(udg_AUIV_party_portrait" + cell + ", udg_AUP_TypeIcon[udg_AUP_TypeI])"}) {
        Require(Contains(text, part), "生成文本缺少：" + part + "\n" + text);
    }
    PartySystemOptions keep = options;
    keep.includePortraitRegistration = false;
    const auto kept = GeneratePartySystem(keep);
    Require(kept.Succeeded() && kept.value->triggers.size() == 7 && !Contains(kept.value->featureText, "Party_Portrait (init)"),
        "已有头像触发器时不再生成");

    // 模板：头像框架按数值换贴图、为空时隐藏；生命数字写成“当前/最大”，死亡时不显示。
    const auto ui = GenerateUiTriggers(d);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(Contains(ui.value->featureText, "OperatorCompareString(udg_AUIV_party_portrait[GetForLoopIndexA()], OperatorNotEqualENE, \"\")"),
        "头像数值为空时隐藏");
    Require(Contains(ui.value->featureText, "udg_AUIV_party_hp_now[GetForLoopIndexA()]")
            && Contains(ui.value->featureText, "udg_AUIV_party_hp_max[GetForLoopIndexA()]"),
        "生命数字取当前与最大生命");

    Fixture f;
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");
    const auto applied = f.Apply(kPartyTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "带头像写入：" + First(applied.diagnostics));
    Require(f.Trigger("团队 头像 Party_Portrait")->runOnInit != 0, "头像触发器在地图初始化时运行");
    const std::string script = f.Script();
    for (const char* part : {"set udg_AUP_TypeCode[1] = 'Hpal'", "if ((GetUnitTypeId(udg_AUP_Unit[udg_AUP_I]) == udg_AUP_TypeCode[udg_AUP_TypeI])) then"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part + "\n" + script);
    }
    if (AudastraUiTest::CheckWithGameScripts(f, script)) std::cout << "party portrait script checked against game scripts\n";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        TestRefusals();
        TestGeneratedText();
        TestAppliedAndCompiled();
        TestBuffs();
        TestNumbersAndPortraits();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui party trigger tests passed\n";
    return 0;
}
