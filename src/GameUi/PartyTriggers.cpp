#include "War3MapCore/GameUi/PartyTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <cmath>

// 团队框架的自然 T。名单在 AUP_Unit[1..AUP_Count]，界面数组按格子下标 0..格数-1。
// 刷新先做所有玩家相同的同步部分（移出已移除的单位、记下坐标），再按本机玩家算“过远”并写界面数值：
// 本机部分只做数值运算和界面调用，不创建句柄。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::string_view kKey = "Party";

std::string V(std::string_view name) { return "udg_AUP_" + std::string(name); }
std::string At(std::string_view name, std::string_view index) { return V(name) + "[" + std::string(index) + "]"; }

std::string SameUnit(std::string_view a, std::string_view b) {
    return Call("OperatorCompareUnit", {std::string(a), "OperatorEqualENE", std::string(b)});
}
std::string State(std::string_view state, std::string_view unit) {
    return Call("GetUnitStateSwap", {std::string(state), std::string(unit)});
}

// 框架所在的重复框架（自身或最近的祖先）；框架不存在时 found 为假。
const UiFrame* RepeatedOwner(const std::vector<UiFrame>& frames, std::string_view id, const UiFrame* group, bool& found) {
    for (const auto& frame : frames) {
        const UiFrame* owner = frame.repeat ? &frame : group;
        if (frame.id == id) {
            found = true;
            return owner;
        }
        const UiFrame* inner = RepeatedOwner(frame.children, id, owner, found);
        if (found) return inner;
    }
    return nullptr;
}

class Generator {
public:
    explicit Generator(const PartySystemOptions& options) : o_(options), b_(options.frame) {}

    Result<PartySystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<PartySystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        if (Buffs() && o_.includeRegistration) text_.Trigger(Name("登记", "Register"), {}, Registration(), {}, "init");
        // 被运行的触发器写在前面。
        text_.Trigger(Name("加入", "Join"), {}, Join());
        text_.Trigger(Name("移出", "Leave"), {}, Leave());
        if (o_.autoHeroes) {
            text_.Trigger(Name("候选", "Candidate"), {}, Candidate());
            text_.Trigger(Name("初始化", "Init"), {},
                Block(Call("ForGroupMultiple", {Call("GetUnitsInRectAll", {"GetPlayableMapRect()"})}), "actions",
                    {Set(V("Member"), "GetEnumUnit()"), RunTrigger("候选", "Candidate")}),
                {}, "init");
            text_.Trigger(Name("英雄进入", "HeroEnter"), {Call("TriggerRegisterEnterRectSimple", {"GetPlayableMapRect()"})},
                {Set(V("Member"), "GetTriggerUnit()"), RunTrigger("候选", "Candidate")});
        }
        text_.Trigger(Name("刷新", "Refresh"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kPartyRefreshSeconds)})}, Refresh());
        if (!b_.select.empty()) {
            text_.Trigger(Name("选中", "Select"), {Call("AudastraUITriggerRegisterEvent", {Quote(UiEventKey(d().id, b_.select))})}, Select());
        }

        PartySystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        plan.slotCount = slots_;
        plan.memberVariable = "AUP_Member";
        plan.joinTrigger = Name("加入", "Join");
        plan.leaveTrigger = Name("移出", "Leave");
        if (Buffs()) plan.registrationTrigger = Name("登记", "Register");
        return Result<PartySystemPlan>::Success(std::move(plan));
    }

