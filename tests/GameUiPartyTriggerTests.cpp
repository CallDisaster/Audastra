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
             "set udg_AUIV_party_hp[(udg_AUP_I - 1)]", "call TriggerRegisterEnterRectSimple(", "IsUnitIllusionBJ(udg_AUP_Member)"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part);
    }
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
    std::cout << "game ui party trigger tests passed\n";
    return 0;
}
