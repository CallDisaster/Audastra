#include "AudastraUiCatalog.h"
#include "War3MapCore/GameUi/CastTriggers.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

// 读条系统生成的自然 T：用合成的原版签名层（施法事件、单位与计时器函数、YDWE 运行触发器参数块）加 AudastraUI 包，
// 生成文本须整批写进“读条系统”类别、写成 WTG/WCT 再读回，并经同一生产输出器生成能通过 JASS 检查的脚本。
namespace {
using namespace War3Map;
using namespace War3Map::GameUi;
using namespace War3Map::Trigger;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
std::string First(const std::vector<Diagnostic>& diagnostics) {
    return diagnostics.empty() ? std::string("（无诊断）") : diagnostics.front().code + " " + diagnostics.front().message;
}
bool Contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

// 原版与 YDWE 签名（参数类型按 TriggerData 写法）；名字与预设取自真实地图里用过的写法。
AudastraUiTest::Catalog::Layer SpellLayer() {
    AudastraUiTest::Catalog::Layer layer;
    layer.name = "spell";
    layer.define =
        "[TriggerTypes]\n"
        "unit=0,1,1,Unit\nplayer=0,1,1,Player\ntimer=0,1,1,Timer\ntrigger=0,1,1,Trigger\nlocation=0,1,1,Location\n"
        "abilcode=0,1,1,Ability,integer\nintegervar=1,0,0,IntegerVariable,integer\n"
        "playerunitevent=0,0,0,PlayerUnitEvent\nperiodicoption=0,0,0,Periodic,boolean\ntypename=0,0,0,TypeName\n"
        "executemode=0,0,0,ExecuteMode,boolean\n"
        "[TriggerParams]\n"
        "PlayerUnitEventSpellChannel=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_CHANNEL,Channel\n"
        "PlayerUnitEventSpellEffect=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_EFFECT,Effect\n"
        "PlayerUnitEventSpellFinish=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_FINISH,Finish\n"
        "PlayerUnitEventSpellEndCast=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_ENDCAST,EndCast\n"
        "PeriodicOptionOneTime=0,periodicoption,false,OneTime\n"
        "typename01_integer=0,typename,integer,Integer\ntypename02_real=0,typename,real,Real\n"
        "typename07_unit=0,typename,unit,Unit\n"
        "tirggerexecute_notcondition=0,executemode,false,NoCondition\n"
        "OperatorSubtract=0,ArithmeticOperator,-,Subtract\nOperatorDivide=0,ArithmeticOperator,/,Divide\n"
        "OperatorLess=0,ComparisonOperator,<,Less\nOperatorGreater=0,ComparisonOperator,>,Greater\n"
        "OperatorGreaterEq=0,ComparisonOperator,>=,GreaterEq\n"
        "[TriggerTypeDefaults]\ntimer=CreateTimer()\n";
    layer.event =
        "[TriggerRegisterAnyUnitEventBJ]\n[[.args]]\ntype=playerunitevent\n"
        "[TriggerRegisterTimerEventPeriodic]\n[[.args]]\ntype=real\n";
    layer.condition =
        "[OperatorCompareAbilityId]\nreturns=boolean\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=abilcode\n"
        "[OperatorCompareUnit]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=unit\n"
        "[OperatorComparePlayer]\nreturns=boolean\n[[.args]]\ntype=player\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=player\n";
    layer.action =
        "[StartTimerBJ]\n[[.args]]\ntype=timer\n[[.args]]\ntype=periodicoption\n[[.args]]\ntype=real\n"
        "[ConditionalTriggerExecute]\n[[.args]]\ntype=trigger\n"
        "[RemoveLocation]\n[[.args]]\ntype=location\n"
        "[ForLoopVarMultiple]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[YDWEExecuteTriggerMultiple]\n[[.args]]\ntype=trigger\n[[.args]]\ntype=executemode\n"
        "[YDWESetAnyTypeLocalVariable]\n[[.args]]\ntype=typename\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=Null\n";
    layer.call =
        "[GetSpellAbilityId]\nreturns=abilcode\n[GetTriggerUnit]\nreturns=unit\n[GetSpellTargetUnit]\nreturns=unit\n"
        "[GetSpellTargetLoc]\nreturns=location\n"
        "[GetLocationX]\nreturns=real\n[[.args]]\ntype=location\n[GetLocationY]\nreturns=real\n[[.args]]\ntype=location\n"
        "[GetUnitAbilityLevel]\nreturns=integer\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n"
        "[TimerGetElapsed]\nreturns=real\n[[.args]]\ntype=timer\n"
        "[R2I]\nreturns=integer\n[[.args]]\ntype=real\n"
        "[IsUnitDeadBJ]\nreturns=boolean\n[[.args]]\ntype=unit\n"
        "[GetOwningPlayer]\nreturns=player\n[[.args]]\ntype=unit\n[GetLocalPlayer]\nreturns=player\n"
        "[GetConvertedPlayerId]\nreturns=integer\n[[.args]]\ntype=player\n"
        "[GetAbilityName]\nreturns=string\n[[.args]]\ntype=abilcode\n"
        "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=scriptcode\n";
    return layer;
}

struct Fixture {
    AudastraUiTest::Catalog files;
    UiCatalog catalog;
    TriggerDocument doc;

