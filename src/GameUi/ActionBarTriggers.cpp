#include "War3MapCore/GameUi/ActionBarTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "UnitSourceLines.h"
#include "War3MapCore/GameUi/UiTriggers.h"

// 技能栏的自然 T。每个玩家的当前单位在 AUA_<键>_Unit[玩家编号]（1 起）。刷新先做所有玩家相同的同步部分（清掉已移除的单位），
// 再按本机玩家编号 AUA_<键>_Me 读自己的单位写界面数值。本机部分只给整数、实数、文字、技能码等非句柄变量赋值，
// 不把单位写进变量，避免各玩家的句柄引用不同。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::size_t kMaxKeyLength = 24;

class Generator {
public:
    explicit Generator(const ActionBarSystemOptions& options)
        : o_(options), b_(options.frame), source_("AUA_" + options.key, options.source, options.players) {}

    Result<ActionBarSystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<ActionBarSystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        if (o_.includeRegistration) text_.Trigger(Name("登记", "Register"), {}, Registration(), {}, "init");
        source_.Triggers(text_, [this](std::string_view role, std::string_view roleKey) { return Name(role, roleKey); });
        text_.Trigger(Name("刷新", "Refresh"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kActionBarRefreshSeconds)})}, Refresh());
        if (!b_.use.empty()) {
            text_.Trigger(Name("使用", "Use"), {Call("AudastraUITriggerRegisterEvent", {Quote(UiEventKey(d().id, b_.use))})}, Use());
        }

        ActionBarSystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        plan.slotCount = slots_;
        if (o_.source == TargetSource::Focus) {
            plan.memberVariable = Var("Member");
            plan.playerVariable = Var("Player");
            plan.setTrigger = Name("设定", "Set");
        }
        plan.registrationTrigger = Name("登记", "Register");
        return Result<ActionBarSystemPlan>::Success(std::move(plan));
    }

