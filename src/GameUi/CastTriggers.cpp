#include "War3MapCore/GameUi/CastTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <algorithm>
#include <cmath>

// 读条系统的自然 T。施法者各占一格（数组下标 1..AUC_Count），结束时用最后一格填空位。
// 派发先把该格复制到第 0 格再释放，效果触发器的参数取自第 0 格：效果触发器里再引发的施法事件改动格子也不影响本次参数。
// 遍历用自己的整数变量循环（不用 bj_forLoopAIndex），效果触发器里的“循环 A”不会打乱系统的遍历。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::string_view kKey = "Cast";

std::string V(std::string_view name) { return "udg_AUC_" + std::string(name); }
std::string At(std::string_view name, std::string_view index) { return V(name) + "[" + std::string(index) + "]"; }

Lines LoopVar(std::string_view variable, std::string_view from, std::string_view to, const Lines& body) {
    return ForLoopVar(V(variable), from, to, body);
}

bool ValidRawcode(std::string_view code) {
    return code.size() == 4 && std::all_of(code.begin(), code.end(), [](unsigned char c) {
        return c > 0x20 && c < 0x7f && c != '\'' && c != '\\';
    });
}

// 每格记录的数组：名字、类型。派发复制到第 0 格的是前 8 个（效果触发器的参数来源）。
struct SlotArray {
    std::string_view name;
    std::string_view type;
};
constexpr SlotArray kSlotArrays[] = {{"Unit", "unit"}, {"SlotReg", "integer"}, {"Ability", "abilcode"},
    {"Level", "integer"}, {"Target", "unit"}, {"X", "real"}, {"Y", "real"}, {"Ticks", "integer"},
    {"Stage", "integer"}, {"Start", "real"}, {"Length", "real"}};
constexpr std::size_t kDispatchArrays = 8;

class Generator {
public:
    explicit Generator(const CastSystemOptions& options) : o_(options) {}

    Result<CastSystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<CastSystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        if (o_.includeRegistration) text_.Trigger(Name("登记", "Register"), {}, Registration(), {}, "init");
        text_.Trigger(Name("初始化", "Init"), {}, {Call("StartTimerBJ", {V("Clock"), "PeriodicOptionOneTime", "1000000.00"})}, {}, "init");
        text_.Trigger(Name("查找技能", "FindAbility"), {}, FindAbility());
        text_.Trigger(Name("查找施法者", "FindSlot"), {}, FindSlot());
        text_.Trigger(Name("释放", "Free"), {}, Free());
        text_.Trigger(Name("派发", "Dispatch"), {}, Dispatch());
        text_.Trigger(Name("开始", "Begin"), {SpellEvent("PlayerUnitEventSpellChannel")}, Begin());
        text_.Trigger(Name("生效", "Effect"), {SpellEvent("PlayerUnitEventSpellEffect")}, Effect());
        text_.Trigger(Name("结束", "Finish"), {SpellEvent("PlayerUnitEventSpellFinish")}, Finish());
        text_.Trigger(Name("停止", "Stop"), {SpellEvent("PlayerUnitEventSpellEndCast")}, Stop());
        text_.Trigger(Name("刷新", "Refresh"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kCastRefreshSeconds)})}, Refresh());
        if (o_.bar) text_.Trigger(Name("显示", "Show"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kCastRefreshSeconds)})}, Show());

        CastSystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        plan.registrationTrigger = Name("登记", "Register");
        return Result<CastSystemPlan>::Success(std::move(plan));
    }