    Fixture() {
        files.extra = SpellLayer();
        const auto error = files.Load();
        Require(error.empty(), error);
        catalog = std::move(*files.catalog);
        doc = std::move(*files.document);
    }

    Result<TriggerGraphEditReceipt> Apply(std::string_view category, const std::string& text) {
        const auto binding = CaptureTriggerGraphBinding(doc, catalog);
        Require(binding.Succeeded(), "capture binding");
        TriggerGraphEditBatch batch{*binding.value, {}};
        batch.operations.emplace_back(GraphApplyFeatureText{std::string(category), text, {}, std::nullopt});
        return ApplyTriggerGraphEditBatch(doc, catalog, batch);
    }

    [[nodiscard]] const TriggerRecord* Trigger(std::string_view name) const {
        const auto found = std::find_if(doc.triggers.begin(), doc.triggers.end(), [&](const auto& row) { return row.name.text == name; });
        return found == doc.triggers.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::string Text(std::string_view name) const {
        const TriggerRecord* trigger = Trigger(name);
        Require(trigger != nullptr, "没有触发器 " + std::string(name));
        const auto text = RenderTriggerText(doc, catalog, trigger->id, 1u << 20, nullptr, nullptr);
        Require(text.Succeeded() && text.value, "导出 " + std::string(name) + "：" + First(text.diagnostics));
        return *text.value;
    }
};

// 两个效果触发器：读到参数后写进变量，证明参数块的名字能被效果触发器取到。
constexpr std::string_view kEffects =
    "变量:\n"
    "    Got: integer = 0\n"
    "触发器 效果 读条 Bolt:\n"
    "    actions:\n"
    "        SetVariable(udg_Got, YDWEGetAnyTypeLocalVariable(\"阶段\"))\n"
    "触发器 效果 引导 Ritual:\n"
    "    actions:\n"
    "        SetVariable(udg_Got, YDWEGetAnyTypeLocalVariable(\"跳数\"))\n";

CastSystemOptions TwoAbilities() {
    CastSystemOptions options;
    options.abilities = {CastAbility{"A000", 1.5, 0.0, 0.0, "效果 读条 Bolt"}, CastAbility{"A001", 0.0, 3.0, 0.5, "效果 引导 Ritual"}};
    return options;
}

void TestRefusals() {
    CastSystemOptions options;
    options.abilities = {CastAbility{"A0", 1.0, 0.0, 0.0, "效果"}, CastAbility{"A001", 0.0, 2.0, 0.01, "效果"},
        CastAbility{"A002", 0.0, 0.0, 0.5, "效果"}, CastAbility{"A003", -1.0, 0.0, 0.0, "效果"}, CastAbility{"A004", 1.0, 0.0, 0.0, ""},
        CastAbility{"A004", 1.0, 0.0, 0.0, "效果"}};
    const auto refused = GenerateCastSystem(options);
    Require(!refused.Succeeded() && refused.diagnostics.size() == 6, "不合法的登记一次全部列出：" + First(refused.diagnostics));
    for (const char* code : {"AUC_ABILITY_INVALID", "AUC_SECONDS_INVALID", "AUC_EFFECT_INVALID", "AUC_ABILITY_DUPLICATE"}) {
        Require(std::any_of(refused.diagnostics.begin(), refused.diagnostics.end(), [&](const auto& d) { return d.code == code; }),
            std::string("缺少诊断 ") + code);
    }

    UiDescription bar;
    bar.id = "castbar";
    bar.values = {UiValue{"shown", "", UiValueType::Real, 0, "0", std::nullopt, std::nullopt}};
    CastSystemOptions withBar;
    withBar.bar = CastBarBinding{&bar, "shown", "missing", {}, {}, "nostate", {}, 0.5};
    const auto badBar = GenerateCastSystem(withBar);
    Require(!badBar.Succeeded() && badBar.diagnostics.size() == 3, "读条条绑定的类型、数值、状态问题一起报：" + First(badBar.diagnostics));
}

void TestGeneratedText() {
    const auto plan = GenerateCastSystem(TwoAbilities());
    Require(plan.Succeeded(), "生成：" + First(plan.diagnostics));
    Require(plan.value->triggers.size() == 11 && plan.value->triggers.front() == "读条 登记 Cast_Register",
        "生成 11 个触发器，登记在最前");
    Require(plan.value->registrationTrigger == "读条 登记 Cast_Register", "登记触发器名");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"触发器 读条 登记 Cast_Register (init):", "SetVariable(udg_AUC_RegAbility[2], 'A001')",
             "SetVariable(udg_AUC_RegEffect[1], `效果 读条 Bolt`)", "SetVariable(udg_AUC_RegTick[2], 0.5)",
             "TriggerRegisterAnyUnitEventBJ(PlayerUnitEventSpellEndCast)", "ForLoopVarMultiple(udg_AUC_TickI, 1, udg_AUC_Count)",
             "YDWEExecuteTriggerMultiple(udg_AUC_RegEffect[udg_AUC_SlotReg[0]], tirggerexecute_notcondition)",
             "YDWESetAnyTypeLocalVariable(typename07_unit, \"施法单位\", udg_AUC_Unit[0])"}) {
        Require(Contains(text, part), std::string("生成文本缺少：") + part);
    }
    Require(!Contains(text, "GetForLoopIndexA") && !Contains(text, "ForLoopAMultiple"), "系统不用循环 A");