private:
    [[nodiscard]] const UiDescription& d() const { return *b_.description; }
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Var(std::string_view name) const { return source_.Var(name); }
    [[nodiscard]] std::string V(std::string_view name) const { return source_.V(name); }
    [[nodiscard]] std::string At(std::string_view name, std::string_view index) const { return V(name) + "[" + std::string(index) + "]"; }
    [[nodiscard]] std::string Name(std::string_view role, std::string_view roleKey) const {
        return TargetTriggerName(o_.name, o_.key, role, roleKey);
    }
    // 本机玩家的当前单位。
    [[nodiscard]] std::string Unit() const { return source_.At(V("Me")); }
    // 第 S 格（1 起）在界面数组里的格（0 起）。
    [[nodiscard]] std::string Cell(const std::string& id) const {
        return "udg_" + UiValueVariableName(d().id, id) + "[" + Int(V("S"), "OperatorSubtract", "1") + "]";
    }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUA_OPTION_INVALID", "技能栏的名字不能为空，也不能含双引号或控制字符");
        if (!IsValidUiId(o_.key) || o_.key.size() > kMaxKeyLength)
            Error("AUA_OPTION_INVALID", "技能栏的键应为 ASCII 字母或下划线开头的标识符，最长 " + std::to_string(kMaxKeyLength));
        if (o_.players < 1 || o_.players > kActionBarMaxPlayers)
            Error("AUA_OPTION_INVALID", "玩家数应在 1 到 " + std::to_string(kActionBarMaxPlayers) + " 之间");
        if (!b_.description) {
            Error("AUA_FRAME_INVALID", "技能栏没有界面描述");
            return;
        }
        const UiValue* shown = b_.shown.empty() ? nullptr : FindValue(d(), b_.shown);
        if (!shown || shown->type != UiValueType::Boolean || shown->size < 1 || shown->size > kActionBarMaxSlots) {
            Error("AUA_FRAME_INVALID", "技能栏需要“显示”布尔数组（1 到 " + std::to_string(kActionBarMaxSlots) + " 格），格数即格数");
            return;
        }
        slots_ = shown->size;
        const auto value = [&](const std::string& id, UiValueType type, std::uint32_t size, const char* what) {
            if (id.empty()) return;
            if (const auto problem = CheckValueBinding(d(), id, type, size, what)) Error("AUA_FRAME_INVALID", *problem);
        };
        value(b_.barShown, UiValueType::Boolean, 0, "布尔");
        value(b_.icon, UiValueType::String, slots_, "字符串");
        value(b_.key, UiValueType::String, slots_, "字符串");
        value(b_.level, UiValueType::Integer, slots_, "整数");
        value(b_.cooldown, UiValueType::Real, slots_, "实数");
        value(b_.cooldownText, UiValueType::String, slots_, "字符串");
        value(b_.noMana, UiValueType::Boolean, slots_, "布尔");
        if (!o_.abilityState && (!b_.cooldown.empty() || !b_.cooldownText.empty() || !b_.noMana.empty()))
            Error("AUA_OPTION_INVALID", "冷却比例、冷却文字与法力不足要开技能状态（YDWE 技能属性）");
        if (!b_.use.empty()) CheckUse();
        CheckEntries();
    }

    void CheckUse() {
        const UiEvent* event = FindEvent(d(), b_.use);
        if (!event) {
            Error("AUA_FRAME_INVALID", "界面 " + d().id + " 没有事件 " + b_.use);
            return;
        }
        if (event->kind != UiEventKind::Click) {
            Error("AUA_FRAME_INVALID", "使用技能要用点击事件，事件 " + b_.use + " 不是点击");
            return;
        }
        bool found = false;
        const UiFrame* owner = RepeatedOwner(d().frames, event->frame, nullptr, found);
        if (!owner || owner->repeat->count != slots_) {
            Error("AUA_FRAME_INVALID", "事件 " + b_.use + " 的框架 " + event->frame + " 应在 " + std::to_string(slots_) + " 个的重复框架里，按实例对应格");
        }
    }

    void CheckEntries() {
        if (o_.entries.size() > kActionBarMaxEntries) Error("AUA_ENTRY_INVALID", "最多登记 " + std::to_string(kActionBarMaxEntries) + " 格");
        for (std::size_t i = 0; i < o_.entries.size(); ++i) {
            const ActionBarEntry& entry = o_.entries[i];
            const std::string at = "第 " + std::to_string(i + 1) + " 项（" + entry.unit + " 第 " + std::to_string(entry.slot) + " 格）：";
            if (!ValidRawcode(entry.unit)) Error("AUA_ENTRY_INVALID", at + "单位类型应为 4 个字符");
            if (!ValidRawcode(entry.ability)) Error("AUA_ENTRY_INVALID", at + "技能应为 4 个字符");
            if (entry.slot < 1 || entry.slot > slots_) Error("AUA_ENTRY_INVALID", at + "格应在 1 到 " + std::to_string(slots_) + " 之间");
            if (!UsableLabel(entry.icon)) Error("AUA_ENTRY_INVALID", at + "需要图标路径");
            if (!entry.key.empty() && !UsableLabel(entry.key)) Error("AUA_ENTRY_INVALID", at + "热键不能含双引号或控制字符");
            for (std::size_t j = 0; j < i; ++j) {
                if (o_.entries[j].unit == entry.unit && o_.entries[j].slot == entry.slot)
                    Error("AUA_ENTRY_INVALID", at + "这一格已在第 " + std::to_string(j + 1) + " 项登记");
            }
        }
    }

    void Variables() {
        source_.Variables(text_);
        text_.Variable(Var("P"), "integer", std::nullopt, "0");
        text_.Variable(Var("Me"), "integer", std::nullopt, "0");
        text_.Variable(Var("S"), "integer", std::nullopt, "0");
        text_.Variable(Var("I"), "integer", std::nullopt, "0");
        text_.Variable(Var("L"), "integer", std::nullopt, "0");
        if (!b_.use.empty()) text_.Variable(Var("Click"), "integer", std::nullopt, "0");
        text_.Variable(Var("LastType"), "unitcode");
        text_.Variable(Var("Count"), "integer", std::nullopt, "0");
        text_.Variable(Var("Type"), "unitcode", 0);
        text_.Variable(Var("Slot"), "integer", 0);
        text_.Variable(Var("Ability"), "abilcode", 0);
        text_.Variable(Var("Icon"), "string", 0);
        text_.Variable(Var("Key"), "string", 0);
        text_.Variable(Var("SlotUsed"), "boolean", 0);
        text_.Variable(Var("SlotAbility"), "abilcode", 0);
        text_.Variable(Var("SlotIcon"), "string", 0);
        text_.Variable(Var("SlotKey"), "string", 0);
        if (o_.abilityState) {
            text_.Variable(Var("Cooldown"), "real", std::nullopt, "0");
            text_.Variable(Var("Total"), "real", std::nullopt, "0");
        }
    }

    Lines Registration() const {
        Lines out{Call("CommentString", {Quote("每格一组：单位类型、第几格（1 起）、技能、图标路径、热键；热键要和物编里这个技能的热键一致，点格子时按下它")}),
            Set(V("Count"), std::to_string(o_.entries.size()))};
        for (std::size_t i = 0; i < o_.entries.size(); ++i) {
            const ActionBarEntry& entry = o_.entries[i];
            const std::string index = std::to_string(i + 1);
            out.push_back(Set(At("Type", index), "'" + entry.unit + "'"));
            out.push_back(Set(At("Slot", index), std::to_string(entry.slot)));
            out.push_back(Set(At("Ability", index), "'" + entry.ability + "'"));
            out.push_back(Set(At("Icon", index), Quote(entry.icon)));
            out.push_back(Set(At("Key", index), Quote(entry.key)));
        }
        return out;
    }

    // 单位类型变了才按登记重建每格的技能表。
    Lines Rebuild() const {
        const std::string slot = At("Slot", V("I"));
        Lines out{Set(V("LastType"), Call("GetUnitTypeId", {Unit()}))};
        Append(out, ForLoopVar(V("S"), "1", std::to_string(slots_), {Set(At("SlotUsed", V("S")), "false")}));
        Append(out, ForLoopVar(V("I"), "1", V("Count"),
            If({Call("OperatorCompareUnitCode", {At("Type", V("I")), "OperatorEqualENE", V("LastType")}),
                   CompareInt(slot, "OperatorGreaterEq", "1"), CompareInt(slot, "OperatorLessEq", std::to_string(slots_))},
                {Set(At("SlotUsed", slot), "true"), Set(At("SlotAbility", slot), At("Ability", V("I"))),
                    Set(At("SlotIcon", slot), At("Icon", V("I"))), Set(At("SlotKey", slot), At("Key", V("I")))})));
        return out;
    }

    // 剩余冷却与法力消耗取自 YDWE 技能属性。剩余超过总冷却（被触发器加长）或总冷却为 0 时比例为 1。
    Lines AbilityState() const {
        const std::string ability = At("SlotAbility", V("S"));
        Lines out{Set(V("Cooldown"), Call("YDWEGetUnitAbilityState", {Unit(), ability, "ABILITY_STATE_COOLDOWN"})),
            Set(V("Total"), Call("YDWEGetUnitAbilityDataReal", {Unit(), ability, V("L"), "ABILITY_DATA_COOL"}))};
        if (!b_.cooldown.empty()) {
            Append(out, If({CompareReal(V("Cooldown"), "OperatorGreater", "0")},
                If({CompareReal(V("Total"), "OperatorGreater", V("Cooldown"))},
                    {Set(Cell(b_.cooldown), Arith(V("Cooldown"), "OperatorDivide", V("Total")))}, {Set(Cell(b_.cooldown), "1")}),
                {Set(Cell(b_.cooldown), "0")}));
        }
        if (!b_.cooldownText.empty()) {
            Append(out, If({CompareReal(V("Cooldown"), "OperatorGreater", "0")},
                If({CompareReal(V("Cooldown"), "OperatorLess", "10")}, {Set(Cell(b_.cooldownText), Call("AudastraUIFormatReal", {V("Cooldown"), "1"}))},
                    {Set(Cell(b_.cooldownText), Call("AudastraUIFormatReal", {V("Cooldown"), "0"}))}),
                {Set(Cell(b_.cooldownText), Quote(""))}));
        }
        if (!b_.noMana.empty()) {
            const std::string cost = Call("I2R", {Call("YDWEGetUnitAbilityDataInteger", {Unit(), ability, V("L"), "ABILITY_DATA_COST"})});
            Append(out, If({CompareReal(Call("GetUnitStateSwap", {"UnitStateMana", Unit()}), "OperatorLess", cost)},
                {Set(Cell(b_.noMana), "true")}, {Set(Cell(b_.noMana), "false")}));
        }
        return out;
    }

    Lines Refresh() const {
        Lines out = source_.Cleanup(V("P"));
        out.push_back(Call("CommentString", {Quote("以下只改本机界面")}));
        out.push_back(Set(V("Me"), Call("GetConvertedPlayerId", {"GetLocalPlayer()"})));
        Lines slot;
        if (!b_.icon.empty()) slot.push_back(Set(Cell(b_.icon), At("SlotIcon", V("S"))));
        if (!b_.key.empty()) slot.push_back(Set(Cell(b_.key), At("SlotKey", V("S"))));
        if (!b_.level.empty()) slot.push_back(Set(Cell(b_.level), V("L")));
        if (o_.abilityState) Append(slot, AbilityState());
        slot.insert(slot.begin(), Set(Cell(b_.shown), "true"));
        Lines each{Set(V("L"), "0")};
        Append(each, If({IsTrue(At("SlotUsed", V("S")))},
            {Set(V("L"), Call("GetUnitAbilityLevelSwapped", {At("SlotAbility", V("S")), Unit()}))}));
        Append(each, If({CompareInt(V("L"), "OperatorGreater", "0")}, slot, {Set(Cell(b_.shown), "false")}));

        Lines present;
        if (!b_.barShown.empty()) present.push_back(Set("udg_" + UiValueVariableName(d().id, b_.barShown), "true"));
        Append(present, If({Call("OperatorCompareUnitCode", {Call("GetUnitTypeId", {Unit()}), "OperatorNotEqualENE", V("LastType")})}, Rebuild()));
        Append(present, ForLoopVar(V("S"), "1", std::to_string(slots_), each));
        Lines absent;
        if (!b_.barShown.empty()) absent.push_back(Set("udg_" + UiValueVariableName(d().id, b_.barShown), "false"));
        Append(absent, ForLoopVar(V("S"), "1", std::to_string(slots_), {Set(Cell(b_.shown), "false")}));
        Append(out, If({Call("OperatorCompareUnit", {Unit(), "OperatorNotEqualENE", "UnitNull"})}, present, absent));
        out.push_back(Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d(), "更新", "Update"))}));
        return out;
    }

    // 选中的就是当前单位时按下这一格的热键，交给游戏的命令面板；否则先选中当前单位。
    Lines Use() const {
        const std::string slot = V("Click");
        Lines press = If({IsTrue(Call("IsUnitSelected", {Unit(), "GetLocalPlayer()"})),
                             Call("OperatorCompareString", {At("SlotKey", slot), "OperatorNotEqualENE", Quote("")})},
            {Call("ForceUIKeyBJ", {"GetLocalPlayer()", At("SlotKey", slot)})}, {Call("SelectUnitForPlayerSingle", {Unit(), "GetLocalPlayer()"})});
        Lines out{Call("CommentString", {Quote("界面点击只在本机发生：选中的就是当前单位时按下这一格的热键，否则先选中当前单位")}),
            Set(V("Me"), Call("GetConvertedPlayerId", {"GetLocalPlayer()"})), Set(slot, Int("AudastraUIGetTriggerIndex()", "OperatorAdd", "1"))};
        Append(out, If({Call("OperatorCompareUnit", {Unit(), "OperatorNotEqualENE", "UnitNull"}), CompareInt(slot, "OperatorGreaterEq", "1"),
                           CompareInt(slot, "OperatorLessEq", std::to_string(slots_)), IsTrue(At("SlotUsed", slot))},
            press));
        return out;
    }

    const ActionBarSystemOptions& o_;
    const ActionBarFrameBinding& b_;
    UnitSourceLines source_;
    std::uint32_t slots_ = 0;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<ActionBarSystemPlan> GenerateActionBarSystem(const ActionBarSystemOptions& options) {
    return Generator(options).Run();
}

} // namespace War3Map::GameUi