private:
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Name(std::string_view role, std::string_view key) const { return CastTriggerName(o_.name, role, key); }
    [[nodiscard]] std::string Run(std::string_view role, std::string_view key) const {
        return Call("ConditionalTriggerExecute", {TriggerRef(Name(role, key))});
    }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUC_OPTION_INVALID", "读条系统的名字不能为空，也不能含双引号或控制字符");
        for (std::size_t i = 0; i < o_.abilities.size(); ++i) {
            const CastAbility& a = o_.abilities[i];
            const std::string at = "第 " + std::to_string(i + 1) + " 个登记（" + a.ability + "）：";
            if (!ValidRawcode(a.ability)) Error("AUC_ABILITY_INVALID", at + "技能码应为 4 个字符");
            const auto seconds = [&](double value, const char* what) {
                if (!std::isfinite(value) || value < 0.0 || value > 3600.0) Error("AUC_SECONDS_INVALID", at + what + "应在 0 到 3600 秒之间");
            };
            seconds(a.castSeconds, "读条秒数");
            seconds(a.channelSeconds, "引导秒数");
            seconds(a.tickSeconds, "跳间隔");
            if (a.tickSeconds > 0.0 && a.tickSeconds < kCastRefreshSeconds) {
                Error("AUC_SECONDS_INVALID", at + "跳间隔不能小于刷新周期 " + Real(kCastRefreshSeconds) + " 秒");
            }
            if (a.tickSeconds > 0.0 && a.channelSeconds <= 0.0) Error("AUC_SECONDS_INVALID", at + "有跳间隔时须有引导秒数");
            if (!UsableLabel(a.effectTrigger) || a.effectTrigger.find('`') != std::string::npos) {
                Error("AUC_EFFECT_INVALID", at + "需要效果触发器");
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (o_.abilities[j].ability == a.ability) Error("AUC_ABILITY_DUPLICATE", at + "技能已在第 " + std::to_string(j + 1) + " 个登记");
            }
        }
        if (o_.bar) CheckBar(*o_.bar);
    }

    void CheckBar(const CastBarBinding& bar) {
        if (!bar.description) {
            Error("AUC_BAR_INVALID", "读条条没有界面描述");
            return;
        }
        const UiDescription& d = *bar.description;
        const auto value = [&](const std::string& id, UiValueType type, const char* what, bool required) {
            if (id.empty()) {
                if (required) Error("AUC_BAR_INVALID", std::string("读条条需要") + what + "数值");
                return;
            }
            if (const auto problem = CheckValueBinding(d, id, type, 0, what)) Error("AUC_BAR_INVALID", *problem);
        };
        value(bar.visible, UiValueType::Boolean, "布尔", true);
        value(bar.progress, UiValueType::Real, "实数", true);
        value(bar.text, UiValueType::String, "字符串", false);
        value(bar.remaining, UiValueType::Real, "实数", false);
        for (const std::string* state : {&bar.channel, &bar.interrupted}) {
            if (!state->empty() && !FindState(d, *state)) Error("AUC_BAR_INVALID", "界面 " + d.id + " 没有状态 " + *state);
        }
        if (!std::isfinite(bar.flashSeconds) || bar.flashSeconds < 0.0 || bar.flashSeconds > 10.0) {
            Error("AUC_BAR_INVALID", "打断提示时长应在 0 到 10 秒之间");
        }
    }

    void Variables() {
        const auto var = [&](std::string_view name, std::string_view type, bool array = false, const std::string& initial = {}) {
            text_.Variable("AUC_" + std::string(name), type, array ? std::optional<std::uint32_t>(0) : std::nullopt, initial);
        };
        var("Clock", "timer");
        var("RegCount", "integer", false, "0");
        var("RegAbility", "abilcode", true);
        var("RegCast", "real", true);
        var("RegChannel", "real", true);
        var("RegTick", "real", true);
        var("RegEffect", "trigger", true);
        var("Count", "integer", false, "0");
        for (const auto& slot : kSlotArrays) var(slot.name, slot.type, true);
        var("FindAbility", "abilcode");
        var("FindUnit", "unit");
        var("FindI", "integer", false, "0");
        var("Reg", "integer", false, "0");
        var("Slot", "integer", false, "0");
        var("Phase", "integer", false, "0");
        var("Point", "location");
        var("Now", "real", false, "0");
        var("TickI", "integer", false, "0");
        if (o_.bar) {
            var("FlashUntil", "real", true);
            var("FlashName", "string", true);
            var("ShowI", "integer", false, "0");
            var("ShowSlot", "integer", false, "0");
            var("ShowNow", "real", false, "0");
            var("ShowRatio", "real", false, "0");
            var("Shown", "integer", false, "0");
        }
    }

    [[nodiscard]] static std::string SpellEvent(std::string_view event) {
        return Call("TriggerRegisterAnyUnitEventBJ", {std::string(event)});
    }

    Lines Registration() const {
        Lines out{Call("CommentString", {Quote("每个技能一组：技能、读条秒数、引导秒数、跳间隔（0 为不跳）、效果触发器；秒数须与物编一致")})};
        out.push_back(Set(V("RegCount"), std::to_string(o_.abilities.size())));
        for (std::size_t i = 0; i < o_.abilities.size(); ++i) {
            const CastAbility& a = o_.abilities[i];
            const std::string index = std::to_string(i + 1);
            out.push_back(Set(At("RegAbility", index), "'" + a.ability + "'"));
            out.push_back(Set(At("RegCast", index), Real(a.castSeconds)));
            out.push_back(Set(At("RegChannel", index), Real(a.channelSeconds)));
            out.push_back(Set(At("RegTick", index), Real(a.tickSeconds)));
            out.push_back(Set(At("RegEffect", index), TriggerRef(a.effectTrigger)));
        }
        return out;
    }

    // 输入 AUC_FindAbility，输出 AUC_Reg（登记序号，没有为 0）。
    static Lines FindAbility() {
        Lines out{Set(V("Reg"), "0")};
        Append(out, LoopVar("FindI", "1", V("RegCount"),
            If({Call("OperatorCompareAbilityId", {At("RegAbility", V("FindI")), "OperatorEqualENE", V("FindAbility")})},
                {Set(V("Reg"), V("FindI"))})));
        return out;
    }

    // 输入 AUC_FindUnit，输出 AUC_Slot（所占的格，没有为 0）。
    static Lines FindSlot() {
        Lines out{Set(V("Slot"), "0")};
        Append(out, LoopVar("FindI", "1", V("Count"),
            If({Call("OperatorCompareUnit", {At("Unit", V("FindI")), "OperatorEqualENE", V("FindUnit")})},
                {Set(V("Slot"), V("FindI"))})));
        return out;
    }

    // 释放 AUC_Slot：最后一格移进来。
    static Lines Free() {
        Lines out;
        for (const auto& slot : kSlotArrays) out.push_back(Set(At(slot.name, V("Slot")), At(slot.name, V("Count"))));
        out.push_back(Set(V("Count"), Int(V("Count"), "OperatorSubtract", "1")));
        return out;
    }

    // 输入 AUC_Slot、AUC_Phase。完成与打断同时释放这一格。
    Lines Dispatch() const {
        Lines out;
        for (std::size_t i = 0; i < kDispatchArrays; ++i) out.push_back(Set(At(kSlotArrays[i].name, "0"), At(kSlotArrays[i].name, V("Slot"))));
        Append(out, If({CompareInt(V("Phase"), "OperatorNotEqual", std::to_string(kCastPhaseTick))}, {Run("释放", "Free")}));
        const auto param = [](std::string_view type, std::string_view name, const std::string& value) {
            return Call("YDWESetAnyTypeLocalVariable", {std::string(type), Quote(name), value});
        };
        Append(out, Block(Call("YDWEExecuteTriggerMultiple", {At("RegEffect", At("SlotReg", "0")), "tirggerexecute_notcondition"}), "params",
            {param("typename07_unit", "施法单位", At("Unit", "0")), param("typename07_unit", "目标单位", At("Target", "0")),
                param("typename02_real", "目标X", At("X", "0")), param("typename02_real", "目标Y", At("Y", "0")),
                param("typename01_integer", "技能", At("Ability", "0")), param("typename01_integer", "等级", At("Level", "0")),
                param("typename01_integer", "阶段", V("Phase")), param("typename01_integer", "跳数", At("Ticks", "0"))}));
        return out;
    }

    Lines FindCaster() const {
        return {Set(V("FindUnit"), "GetTriggerUnit()"), Run("查找施法者", "FindSlot")};
    }

    // 该格是本次施法的技能。
    [[nodiscard]] static std::string SameSpell() {
        return Call("OperatorCompareAbilityId", {At("Ability", V("Slot")), "OperatorEqualENE", "GetSpellAbilityId()"});
    }

    Lines Begin() const {
        Lines record;
        Append(record, FindCaster());
        Append(record, If({CompareInt(V("Slot"), "OperatorEqual", "0")},
            {Set(V("Count"), Int(V("Count"), "OperatorAdd", "1")), Set(V("Slot"), V("Count"))}));
        const std::string slot = V("Slot");
        Append(record, {
            Set(At("Unit", slot), "GetTriggerUnit()"),
            Set(At("SlotReg", slot), V("Reg")),
            Set(At("Ability", slot), "GetSpellAbilityId()"),
            Set(At("Level", slot), Call("GetUnitAbilityLevel", {"GetTriggerUnit()", "GetSpellAbilityId()"})),
            Set(At("Target", slot), "GetSpellTargetUnit()"),
            Set(V("Point"), "GetSpellTargetLoc()"),
            Set(At("X", slot), Call("GetLocationX", {V("Point")})),
            Set(At("Y", slot), Call("GetLocationY", {V("Point")})),
            Call("RemoveLocation", {V("Point")}),
            Set(At("Ticks", slot), "0"),
            Set(At("Stage", slot), "1"),
            Set(At("Start", slot), Call("TimerGetElapsed", {V("Clock")})),
            Set(At("Length", slot), At("RegCast", V("Reg"))),
        });
        Lines out{Set(V("FindAbility"), "GetSpellAbilityId()"), Run("查找技能", "FindAbility")};
        Append(out, If({CompareInt(V("Reg"), "OperatorGreater", "0")}, record));
        return out;
    }

    Lines Effect() const {
        const std::string slot = V("Slot");
        const std::string reg = At("SlotReg", slot);
        Lines out = FindCaster();
        Append(out, If({CompareInt(slot, "OperatorGreater", "0"), SameSpell(), CompareInt(At("Stage", slot), "OperatorEqual", "1")},
            If({CompareReal(At("RegChannel", reg), "OperatorGreater", "0.00")},
                {Set(At("Stage", slot), "2"), Set(At("Start", slot), Call("TimerGetElapsed", {V("Clock")})),
                    Set(At("Length", slot), At("RegChannel", reg))},
                {Set(V("Phase"), std::to_string(kCastPhaseFinish)), Run("派发", "Dispatch")})));
        return out;
    }

    // 应有的跳数：引导秒数 ÷ 跳间隔（取整，容许浮点误差）。
    [[nodiscard]] static std::string ExpectedTicks(const std::string& slot) {
        const std::string reg = At("SlotReg", slot);
        return Call("R2I", {Arith(Arith(At("RegChannel", reg), "OperatorDivide", At("RegTick", reg)), "OperatorAdd", "0.001")});
    }

    Lines Finish() const {
        const std::string slot = V("Slot");
        const auto channeling = [&] { return Lines{CompareInt(slot, "OperatorGreater", "0"), SameSpell(), CompareInt(At("Stage", slot), "OperatorEqual", "2")}; };
        // 刷新晚于引导结束时补上最后一跳；效果触发器可能改动格子，补完重新查找。
        Lines catchUp{Set(At("Ticks", slot), Int(At("Ticks", slot), "OperatorAdd", "1")), Set(V("Phase"), std::to_string(kCastPhaseTick)),
            Run("派发", "Dispatch")};
        Append(catchUp, FindCaster());
        Lines finish;
        Append(finish, If({CompareReal(At("RegTick", At("SlotReg", slot)), "OperatorGreater", "0.00"),
            CompareInt(At("Ticks", slot), "OperatorLess", ExpectedTicks(slot))}, catchUp));
        Append(finish, If(channeling(), {Set(V("Phase"), std::to_string(kCastPhaseFinish)), Run("派发", "Dispatch")}));
        Lines out = FindCaster();
        Append(out, If(channeling(), finish));
        return out;
    }

    Lines Stop() const {
        const std::string slot = V("Slot");
        Lines interrupt;
        if (o_.bar) {
            const std::string player = Call("GetConvertedPlayerId", {Call("GetOwningPlayer", {"GetTriggerUnit()"})});
            interrupt.push_back(Set(At("FlashUntil", player),
                Arith(Call("TimerGetElapsed", {V("Clock")}), "OperatorAdd", Real(o_.bar->flashSeconds))));
            interrupt.push_back(Set(At("FlashName", player), Call("GetAbilityName", {"GetSpellAbilityId()"})));
        }
        interrupt.push_back(Set(V("Phase"), std::to_string(kCastPhaseInterrupt)));
        interrupt.push_back(Run("派发", "Dispatch"));
        Lines out = FindCaster();
        Append(out, If({CompareInt(slot, "OperatorGreater", "0"), SameSpell()}, interrupt));
        return out;
    }

    Lines Refresh() const {
        const std::string slot = V("Slot");
        const std::string reg = At("SlotReg", slot);
        const std::string elapsed = Arith(V("Now"), "OperatorSubtract", At("Start", slot));
        const std::string due = Arith(Call("I2R", {Int(At("Ticks", slot), "OperatorAdd", "1")}), "OperatorMultiply", At("RegTick", reg));
        Lines tick = If({CompareInt(At("Stage", slot), "OperatorEqual", "2"), CompareReal(At("RegTick", reg), "OperatorGreater", "0.00"),
                            CompareInt(At("Ticks", slot), "OperatorLess", ExpectedTicks(slot)), CompareReal(elapsed, "OperatorGreaterEq", due)},
            {Set(At("Ticks", slot), Int(At("Ticks", slot), "OperatorAdd", "1")), Set(V("Phase"), std::to_string(kCastPhaseTick)),
                Run("派发", "Dispatch")});
        // 施法者死亡或已移除时游戏未必发停止事件：按打断处理并释放。
        Lines body{Set(slot, V("TickI"))};
        Append(body, If({Call("OperatorCompareBoolean", {Call("IsUnitDeadBJ", {At("Unit", slot)}), "OperatorEqualENE", "true"})},
            {Set(V("Phase"), std::to_string(kCastPhaseInterrupt)), Run("派发", "Dispatch")}, tick));
        Lines out{Set(V("Now"), Call("TimerGetElapsed", {V("Clock")}))};
        Append(out, LoopVar("TickI", "1", V("Count"), body));
        return out;
    }

    Lines Show() const {
        const CastBarBinding& bar = *o_.bar;
        const UiDescription& d = *bar.description;
        const auto value = [&](const std::string& id) { return "udg_" + UiValueVariableName(d.id, id); };
        const auto runUi = [&](std::string_view role, std::string_view key) {
            return Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d, role, key))});
        };
        const auto state = [&](const std::string& id) {
            return Call("ConditionalTriggerExecute", {TriggerRef(UiStateTriggerName(d, *FindState(d, id)))});
        };
        const bool hasStates = !d.states.empty();
        const std::string slot = V("ShowSlot");
        const std::string localId = Call("GetConvertedPlayerId", {"GetLocalPlayer()"});

        // 本机玩家的施法者里最近开始的一格（读条或引导有时长才显示）。
        Lines out{Call("CommentString", {Quote("只改本机界面；同步数据只在施法事件里改")}),
            Set(V("ShowNow"), Call("TimerGetElapsed", {V("Clock")})), Set(slot, "0")};
        Append(out, LoopVar("ShowI", "1", V("Count"),
            If({Call("OperatorComparePlayer", {Call("GetOwningPlayer", {At("Unit", V("ShowI"))}), "OperatorEqualENE", "GetLocalPlayer()"}),
                    CompareReal(At("Length", V("ShowI")), "OperatorGreater", "0.00")},
                {Set(slot, V("ShowI"))})));

        const std::string elapsed = Arith(V("ShowNow"), "OperatorSubtract", At("Start", slot));
        Lines casting{Set(V("ShowRatio"), Arith(elapsed, "OperatorDivide", At("Length", slot)))};
        Append(casting, If({CompareReal(V("ShowRatio"), "OperatorGreater", "1.00")}, {Set(V("ShowRatio"), "1.00")}));
        Append(casting, If({CompareInt(At("Stage", slot), "OperatorEqual", "2")},
            {Set(V("ShowRatio"), Arith("1.00", "OperatorSubtract", V("ShowRatio")))}));
        casting.push_back(Set(value(bar.visible), "true"));
        casting.push_back(Set(value(bar.progress), V("ShowRatio")));
        if (!bar.text.empty()) casting.push_back(Set(value(bar.text), Call("GetAbilityName", {At("Ability", slot)})));
        if (!bar.remaining.empty()) {
            casting.push_back(Set(value(bar.remaining), Arith(At("Length", slot), "OperatorSubtract", elapsed)));
            Append(casting, If({CompareReal(value(bar.remaining), "OperatorLess", "0.00")}, {Set(value(bar.remaining), "0.00")}));
        }
        // 状态只在切换时运行：1 读条、2 引导、3 打断、0 不显示。
        if (hasStates) {
            Lines toChannel{Set(V("Shown"), "2")};
            toChannel.push_back(bar.channel.empty() ? runUi("基础状态", "BaseState") : state(bar.channel));
            Append(casting, If({CompareInt(At("Stage", slot), "OperatorEqual", "2"), CompareInt(V("Shown"), "OperatorNotEqual", "2")}, toChannel));
            Append(casting, If({CompareInt(At("Stage", slot), "OperatorEqual", "1"), CompareInt(V("Shown"), "OperatorNotEqual", "1")},
                {Set(V("Shown"), "1"), runUi("基础状态", "BaseState")}));
        } else {
            casting.push_back(Set(V("Shown"), At("Stage", slot)));
        }
        casting.push_back(runUi("更新", "Update"));

        Lines flash{Set(V("Shown"), "3"), Set(value(bar.visible), "true"), Set(value(bar.progress), "1.00")};
        if (!bar.text.empty()) flash.push_back(Set(value(bar.text), At("FlashName", localId)));
        if (!bar.remaining.empty()) flash.push_back(Set(value(bar.remaining), "0.00"));
        if (!bar.interrupted.empty()) flash.push_back(state(bar.interrupted));
        flash.push_back(runUi("更新", "Update"));

        Lines hide{Set(V("Shown"), "0"), Set(value(bar.visible), "false")};
        if (hasStates) hide.push_back(runUi("基础状态", "BaseState"));
        hide.push_back(runUi("更新", "Update"));

        Lines idle = If({CompareReal(At("FlashUntil", localId), "OperatorGreater", V("ShowNow"))},
            If({CompareInt(V("Shown"), "OperatorNotEqual", "3")}, flash),
            If({CompareInt(V("Shown"), "OperatorNotEqual", "0")}, hide));
        Append(out, If({CompareInt(slot, "OperatorGreater", "0")}, casting, idle));
        return out;
    }

    const CastSystemOptions& o_;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<CastSystemPlan> GenerateCastSystem(const CastSystemOptions& options) {
    return Generator(options).Run();
}

std::string CastTriggerName(std::string_view name, std::string_view role, std::string_view key) {
    return std::string(name) + " " + std::string(role) + " " + std::string(kKey) + "_" + std::string(key);
}

} // namespace War3Map::GameUi