    CastSystemOptions keep = TwoAbilities();
    keep.includeRegistration = false;
    const auto without = GenerateCastSystem(keep);
    Require(without.Succeeded() && without.value->triggers.size() == 10 && !Contains(without.value->featureText, "Cast_Register (init)"),
        "不生成登记触发器时其余照常");
}

std::string GeneratedScript(const Fixture& f) {
    Script::MapScriptInputs inputs;
    inputs.triggers = &f.doc;
    inputs.catalog = &f.catalog;
    const auto generated = Script::GenerateMapScript(inputs);
    Require(generated.Succeeded(), "整图脚本生成：" + First(generated.diagnostics));
    return generated.text;
}

void TestAppliedAndCompiled() {
    Fixture f;
    const auto effects = f.Apply("技能", std::string(kEffects));
    Require(effects.Succeeded(), "效果触发器：" + First(effects.diagnostics));
    const auto plan = GenerateCastSystem(TwoAbilities());
    Require(plan.Succeeded(), "生成");
    const auto applied = f.Apply(kCastTriggerCategory, plan.value->featureText);
    Require(applied.Succeeded(), "写进读条系统类别：" + First(applied.diagnostics));
    for (const auto& name : plan.value->triggers) Require(f.Trigger(name) != nullptr, "缺少触发器 " + name);
    Require(f.Trigger("读条 登记 Cast_Register")->runOnInit != 0 && f.Trigger("读条 初始化 Cast_Init")->runOnInit != 0,
        "登记与初始化在地图初始化时运行");
    Require(Contains(f.Text("读条 派发 Cast_Dispatch"), "params:"), "派发带参数块");

    // 写成 WTG/WCT 再读回，触发器文本不变。
    const std::string before = f.Text("读条 刷新 Cast_Refresh");
    const auto encoded = WtgWctCodec{}.Serialize(f.doc, f.catalog);
    Require(encoded.Succeeded(), "写出：" + First(encoded.diagnostics));
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, f.catalog);
    Require(parsed.Succeeded(), "读回：" + First(parsed.diagnostics));
    f.doc = std::move(*parsed.value);
    Require(f.Text("读条 刷新 Cast_Refresh") == before, "往返后触发器文本不变");

    // 作者改过登记后重新生成系统：不生成登记触发器，登记保留。
    const std::string registration = f.Text("读条 登记 Cast_Register");
    CastSystemOptions keep;
    keep.includeRegistration = false;
    const auto again = GenerateCastSystem(keep);
    Require(again.Succeeded() && f.Apply(kCastTriggerCategory, again.value->featureText).Succeeded(), "重新生成系统");
    Require(f.Text("读条 登记 Cast_Register") == registration, "登记触发器保留作者的内容");

    // 同一生产输出器生成整图脚本：遍历用自己的变量，参数块经运行触发器传入。
    const std::string script = GeneratedScript(f);
    for (const char* part : {"exitwhen udg_AUC_TickI > udg_AUC_Count", "call TriggerExecute(ydl_trigger)", "set udg_AUC_Clock = CreateTimer()",
             "Cast_Dispatch)", "set udg_AUC_RegAbility[2] = 'A001'"}) {
        Require(Contains(script, part), std::string("脚本缺少：") + part + "\n" + script.substr(0, 4000));
    }
}