private:
    [[nodiscard]] const UiDescription& d() const { return *b_.description; }
    [[nodiscard]] bool Buffs() const { return !b_.buffIcons.empty(); }
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Name(std::string_view role, std::string_view key) const { return PartyTriggerName(o_.name, role, key); }
    [[nodiscard]] std::string RunTrigger(std::string_view role, std::string_view key) const {
        return Call("ConditionalTriggerExecute", {TriggerRef(Name(role, key))});
    }
    [[nodiscard]] std::string Value(const std::string& id) const { return "udg_" + UiValueVariableName(d().id, id); }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUP_OPTION_INVALID", "团队框架的名字不能为空，也不能含双引号或控制字符");
        if (!b_.description) {
            Error("AUP_FRAME_INVALID", "团队框架没有界面描述");
            return;
        }
        if (b_.shown.empty()) {
            Error("AUP_FRAME_INVALID", "团队框架需要“显示”布尔数组数值");
            return;
        }
        const UiValue* shown = FindValue(d(), b_.shown);
        if (!shown || shown->type != UiValueType::Boolean || shown->size == 0) {
            Error("AUP_FRAME_INVALID", shown ? "界面 " + d().id + " 的数值 " + b_.shown + " 应为布尔数组" : "界面 " + d().id + " 没有数值 " + b_.shown);
            return;
        }
        slots_ = shown->size;
        const auto value = [&](const std::string& id, UiValueType type, const char* what) {
            if (id.empty()) return;
            if (const auto problem = CheckValueBinding(d(), id, type, slots_, what)) Error("AUP_FRAME_INVALID", *problem);
        };
        value(b_.health, UiValueType::Real, "实数");
        value(b_.mana, UiValueType::Real, "实数");
        value(b_.name, UiValueType::String, "字符串");
        value(b_.dead, UiValueType::Boolean, "布尔");
        value(b_.outOfRange, UiValueType::Boolean, "布尔");
        if (!b_.outOfRange.empty() && (!std::isfinite(o_.farRange) || o_.farRange < 100.0 || o_.farRange > 100000.0)) {
            Error("AUP_OPTION_INVALID", "过远距离应在 100 到 100000 之间");
        }
        if (!b_.select.empty()) CheckSelect();
        CheckBuffs();
    }

    void CheckBuffs() {
        for (std::size_t i = 0; i < b_.buffIcons.size(); ++i) {
            if (const auto problem = CheckValueBinding(d(), b_.buffIcons[i], UiValueType::String, slots_, "字符串")) Error("AUP_FRAME_INVALID", *problem);
            for (std::size_t j = 0; j < i; ++j) {
                if (b_.buffIcons[j] == b_.buffIcons[i]) Error("AUP_FRAME_INVALID", "图标数值 " + b_.buffIcons[i] + " 绑定了两次");
            }
        }
        if (!Buffs()) {
            if (!o_.buffs.empty()) Error("AUP_OPTION_INVALID", "登记了增减益，但没有绑定图标数值");
            return;
        }
        if (o_.buffs.size() > 64) Error("AUP_BUFF_INVALID", "最多登记 64 个增减益");
        for (std::size_t i = 0; i < o_.buffs.size(); ++i) {
            const PartyBuff& buff = o_.buffs[i];
            const std::string at = "第 " + std::to_string(i + 1) + " 个增减益（" + buff.buff + "）：";
            if (!ValidRawcode(buff.buff)) Error("AUP_BUFF_INVALID", at + "增减益码应为 4 个字符");
            if (!UsableLabel(buff.icon)) Error("AUP_BUFF_INVALID", at + "需要图标路径");
            for (std::size_t j = 0; j < i; ++j) {
                if (o_.buffs[j].buff == buff.buff) Error("AUP_BUFF_INVALID", at + "已在第 " + std::to_string(j + 1) + " 个登记");
            }
        }
    }

    void CheckSelect() {
        const UiEvent* event = FindEvent(d(), b_.select);
        if (!event) {
            Error("AUP_FRAME_INVALID", "界面 " + d().id + " 没有事件 " + b_.select);
            return;
        }
        if (event->kind != UiEventKind::Click) {
            Error("AUP_FRAME_INVALID", "选中队员要用点击事件，事件 " + b_.select + " 不是点击");
            return;
        }
        bool found = false;
        const UiFrame* owner = RepeatedOwner(d().frames, event->frame, nullptr, found);
        if (!owner || owner->repeat->count != slots_) {
            Error("AUP_FRAME_INVALID", "事件 " + b_.select + " 的框架 " + event->frame + " 应在重复 " + std::to_string(slots_) + " 次的框架里");
        }
    }

    void Variables() {
        const auto var = [&](std::string_view name, std::string_view type, bool array = false, const std::string& initial = {}) {
            text_.Variable("AUP_" + std::string(name), type, array ? std::optional<std::uint32_t>(0) : std::nullopt, initial);
        };
        var("Member", "unit");
        var("Unit", "unit", true);
        var("Count", "integer", false, "0");
        var("Found", "integer", false, "0");
        var("FindI", "integer", false, "0");
        var("I", "integer", false, "0");
        if (!b_.outOfRange.empty()) {
            var("X", "real", true);
            var("Y", "real", true);
            var("Point", "location");
            var("Self", "integer", false, "0");
            var("DX", "real", false, "0");
            var("DY", "real", false, "0");
        }
        if (!b_.select.empty()) var("Click", "integer", false, "0");
        if (Buffs()) {
            var("BuffCount", "integer", false, "0");
            var("BuffCode", "buffcode", true);
            var("BuffIcon", "string", true);
            var("BuffHit", "string", true);
            var("BuffN", "integer", false, "0");
            var("BuffI", "integer", false, "0");
        }
    }

    Lines Registration() const {
        Lines out{Call("CommentString", {Quote("每个增减益一组：增减益、图标路径；每格按这里的顺序显示队员身上有的增减益")}),
            Set(V("BuffCount"), std::to_string(o_.buffs.size()))};
        for (std::size_t i = 0; i < o_.buffs.size(); ++i) {
            const std::string index = std::to_string(i + 1);
            out.push_back(Set(At("BuffCode", index), "'" + o_.buffs[i].buff + "'"));
            out.push_back(Set(At("BuffIcon", index), Quote(o_.buffs[i].icon)));
        }
        return out;
    }

    // AUP_Member 在名单里的格子 → AUP_Found（不在为 0）。
    static Lines Find() {
        Lines out{Set(V("Found"), "0")};
        Append(out, ForLoopVar(V("FindI"), "1", V("Count"), If({SameUnit(At("Unit", V("FindI")), V("Member"))}, {Set(V("Found"), V("FindI"))})));
        return out;
    }

    Lines Join() const {
        Lines out{Call("CommentString", {Quote("先把要加入的单位设给 AUP_Member；已在名单里或名单已满时不加入")})};
        Append(out, Find());
        Append(out, If({CompareInt(V("Found"), "OperatorEqual", "0"), CompareInt(V("Count"), "OperatorLess", std::to_string(slots_)),
                           Call("OperatorCompareUnit", {V("Member"), "OperatorNotEqualENE", "UnitNull"})},
            {Set(V("Count"), Int(V("Count"), "OperatorAdd", "1")), Set(At("Unit", V("Count")), V("Member"))}));
        return out;
    }

    static Lines Leave() {
        Lines out{Call("CommentString", {Quote("先把要移出的单位设给 AUP_Member；后面的队员前移一格")})};
        Append(out, Find());
        Lines remove = ForLoopVar(V("FindI"), V("Found"), Int(V("Count"), "OperatorSubtract", "1"),
            {Set(At("Unit", V("FindI")), At("Unit", Int(V("FindI"), "OperatorAdd", "1")))});
        remove.push_back(Set(At("Unit", V("Count")), "UnitNull"));
        remove.push_back(Set(V("Count"), Int(V("Count"), "OperatorSubtract", "1")));
        Append(out, If({CompareInt(V("Found"), "OperatorGreater", "0")}, remove));
        return out;
    }

    // 用户玩家（正在游戏）的英雄，不含幻象。
    Lines Candidate() const {
        const std::string owner = Call("GetOwningPlayer", {V("Member")});
        return If({IsTrue(Call("IsUnitType", {V("Member"), "UnitTypeHero"})),
                      Call("OperatorCompareBoolean", {Call("IsUnitIllusionBJ", {V("Member")}), "OperatorEqualENE", "false"}),
                      Call("OperatorComparePlayerControl", {Call("GetPlayerController", {owner}), "OperatorEqualENE", "MapControlUser"}),
                      Call("OperatorComparePlayerSlotStatus", {Call("GetPlayerSlotState", {owner}), "OperatorEqualENE", "PlayerSlotStatePlaying"})},
            {RunTrigger("加入", "Join")});
    }

    // 比例 = 当前 ÷ 最大；最大为 0 时为 0。
    Lines Ratio(const std::string& target, std::string_view current, std::string_view maximum, const std::string& unit) const {
        return If({CompareReal(State(maximum, unit), "OperatorGreater", "0")},
            {Set(target, Arith(State(current, unit), "OperatorDivide", State(maximum, unit)))}, {Set(target, "0")});
    }

    Lines Refresh() const {
        const std::string unit = At("Unit", V("I"));
        // 已移除的单位最大生命为 0。移出后这一轮跳过下一格，下一轮再处理。
        Lines out = ForLoopVar(V("I"), "1", V("Count"),
            If({CompareReal(State("UnitStateMaxLife", unit), "OperatorLessEq", "0")}, {Set(V("Member"), unit), RunTrigger("移出", "Leave")}));
        if (!b_.outOfRange.empty()) {
            Append(out, ForLoopVar(V("I"), "1", V("Count"),
                {Set(V("Point"), Call("GetUnitLoc", {unit})), Set(At("X", V("I")), Call("GetLocationX", {V("Point")})),
                    Set(At("Y", V("I")), Call("GetLocationY", {V("Point")})), Call("RemoveLocation", {V("Point")})}));
        }
        out.push_back(Call("CommentString", {Quote("以下只改本机界面")}));
        if (!b_.outOfRange.empty()) {
            out.push_back(Set(V("Self"), "0"));
            Append(out, ForLoopVar(V("I"), "1", V("Count"),
                If({CompareInt(V("Self"), "OperatorEqual", "0"),
                       Call("OperatorComparePlayer", {Call("GetOwningPlayer", {unit}), "OperatorEqualENE", "GetLocalPlayer()"})},
                    {Set(V("Self"), V("I"))})));
        }

        const std::string cell = Int(V("I"), "OperatorSubtract", "1");
        const auto at = [&](const std::string& id) { return Value(id) + "[" + cell + "]"; };
        Lines filled{Set(at(b_.shown), "true")};
        if (!b_.health.empty()) Append(filled, Ratio(at(b_.health), "UnitStateLife", "UnitStateMaxLife", unit));
        if (!b_.mana.empty()) Append(filled, Ratio(at(b_.mana), "UnitStateMana", "UnitStateMaxMana", unit));
        if (!b_.name.empty()) {
            if (o_.nameSource == PartyNameSource::Player) {
                filled.push_back(Set(at(b_.name), Call("GetPlayerName", {Call("GetOwningPlayer", {unit})})));
            } else {
                Append(filled, If({IsTrue(Call("IsUnitType", {unit, "UnitTypeHero"}))}, {Set(at(b_.name), Call("GetHeroProperName", {unit}))},
                    {Set(at(b_.name), Call("GetUnitName", {unit}))}));
            }
        }
        if (!b_.dead.empty()) filled.push_back(Set(at(b_.dead), Call("IsUnitDeadBJ", {unit})));
        if (!b_.outOfRange.empty()) {
            filled.push_back(Set(V("DX"), Arith(At("X", V("I")), "OperatorSubtract", At("X", V("Self")))));
            filled.push_back(Set(V("DY"), Arith(At("Y", V("I")), "OperatorSubtract", At("Y", V("Self")))));
            const std::string distance = Arith(Arith(V("DX"), "OperatorMultiply", V("DX")), "OperatorAdd", Arith(V("DY"), "OperatorMultiply", V("DY")));
            filled.push_back(Set(at(b_.outOfRange), "false"));
            Append(filled, If({CompareInt(V("Self"), "OperatorGreater", "0"), CompareReal(distance, "OperatorGreater", Real(o_.farRange * o_.farRange))},
                {Set(at(b_.outOfRange), "true")}));
        }
        if (Buffs()) Append(filled, BuffIcons(unit, at));
        Append(out, ForLoopVar(V("I"), "1", std::to_string(slots_),
            If({CompareInt(V("I"), "OperatorLessEq", V("Count"))}, filled, {Set(at(b_.shown), "false")})));
        out.push_back(Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d(), "更新", "Update"))}));
        return out;
    }

    // 按登记顺序收集队员身上的增减益图标，填进各图标数值，多出的为空。
    template <typename CellRef>
    Lines BuffIcons(const std::string& unit, const CellRef& at) const {
        const std::string slots = std::to_string(b_.buffIcons.size());
        Lines out;
        for (std::size_t k = 1; k <= b_.buffIcons.size(); ++k) out.push_back(Set(At("BuffHit", std::to_string(k)), Quote("")));
        out.push_back(Set(V("BuffN"), "0"));
        Append(out, ForLoopVar(V("BuffI"), "1", V("BuffCount"),
            If({CompareInt(V("BuffN"), "OperatorLess", slots), IsTrue(Call("UnitHasBuffBJ", {unit, At("BuffCode", V("BuffI"))}))},
                {Set(V("BuffN"), Int(V("BuffN"), "OperatorAdd", "1")), Set(At("BuffHit", V("BuffN")), At("BuffIcon", V("BuffI")))})));
        for (std::size_t k = 0; k < b_.buffIcons.size(); ++k) out.push_back(Set(at(b_.buffIcons[k]), At("BuffHit", std::to_string(k + 1))));
        return out;
    }

    Lines Select() const {
        Lines out{Call("CommentString", {Quote("界面点击只在本机发生；这里只改本机的选择")}),
            Set(V("Click"), Int("AudastraUIGetTriggerIndex()", "OperatorAdd", "1"))};
        Append(out, If({CompareInt(V("Click"), "OperatorGreaterEq", "1"), CompareInt(V("Click"), "OperatorLessEq", V("Count"))},
            {Call("SelectUnitForPlayerSingle", {At("Unit", V("Click")), "GetLocalPlayer()"})}));
        return out;
    }

    const PartySystemOptions& o_;
    const PartyFrameBinding& b_;
    std::uint32_t slots_ = 0;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<PartySystemPlan> GeneratePartySystem(const PartySystemOptions& options) {
    return Generator(options).Run();
}

std::string PartyTriggerName(std::string_view name, std::string_view role, std::string_view key) {
    return std::string(name) + " " + std::string(role) + " " + std::string(kKey) + "_" + std::string(key);
}

} // namespace War3Map::GameUi