void TestBarBinding() {
    Fixture f;
    Require(f.Apply("技能", std::string(kEffects)).Succeeded(), "效果触发器");
    UiDescription bar;
    bar.id = "castbar";
    bar.label = "施法条";
    bar.values = {UiValue{"shown", "", UiValueType::Boolean, 0, "false", std::nullopt, std::nullopt},
        UiValue{"progress", "", UiValueType::Real, 0, "0", 0.0, 1.0}, UiValue{"label", "", UiValueType::String, 0, "", std::nullopt, std::nullopt},
        UiValue{"left", "", UiValueType::Real, 0, "0", std::nullopt, std::nullopt}};
    UiFrame back;
    back.id = "back";
    back.type = UiFrameType::Backdrop;
    back.width = 0.2;
    back.height = 0.02;
    back.texture = "UI\\Widgets\\ToolTips\\Human\\human-tooltip-background";
    back.anchors = {UiAnchor{}};
    back.visible.binding = UiBinding{"shown"};
    bar.frames = {back};
    bar.states = {UiState{"interrupt", "打断", {UiStateOverride{"back", std::string("ReplaceableTextures\\TeamColor\\TeamColor00"), {}, {}, {}}}},
        UiState{"channel", "引导", {}}};
    const auto ui = GenerateUiTriggers(bar);
    Require(ui.Succeeded(), "界面触发器：" + First(ui.diagnostics));
    Require(f.Apply(kUiTriggerCategory, ui.value->featureText).Succeeded(), "写进界面触发器");

    CastSystemOptions options = TwoAbilities();
    options.bar = CastBarBinding{&bar, "shown", "progress", "label", "left", "channel", "interrupt", 0.5};
    const auto plan = GenerateCastSystem(options);
    Require(plan.Succeeded(), "带读条条生成：" + First(plan.diagnostics));
    Require(plan.value->triggers.back() == "读条 显示 Cast_Show", "显示触发器在最后");
    const std::string& text = plan.value->featureText;
    for (const char* part : {"SetVariable(udg_AUIV_castbar_progress, udg_AUC_ShowRatio)", "ConditionalTriggerExecute(`施法条 更新 castbar_Update`)",
             "ConditionalTriggerExecute(`施法条 状态 打断 castbar_State_interrupt`)", "ConditionalTriggerExecute(`施法条 状态 引导 castbar_State_channel`)",
             "SetVariable(udg_AUC_FlashName[GetConvertedPlayerId(GetOwningPlayer(GetTriggerUnit()))], GetAbilityName(GetSpellAbilityId()))"}) {
        Require(Contains(text, part), std::string("读条条文本缺少：") + part);
    }
    const auto applied = f.Apply(kCastTriggerCategory, text);
    Require(applied.Succeeded(), "带读条条写入：" + First(applied.diagnostics));
    const std::string script = GeneratedScript(f);
    Require(Contains(script, "GetLocalPlayer()"), "显示只按本机玩家");
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    try {
        TestRefusals();
        TestGeneratedText();
        TestAppliedAndCompiled();
        TestBarBinding();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "game ui cast trigger tests passed\n";
    return 0;
}
